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
#include "parser.h"

// I/O redirection commands currently supported
// < (Input from file)
// | (Pipe)
// > (Redirect stdout to file)

// Redirection only works for input to the first command and output to
// the last command.

// Things to maybe implement
// >>
// &> and &| (or >&)

// I've rolled my own parser which is kind of ugly. The data structure provides
// more information than is actually used during execution (so that we can
// extend it to add more features).

// Parses a single command, starting from idx and ending at either EOF or
// a pipe. Returns NULL if you did something stupid (trying to redirect to
// something which is not a filename, for example)
node_t *parsecmd(char *cmd, int startidx, int *endidx) {
  int i;
  stringbuilder_t *sb;

  node_t *ptr = calloc(1,sizeof(node_t));

  sb = nstringb();

  // Parse the first token; if it is < or >, then the command is empty
  // (this is legal! try typing something like "<Makefile|cat" in zsh.
  // bash won't do anything, but zsh will do cat<Makefile|cat.)
  
  i = parse_token(sb, cmd, startidx);
  if (sb->curlen == 0) {
    if (cmd[i] == '<') {
      // Command is empty
      ptr->str = malloc(1);
      *(ptr->str) = 0;

      // Get the next token and process it correctly
      ++i;
      i = parse_token(sb, cmd, i);
      // If this one is also empty, that's a parse error.
      // It indicates either that you have two consequent redirection commands
      // or you haven't given a file name for redirection (for example ls <)
      if (sb->curlen == 0) {
	fprintf(stderr, "Parse error\n");
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
      if (cmd[i+1] == '>') {
	++i;
	if (ptr->nouts == 0)
	  ptr->append = 1;
      }
      ptr->str = malloc(1);
      *(ptr->str) = 0;
      ++i;
      // Get the next token and process it correctly
      i = parse_token(sb, cmd, i);
      // If this one is also empty, that's a parse error.
      // It indicates either that you have two consequent redirection commands
      // or you haven't given a file name for redirection (for example ls >)
      if (sb->curlen == 0) {
	fprintf(stderr, "Parse error\n");
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
      fprintf(stderr, "Parse error\n");
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
	  fprintf(stderr, "Parse error\n");
	  free(ptr);
	  destroy_stringb(sb);
	  return NULL;
	}
	ptr->nins++;
	ptr->ins = realloc(ptr->ins, ptr->nins * sizeof(char *));
	ptr->ins[ptr->nins-1] = to_string(sb);
      }
      
      else if (cmd[i] == '>') {
	if (cmd[i+1] == '>') {
	  ++i;
	  if (ptr->nouts == 0)
	    ptr->append = 1;
	}
	i = parse_token(sb, cmd, i+1);
	if (sb->curlen == 0) {
	  fprintf(stderr, "Parse error\n");
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
