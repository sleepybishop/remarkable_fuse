#include "render_png.h"
#include "template_renderer.h"
#include <math.h>

typedef struct {
  float alpha;
  float width_scale;
  bool skip;
} png_brush_meta;

static png_brush_meta get_png_brush(unsigned pen_type) {
  png_brush_meta bm = {1.0f, 1.0f, false};
  switch (pen_type) {
  case 0: // Ballpoint v1
    bm.alpha = 1.0f;
    bm.width_scale = 1.0f;
    break;
  case 1: // Marker v1
    bm.alpha = 0.7f;
    bm.width_scale = 1.4f;
    break;
  case 2: // Fineliner v1
    bm.alpha = 1.0f;
    bm.width_scale = 0.6f;
    break;
  case 3: // Sharp pencil v1
    bm.alpha = 1.0f;
    bm.width_scale = 0.6f;
    break;
  case 4: // Tilt pencil v1
    bm.alpha = 0.8f;
    bm.width_scale = 1.0f;
    break;
  case 5: // Highlighter v1
    bm.alpha = 0.3f;
    bm.width_scale = 2.0f;
    break;
  case 6: // Eraser
    bm.alpha = 1.0f;
    bm.width_scale = 1.0f;
    bm.skip = false;
    break;
  case 7: // Eraser area
    bm.alpha = 1.0f;
    bm.width_scale = 1.0f;
    bm.skip = false;
    break;
  case 8: // Erase all
    bm.alpha = 1.0f;
    bm.width_scale = 1.0f;
    bm.skip = false;
    break;
  case 12: // Ballpoint v2
    bm.alpha = 1.0f;
    bm.width_scale = 1.0f;
    break;
  case 13: // Mechanical pencil
    bm.alpha = 1.0f;
    bm.width_scale = 0.5f;
    break;
  case 14: // Marker v2
    bm.alpha = 0.7f;
    bm.width_scale = 1.4f;
    break;
  case 15: // Fineliner v2
    bm.alpha = 1.0f;
    bm.width_scale = 0.6f;
    break;
  case 16: // Pencil v2
    bm.alpha = 0.8f;
    bm.width_scale = 1.0f;
    break;
  case 17: // Sharp pencil v2
    bm.alpha = 1.0f;
    bm.width_scale = 0.6f;
    break;
  case 18: // Highlighter v2
    bm.alpha = 0.3f;
    bm.width_scale = 1.0f;
    break;
  case 21: // Calligraphy
    bm.alpha = 1.0f;
    bm.width_scale = 1.2f;
    break;
  case 23: // Shader
    bm.alpha = 0.6f;
    bm.width_scale = 1.0f;
    break;
  default:
    break;
  }
  return bm;
}

typedef struct {
  uint8_t r, g, b, a;
} canvas_pixel;

static uint32_t png_crc_table[256];
static bool png_crc_table_computed = false;

static void png_make_crc_table(void) {
  for (uint32_t i = 0; i < 256; i++) {
    uint32_t c = i;
    for (int j = 0; j < 8; j++) {
      if (c & 1) {
        c = 0xedb88320L ^ (c >> 1);
      } else {
        c = c >> 1;
      }
    }
    png_crc_table[i] = c;
  }
  png_crc_table_computed = true;
}

static uint32_t png_update_crc(uint32_t crc, const uint8_t *buf, size_t len) {
  if (!png_crc_table_computed) {
    png_make_crc_table();
  }
  uint32_t c = crc;
  for (size_t i = 0; i < len; i++) {
    c = png_crc_table[(c ^ buf[i]) & 0xff] ^ (c >> 8);
  }
  return c;
}

