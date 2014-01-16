#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <limits.h>
#include <ctype.h>
#include <errno.h>
#include <string.h>
#include <sys/wait.h>
#include <fcntl.h>

#include "stringbuilder.h"

// I/O redirection commands that could probably supported:
// < (Input from file, same as 0<)
// | (Pipe)
// > (Redirect stdout to file, same as 1>)
// 2> (Redirect stderr, and for general values of 2 redirect the specified file
// descriptor (to file, or to other file descriptor, or close the descriptor))
// &> (Redirect both stderr and stdout to file; this is just shorthand)
// >> (Append stdout to file, same as 1>>)
// 2>> (Append stderr to file)
// &>> (Append both stderr and stdout to file)
// &| (this is bash shorthand for 2>&1 |. pipes both stdout and stderr)

// Bash would let you globally redirect file descriptors as well (using exec)
// but I don't really feel like implementing that (exec, that is)

// Bash also supports using parentheses to group redirections (e.g. a | b < c
// vs (a | b) < c; since you can always reorder arguments to avoid using
// parentheses I don't see the point.

// How parsing works (with regards to redirection):
// Upon encountering any redirection, we first need to grab any surrounding
// characters that specify variants.
//   A < means we need to attach the next token (interpreted as a file name) 
//   to stdin. (Or whatever file descriptor was specified)
//   A > means we need to attach the next token (interpreted as a file name)
//   to stdout. (Or whatever file descriptor was specified)
//   A | ends the current command (yes, that's right, > doesn't, so a > b c
//   is equivalent to a c > b) and means we need to attach the stdout (or
//   whatever file descriptor) to the stdin (or whatever file descriptor)

// Redirection commands will be processed in the order they are given.
// Yes, it's legal to redirect an output stream to multiple places. Just
// attach the stdin of each of those places to the read side of the pipe.
// It is also legal to take multiple inputs (try cat < file1 < file2), and
// that, of course, is implemented by attaching all of them to the write
// side of the pipe.

typedef struct node {
  char *str;

  int nargs;
  char **args;

  //int fdtable[10][2];

  int nins;
  char **ins; // Files redirected to input
  int nouts;
  char **outs; // Files output is redirected to
  // Do the same for the error stream, and maybe other file descriptors if it
  // comes up
} node_t;

// Helper functions for I/O redirection

void writetopipe(FILE *f, int fd) {
  // Writes the contents of f to fd
  char c;
  while ((c = getc(f))) { /* Parentheses are to keep the compiler quiet */
    write(fd, &c, 1);
  }
}

void writetofile(FILE *f, int fd) {
  // Writes the output from fd into f
  char c;
  while(read(fd, &c, 1) > 0) {
    putc(c, f);
  }
}

