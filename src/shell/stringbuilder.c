#include <string.h>
#include <stdlib.h>
#include <ctype.h>

typedef struct stringbuilder {
  char *buf;
  int curlen;
  int size;
} stringbuilder_t;

// Returns a null-terminated string
char *to_string(stringbuilder_t *ptr) {
  char *t = malloc(ptr->curlen + 1);
  memcpy(t, ptr->buf, ptr->curlen);
  t[ptr->curlen] = 0;
  return t;
}

void erase_stringb(stringbuilder_t *ptr) {
  free(ptr->buf);
  ptr->buf = malloc(10);
  ptr->curlen = 0;
  ptr->size = 10;
}

void destroy_stringb(stringbuilder_t *ptr) {
  if (ptr) {
    if (ptr->buf)
      free(ptr->buf);
    free(ptr);
  }
  return;
}

stringbuilder_t *nstringb() {
  // Allocates a new string builder
  stringbuilder_t *ptr;
  ptr = malloc(sizeof(stringbuilder_t));
  ptr->buf = malloc(10);
  ptr->curlen = 0;
  ptr->size = 10;
  return ptr;
}

void addchar(stringbuilder_t *ptr, char c) {
  if (ptr->curlen == ptr->size) {
    // Allocate more memory
    ptr->size += 10;
    ptr->buf = realloc(ptr->buf, ptr->size);
  }
  ptr->buf[ptr->curlen++] = c;
  return;
}

void delchar(stringbuilder_t *ptr) {
  if (ptr->curlen)
    ptr->curlen--;
}

int parse_token(stringbuilder_t *ptr, char *buf, int idx) {
  // Parses one token from the buffer into the string builder (which is emptied)
  erase_stringb(ptr);

  // Return value is the ending index (so that we can continue the parsing)
  int i;
  // Remove leading whitespace
  for(i = idx; buf[i] && isspace(buf[i]); ++i);

  for (; buf[i] && (buf[i] != '<' && buf[i] != '|' &&
		    buf[i] != '>' && !isspace(buf[i])); ++i) {
    switch(buf[i]) {
    case '"':
      // Read characters until the next quote; there is, however, the rather
      // special case of backslashes, which *still* escape the next character.

      // I'm going to assume that quotes are balanced for now, although it
      // will try not to die if quotes aren't balanced. Handling that is
      // actually on the input side, not the parsing part though (we just
      // shouldn't accept a command with unbalanced quotes)

      // The spec says not to expect newlines, but meh. It's not that hard
      // to insert support for them (just read another line if quotes are
      // unbalanced)
      for (i++; buf[i] && buf[i] != '"'; ++i) {
	if (buf[i] == '\\') {
	  // Read the next character, unless it is a newline.
	  ++i;
	  if (buf[i] != '\n')
	    addchar(ptr, buf[i]);
	}
	else {
	  // Just read the character
	  addchar(ptr, buf[i]);
	}
      }
      break;
    case '\\':
      // Read the next character as a literal, unless it is newline
      ++i;
      if (buf[i] != '\n')
	addchar(ptr, buf[i]);
      break;

      // Other things to support: `, $, ;, (, ), *, &, '
    default:
      // Read the character
      addchar(ptr, buf[i]);
      break;
    }

    // Now, it is possible for the string builder to be empty here
    // (e.g. we've hit a > or <) but we can't handle that here, because if we
    // did, we can't distinguish between ls > > and ls > \> (the latter is
    // a legal command!)
  }
  return i;
}