static void custom_png_write_chunk(FILE *f, const char *type,
                                   const uint8_t *data, uint32_t len) {
  uint8_t len_buf[4] = {(uint8_t)((len >> 24) & 0xff),
                        (uint8_t)((len >> 16) & 0xff),
                        (uint8_t)((len >> 8) & 0xff), (uint8_t)(len & 0xff)};
  fwrite(len_buf, 1, 4, f);
  fwrite(type, 1, 4, f);

  uint32_t crc = png_update_crc(0xffffffffL, (const uint8_t *)type, 4);
  if (len > 0 && data != NULL) {
    fwrite(data, 1, len, f);
    crc = png_update_crc(crc, data, len);
  }

  uint32_t final_crc = crc ^ 0xffffffffL;
  uint8_t crc_buf[4] = {
      (uint8_t)((final_crc >> 24) & 0xff), (uint8_t)((final_crc >> 16) & 0xff),
      (uint8_t)((final_crc >> 8) & 0xff), (uint8_t)(final_crc & 0xff)};
  fwrite(crc_buf, 1, 4, f);
}

static void write_png_to_stream(FILE *stream, canvas_pixel *canvas, int width,
                                int height) {
  const uint8_t png_sig[8] = {0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a};
  fwrite(png_sig, 1, 8, stream);

  uint8_t ihdr[13];
  ihdr[0] = (uint8_t)((width >> 24) & 0xff);
  ihdr[1] = (uint8_t)((width >> 16) & 0xff);
  ihdr[2] = (uint8_t)((width >> 8) & 0xff);
  ihdr[3] = (uint8_t)(width & 0xff);
  ihdr[4] = (uint8_t)((height >> 24) & 0xff);
  ihdr[5] = (uint8_t)((height >> 16) & 0xff);
  ihdr[6] = (uint8_t)((height >> 8) & 0xff);
  ihdr[7] = (uint8_t)(height & 0xff);
  ihdr[8] = 8;
  ihdr[9] = 6; // Color type 6: RGBA
  ihdr[10] = 0;
  ihdr[11] = 0;
  ihdr[12] = 0;
  custom_png_write_chunk(stream, "IHDR", ihdr, 13);

  size_t u_size = ((size_t)width * 4 + 1) * (size_t)height;
  uint8_t *u_buf = calloc(1, u_size);
  if (!u_buf) {
    return;
  }
  size_t u_ptr = 0;
  for (int y = 0; y < height; y++) {
    u_buf[u_ptr++] = 0;
    for (int x = 0; x < width; x++) {
      u_buf[u_ptr++] = canvas[y * width + x].r;
      u_buf[u_ptr++] = canvas[y * width + x].g;
      u_buf[u_ptr++] = canvas[y * width + x].b;
      u_buf[u_ptr++] = canvas[y * width + x].a;
    }
  }

  uint32_t s1 = 1;
  uint32_t s2 = 0;
  for (size_t i = 0; i < u_size; i++) {
    s1 = (s1 + u_buf[i]) % 65521;
    s2 = (s2 + s1) % 65521;
  }
  uint32_t adler = (s2 << 16) | s1;

  size_t num_blocks = (u_size + 65534) / 65535;
  size_t zlib_size = 2 + num_blocks * 5 + u_size + 4;
  uint8_t *zlib_buf = malloc(zlib_size);
  if (!zlib_buf) {
    free(u_buf);
    return;
  }

  size_t z_ptr = 0;
  zlib_buf[z_ptr++] = 0x78;
  zlib_buf[z_ptr++] = 0x01;

  size_t bytes_written = 0;
  while (bytes_written < u_size) {
    uint16_t len = (u_size - bytes_written > 65535)
                       ? 65535
                       : (uint16_t)(u_size - bytes_written);
    uint8_t bfinal = (bytes_written + len == u_size) ? 1 : 0;

    zlib_buf[z_ptr++] = bfinal;
    zlib_buf[z_ptr++] = (uint8_t)(len & 0xff);
    zlib_buf[z_ptr++] = (uint8_t)((len >> 8) & 0xff);
    zlib_buf[z_ptr++] = (uint8_t)((~len) & 0xff);
    zlib_buf[z_ptr++] = (uint8_t)(((~len) >> 8) & 0xff);

    memcpy(zlib_buf + z_ptr, u_buf + bytes_written, len);
    z_ptr += len;
    bytes_written += len;
  }

  zlib_buf[z_ptr++] = (uint8_t)((adler >> 24) & 0xff);
  zlib_buf[z_ptr++] = (uint8_t)((adler >> 16) & 0xff);
  zlib_buf[z_ptr++] = (uint8_t)((adler >> 8) & 0xff);
  zlib_buf[z_ptr++] = (uint8_t)(adler & 0xff);

  custom_png_write_chunk(stream, "IDAT", zlib_buf, (uint32_t)z_ptr);
  custom_png_write_chunk(stream, "IEND", NULL, 0);

  free(zlib_buf);
  free(u_buf);
}

