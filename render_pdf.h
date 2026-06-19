#ifndef RENDER_PDF_H
#define RENDER_PDF_H

#include "remfmt.h"
#include <stdio.h>

void remfmt_render_pdf(FILE *stream, remfmt_stroke_vec *strokes,
                       remfmt_render_params *prm);
void remfmt_render_notebook_pdf(FILE *stream, int num_pages,
                                remfmt_stroke_vec **pages_strokes,
                                remfmt_render_params **pages_prms);

#endif
