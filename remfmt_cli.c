#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "remfmt.h"

int main(int argc, char *argv[]) {
  if (argc < 2)
    fprintf(stderr, "usage: %s <input.rm>\n", argv[0]);

  FILE *in = fopen(argv[1], "rb");
  FILE *out = stdout;

  if (!in)
    return -1;

  remfmt_stroke_vec *strokes = remfmt_parse(in);
  if (strokes)
    remfmt_render(out, strokes, NULL);

  remfmt_stroke_cleanup(strokes);
  fclose(in);
  return 0;
}
