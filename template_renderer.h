#ifndef TEMPLATE_RENDERER_H
#define TEMPLATE_RENDERER_H

#include "deps/sds/sds.h"
#include "remfmt.h"
#include <stdbool.h>

unsigned char *load_template_data(const char *template_dir,
                                  const char *template_name, int *w, int *h);

sds load_template_svg_background(const char *template_dir,
                                 const char *template_name);

#endif
