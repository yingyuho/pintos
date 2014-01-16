#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <limits.h>
#include <ctype.h>
#include <errno.h>
#include <string.h>
#include <sys/wait.h>
#include <fcntl.h>

#include "parser.h"
#include "stringbuilder.h"

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

  // TODO: handle unbalanced quotes here one way or another
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
    if (j%10 == 0) {
      base = realloc(base, (10 + j) * sizeof(node_t*));
    }
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
    }
    if (i < j-1) {
      pipe(pipefds[1]);
    }

    k = fork();
    if (!k) {
      k = i+1;
      break;
    }
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
    k = 0;
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
	i = open(base[j-1]->outs[0], O_WRONLY | O_CREAT,
		 S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
	// >> is implemented by also ORing O_APPEND of course
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

  // Loop should end here (once we add a loop)
  
  free(base);
  free(cmdbuf);
  free(dirbuf);
  free(hostname);
  return 0;
}
