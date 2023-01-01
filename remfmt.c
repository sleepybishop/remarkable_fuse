#include "remfmt.h"
#include "struct.h"

static const char rmv5_magic[] = "reMarkable .lines file, version=%d          ";

static uint32_t svg_color[] = {0x000000, 0x7d7d7d, 0xffffff, 0xebcb8b,
                               0xfe93bf, 0xa2f567, 0x000088, 0x880000,
                               0x0d0d0d, 0x0000aa};

static const char *svg_tpl[] = {
    "<svg xmlns=\"http://www.w3.org/2000/svg\" height=\"%d\" width=\"%d\">\n"
    "  <defs>\n"
    "    <pattern id=\"brush\" x=\"0\" y=\"0\" "
    "patternUnits=\"userSpaceOnUse\">\n"
    "      <image x=\"0\" y=\"0\" href=\"%s\"></image>\n"
    "    </pattern>\n"
    "  </defs>\n"
    "  <g transform=\"rotate(%d %d %d)\">\n"
    "    <!--<image x=\"0\" y=\"0\" href=\"%s\"></image>-->\n",
    "    <polyline style=\"fill:none; stroke:#%06x; "
    "stroke-width:%.3f;opacity:%.3f\" stroke-linejoin=\"round\" "
    "stroke-linecap=\"%s\" points=\"%s\"/>\n",
    "  </g>\n"
    "</svg>\n"};

static const char _tpl_path[] = "./remarkable/templates/%s.svg";

enum { SVG_HEADER = 0, SVG_POLYLINE = 1, SVG_FOOTER = 2 };

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
} pen;

static void set_pen_attr(remfmt_stroke *st) {
  st->calc_width = st->width;
  st->opacity = 1.0;
  st->square_cap = false;
  switch (st->pen) {
  case SHARP_PENCIL:
  case SHARP_PENCIL_V2:
    st->opacity = 0.90;
    break;
  case MARKER:
  case MARKER_V2:
    break;
  case FINELINER:
  case FINELINER_V2:
    st->calc_width = 0.4 * pow(st->calc_width, 4);
    break;
  case HIGHLIGHTER:
    st->color = YELLOW;
  case HIGHLIGHTER_V2:
    st->opacity = 0.25;
    st->square_cap = true;
    break;
  case ERASER:
    st->color = WHITE;
    st->square_cap = true;
    st->opacity = 0.0;
    break;
  case ERASE_AREA:
    st->color = WHITE;
    st->square_cap = true;
    st->opacity = 0.0;
    break;
  default:
    break;
  }
}

static float clampf(float f, float lo, float hi) {
  return (f < lo) ? lo : ((f < hi) ? f : hi);
}

static float get_seg_width(remfmt_stroke *st, remfmt_seg *sg) {
  float width;
  switch (st->pen) {
  default:
    width = sg->width;
  }
  return clampf(width, 0.1, 4.0 * sg->width);
}

static float get_seg_alpha(remfmt_stroke *st, remfmt_seg *sg) {
  float alpha;
  switch (st->pen) {
  case TILT_PENCIL:
  case PENCIL_V2:
    alpha = 0.45 * sg->pressure - (sg->speed / 26.0);
    break;
  default:
    alpha = st->opacity;
  }
  return clampf(alpha, 0.0, 1.0);
}

void remfmt_render_rm5(FILE *stream, remfmt_stroke_vec *strokes) {
  char buf[64] = {0};
  snprintf(buf, 44, rmv5_magic, 5);
  fwrite(buf, 1, 43, stream);

  int num_layers = kv_A(*strokes, strokes->n - 1).layer + 1;
  struct_pack(buf, "<I", num_layers);
  fwrite(buf, 4, 1, stream);
  for (int l = 0; l < num_layers; l++) {
    int num_strokes = kv_size(*strokes);
    struct_pack(buf, "<I", num_strokes);
    fwrite(buf, 4, 1, stream);

    for (int i = 0; i < num_strokes; i++) {
      remfmt_stroke *st = &kv_A(*strokes, i);
      int num_segments = kv_size(st->segments);
      struct_pack(buf, "<IIfffI", st->pen, st->color, st->unk1, st->width,
                  st->unk2, num_segments);
      fwrite(buf, 4, 6, stream);
      for (int j = 0; j < num_segments; j++) {
        remfmt_seg *sg = &kv_A(st->segments, j);
        struct_pack(buf, "<ffffff", sg->x, sg->y, sg->speed, sg->tilt,
                    sg->width, sg->pressure);
        fwrite(buf, 4, 6, stream);
      }
    }
  }
}

void remfmt_render_png(FILE *stream, remfmt_stroke_vec *strokes,
                       remfmt_render_params *prm) {}

