#include "render_xoj.h"
#include <unistd.h>
#include <zlib.h>

static void render_xoj_page(gzFile gf, remfmt_stroke_vec *strokes,
                            remfmt_render_params *prm) {
  char cbuf[65536];
  int bpos = 0;

  float page_w = (prm && prm->landscape) ? 1872.0f : 1404.0f;
  float page_h = (prm && prm->landscape) ? 1404.0f : 1872.0f;

  bpos += snprintf(cbuf + bpos, sizeof(cbuf) - bpos,
                   "<page width=\"%.2f\" height=\"%.2f\">\n", page_w, page_h);
  bpos += snprintf(
      cbuf + bpos, sizeof(cbuf) - bpos,
      "<background type=\"solid\" color=\"#ffffffff\" style=\"plain\" />\n");
  bpos += snprintf(cbuf + bpos, sizeof(cbuf) - bpos, "<layer>\n");
  gzwrite(gf, cbuf, bpos);
  bpos = 0;

  if (strokes != NULL) {
    for (int i = 0; i < kv_size(*strokes); i++) {
      remfmt_stroke st = kv_A(*strokes, i);
      set_pen_attr(&st);
      if (st.pen == 99) { /* PEN_IMAGE */
        continue;
      }
      if (kv_size(st.segments) == 0)
        continue;

      const char *color_str = "black";
      if (st.has_custom_color) {
        color_str = "black";
      } else {
        if (st.color == GRAY || st.color == GRAYHL)
          color_str = "gray";
        else if (st.color == WHITE)
          color_str = "white";
        else if (st.color == YELLOWHL)
          color_str = "yellow";
        else if (st.color == GREENHL)
          color_str = "green";
        else if (st.color == PINKHL)
          color_str = "magenta";
        else if (st.color == BLUE)
          color_str = "blue";
        else if (st.color == RED)
          color_str = "red";
      }

      unsigned pen_type = st.pen;
      if (st.version == 6) {
        pen_type = map_v6_pen(pen_type);
      }

      const char *tool_str = "pen";
      if (pen_type == HIGHLIGHTER || pen_type == HIGHLIGHTER_V2)
        tool_str = "highlighter";
      else if (pen_type == ERASER || pen_type == ERASE_AREA)
        tool_str = "eraser";

      float width = st.calc_width * 1.5f;
      if (width < 0.1f)
        width = 2.0f;

      bpos += snprintf(cbuf + bpos, sizeof(cbuf) - bpos,
                       "<stroke tool=\"%s\" color=\"%s\" width=\"%.2f\">\n",
                       tool_str, color_str, width);

      float xOffset = (st.version == 6) ? ((float)DEV_W / 2.0f) : 0.0f;

      for (int j = 0; j < kv_size(st.segments); j++) {
        remfmt_seg *sg = &kv_A(st.segments, j);
        float x = sg->x + xOffset;
        float y = sg->y;

        if (prm && prm->landscape) {
          float rx = (float)DEV_H - y;
          float ry = x;
          x = rx;
          y = ry;
        }

        bpos += snprintf(cbuf + bpos, sizeof(cbuf) - bpos, "%.2f %.2f ", x, y);
        if (bpos > 60000) {
          gzwrite(gf, cbuf, bpos);
          bpos = 0;
        }
      }
      bpos += snprintf(cbuf + bpos, sizeof(cbuf) - bpos, "\n</stroke>\n");
      if (bpos > 60000) {
        gzwrite(gf, cbuf, bpos);
        bpos = 0;
      }
    }
  }

  bpos += snprintf(cbuf + bpos, sizeof(cbuf) - bpos, "</layer>\n</page>\n");
  if (bpos > 0) {
    gzwrite(gf, cbuf, bpos);
  }
}

void remfmt_render_xoj(FILE *stream, remfmt_stroke_vec *strokes,
                       remfmt_render_params *prm) {
  char tmp[] = "/tmp/remfs_xoj_XXXXXX";
  int fd = mkstemp(tmp);
  if (fd == -1)
    return;
  gzFile gf = gzdopen(fd, "wb1");
  if (!gf) {
    close(fd);
    unlink(tmp);
    return;
  }

  gzprintf(gf, "<?xml version=\"1.0\" standalone=\"no\"?>\n");
  gzprintf(gf, "<xournal version=\"0.4.5\">\n");
  gzprintf(gf, "<title>Xournal document - see "
               "http://math.mit.edu/~auroux/software/xournal/</title>\n");

  render_xoj_page(gf, strokes, prm);

  gzwrite(gf, "</xournal>\n", 11);
  gzclose(gf);

  FILE *in = fopen(tmp, "rb");
  if (in) {
    char buf[8192];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
      fwrite(buf, 1, n, stream);
    }
    fclose(in);
  }
  unlink(tmp);
}

void remfmt_render_notebook_xoj(FILE *stream, int num_pages,
                                remfmt_stroke_vec **pages_strokes,
                                remfmt_render_params **pages_prms) {
  char tmp[] = "/tmp/remfs_xoj_XXXXXX";
  int fd = mkstemp(tmp);
  if (fd == -1)
    return;
  gzFile gf = gzdopen(fd, "wb1");
  if (!gf) {
    close(fd);
    unlink(tmp);
    return;
  }

  gzprintf(gf, "<?xml version=\"1.0\" standalone=\"no\"?>\n");
  gzprintf(gf, "<xournal version=\"0.4.5\">\n");
  gzprintf(gf, "<title>Xournal document - see "
               "http://math.mit.edu/~auroux/software/xournal/</title>\n");

  for (int p = 0; p < num_pages; p++) {
    render_xoj_page(gf, pages_strokes[p], pages_prms[p]);
  }

  gzwrite(gf, "</xournal>\n", 11);
  gzclose(gf);

  FILE *in = fopen(tmp, "rb");
  if (in) {
    char buf[8192];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
      fwrite(buf, 1, n, stream);
    }
    fclose(in);
  }
  unlink(tmp);
}