// Parses a single command, starting from idx and ending at either EOF or
// a pipe. Returns NULL if you did something stupid (trying to redirect to
// something which is not a filename, for example)
node_t *parsecmd(char *cmd, int startidx, int *endidx) {
  int i;
  stringbuilder_t *sb;

  node_t *ptr = calloc(1,sizeof(node_t));
  // I do make the (mostly portable) assumption that NULL is 0 here

  sb = nstringb();

  // Parse the first token; if it is < or >, then the command is empty
  // (this is legal! try typing something like "<Makefile|cat" in zsh.
  // bash won't do anything, but zsh will do cat<Makefile|cat.
  // Multiple input actually doesn't seem like a bizarre or meaningless thing
  // to do; consider doing something like ls | cat < file1 > file2.
  // Multiple output is kind of weird, but maybe you want to write some
  // intermediate results to file (on, say, a long-running command and/or you
  // want to write many such files, so you don't want to keep coming back to
  // continue running)
  
  i = parse_token(sb, cmd, startidx);
  if (sb->curlen == 0) {
    if (cmd[i] == '<') {
      // Command is empty
      ptr->str = malloc(1);
      *(ptr->str) = 0;

      // Get the next token and process it correctly
      i = parse_token(sb, cmd, i+1);
      // If this one is also empty, that's a parse error.
      // It indicates either that you have two consequent redirection commands
      // or you haven't given a file name for redirection (for example ls >)
      if (sb->curlen == 0) {
	free(ptr);
	destroy_stringb(sb);
	return NULL;
      }
      // Otherwise put the command into the right place
      ptr->nins++;
      ptr->ins = realloc(ptr->ins, ptr->nins * sizeof(char *));
      ptr->ins[ptr->nins-1] = to_string(sb);
      // Command is empty
    }
    else if (cmd[i] == '>') {
      ptr->str = malloc(1);
      *(ptr->str) = 0;

      // Get the next token and process it correctly
      i = parse_token(sb, cmd, i+1);
      // If this one is also empty, that's a parse error.
      // It indicates either that you have two consequent redirection commands
      // or you haven't given a file name for redirection (for example ls >)
      if (sb->curlen == 0) {
	free(ptr);
	destroy_stringb(sb);
	return NULL;
      }
      // Otherwise put the command into the right place
      ptr->nouts++;
      ptr->outs = realloc(ptr->outs, ptr->nouts * sizeof(char *));
      ptr->outs[ptr->nouts-1] = to_string(sb);
    }
    else if (cmd[i] == 0) {
      // Empty command with no redirection? I assume the user just typed a
      // newline at the terminal... Should be handled properly at the running
      // side, by checking whether ptr->str is null.
      *endidx = i;
      destroy_stringb(sb);
      return ptr;
    }
    else {
      // Probably hit a pipe; that's a parse error
      free(ptr);
      destroy_stringb(sb);
      return NULL;
    }
  }
  else {
    ptr->str = to_string(sb);
    ptr->nargs++;
    ptr->args = malloc(sizeof(char*));
    ptr->args[0] = to_string(sb);
  }

  while (cmd[i] && (cmd[i] != '|')) {
    // Continue parsing arguments
    i = parse_token(sb, cmd, i);
    if (sb->curlen == 0) {
      if (cmd[i] == '<') {
	i = parse_token(sb, cmd, i+1);
	if (sb->curlen == 0) {
	  free(ptr);
	  destroy_stringb(sb);
	  return NULL;
	}
	ptr->nins++;
	ptr->ins = realloc(ptr->ins, ptr->nins * sizeof(char *));
	ptr->ins[ptr->nins-1] = to_string(sb);
      }
      
      else if (cmd[i] == '>') {
	i = parse_token(sb, cmd, i+1);
	if (sb->curlen == 0) {
	  free(ptr);
	  destroy_stringb(sb);
	  return NULL;
	}
	ptr->nouts++;
	ptr->outs = realloc(ptr->outs, ptr->nouts * sizeof(char *));
	ptr->outs[ptr->nouts-1] = to_string(sb);
      }
      else {
	// We've hit either the end of the command or a pipe; it doesn't
	// really matter which (the caller can check, since we provide the end
	// index)
	break;
      }
    }
    else {
      // Add the argument
      ptr->nargs++;
      ptr->args = realloc(ptr->args, ptr->nargs * sizeof(char *));
      ptr->args[ptr->nargs-1] = to_string(sb);
    }
  }
  // Pad argument list with a NULL
  ptr->nargs++;
  ptr->args = realloc(ptr->args, ptr->nargs * sizeof(char *));
  ptr->args[ptr->nargs-1] = NULL;
  *endidx = i;
  destroy_stringb(sb);
  return ptr;
}