static void draw_circle(canvas_pixel *canvas, uint8_t *mask, int width,
                        int height, float x, float y, float r,
                        uint32_t stroke_color, float alpha, bool is_eraser) {
  float min_x = x - r - 1.0f;
  float max_x = x + r + 1.0f;
  float min_y = y - r - 1.0f;
  float max_y = y + r + 1.0f;

  int start_x = (int)floorf(min_x);
  if (start_x < 0)
    start_x = 0;
  int end_x = (int)ceilf(max_x);
  if (end_x >= width)
    end_x = width - 1;
  int start_y = (int)floorf(min_y);
  if (start_y < 0)
    start_y = 0;
  int end_y = (int)ceilf(max_y);
  if (end_y >= height)
    end_y = height - 1;

  uint8_t src_r = 0, src_g = 0, src_b = 0;
  if (mask == NULL) {
    src_r = (uint8_t)((stroke_color >> 16) & 0xff);
    src_g = (uint8_t)((stroke_color >> 8) & 0xff);
    src_b = (uint8_t)(stroke_color & 0xff);
  }

  for (int py = start_y; py <= end_y; py++) {
    for (int px = start_x; px <= end_x; px++) {
      float cx = (float)px + 0.5f;
      float cy = (float)py + 0.5f;

      float dx_p = cx - x;
      float dy_p = cy - y;
      float dist2 = dx_p * dx_p + dy_p * dy_p;
      float r_plus = r + 0.5f;
      if (dist2 > r_plus * r_plus)
        continue;

      float dist = sqrtf(dist2);
      float coverage = 0.0f;
      if (dist <= r - 0.5f) {
        coverage = 1.0f;
      } else if (dist < r + 0.5f) {
        coverage = r + 0.5f - dist;
      }

      if (coverage > 0.001f) {
        int idx = py * width + px;
        if (mask != NULL) {
          uint8_t coverage_val = (uint8_t)(coverage * 255.0f + 0.5f);
          if (coverage_val > mask[idx]) {
            mask[idx] = coverage_val;
          }
        } else {
          float final_alpha = alpha * coverage;
          canvas_pixel *pixel = &canvas[idx];

          if (is_eraser) {
            pixel->a = (uint8_t)(pixel->a * (1.0f - final_alpha) + 0.5f);
          } else {
            float dst_a = pixel->a / 255.0f;
            float out_a = final_alpha + dst_a * (1.0f - final_alpha);
            if (out_a > 0.001f) {
              pixel->r = (uint8_t)((src_r * final_alpha +
                                    pixel->r * dst_a * (1.0f - final_alpha)) /
                                       out_a +
                                   0.5f);
              pixel->g = (uint8_t)((src_g * final_alpha +
                                    pixel->g * dst_a * (1.0f - final_alpha)) /
                                       out_a +
                                   0.5f);
              pixel->b = (uint8_t)((src_b * final_alpha +
                                    pixel->b * dst_a * (1.0f - final_alpha)) /
                                       out_a +
                                   0.5f);
              pixel->a = (uint8_t)(out_a * 255.0f + 0.5f);
            }
          }
        }
      }
    }
  }
}

