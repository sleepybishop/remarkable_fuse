#ifndef RENDER_PNG_H
#define RENDER_PNG_H

#include "remfmt.h"
#include <stdio.h>

void remfmt_render_png(FILE *stream, remfmt_stroke_vec *strokes,
                       remfmt_render_params *prm);

#endif
