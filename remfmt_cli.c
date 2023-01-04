#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "remfmt.h"

int main(int argc, char *argv[]) {
  if (argc < 3) {
    fprintf(stderr, "usage: %s <input.rm> (svg|rm)\n", argv[0]);
    exit(1);
  }

  FILE *in = fopen(argv[1], "rb");
  FILE *out = stdout;

  if (!in)
    return -1;

  remfmt_stroke_vec *strokes = remfmt_parse(in);
  if (strokes) {
    if (strcmp(argv[2], "svg") == 0) {
      remfmt_render_svg(out, strokes, NULL);
    } else if (strcmp(argv[2], "png") == 0) {
      remfmt_render_png(out, strokes, NULL);
    } else {
      remfmt_render_rm(out, strokes);
    }
  }

  remfmt_stroke_cleanup(strokes);
  fclose(in);
  return 0;
}