static void draw_segment(canvas_pixel *canvas, uint8_t *mask, int width,
                         int height, float x1, float y1, float x2, float y2,
                         float r, uint32_t stroke_color, float alpha,
                         bool is_eraser) {
  float dx = x2 - x1;
  float dy = y2 - y1;
  float l2 = dx * dx + dy * dy;

  float min_x = fminf(x1, x2) - r - 1.0f;
  float max_x = fmaxf(x1, x2) + r + 1.0f;
  float min_y = fminf(y1, y2) - r - 1.0f;
  float max_y = fmaxf(y1, y2) + r + 1.0f;

  int start_x = (int)floorf(min_x);
  if (start_x < 0)
    start_x = 0;
  int end_x = (int)ceilf(max_x);
  if (end_x >= width)
    end_x = width - 1;
  int start_y = (int)floorf(min_y);
  if (start_y < 0)
    start_y = 0;
  int end_y = (int)ceilf(max_y);
  if (end_y >= height)
    end_y = height - 1;

  uint8_t src_r = 0, src_g = 0, src_b = 0;
  if (mask == NULL) {
    src_r = (uint8_t)((stroke_color >> 16) & 0xff);
    src_g = (uint8_t)((stroke_color >> 8) & 0xff);
    src_b = (uint8_t)(stroke_color & 0xff);
  }

  for (int py = start_y; py <= end_y; py++) {
    for (int px = start_x; px <= end_x; px++) {
      float cx = (float)px + 0.5f;
      float cy = (float)py + 0.5f;

      float t = 0.0f;
      if (l2 > 0.0f) {
        t = ((cx - x1) * dx + (cy - y1) * dy) / l2;
        if (t < 0.0f)
          t = 0.0f;
        else if (t > 1.0f)
          t = 1.0f;
      }
      float proj_x = x1 + t * dx;
      float proj_y = y1 + t * dy;

      float dx_p = cx - proj_x;
      float dy_p = cy - proj_y;
      float dist2 = dx_p * dx_p + dy_p * dy_p;
      float r_plus = r + 0.5f;
      if (dist2 > r_plus * r_plus)
        continue;

      float dist = sqrtf(dist2);
      float coverage = 0.0f;
      if (dist <= r - 0.5f) {
        coverage = 1.0f;
      } else if (dist < r + 0.5f) {
        coverage = r + 0.5f - dist;
      }

      if (coverage > 0.001f) {
        int idx = py * width + px;
        if (mask != NULL) {
          uint8_t coverage_val = (uint8_t)(coverage * 255.0f + 0.5f);
          if (coverage_val > mask[idx]) {
            mask[idx] = coverage_val;
          }
        } else {
          float final_alpha = alpha * coverage;
          canvas_pixel *pixel = &canvas[idx];

          if (is_eraser) {
            pixel->a = (uint8_t)(pixel->a * (1.0f - final_alpha) + 0.5f);
          } else {
            float dst_a = pixel->a / 255.0f;
            float out_a = final_alpha + dst_a * (1.0f - final_alpha);
            if (out_a > 0.001f) {
              pixel->r = (uint8_t)((src_r * final_alpha +
                                    pixel->r * dst_a * (1.0f - final_alpha)) /
                                       out_a +
                                   0.5f);
              pixel->g = (uint8_t)((src_g * final_alpha +
                                    pixel->g * dst_a * (1.0f - final_alpha)) /
                                       out_a +
                                   0.5f);
              pixel->b = (uint8_t)((src_b * final_alpha +
                                    pixel->b * dst_a * (1.0f - final_alpha)) /
                                       out_a +
                                   0.5f);
              pixel->a = (uint8_t)(out_a * 255.0f + 0.5f);
            }
          }
        }
      }
    }
  }
}

