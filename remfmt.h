#ifndef REMFMT_H
#define REMFMT_H

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "deps/sds/sds.h"
#include <kvec.h>

#define DEV_W 1404
#define DEV_H 1872

typedef enum {
  BLACK = 0,
  GRAY = 1,
  WHITE = 2,
  YELLOWHL,
  GREENHL,
  PINKHL,
  BLUE,
  RED,
  GRAYHL,
} remfmt_stroke_color;

typedef enum {
  BRUSH = 0,
  TILT_PENCIL = 1,
  BALLPOINT = 2,
  MARKER = 3,
  FINELINER = 4,
  HIGHLIGHTER = 5,
  ERASER = 6,
  SHARP_PENCIL = 7,
  ERASE_AREA = 8,

  BRUSH_V2 = 12,
  SHARP_PENCIL_V2 = 13,
  PENCIL_V2 = 14,
  BALLPOINT_V2 = 15,
  MARKER_V2 = 16,
  FINELINER_V2 = 17,
  HIGHLIGHTER_V2 = 18,
  CALLIGRAPHY = 21,
  SHADER = 23,
} remfmt_pen;

typedef struct {
  bool landscape;
  bool annotation;
  char *template_name;
  char *template_dir;
  float canvas_width;
  float canvas_height;
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

  bool has_custom_color;
  uint32_t custom_color;
} remfmt_stroke;

typedef kvec_t(remfmt_stroke) remfmt_stroke_vec;

// Common helpers and shared variables
extern uint32_t svg_color[];
extern const char rmv_magic[];

void set_pen_attr(remfmt_stroke *st);
float clampf(float f, float lo, float hi);
float get_seg_width(remfmt_stroke *st, remfmt_seg *sg);
float get_seg_alpha(remfmt_stroke *st, remfmt_seg *sg);
unsigned map_v6_pen(unsigned pen_id);

void remfmt_render_rm(FILE *stream, remfmt_stroke_vec *strokes);

// Sub-modules
#include "render_pdf.h"
#include "render_png.h"
#include "render_svg.h"
#include "render_xoj.h"
#include "rm_parser.h"
#include "template_renderer.h"

#endif
