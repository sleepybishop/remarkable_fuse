#ifndef REMFMT_H
#define REMFMT_H

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <kstr.h>

#define DEV_W 1404
#define DEV_H 1872

typedef enum {
  BLACK = 0,
  GRAY = 1,
  WHITE = 2,
  YELLOWHL,
  PINKHL,
  GREENHL,
  BLUE,
  RED,
  GRAYHL,
} remfmt_stroke_color;

typedef struct {
  bool landscape;
  bool annotation;
  char *template_name;
} remfmt_render_params;

typedef struct {
  float x;
  float y;
  float speed;
  float tilt;
  float width;
  float pressure;
} remfmt_seg;

typedef struct {
  float height;
  float width;
  float x;
  float y;
} remfmt_hilite;

typedef kvec_t(remfmt_seg) remfmt_seg_vec;
typedef kvec_t(remfmt_hilite) remfmt_hilite_vec;

typedef struct {
  unsigned layer;
  unsigned pen;
  unsigned color;
  float unk1;
  float unk2;
  float width;

  float calc_width;
  float opacity;
  bool square_cap;

  remfmt_seg_vec segments;
  int version;
} remfmt_stroke;

typedef kvec_t(remfmt_stroke) remfmt_stroke_vec;

void remfmt_render_rm(FILE *stream, remfmt_stroke_vec *strokes);
void remfmt_render_png(FILE *stream, remfmt_stroke_vec *strokes,
                       remfmt_render_params *prm);
void remfmt_render_svg(FILE *stream, remfmt_stroke_vec *strokes,
                       remfmt_render_params *prm);
void remfmt_stroke_cleanup(remfmt_stroke_vec *strokes);
remfmt_stroke_vec *remfmt_parse(FILE *stream);

#endif