void remfmt_render_png(FILE *stream, remfmt_stroke_vec *strokes,
                       remfmt_render_params *prm) {
  float min_x = 0.0f;
  float max_x =
      (prm && prm->canvas_width > 0.0f) ? prm->canvas_width : (float)DEV_W;
  float min_y = 0.0f;
  float max_y =
      (prm && prm->canvas_height > 0.0f) ? prm->canvas_height : (float)DEV_H;

  if (strokes != NULL && !(prm && prm->annotation)) {
    for (int i = 0; i < kv_size(*strokes); i++) {
      remfmt_stroke *st = &kv_A(*strokes, i);
      float dev_w =
          (prm && prm->canvas_width > 0.0f) ? prm->canvas_width : (float)DEV_W;
      float xOffset = (st->version == 6) ? (dev_w / 2.0f) : 0.0f;
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

  int width = port_w;
  int height = port_h;
  if (prm && prm->landscape) {
    width = port_h;
    height = port_w;
  }

  canvas_pixel *canvas =
      malloc((size_t)width * (size_t)height * sizeof(canvas_pixel));
  if (!canvas) {
    return;
  }
  if (prm && prm->annotation) {
    memset(canvas, 0, (size_t)width * (size_t)height * sizeof(canvas_pixel));
  } else {
    memset(canvas, 255, (size_t)width * (size_t)height * sizeof(canvas_pixel));
  }

  uint8_t *mask = malloc((size_t)width * (size_t)height);

  if (prm && !prm->annotation && prm->template_dir && prm->template_name &&
      prm->template_name[0] != '\0') {
    int tw, th;
    unsigned char *tdata =
        load_template_data(prm->template_dir, prm->template_name, &tw, &th);
    if (tdata) {
      for (int y = 0; y < height && y < th; y++) {
        for (int x = 0; x < width && x < tw; x++) {
          int dst_idx = y * width + x;
          if (prm->landscape) {
            int x_orig = y;
            int y_orig = port_h - x - 1;
            if (x_orig >= 0 && x_orig < tw && y_orig >= 0 && y_orig < th) {
              int src_idx = (y_orig * tw + x_orig) * 3;
              canvas[dst_idx].r = tdata[src_idx];
              canvas[dst_idx].g = tdata[src_idx + 1];
              canvas[dst_idx].b = tdata[src_idx + 2];
            }
          } else {
            int src_idx = (y * tw + x) * 3;
            canvas[dst_idx].r = tdata[src_idx];
            canvas[dst_idx].g = tdata[src_idx + 1];
            canvas[dst_idx].b = tdata[src_idx + 2];
          }
        }
      }
      free(tdata);
    }
  }

  if (strokes != NULL) {
    for (int i = 0; i < kv_size(*strokes); i++) {
      remfmt_stroke *st = &kv_A(*strokes, i);
      int num_points = kv_size(st->segments);
      if (num_points == 0)
        continue;

      unsigned pen_type = st->pen;
      if (st->version == 6) {
        pen_type = map_v6_pen(pen_type);
      }
      png_brush_meta bm = get_png_brush(pen_type);
      if (bm.skip)
        continue;

      uint32_t stroke_color;
      if (st->has_custom_color) {
        stroke_color = st->custom_color;
      } else {
        stroke_color = (st->color < 14) // size of svg_color array
                           ? svg_color[st->color]
                           : 0x000000;
      }

      float dev_w_st =
          (prm && prm->canvas_width > 0.0f) ? prm->canvas_width : (float)DEV_W;
      float xOffset = (st->version == 6) ? (dev_w_st / 2.0f) : 0.0f;

      bool is_hl = (pen_type == 5 || pen_type == 18);
      bool is_eraser = (pen_type == 6 || pen_type == 7 || pen_type == 8);

      int box_min_x = 0, box_max_x = 0, box_min_y = 0, box_max_y = 0;
      if (is_hl && mask != NULL) {
        float s_min_x = 1e9f, s_max_x = -1e9f, s_min_y = 1e9f, s_max_y = -1e9f;
        for (int j = 0; j < num_points; j++) {
          remfmt_seg sg = kv_A(st->segments, j);
          float x = sg.x + xOffset - min_x;
          float y = sg.y - min_y;
          if (prm && prm->landscape) {
            float rx = (float)port_h - y;
            float ry = x;
            x = rx;
            y = ry;
          }
          float r = (sg.width * st->width * bm.width_scale) / 4.0f + 2.0f;
          if (x - r < s_min_x)
            s_min_x = x - r;
          if (x + r > s_max_x)
            s_max_x = x + r;
          if (y - r < s_min_y)
            s_min_y = y - r;
          if (y + r > s_max_y)
            s_max_y = y + r;
        }
        box_min_x = (int)floorf(s_min_x) - 1;
        if (box_min_x < 0)
          box_min_x = 0;
        box_max_x = (int)ceilf(s_max_x) + 1;
        if (box_max_x >= width)
          box_max_x = width - 1;
        box_min_y = (int)floorf(s_min_y) - 1;
        if (box_min_y < 0)
          box_min_y = 0;
        box_max_y = (int)ceilf(s_max_y) + 1;
        if (box_max_y >= height)
          box_max_y = height - 1;

        for (int y = box_min_y; y <= box_max_y; y++) {
          memset(&mask[y * width + box_min_x], 0, box_max_x - box_min_x + 1);
        }
      }

      if (num_points == 1) {
        remfmt_seg pt = kv_A(st->segments, 0);
        float x = pt.x + xOffset - min_x;
        float y = pt.y - min_y;
        if (prm && prm->landscape) {
          float rx = (float)port_h - y;
          float ry = x;
          x = rx;
          y = ry;
        }
        float r = (pt.width * st->width * bm.width_scale) / 4.0f;
        if (r < 0.5f)
          r = 0.5f;

        draw_circle(canvas, is_hl ? mask : NULL, width, height, x, y, r,
                    stroke_color, bm.alpha,
                    is_eraser && prm && prm->annotation);
      } else {
        for (int j = 1; j < num_points; j++) {
          remfmt_seg prev = kv_A(st->segments, j - 1);
          remfmt_seg curr = kv_A(st->segments, j);

          float x1 = prev.x + xOffset - min_x;
          float y1 = prev.y - min_y;
          float x2 = curr.x + xOffset - min_x;
          float y2 = curr.y - min_y;

          if (prm && prm->landscape) {
            float rx1 = (float)port_h - y1;
            float ry1 = x1;
            float rx2 = (float)port_h - y2;
            float ry2 = x2;
            x1 = rx1;
            y1 = ry1;
            x2 = rx2;
            y2 = ry2;
          }

          float segWidth = ((prev.width + curr.width) / 2.0f) *
                           (st->width / 2.0f) * bm.width_scale * 0.5f;
          if (segWidth < 0.4f)
            segWidth = 0.4f;

          float r = segWidth / 2.0f;

          draw_segment(canvas, is_hl ? mask : NULL, width, height, x1, y1, x2,
                       y2, r, stroke_color, bm.alpha,
                       is_eraser && prm && prm->annotation);
        }
      }

      if (is_hl && mask != NULL) {
        uint8_t src_r = (uint8_t)((stroke_color >> 16) & 0xff);
        uint8_t src_g = (uint8_t)((stroke_color >> 8) & 0xff);
        uint8_t src_b = (uint8_t)(stroke_color & 0xff);
        float alpha = bm.alpha;

        for (int py = box_min_y; py <= box_max_y; py++) {
          for (int px = box_min_x; px <= box_max_x; px++) {
            int idx = py * width + px;
            if (mask[idx] > 0) {
              float coverage = mask[idx] / 255.0f;
              float final_alpha = alpha * coverage;
              canvas_pixel *pixel = &canvas[idx];

              float dst_a = pixel->a / 255.0f;
              float out_a = final_alpha + dst_a * (1.0f - final_alpha);
              if (out_a > 0.001f) {
                pixel->r = (uint8_t)((src_r * final_alpha +
                                      pixel->r * dst_a * (1.0f - final_alpha)) /
                                         out_a +
                                     0.5f);
                pixel->g = (uint8_t)((src_g * final_alpha +
                                      pixel->g * dst_a * (1.0f - final_alpha)) /
                                         out_a +
                                     0.5f);
                pixel->b = (uint8_t)((src_b * final_alpha +
                                      pixel->b * dst_a * (1.0f - final_alpha)) /
                                         out_a +
                                     0.5f);
                pixel->a = (uint8_t)(out_a * 255.0f + 0.5f);
              }
            }
          }
        }
      }
    }
  }

  write_png_to_stream(stream, canvas, width, height);
  free(canvas);
  if (mask)
    free(mask);
}
