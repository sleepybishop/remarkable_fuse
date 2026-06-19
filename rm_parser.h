#ifndef RM_PARSER_H
#define RM_PARSER_H

#include "remfmt.h"
#include <stdio.h>

void remfmt_stroke_cleanup(remfmt_stroke_vec *strokes);
remfmt_stroke_vec *remfmt_parse(const char *path);

#endif
