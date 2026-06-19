#ifndef RENDER_SVG_H
#define RENDER_SVG_H

#include "remfmt.h"
#include <stdio.h>

void remfmt_render_svg(FILE *stream, remfmt_stroke_vec *strokes,
                       remfmt_render_params *prm);

#endif