int main() {
  // Print a prompt
  char *dirbuf;
  char *hostname;
  char *cmdbuf;

  node_t **base;

  int i, j, k;
  int pipefds[2][2];

  dirbuf = malloc(PATH_MAX + 1);
  hostname = malloc(HOST_NAME_MAX + 1);
  cmdbuf = malloc(1025); // Apparently this should be a kernel parameter;
  // ARG_MAX is above 2 million on my system!
  
  
  getcwd(dirbuf, PATH_MAX);
  gethostname(hostname, HOST_NAME_MAX);
  
  printf("%s@%s:%s>", getlogin(), hostname, dirbuf);
  // Get a command from stdin

  // TODO: fix this. We need to check whether cmdbuf has unbalanced quotes
  // or ends with a backslash; in those cases we need to read another line
  fgets(cmdbuf, 1024, stdin);
  
  // Tokenize it (Relevant separators for the redirection structure are
  // <, >, and |; we can just look at the adjacent characters to determine
  // any variants)

  base = malloc(sizeof (node_t*) * 10); // Change later
  // Tokenize (building the data structure as we go)
  i = 0;
  for (j = 0; cmdbuf[i]; ++j) {
    base[j] = parsecmd(cmdbuf, i, &i); // This is kind of a poor idea
    // If we haven't hit a pipe we're done; otherwise increment i
    if (cmdbuf[i] == '|')
      ++i;
  }

  // j is the number of nodes; I should probably store this in a better named
  // variable

  /*

  // Print everything out to check that nothing is too stupid
  for (k = 0; k < j; ++k) {
    printf("Node %d:\n", k);
    // Print the command
    if (base[k]->str) {
      printf("Command: %s\n", base[k]->str);
      
      printf("Args: \n");
      for (i = 0; i < base[k]->nargs; ++i)
	printf("%s ", base[k]->args[i]);
      printf("\n");
      
      // TODO: print the redirections too (I'm feeling lazy)
    }
    else {
      printf("Null command");
    }
  }

  */

  for (k = 1; k < j; ++k) {
    // If the command is null then we have a parse error
    if (base[k]->str == NULL) {
      fprintf(stderr, "Parse error\n");
      // TODO: replace with continue once we write the loop
      return 0;
    }
  }

  // Now we do some forking.
  for (i = 0; i < j; ++i) {

    // Make a pipe for ...piping, if necessary.

    if (i > 0) {
      if (i > 1)
	close(pipefds[0][0]);
      close(pipefds[1][1]);
      pipefds[0][0] = pipefds[1][0];
      //pipefds[0][1] = pipefds[1][1];
    }
    if (i < j-1) {
      pipe(pipefds[1]);
    }

    k = fork();
    if (k) {
      if (k == -1) {
	fprintf(stderr, "Something went wrong\n");
	return -1;
      }
      k = i+1;
      break;
    }
  }

  if (k == 0) {
    // We are in the "parent" process
    
    // Close that last pipe
    if (j > 1) {
      close(pipefds[0][0]);
    }

    // Do something reasonable (like wait for all things to finish)
    for(; k<j; ++k) {
      wait(&i);
    }
  }
  else {
    k--;
    // We are in the "child" process corresponding to node base[k]

    // Set up I/O
    if (k > 0) {
      // Set our input properly
      dup2(pipefds[0][0], STDIN_FILENO);
    }
    else {
      // Check whether to take input from file or stdin
      if (base[0]->nins) {
	// Open the named file (I assume there's only one for now; multiple
	// input involves another fork)
	i = open(base[0]->ins[0], O_RDONLY);
	dup2(i, STDIN_FILENO);
      }
    }
    if (k < j-1) {
      // Set our output properly
      close(pipefds[1][0]);
      dup2(pipefds[1][1], STDOUT_FILENO);
    }
    else {
      // Check whether to output to file or stdout
      if (base[j-1]->nouts) {
	i = open(base[j-1]->outs[0], O_WRONLY);
	dup2(i, STDOUT_FILENO);
      }
    }
    // Execute our command with the given arguments
    if (base[k]->str)
      execvp(base[k]->args[0],base[k]->args);
    else {
      // We will do something stupid :D
      // If the first command is the only command and is empty, ignore it
      if (j == 1)
	return 0;
      // Otherwise run cat instead
      execlp("cat","cat",(char*) NULL);
    }
  }

  // Fork new processes (there would be an exception if we were writing a
  // real shell, which is exec...)

  /*
   
    // TODO: rewrite this while iterating through the command structs

  k = fork();
  if (k == -1)
    // Check for error conditions
    switch (errno) {
    case EAGAIN:
      fprintf(stderr, "Too many processes or out of memory\n");
      break;
    case ENOMEM:
      fprintf(stderr, "Not enough memory\n");
      break;
    case ENOSYS:
      fprintf(stderr, "Get a real computer\n");
      // ENOSYS indicates that the system doesn't support forks
      break;
    }
  else if(k) {
    // Parent process; wait for child to finish

    // If we support &, then we have to be more clever and use WNOHANG
    // every time we go through the loop (and not wait here if we had a &)
    wait(&j);

    // We should only have one child (we're not supporting ctrl+z either)
    // so there's no point using waitpid()
  }
  else {
    // Child process

    // Set I/O file descriptors to correct values (in this case, not really)

    execvp(args[0],args);
    switch(errno) {
      // If we got here then we had an error; no need to check return value
    case E2BIG:
      // should never happen, 1024 is much smaller than ARG_MAX on any
      // reasonable system
      fprintf(stderr, "Too many arguments\n");
      break;
    case EACCES:
      // No execute permission, or the file or script interpreter is not a file
      // or the path can't be resolved.
      fprintf(stderr, "Permission denied\n");
      break;
    case EFAULT:
      // This should really never happen, considering I allocated args[0]
      // myself!
      fprintf(stderr, "Your computer is probably dying\n");
      break;
    case EINVAL:
      fprintf(stderr, "ELF executable tried to name more than one interpreter\n");
      break;
    case EIO:
      fprintf(stderr, "I/O error\n");
      break;
    case EISDIR:
      fprintf(stderr, "ELF interpreter is a directory\n");
      break;
    case ELIBBAD:
      fprintf(stderr, "Bad ELF interpreter\n");
      break;
    case ELOOP:
      fprintf(stderr, "Too many symlinks (infinite loop?)\n");
      break;
    case EMFILE:
      fprintf(stderr, "Too many files opened by process\n");
      break;
    case ENAMETOOLONG: // That's an appropriately named macro
      fprintf(stderr, "Filename too long\n");
      break;
    case ENFILE:
      fprintf(stderr, "Too many files opened by user\n");
      break;
    case ENOENT:
      fprintf(stderr, "Command not found\n");
      break;
    case ENOEXEC:
      fprintf(stderr, "Exec format error\n");
      break;
    case ENOMEM:
      fprintf(stderr, "Out of kernel memory\n");
      break;
    case ENOTDIR:
      // Indicates that something in the path prefix is not a directory
      fprintf(stderr, "Not a directory\n");
      break;
    case EPERM:
      // Not being allowed to run a suid / sgid file
      // file system is mounted nosuid, or process is being traced
      fprintf(stderr, "Permission denied\n");
      break;
    case ETXTBSY:
      fprintf(stderr, "Executable is opened for writing\n");
      break;
    }
  }

  */

  // Free things (related to this command)
  for (k = 0; k < j; ++k) {
    if (base[k]->str)
      free(base[k]->str);
    for (i = 0; i < base[k]->nargs; ++i)
      free(base[k]->args[i]);
    if (base[k]->args)
      free(base[k]->args);
    for (i = 0; i < base[k]->nins; ++i)
      free(base[k]->ins[i]);
    if (base[k]->ins)
      free(base[k]->ins);
    for (i = 0; i < base[k]->nouts; ++i)
      free(base[k]->outs[i]);
    if (base[k]->outs)
      free(base[k]->outs);
    free(base[k]);
  }

  // Loop should end here, and we should deallocate stuff
  
  free(base);
  free(cmdbuf);
  free(dirbuf);
  free(hostname);
  return 0;
}
