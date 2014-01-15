#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <limits.h>
#include <ctype.h>
#include <errno.h>
#include <string.h>
#include <sys/wait.h>

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

// A directed node struct. Has any number of parents and children, but
// the way we build it ensures that it will be a tree.

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
} node_t;

// Helper functions for I/O redirection

void writetopipe(FILE *f, int fd) {
  // Writes the contents of f to fd
  char c;
  while (c = getc(f)) {
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
  int i, fdn;
  stringbuilder_t *sb;
  char *tok;

  node_t *ptr = malloc(sizeof(node_t));

  // for (i = 0; i < 10; ++i)
  //  for (j = 0; j < 2; ++j)
  //    ptr->fdtable[i][j] = -1;

  ptr->str = NULL;
  ptr->nins = 0;
  ptr->nouts = 0;
  ptr->nargs = 0;

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
    else if (*(sb->buf) == '|') {
      // This, on the other hand, is a parse error!
      free(ptr);
      destroy_stringb(sb);
      return NULL;
    }
    else {
      // Empty command with no redirection? I assume the user just typed a
      // newline at the terminal... Should be handled properly at the running
      // side, by checking whether ptr->str is null.
      return ptr;
    }
  }
  else {
    ptr->str = to_string(sb);
  }

  while (cmd[i] && (cmd[i] != '|')) {
    // Continue parsing arguments
    i = parse_token(sb, cmd, startidx);
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
	 
  *endidx = i;
  return ptr;
}


int main() {
  // Print a prompt
  char *dirbuf;
  char *hostname;
  char *cmdbuf;
  char *buf;

  char **args;
  node_t *base;
  int nargs;

  stringbuilder_t *st;
  
  int i, j, k;

  dirbuf = malloc(PATH_MAX + 1);
  hostname = malloc(HOST_NAME_MAX + 1);
  cmdbuf = malloc(1025); // Apparently this should be a kernel parameter;
  // ARG_MAX is above 2 million on my system!
  
  buf = malloc(1025);
  
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

  // TODO: Finish writing the tokenization code (see parsecmd() above)

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

  // Free the things
  for (j = 0; j < nargs; ++j)
    free(args[j]);
  free(args);

  // Loop should end here, and we should deallocate stuff
  
  free(cmdbuf);
  free(dirbuf);
  free(hostname);
  return 0;
}
