#ifndef parser_h_
#define parser_h_

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

node_t *parsecmd(char *cmd, int startidx, int *endidx);

#endif /* parser_h */
