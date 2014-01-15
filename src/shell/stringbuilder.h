#ifndef strb_h_
#define strb_h_

#include <string.h>

// Minimal string builder library for parsing

typedef struct stringbuilder {
  char *buf;
  int curlen;
  int size;
} stringbuilder_t;

// Returns a null-terminated string
char *to_string(stringbuilder_t *ptr);

void erase_stringb(stringbuilder_t *ptr);

void destroy_stringb(stringbuilder_t *ptr);

stringbuilder_t *nstringb();

void addchar(stringbuilder_t *ptr, char c);
  
void delchar(stringbuilder_t *ptr);

int parse_token(stringbuilder_t *ptr, char *buf, int idx);

#endif /* strb_h_ */
