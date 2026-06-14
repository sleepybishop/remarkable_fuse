#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "remfmt.h"

int main(int argc, char *argv[]) {
  remfmt_render_params prm = {0};
  int arg_idx = 1;
  while (arg_idx < argc && argv[arg_idx][0] == '-') {
    if (strcmp(argv[arg_idx], "--template-dir") == 0 && arg_idx + 1 < argc) {
      prm.template_dir = argv[++arg_idx];
    } else if (strcmp(argv[arg_idx], "--template-name") == 0 &&
               arg_idx + 1 < argc) {
      prm.template_name = argv[++arg_idx];
    }
    arg_idx++;
  }

  if (arg_idx + 1 >= argc) {
    fprintf(stderr,
            "usage: %s [--template-dir <dir> --template-name <name>] "
            "<input.rm> (svg|png|pdf|rm)\n",
            argv[0]);
    exit(1);
  }

  FILE *out = stdout;

  remfmt_stroke_vec *strokes = remfmt_parse(argv[arg_idx]);
  if (!strokes) {
    fprintf(stderr, "error: failed to parse input file %s\n", argv[arg_idx]);
    return 1;
  }

  if (strcmp(argv[arg_idx + 1], "svg") == 0) {
    remfmt_render_svg(out, strokes, &prm);
  } else if (strcmp(argv[arg_idx + 1], "png") == 0) {
    remfmt_render_png(out, strokes, &prm);
  } else if (strcmp(argv[arg_idx + 1], "pdf") == 0) {
    remfmt_render_pdf(out, strokes, &prm);
  } else if (strcmp(argv[arg_idx + 1], "rm") == 0) {
    remfmt_render_rm(out, strokes);
  } else {
    fprintf(stderr, "error: unknown format %s (must be svg, png, pdf, or rm)\n",
            argv[arg_idx + 1]);
    remfmt_stroke_cleanup(strokes);
    return 1;
  }

  remfmt_stroke_cleanup(strokes);
  return 0;
}