void remfmt_render_svg(FILE *stream, remfmt_stroke_vec *strokes,
                       remfmt_render_params *prm) {
  kstr tpl_path = {0, 0, 0};
  const char brush_pat[] = "none";
  if (prm && prm->template_name && prm->template_name[0] != '\0') {
    kstr_cat(&tpl_path, _tpl_path, prm->template_name);
  } else {
    kstr_cat(&tpl_path, _tpl_path, "Blank");
  }
  if (prm && prm->landscape) {
    fprintf(stream, svg_tpl[SVG_HEADER], DEV_W, DEV_H, brush_pat, 90, 936, 936,
            tpl_path.a);
  } else {
    fprintf(stream, svg_tpl[SVG_HEADER], DEV_H, DEV_W, brush_pat, 0, 0, 0,
            tpl_path.a);
  }
  kv_destroy(tpl_path);
  for (int i = 0; i < kv_size(*strokes); i++) {
    remfmt_stroke st = kv_A(*strokes, i);
    set_pen_attr(&st);

    uint32_t seg_color = svg_color[st.color];
    float seg_width = st.calc_width, lsw = seg_width;
    float seg_alpha = st.opacity;
    const char fmt[] = "%.3f %.3f ";

    if (st.pen == HIGHLIGHTER_V2)
      seg_color = svg_color[st.color];

    if (prm && prm->annotation)
      seg_color = svg_color[prm->note_color];

    kstr pv = {0, 0, 0};
    for (int j = 0; j < kv_size(st.segments); j++) {
      remfmt_seg sg = kv_A(st.segments, j);
      float x = sg.x, y = sg.y;

      seg_width = get_seg_width(&st, &sg);
      seg_alpha = get_seg_alpha(&st, &sg);

      kstr_cat(&pv, fmt, x, y);
      if (lsw != seg_width) {
        fprintf(stream, svg_tpl[SVG_POLYLINE], seg_color, seg_width, seg_alpha,
                st.square_cap ? "square" : "round", pv.a);
        kv_size(pv) = 0;
        kstr_cat(&pv, fmt, x, y);
        lsw = seg_width;
      }
    }
    if (kv_size(pv) > 0) {
      fprintf(stream, svg_tpl[SVG_POLYLINE], seg_color, seg_width, seg_alpha,
              st.square_cap ? "square" : "round", pv.a);
    }
    kv_destroy(pv);
  }
  fprintf(stream, svg_tpl[SVG_FOOTER]);
}

void remfmt_stroke_cleanup(remfmt_stroke_vec *strokes) {
  if (strokes == NULL)
    return;
  for (int i = 0; i < kv_size(*strokes); i++) {
    remfmt_stroke st = kv_A(*strokes, i);
    if (kv_size(st.segments) > 0)
      kv_destroy(st.segments);
  }
  kv_destroy(*strokes);
  free(strokes);
}

remfmt_stroke_vec *remfmt_parse(FILE *stream) {
  char buf[64] = {0};
  int got, version = 0, num_layers = 0;
  if (fscanf(stream, rmv5_magic, &version) == 0)
    return NULL;
  if (version != 3 && version != 5)
    return NULL;

  fseek(stream, 43, SEEK_SET);
  got = fread(buf, 4, 1, stream);
  if (got == 1)
    struct_unpack(buf, "<I", &num_layers);

  if (num_layers < 1)
    return NULL;

  remfmt_stroke_vec *strokes = calloc(1, sizeof(remfmt_stroke_vec));
  if (strokes == NULL)
    return NULL;
  for (int l = 0; l < num_layers; l++) {
    int num_strokes = 0;
    got = fread(buf, 4, 1, stream);
    if (got == 1)
      struct_unpack(buf, "<I", &num_strokes);

    for (int i = 0; i < num_strokes; i++) {
      int num_segments = 0;
      remfmt_stroke st = {0};
      switch (version) {
      case 3:
        got = fread(buf, 4, 5, stream);
        struct_unpack(buf, "<IIffI", &st.pen, &st.color, &st.unk1, &st.width,
                      &num_segments);
        break;
      case 5:
        got = fread(buf, 4, 6, stream);
        struct_unpack(buf, "<IIfffI", &st.pen, &st.color, &st.unk1, &st.width,
                      &st.unk2, &num_segments);
        break;
      }
      if (got != ((version == 3) ? 5 : 6))
        break;
      st.layer = l;
      for (int j = 0; j < num_segments; j++) {
        remfmt_seg sg = {0};
        got = fread(buf, 4, 6, stream);
        if (got != 6)
          break;
        struct_unpack(buf, "<ffffff", &sg.x, &sg.y, &sg.speed, &sg.tilt,
                      &sg.width, &sg.pressure);
        kv_push(remfmt_seg, st.segments, sg);
      }
      kv_push(remfmt_stroke, *strokes, st);
    }
  }
  return strokes;
}
