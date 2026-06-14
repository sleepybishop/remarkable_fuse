#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "remfmt.h"

int main(int argc, char *argv[]) {
  if (argc < 3) {
    fprintf(stderr, "usage: %s <input.rm> (svg|png|pdf|rm)\n", argv[0]);
    exit(1);
  }

  FILE *out = stdout;

  remfmt_stroke_vec *strokes = remfmt_parse(argv[1]);
  if (!strokes) {
    fprintf(stderr, "error: failed to parse input file %s\n", argv[1]);
    return 1;
  }

  if (strcmp(argv[2], "svg") == 0) {
    remfmt_render_svg(out, strokes, NULL);
  } else if (strcmp(argv[2], "png") == 0) {
    remfmt_render_png(out, strokes, NULL);
  } else if (strcmp(argv[2], "pdf") == 0) {
    remfmt_render_pdf(out, strokes, NULL);
  } else if (strcmp(argv[2], "rm") == 0) {
    remfmt_render_rm(out, strokes);
  } else {
    fprintf(stderr, "error: unknown format %s (must be svg, png, pdf, or rm)\n",
            argv[2]);
    remfmt_stroke_cleanup(strokes);
    return 1;
  }

  remfmt_stroke_cleanup(strokes);
  return 0;
}
