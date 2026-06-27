#include "render_svg.h"
#include "template_renderer.h"
#include <math.h>

static const char *svg_tpl[] = {
    "<svg xmlns=\"http://www.w3.org/2000/svg\" height=\"%d\" width=\"%d\">\n"
    "  <g transform=\"rotate(%d %d %d)\">\n",
    "    <polyline style=\"fill:none; stroke:#%06x; "
    "stroke-width:%.3f;opacity:%.3f\" stroke-linejoin=\"round\" "
    "stroke-linecap=\"%s\" points=\"%s\"/>\n",
    "  </g>\n"
    "</svg>\n"};

enum { SVG_HEADER = 0, SVG_POLYLINE = 1, SVG_FOOTER = 2 };

void remfmt_render_svg(FILE *stream, remfmt_stroke_vec *strokes,
                       remfmt_render_params *prm) {

  float min_x = 0.0f;
  float max_x = (float)DEV_W;
  float min_y = 0.0f;
  float max_y = (float)DEV_H;

  if (strokes != NULL && !(prm && prm->annotation)) {
    for (int i = 0; i < kv_size(*strokes); i++) {
      remfmt_stroke *st = &kv_A(*strokes, i);
      float xOffset = (st->version == 6) ? ((float)DEV_W / 2.0f) : 0.0f;
      for (int j = 0; j < kv_size(st->segments); j++) {
        remfmt_seg sg = kv_A(st->segments, j);
        float x = sg.x + xOffset;
        float y = sg.y;
        if (x < min_x)
          min_x = x;
        if (x > max_x)
          max_x = x;
        if (y < min_y)
          min_y = y;
        if (y > max_y)
          max_y = y;
      }
    }
  }

  int port_w = (int)ceilf(max_x - min_x);
  int port_h = (int)ceilf(max_y - min_y);

  sds b64_img = sdsempty();
  if (prm && prm->template_dir && prm->template_name &&
      prm->template_name[0] != '\0') {
    b64_img =
        load_template_svg_background(prm->template_dir, prm->template_name);
  }

  if (prm && prm->landscape) {
    fprintf(stream, svg_tpl[SVG_HEADER], port_w, port_h, 0, 0, 0);
    if (sdslen(b64_img) > 0)
      fprintf(stream, "    %s", b64_img);
  } else {
    fprintf(stream, svg_tpl[SVG_HEADER], port_h, port_w, 0, 0, 0);
    if (sdslen(b64_img) > 0)
      fprintf(stream, "    %s", b64_img);
  }
  sdsfree(b64_img);

  if (strokes != NULL) {
    for (int i = 0; i < kv_size(*strokes); i++) {
      remfmt_stroke st = kv_A(*strokes, i);
      set_pen_attr(&st);

      uint32_t seg_color;
      if (st.has_custom_color) {
        seg_color = st.custom_color;
      } else {
        seg_color = (st.color < 14) // size of svg_color array
                        ? svg_color[st.color]
                        : 0x000000;
      }
      float seg_width = st.calc_width, lsw = seg_width;
      float seg_alpha = st.opacity;
      const char fmt[] = "%.3f %.3f ";

      if (prm && prm->annotation && (st.pen == HIGHLIGHTER_V2)) {
        if (st.has_custom_color) {
          seg_color = st.custom_color;
        } else {
          seg_color = (st.color < 14) ? svg_color[st.color] : 0x000000;
        }
      }

      float xOffset = (st.version == 6) ? ((float)DEV_W / 2.0f) : 0.0f;

      if (kv_size(st.segments) > 0) {
        remfmt_seg first_sg = kv_A(st.segments, 0);
        lsw = get_seg_width(&st, &first_sg);
        seg_width = lsw;
      }

      sds pv = sdsempty();
      for (int j = 0; j < kv_size(st.segments); j++) {
        remfmt_seg sg = kv_A(st.segments, j);
        float x = sg.x + xOffset - min_x;
        float y = sg.y - min_y;

        if (prm && prm->landscape) {
          float rx = (float)port_h - y;
          float ry = x;
          x = rx;
          y = ry;
        }

        seg_width = get_seg_width(&st, &sg);
        seg_alpha = get_seg_alpha(&st, &sg);

        pv = sdscatprintf(pv, fmt, x, y);
        if (fabsf(lsw - seg_width) > 0.08f * lsw) {
          fprintf(stream, svg_tpl[SVG_POLYLINE], seg_color, lsw, seg_alpha,
                  st.square_cap ? "square" : "round", pv);
          sdsclear(pv);
          pv = sdscatprintf(pv, fmt, x, y);
          lsw = seg_width;
        }
      }
      if (sdslen(pv) > 0) {
        fprintf(stream, svg_tpl[SVG_POLYLINE], seg_color, seg_width, seg_alpha,
                st.square_cap ? "square" : "round", pv);
      }
      sdsfree(pv);
    }
  }
  fprintf(stream, "%s", svg_tpl[SVG_FOOTER]);
}
