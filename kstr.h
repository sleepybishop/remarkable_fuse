#ifndef KSTR_H
#define KSTR_H

#include "kvec.h"
typedef kvec_t(char) kstr;
void kstr_cat(kstr *v, const char *fmt, ...);

#endif
