#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "kstr.h"

void kstr_cat(kstr *v, const char *fmt, ...) {
  va_list args;
  va_start(args, fmt);
  size_t want = v->m, need = vsnprintf(NULL, 0, fmt, args);
  va_end(args);
  while (want < (v->n + need + 1)) {
    want += 2 * ((v->m > 0) ? v->m : 2);
  }
  kv_resize(char, *v, want);
  va_start(args, fmt);
  v->n += vsnprintf(v->a + v->n, v->m - v->n, fmt, args);
  va_end(args);
}
