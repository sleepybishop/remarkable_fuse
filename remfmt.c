#include "remfmt.h"
#include "struct.h"

const char rmv_magic[] = "reMarkable .lines file, version=%d          ";

uint32_t svg_color[] = {0x000000, 0x7d7d7d, 0xffffff, 0xebcb8b, 0xa2f567,
                        0xfe93bf, 0x000088, 0x880000, 0x0d0d0d, 0xffed75,
                        0xa1d87d, 0x8bd0e5, 0xb782cd, 0xf7e851};

void set_pen_attr(remfmt_stroke *st) {
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
  case HIGHLIGHTER_V2:
    st->opacity = 0.25;
    st->square_cap = true;
    break;
  case ERASER:
    st->color = WHITE;
    st->square_cap = true;
    st->opacity = 1.0;
    break;
  case ERASE_AREA:
    st->color = WHITE;
    st->square_cap = true;
    st->opacity = 1.0;
    break;
  case SHADER:
    st->opacity = 0.10;
    break;
  default:
    break;
  }
}

float clampf(float f, float lo, float hi) {
  return (f < lo) ? lo : ((f < hi) ? f : hi);
}

float get_seg_width(remfmt_stroke *st, remfmt_seg *sg) {
  if (sg->width < 0.01f) {
    return st->calc_width;
  }
  float width;
  switch (st->pen) {
  default:
    width = sg->width;
  }
  return clampf(width, 0.1, 4.0 * sg->width);
}

float get_seg_alpha(remfmt_stroke *st, remfmt_seg *sg) {
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

unsigned map_v6_pen(unsigned pen_id) {
  switch (pen_id) {
  case 0:
    return 0; // PAINTBRUSH_1 -> Ballpoint v1
  case 1:
    return 1; // PENCIL_1 -> Marker v1
  case 2:
    return 2; // BALLPOINT_1 -> Fineliner v1
  case 3:
    return 3; // MARKER_1 -> Sharp pencil v1
  case 4:
    return 4; // FINELINER_1 -> Tilt pencil v1
  case 5:
    return 5; // HIGHLIGHTER_1 -> Highlighter v1
  case 6:
    return 6; // ERASER
  case 7:
    return 13; // MECHANICAL_PENCIL_1
  case 8:
    return 8; // ERASER_AREA -> Erase all
  case 12:
    return 12; // PAINTBRUSH_2 -> Ballpoint v2
  case 13:
    return 13; // MECHANICAL_PENCIL_2
  case 14:
    return 16; // PENCIL_2 -> Pencil v2
  case 15:
    return 15; // BALLPOINT_2 -> Fineliner v2
  case 16:
    return 14; // MARKER_2 -> Marker v2
  case 17:
    return 17; // FINELINER_2 -> Sharp pencil v2
  case 18:
    return 18; // HIGHLIGHTER_2
  case 21:
    return 21; // CALIGRAPHY
  case 23:
    return 23; // SHADER
  default:
    return pen_id;
  }
}

void remfmt_render_rm(FILE *stream, remfmt_stroke_vec *strokes) {
  char buf[64] = {0};
  snprintf(buf, 44, rmv_magic, 5);
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
