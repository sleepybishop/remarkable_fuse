#include "remfmt.h"
#include "struct.h"
#include <fcntl.h>
#include <png.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

static unsigned char *load_png_template(const char *filename, int *w, int *h) {
  FILE *fp = fopen(filename, "rb");
  if (!fp)
    return NULL;

  png_structp png =
      png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
  if (!png) {
    fclose(fp);
    return NULL;
  }

  png_infop info = png_create_info_struct(png);
  if (!info) {
    png_destroy_read_struct(&png, NULL, NULL);
    fclose(fp);
    return NULL;
  }

  if (setjmp(png_jmpbuf(png))) {
    png_destroy_read_struct(&png, &info, NULL);
    fclose(fp);
    return NULL;
  }

  png_init_io(png, fp);
  png_read_info(png, info);

  *w = png_get_image_width(png, info);
  *h = png_get_image_height(png, info);
  png_byte color_type = png_get_color_type(png, info);
  png_byte bit_depth = png_get_bit_depth(png, info);

  if (bit_depth == 16)
    png_set_strip_16(png);
  if (color_type == PNG_COLOR_TYPE_PALETTE)
    png_set_palette_to_rgb(png);
  if (color_type == PNG_COLOR_TYPE_GRAY && bit_depth < 8)
    png_set_expand_gray_1_2_4_to_8(png);
  if (png_get_valid(png, info, PNG_INFO_tRNS))
    png_set_tRNS_to_alpha(png);
  if (color_type & PNG_COLOR_MASK_ALPHA)
    png_set_strip_alpha(png);
  if (color_type == PNG_COLOR_TYPE_GRAY ||
      color_type == PNG_COLOR_TYPE_GRAY_ALPHA)
    png_set_gray_to_rgb(png);

  png_read_update_info(png, info);

  size_t rowbytes = png_get_rowbytes(png, info);
  unsigned char *data = malloc(rowbytes * (*h));
  if (!data) {
    png_destroy_read_struct(&png, &info, NULL);
    fclose(fp);
    return NULL;
  }

  png_bytep *row_pointers = malloc(sizeof(png_bytep) * (*h));
  for (int i = 0; i < *h; i++) {
    row_pointers[i] = data + i * rowbytes;
  }

  png_read_image(png, row_pointers);
  free(row_pointers);

  png_destroy_read_struct(&png, &info, NULL);
  fclose(fp);

  return data;
}

static const char b64_table[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
static sds base64_encode(const unsigned char *src, size_t len) {
  sds out = sdsempty();
  size_t i = 0;
  while (i < len) {
    uint32_t octet_a = i < len ? src[i++] : 0;
    uint32_t octet_b = i < len ? src[i++] : 0;
    uint32_t octet_c = i < len ? src[i++] : 0;
    uint32_t triple = (octet_a << 0x10) + (octet_b << 0x08) + octet_c;
    out = sdscatprintf(out, "%c%c%c%c", b64_table[(triple >> 3 * 6) & 0x3F],
                       b64_table[(triple >> 2 * 6) & 0x3F],
                       b64_table[(triple >> 1 * 6) & 0x3F],
                       b64_table[(triple >> 0 * 6) & 0x3F]);
  }
  int pad = len % 3;
  if (pad > 0) {
    out[sdslen(out) - 1] = '=';
    if (pad == 1)
      out[sdslen(out) - 2] = '=';
  }
  return out;
}

typedef struct {
  const uint8_t *data;
  size_t size;
  size_t pos;
} rm_buf;

static const char rmv_magic[] = "reMarkable .lines file, version=%d          ";

static uint32_t svg_color[] = {0x000000, 0x7d7d7d, 0xffffff, 0xebcb8b, 0xa2f567,
                               0xfe93bf, 0x000088, 0x880000, 0x0d0d0d, 0xffed75,
                               0xa1d87d, 0x8bd0e5, 0xb782cd, 0xf7e851};

static const char *svg_tpl[] = {
    "<svg xmlns=\"http://www.w3.org/2000/svg\" height=\"%d\" width=\"%d\">\n"
    "  <g transform=\"rotate(%d %d %d)\">\n",
    "    <polyline style=\"fill:none; stroke:#%06x; "
    "stroke-width:%.3f;opacity:%.3f\" stroke-linejoin=\"round\" "
    "stroke-linecap=\"%s\" points=\"%s\"/>\n",
    "  </g>\n"
    "</svg>\n"};

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
  SHADER = 23,
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
  case SHADER:
    st->opacity = 0.10;
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

static unsigned map_v6_pen(unsigned pen_id) {
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
    bm.skip = true;
    break;
  case 7: // Eraser area
    bm.alpha = 1.0f;
    bm.width_scale = 1.0f;
    bm.skip = true;
    break;
  case 8: // Erase all
    bm.alpha = 1.0f;
    bm.width_scale = 1.0f;
    bm.skip = true;
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
                        uint32_t stroke_color, float alpha) {
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

static void draw_segment(canvas_pixel *canvas, uint8_t *mask, int width,
                         int height, float x1, float y1, float x2, float y2,
                         float r, uint32_t stroke_color, float alpha) {
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
    sds tpl_path = sdsempty();
    tpl_path = sdscatprintf(tpl_path, "%s/%s.png", prm->template_dir,
                            prm->template_name);

    int tw, th;
    unsigned char *tdata = load_png_template(tpl_path, &tw, &th);
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
    sdsfree(tpl_path);
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
        stroke_color = (st->color < sizeof(svg_color) / sizeof(svg_color[0]))
                           ? svg_color[st->color]
                           : 0x000000;
      }

      float dev_w_st =
          (prm && prm->canvas_width > 0.0f) ? prm->canvas_width : (float)DEV_W;
      float xOffset = (st->version == 6) ? (dev_w_st / 2.0f) : 0.0f;

      bool is_hl = (pen_type == 5 || pen_type == 18);

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
                    stroke_color, bm.alpha);
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
                       y2, r, stroke_color, bm.alpha);
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
    sds tpl_path = sdscatprintf(sdsempty(), "%s/%s.png", prm->template_dir,
                                prm->template_name);
    FILE *f = fopen(tpl_path, "rb");
    if (f) {
      fseek(f, 0, SEEK_END);
      long fsize = ftell(f);
      fseek(f, 0, SEEK_SET);
      unsigned char *fbuf = malloc(fsize);
      if (fbuf) {
        if (fread(fbuf, 1, fsize, f) == fsize) {
          sds b64 = base64_encode(fbuf, fsize);
          b64_img =
              sdscatprintf(b64_img,
                           "<image x=\"0\" y=\"0\" width=\"%d\" height=\"%d\" "
                           "href=\"data:image/png;base64,%s\"></image>\n",
                           DEV_W, DEV_H, b64);
          sdsfree(b64);
        }
        free(fbuf);
      }
      fclose(f);
    }
    sdsfree(tpl_path);
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
        seg_color = (st.color < sizeof(svg_color) / sizeof(svg_color[0]))
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
          seg_color = (st.color < sizeof(svg_color) / sizeof(svg_color[0]))
                          ? svg_color[st.color]
                          : 0x000000;
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
  fprintf(stream, svg_tpl[SVG_FOOTER]);
}

void remfmt_render_pdf(FILE *stream, remfmt_stroke_vec *strokes,
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

  int width = port_w;
  int height = port_h;
  if (prm && prm->landscape) {
    width = port_h;
    height = port_w;
  }

  sds pdf_content = sdsempty();
  if (strokes != NULL) {
    for (int i = 0; i < kv_size(*strokes); i++) {
      remfmt_stroke st = kv_A(*strokes, i);
      set_pen_attr(&st);
      int num_points = kv_size(st.segments);
      if (num_points == 0)
        continue;

      uint32_t seg_color;
      if (st.has_custom_color) {
        seg_color = st.custom_color;
      } else {
        seg_color = (st.color < sizeof(svg_color) / sizeof(svg_color[0]))
                        ? svg_color[st.color]
                        : 0x000000;
      }

      float r = ((seg_color >> 16) & 0xff) / 255.0f;
      float g = ((seg_color >> 8) & 0xff) / 255.0f;
      float b = (seg_color & 0xff) / 255.0f;

      float seg_width = st.calc_width, lsw = seg_width;
      float xOffset = (st.version == 6) ? ((float)DEV_W / 2.0f) : 0.0f;

      float seg_alpha = get_seg_alpha(&st, &kv_A(st.segments, 0));
      const char *gs_state = "/GS100";
      if (seg_alpha < 0.15) {
        gs_state = "/GS10";
      } else if (seg_alpha < 0.35) {
        gs_state = "/GS25";
      } else if (seg_alpha < 0.95) {
        gs_state = "/GS90";
      }

      pdf_content = sdscatprintf(pdf_content, "q\n");
      pdf_content = sdscatprintf(pdf_content, "%s gs\n", gs_state);
      pdf_content = sdscatprintf(pdf_content, "%.3f w\n", seg_width);
      pdf_content = sdscatprintf(pdf_content, "%.3f %.3f %.3f RG\n", r, g, b);
      pdf_content = sdscatprintf(pdf_content, "%d J\n", st.square_cap ? 2 : 1);
      pdf_content = sdscatprintf(pdf_content, "1 j\n");

      // First point
      remfmt_seg pt0 = kv_A(st.segments, 0);
      float x0 = pt0.x + xOffset - min_x;
      float y0 = pt0.y - min_y;
      if (prm && prm->landscape) {
        float rx = (float)port_h - y0;
        float ry = x0;
        x0 = rx;
        y0 = ry;
      }
      float yp0 = (float)height - y0;
      pdf_content = sdscatprintf(pdf_content, "%.3f %.3f m\n", x0, yp0);

      if (num_points == 1) {
        pdf_content = sdscatprintf(pdf_content, "%.3f %.3f l\n", x0, yp0);
      }

      for (int j = 1; j < num_points; j++) {
        remfmt_seg sg = kv_A(st.segments, j);
        float x = sg.x + xOffset - min_x;
        float y = sg.y - min_y;

        if (prm && prm->landscape) {
          float rx = (float)port_h - y;
          float ry = x;
          x = rx;
          y = ry;
        }

        float yp = (float)height - y;
        seg_width = get_seg_width(&st, &sg);

        pdf_content = sdscatprintf(pdf_content, "%.3f %.3f l\n", x, yp);
        if (fabsf(lsw - seg_width) > 0.08f * lsw) {
          pdf_content = sdscatprintf(pdf_content, "S\n");
          pdf_content = sdscatprintf(pdf_content, "%.3f w\n", seg_width);
          pdf_content = sdscatprintf(pdf_content, "%.3f %.3f m\n", x, yp);
          lsw = seg_width;
        }
      }
      pdf_content = sdscatprintf(pdf_content, "S\nQ\n");
    }
  }

  int tw = 0, th = 0;
  unsigned char *tdata = NULL;
  if (prm && prm->template_dir && prm->template_name &&
      prm->template_name[0] != '\0') {
    sds tpl_path = sdscatprintf(sdsempty(), "%s/%s.png", prm->template_dir,
                                prm->template_name);
    tdata = load_png_template(tpl_path, &tw, &th);
    sdsfree(tpl_path);
  }

  sds pdf = sdsempty();
  pdf = sdscat(pdf, "%PDF-1.4\n");

  long offsets[6] = {0};

  offsets[1] = (long)sdslen(pdf);
  pdf = sdscat(pdf, "1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n");

  offsets[2] = (long)sdslen(pdf);
  pdf = sdscat(
      pdf, "2 0 obj\n<< /Type /Pages /Kids [ 3 0 R ] /Count 1 >>\nendobj\n");

  offsets[3] = (long)sdslen(pdf);
  if (tdata) {
    pdf = sdscatprintf(
        pdf,
        "3 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [ 0 0 %d "
        "%d ] /Contents 4 0 R /Resources << /XObject << /Im1 5 0 "
        "R >> /ExtGState << /GS25 << /Type /ExtGState /ca 0.25 /CA 0.25 >> "
        "/GS90 << /Type /ExtGState /ca 0.90 /CA 0.90 >> /GS10 << /Type "
        "/ExtGState /ca 0.10 /CA 0.10 >> /GS100 << /Type /ExtGState /ca 1.00 "
        "/CA 1.00 >> >> >> >>\nendobj\n",
        width, height);
  } else {
    pdf = sdscatprintf(
        pdf,
        "3 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [ 0 0 %d "
        "%d ] /Contents 4 0 R /Resources << /ExtGState << /GS25 << /Type "
        "/ExtGState /ca 0.25 /CA 0.25 >> /GS90 << /Type /ExtGState /ca 0.90 "
        "/CA 0.90 >> /GS10 << /Type /ExtGState /ca 0.10 /CA 0.10 >> /GS100 << "
        "/Type /ExtGState /ca 1.00 /CA 1.00 >> >> >> >>\nendobj\n",
        width, height);
  }

  offsets[4] = (long)sdslen(pdf);

  if (tdata) {
    sds bg = sdsempty();
    if (prm && prm->landscape) {
      bg = sdscatprintf(bg, "q\n0 %d -%d 0 %d 0 cm\n/Im1 Do\nQ\n", height,
                        width, width);
    } else {
      bg = sdscatprintf(bg, "q\n%d 0 0 %d 0 0 cm\n/Im1 Do\nQ\n", width, height);
    }
    bg = sdscatsds(bg, pdf_content);
    sdsfree(pdf_content);
    pdf_content = bg;
  }

  pdf = sdscatprintf(
      pdf, "4 0 obj\n<< /Length %ld >>\nstream\n%s\nendstream\nendobj\n",
      (long)sdslen(pdf_content), pdf_content);

  if (tdata) {
    offsets[5] = (long)sdslen(pdf);
    pdf = sdscatprintf(
        pdf,
        "5 0 obj\n<< /Type /XObject /Subtype /Image /Width %d /Height %d "
        "/ColorSpace /DeviceRGB /BitsPerComponent 8 /Length %ld >>\nstream\n",
        tw, th, (long)(tw * th * 3));
    pdf = sdscatlen(pdf, tdata, tw * th * 3);
    pdf = sdscat(pdf, "\nendstream\nendobj\n");
    free(tdata);
  }

  long xref_pos = (long)sdslen(pdf);
  int num_objs = tdata ? 6 : 5;
  pdf = sdscatprintf(pdf, "xref\n0 %d\n", num_objs);
  pdf = sdscat(pdf, "0000000000 65535 f \n");
  for (int i = 1; i < num_objs; i++) {
    pdf = sdscatprintf(pdf, "%010ld 00000 n \n", offsets[i]);
  }

  pdf = sdscatprintf(pdf, "trailer\n<< /Size %d /Root 1 0 R >>\n", num_objs);
  pdf = sdscatprintf(pdf, "startxref\n%ld\n%%%%EOF\n", xref_pos);

  fwrite(pdf, 1, sdslen(pdf), stream);
  sdsfree(pdf);
  sdsfree(pdf_content);
}

void remfmt_render_notebook_pdf(FILE *stream, int num_pages,
                                remfmt_stroke_vec **pages_strokes,
                                remfmt_render_params **pages_prms) {
  if (num_pages <= 0)
    return;

  typedef struct {
    sds path;
    unsigned char *tdata;
    int tw;
    int th;
    int obj_id;
  } unique_template;

  int *page_obj_ids = malloc(num_pages * sizeof(int));
  int *contents_obj_ids = malloc(num_pages * sizeof(int));
  int *page_template_obj_ids = calloc(num_pages, sizeof(int));

  unique_template *utemplates = calloc(num_pages, sizeof(unique_template));
  int num_utemplates = 0;

  // 1. Gather all unique templates and load them once
  for (int i = 0; i < num_pages; i++) {
    remfmt_render_params *prm = pages_prms[i];
    if (prm && prm->template_dir && prm->template_name &&
        prm->template_name[0] != '\0') {
      sds tpl_path = sdscatprintf(sdsempty(), "%s/%s.png", prm->template_dir,
                                  prm->template_name);

      int found_idx = -1;
      for (int j = 0; j < num_utemplates; j++) {
        if (sdscmp(utemplates[j].path, tpl_path) == 0) {
          found_idx = j;
          break;
        }
      }

      if (found_idx == -1) {
        int tw = 0, th = 0;
        unsigned char *tdata = load_png_template(tpl_path, &tw, &th);
        if (tdata != NULL) {
          utemplates[num_utemplates].path = sdsdup(tpl_path);
          utemplates[num_utemplates].tdata = tdata;
          utemplates[num_utemplates].tw = tw;
          utemplates[num_utemplates].th = th;
          utemplates[num_utemplates].obj_id = 0; // will assign later
          num_utemplates++;
        }
      }
      sdsfree(tpl_path);
    }
  }

  // 2. Assign PDF object IDs
  int next_id = 3;
  for (int i = 0; i < num_pages; i++) {
    page_obj_ids[i] = next_id++;
    contents_obj_ids[i] = next_id++;
  }

  for (int j = 0; j < num_utemplates; j++) {
    utemplates[j].obj_id = next_id++;
  }

  // Map each page to its unique template's PDF object ID
  for (int i = 0; i < num_pages; i++) {
    remfmt_render_params *prm = pages_prms[i];
    if (prm && prm->template_dir && prm->template_name &&
        prm->template_name[0] != '\0') {
      sds tpl_path = sdscatprintf(sdsempty(), "%s/%s.png", prm->template_dir,
                                  prm->template_name);
      for (int j = 0; j < num_utemplates; j++) {
        if (sdscmp(utemplates[j].path, tpl_path) == 0) {
          page_template_obj_ids[i] = utemplates[j].obj_id;
          break;
        }
      }
      sdsfree(tpl_path);
    }
  }

  int total_objs = next_id;
  long *offsets = calloc(total_objs, sizeof(long));

  sds pdf = sdsempty();
  pdf = sdscat(pdf, "%PDF-1.4\n");

  offsets[1] = (long)sdslen(pdf);
  pdf = sdscat(pdf, "1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n");

  offsets[2] = (long)sdslen(pdf);
  pdf = sdscat(pdf, "2 0 obj\n<< /Type /Pages /Kids [ ");
  for (int i = 0; i < num_pages; i++) {
    pdf = sdscatprintf(pdf, "%d 0 R ", page_obj_ids[i]);
  }
  pdf = sdscatprintf(pdf, "] /Count %d >>\nendobj\n", num_pages);

  // 3. Render pages
  for (int i = 0; i < num_pages; i++) {
    remfmt_stroke_vec *strokes = pages_strokes[i];
    remfmt_render_params *prm = pages_prms[i];

    float min_x = 0.0f;
    float max_x = (float)DEV_W;
    float min_y = 0.0f;
    float max_y = (float)DEV_H;

    if (strokes != NULL && !(prm && prm->annotation)) {
      for (int k = 0; k < kv_size(*strokes); k++) {
        remfmt_stroke *st = &kv_A(*strokes, k);
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
    int width = port_w;
    int height = port_h;
    if (prm && prm->landscape) {
      width = port_h;
      height = port_w;
    }

    sds pdf_content = sdsempty();
    if (strokes != NULL) {
      for (int k = 0; k < kv_size(*strokes); k++) {
        remfmt_stroke st = kv_A(*strokes, k);
        set_pen_attr(&st);
        int num_points = kv_size(st.segments);
        if (num_points == 0)
          continue;

        uint32_t seg_color;
        if (st.has_custom_color) {
          seg_color = st.custom_color;
        } else {
          seg_color = (st.color < sizeof(svg_color) / sizeof(svg_color[0]))
                          ? svg_color[st.color]
                          : 0x000000;
        }

        float r = ((seg_color >> 16) & 0xff) / 255.0f;
        float g = ((seg_color >> 8) & 0xff) / 255.0f;
        float b = (seg_color & 0xff) / 255.0f;

        float seg_width = st.calc_width, lsw = seg_width;
        float xOffset = (st.version == 6) ? ((float)DEV_W / 2.0f) : 0.0f;

        float seg_alpha = get_seg_alpha(&st, &kv_A(st.segments, 0));
        const char *gs_state = "/GS100";
        if (seg_alpha < 0.15) {
          gs_state = "/GS10";
        } else if (seg_alpha < 0.35) {
          gs_state = "/GS25";
        } else if (seg_alpha < 0.95) {
          gs_state = "/GS90";
        }

        pdf_content = sdscatprintf(pdf_content, "q\n");
        pdf_content = sdscatprintf(pdf_content, "%s gs\n", gs_state);
        pdf_content = sdscatprintf(pdf_content, "%.3f w\n", seg_width);
        pdf_content = sdscatprintf(pdf_content, "%.3f %.3f %.3f RG\n", r, g, b);
        pdf_content =
            sdscatprintf(pdf_content, "%d J\n", st.square_cap ? 2 : 1);
        pdf_content = sdscatprintf(pdf_content, "1 j\n");

        remfmt_seg pt0 = kv_A(st.segments, 0);
        float x0 = pt0.x + xOffset - min_x;
        float y0 = pt0.y - min_y;
        if (prm && prm->landscape) {
          float rx = (float)port_h - y0;
          float ry = x0;
          x0 = rx;
          y0 = ry;
        }
        float yp0 = (float)height - y0;
        pdf_content = sdscatprintf(pdf_content, "%.3f %.3f m\n", x0, yp0);

        if (num_points == 1) {
          pdf_content = sdscatprintf(pdf_content, "%.3f %.3f l\n", x0, yp0);
        }

        for (int j = 1; j < num_points; j++) {
          remfmt_seg sg = kv_A(st.segments, j);
          float x = sg.x + xOffset - min_x;
          float y = sg.y - min_y;

          if (prm && prm->landscape) {
            float rx = (float)port_h - y;
            float ry = x;
            x = rx;
            y = ry;
          }

          float yp = (float)height - y;
          seg_width = get_seg_width(&st, &sg);

          pdf_content = sdscatprintf(pdf_content, "%.3f %.3f l\n", x, yp);
          if (fabsf(lsw - seg_width) > 0.08f * lsw) {
            pdf_content = sdscatprintf(pdf_content, "S\n");
            pdf_content = sdscatprintf(pdf_content, "%.3f w\n", seg_width);
            pdf_content = sdscatprintf(pdf_content, "%.3f %.3f m\n", x, yp);
            lsw = seg_width;
          }
        }
        pdf_content = sdscatprintf(pdf_content, "S\nQ\n");
      }
    }

    offsets[page_obj_ids[i]] = (long)sdslen(pdf);
    if (page_template_obj_ids[i] != 0) {
      pdf = sdscatprintf(
          pdf,
          "%d 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [ 0 0 %d "
          "%d ] /Contents %d 0 R /Resources << /XObject << /Im1 %d 0 "
          "R >> /ExtGState << /GS25 << /Type /ExtGState /ca 0.25 /CA 0.25 >> "
          "/GS90 << /Type /ExtGState /ca 0.90 /CA 0.90 >> /GS10 << /Type "
          "/ExtGState /ca 0.10 /CA 0.10 >> /GS100 << /Type /ExtGState /ca 1.00 "
          "/CA 1.00 >> >> >> >>\nendobj\n",
          page_obj_ids[i], width, height, contents_obj_ids[i],
          page_template_obj_ids[i]);
    } else {
      pdf = sdscatprintf(
          pdf,
          "%d 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [ 0 0 %d "
          "%d ] /Contents %d 0 R /Resources << /ExtGState << /GS25 << /Type "
          "/ExtGState /ca 0.25 /CA 0.25 >> /GS90 << /Type /ExtGState /ca 0.90 "
          "/CA 0.90 >> /GS10 << /Type /ExtGState /ca 0.10 /CA 0.10 >> /GS100 "
          "<< /Type /ExtGState /ca 1.00 /CA 1.00 >> >> >> >>\nendobj\n",
          page_obj_ids[i], width, height, contents_obj_ids[i]);
    }

    offsets[contents_obj_ids[i]] = (long)sdslen(pdf);
    if (page_template_obj_ids[i] != 0) {
      sds bg = sdsempty();
      if (prm && prm->landscape) {
        bg = sdscatprintf(bg, "q\n0 %d -%d 0 %d 0 cm\n/Im1 Do\nQ\n", height,
                          width, width);
      } else {
        bg = sdscatprintf(bg, "q\n%d 0 0 %d 0 0 cm\n/Im1 Do\nQ\n", width,
                          height);
      }
      bg = sdscatsds(bg, pdf_content);
      sdsfree(pdf_content);
      pdf_content = bg;
    }

    pdf = sdscatprintf(
        pdf, "%d 0 obj\n<< /Length %ld >>\nstream\n%s\nendstream\nendobj\n",
        contents_obj_ids[i], (long)sdslen(pdf_content), pdf_content);
    sdsfree(pdf_content);
  }

  // 4. Render unique template image objects
  for (int j = 0; j < num_utemplates; j++) {
    offsets[utemplates[j].obj_id] = (long)sdslen(pdf);
    pdf = sdscatprintf(
        pdf,
        "%d 0 obj\n<< /Type /XObject /Subtype /Image /Width %d /Height %d "
        "/ColorSpace /DeviceRGB /BitsPerComponent 8 /Length %ld >>\nstream\n",
        utemplates[j].obj_id, utemplates[j].tw, utemplates[j].th,
        (long)(utemplates[j].tw * utemplates[j].th * 3));
    pdf = sdscatlen(pdf, utemplates[j].tdata,
                    utemplates[j].tw * utemplates[j].th * 3);
    pdf = sdscat(pdf, "\nendstream\nendobj\n");
  }

  long xref_pos = (long)sdslen(pdf);
  pdf = sdscatprintf(pdf, "xref\n0 %d\n", total_objs);
  pdf = sdscat(pdf, "0000000000 65535 f \n");
  for (int i = 1; i < total_objs; i++) {
    pdf = sdscatprintf(pdf, "%010ld 00000 n \n", offsets[i]);
  }

  pdf = sdscatprintf(pdf, "trailer\n<< /Size %d /Root 1 0 R >>\n", total_objs);
  pdf = sdscatprintf(pdf, "startxref\n%ld\n%%%%EOF\n", xref_pos);

  fwrite(pdf, 1, sdslen(pdf), stream);
  sdsfree(pdf);

  // 5. Cleanup
  for (int j = 0; j < num_utemplates; j++) {
    sdsfree(utemplates[j].path);
    free(utemplates[j].tdata);
  }
  free(utemplates);

  free(page_obj_ids);
  free(contents_obj_ids);
  free(page_template_obj_ids);
  free(offsets);
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

#define TAG_TYPE_ID 0xF
#define TAG_TYPE_LENGTH4 0xC
#define TAG_TYPE_BYTE8 0x8
#define TAG_TYPE_BYTE4 0x4
#define TAG_TYPE_BYTE1 0x1

static uint8_t read_uint8(rm_buf *b) {
  if (b->pos >= b->size)
    return 0;
  return b->data[b->pos++];
}

static uint16_t read_uint16(rm_buf *b) {
  if (b->pos + 2 > b->size)
    return 0;
  uint16_t val =
      (uint16_t)b->data[b->pos] | ((uint16_t)b->data[b->pos + 1] << 8);
  b->pos += 2;
  return val;
}

static uint32_t read_uint32(rm_buf *b) {
  if (b->pos + 4 > b->size)
    return 0;
  uint32_t val = (uint32_t)b->data[b->pos] |
                 ((uint32_t)b->data[b->pos + 1] << 8) |
                 ((uint32_t)b->data[b->pos + 2] << 16) |
                 ((uint32_t)b->data[b->pos + 3] << 24);
  b->pos += 4;
  return val;
}

static float read_float32(rm_buf *b) {
  union {
    uint32_t u;
    float f;
  } val;
  val.u = read_uint32(b);
  return val.f;
}

static double read_float64(rm_buf *b) {
  union {
    uint64_t u;
    double d;
  } val;
  if (b->pos + 8 > b->size)
    return 0.0;
  const uint8_t *buf = b->data + b->pos;
  val.u = (uint64_t)buf[0] | ((uint64_t)buf[1] << 8) |
          ((uint64_t)buf[2] << 16) | ((uint64_t)buf[3] << 24) |
          ((uint64_t)buf[4] << 32) | ((uint64_t)buf[5] << 40) |
          ((uint64_t)buf[6] << 48) | ((uint64_t)buf[7] << 56);
  b->pos += 8;
  return val.d;
}

static uint64_t read_varuint(rm_buf *b) {
  uint64_t result = 0;
  int shift = 0;
  while (b->pos < b->size) {
    uint8_t c = b->data[b->pos++];
    result |= ((uint64_t)(c & 0x7F)) << shift;
    shift += 7;
    if ((c & 0x80) == 0)
      break;
  }
  return result;
}

static void read_crdt_id(rm_buf *b, uint8_t *part1, uint64_t *part2) {
  *part1 = read_uint8(b);
  *part2 = read_varuint(b);
}

static bool read_tag(rm_buf *b, int expectedIndex, int expectedType) {
  uint64_t x = read_varuint(b);
  int index = (int)(x >> 4);
  int type = (int)(x & 0xF);
  return (index == expectedIndex && type == expectedType);
}

static bool check_tag(rm_buf *b, int expectedIndex, int expectedType) {
  size_t pos = b->pos;
  uint64_t x = read_varuint(b);
  int index = (int)(x >> 4);
  int type = (int)(x & 0xF);
  b->pos = pos;
  return (index == expectedIndex && type == expectedType);
}

static void parse_scene_line_item(rm_buf *b, uint8_t version,
                                  remfmt_stroke_vec *strokes,
                                  size_t block_body_pos,
                                  uint32_t block_length) {
  uint8_t p1;
  uint64_t p2;

  if (!read_tag(b, 1, TAG_TYPE_ID))
    return;
  read_crdt_id(b, &p1, &p2);

  if (!read_tag(b, 2, TAG_TYPE_ID))
    return;
  read_crdt_id(b, &p1, &p2);

  if (!read_tag(b, 3, TAG_TYPE_ID))
    return;
  read_crdt_id(b, &p1, &p2);

  if (!read_tag(b, 4, TAG_TYPE_ID))
    return;
  read_crdt_id(b, &p1, &p2);

  if (!read_tag(b, 5, TAG_TYPE_BYTE4))
    return;
  read_uint32(b);

  if (check_tag(b, 6, TAG_TYPE_LENGTH4)) {
    read_tag(b, 6, TAG_TYPE_LENGTH4);
    uint32_t subblock_len = read_uint32(b);
    size_t subblock_start = b->pos;

    uint8_t item_type = read_uint8(b);
    if (item_type == 0x03) { // line item
      if (!read_tag(b, 1, TAG_TYPE_BYTE4))
        goto skip_subblock;
      uint32_t tool_id = read_uint32(b);

      if (!read_tag(b, 2, TAG_TYPE_BYTE4))
        goto skip_subblock;
      uint32_t color_id = read_uint32(b);

      if (!read_tag(b, 3, TAG_TYPE_BYTE8))
        goto skip_subblock;
      double thickness_scale = read_float64(b);

      if (!read_tag(b, 4, TAG_TYPE_BYTE4))
        goto skip_subblock;
      float starting_length = read_float32(b);
      (void)starting_length;

      if (!read_tag(b, 5, TAG_TYPE_LENGTH4))
        goto skip_subblock;
      uint32_t points_len = read_uint32(b);

      size_t points_curr = b->pos;
      if (subblock_start + subblock_len < points_curr + points_len) {
        goto skip_subblock;
      }

      int point_size = (version == 1) ? 24 : 14;
      int num_points = points_len / point_size;

      remfmt_stroke st = {0};
      st.version = 6;
      st.pen = tool_id;
      st.color = color_id;
      st.width = (float)thickness_scale;
      st.layer = 0;
      st.has_custom_color = false;
      st.custom_color = 0;

      for (int i = 0; i < num_points; i++) {
        float x = read_float32(b);
        float y = read_float32(b);

        uint16_t speed = 0;
        uint16_t width = 0;
        uint8_t direction = 0;
        uint8_t pressure = 0;

        if (version == 1) {
          float speed_f = read_float32(b);
          speed = (uint16_t)(speed_f * 4.0f);
          float dir_f = read_float32(b);
          direction = (uint8_t)(255.0f * dir_f / (3.1415926535f * 2.0f));
          float width_f = read_float32(b);
          width = (uint16_t)(width_f * 4.0f);
          float pressure_f = read_float32(b);
          pressure = (uint8_t)(pressure_f * 255.0f);
        } else {
          speed = read_uint16(b);
          width = read_uint16(b);
          direction = read_uint8(b);
          pressure = read_uint8(b);
        }

        remfmt_seg sg = {.x = x,
                         .y = y,
                         .speed = (float)speed,
                         .tilt =
                             (float)direction * (3.1415926535f * 2.0f) / 255.0f,
                         .width = (float)width / 4.0f,
                         .pressure = (float)pressure / 255.0f};

        kv_push(remfmt_seg, st.segments, sg);
      }

      if (read_tag(b, 6, TAG_TYPE_ID)) {
        read_crdt_id(b, &p1, &p2);
      }

      if (check_tag(b, 7, TAG_TYPE_ID)) {
        read_tag(b, 7, TAG_TYPE_ID);
        read_crdt_id(b, &p1, &p2);
      }

      size_t subblock_curr = b->pos;
      size_t remaining = block_body_pos + block_length - subblock_curr;
      if (remaining >= 6) {
        read_uint16(b);
        uint8_t g = read_uint8(b);
        uint8_t rgb_r = read_uint8(b);
        uint8_t rgb_b = read_uint8(b);
        uint8_t a = read_uint8(b);
        (void)a;
        st.has_custom_color = true;
        st.custom_color = ((uint32_t)rgb_r << 16) | ((uint32_t)g << 8) | rgb_b;
      }

      kv_push(remfmt_stroke, *strokes, st);
    }
  skip_subblock:
    b->pos = subblock_start + subblock_len;
  }
}

static void parse_scene_glyph_item(rm_buf *b, uint32_t version,
                                   remfmt_stroke_vec *strokes,
                                   size_t block_body_pos, size_t block_length) {
  uint8_t p1 = 0;
  uint64_t p2 = 0;

  if (!read_tag(b, 1, TAG_TYPE_ID))
    return;
  read_crdt_id(b, &p1, &p2);

  if (!read_tag(b, 2, TAG_TYPE_ID))
    return;
  read_crdt_id(b, &p1, &p2);

  if (!read_tag(b, 3, TAG_TYPE_ID))
    return;
  read_crdt_id(b, &p1, &p2);

  if (!read_tag(b, 4, TAG_TYPE_ID))
    return;
  read_crdt_id(b, &p1, &p2);

  if (!read_tag(b, 5, TAG_TYPE_BYTE4))
    return;
  read_uint32(b); // deleted_length

  if (check_tag(b, 6, TAG_TYPE_LENGTH4)) {
    read_tag(b, 6, TAG_TYPE_LENGTH4);
    uint32_t subblock_len = read_uint32(b);
    size_t subblock_start = b->pos;

    uint8_t item_type = read_uint8(b);
    if (item_type == 0x01) { // glyph item
      if (!read_tag(b, 4, TAG_TYPE_BYTE4))
        goto skip_subblock;
      uint32_t color_id = read_uint32(b);

      if (!read_tag(b, 5, TAG_TYPE_LENGTH4))
        goto skip_subblock;
      uint32_t rects_len = read_uint32(b);
      (void)rects_len;

      uint64_t num_rects = read_varuint(b);

      for (uint64_t i = 0; i < num_rects; i++) {
        double x = read_float64(b);
        double y = read_float64(b);
        double w = read_float64(b);
        double h = read_float64(b);

        remfmt_stroke st = {0};
        st.version = 6;
        st.pen = 18; // HIGHLIGHTER_2
        st.color = color_id;
        st.width = 1.0f;
        st.layer = 0;
        st.has_custom_color = false;
        st.custom_color = 0;

        remfmt_seg sg1 = {.x = (float)x,
                          .y = (float)(y + h / 2.0),
                          .speed = 1.0f,
                          .tilt = 0.0f,
                          .width = (float)h * 4.0f,
                          .pressure = 1.0f};
        kv_push(remfmt_seg, st.segments, sg1);

        remfmt_seg sg2 = {.x = (float)(x + w),
                          .y = (float)(y + h / 2.0),
                          .speed = 1.0f,
                          .tilt = 0.0f,
                          .width = (float)h * 4.0f,
                          .pressure = 1.0f};
        kv_push(remfmt_seg, st.segments, sg2);

        kv_push(remfmt_stroke, *strokes, st);
      }
    }
  skip_subblock:
    b->pos = subblock_start + subblock_len;
  }
}

static remfmt_stroke_vec *remfmt_parse_v6(rm_buf *b) {
  remfmt_stroke_vec *strokes = calloc(1, sizeof(remfmt_stroke_vec));
  if (strokes == NULL)
    return NULL;

  b->pos = 43;

  while (b->pos < b->size) {
    if (b->pos + 4 > b->size)
      break;
    uint32_t block_length = read_uint32(b);

    if (b->pos + 4 > b->size)
      break;
    uint8_t unknown = read_uint8(b);
    (void)unknown;
    uint8_t min_version = read_uint8(b);
    (void)min_version;
    uint8_t current_version = read_uint8(b);
    uint8_t block_type = read_uint8(b);

    size_t block_body_pos = b->pos;

    if (block_type == 0x05) { // BlockTypeSceneLineItem
      parse_scene_line_item(b, current_version, strokes, block_body_pos,
                            block_length);
    } else if (block_type == 0x03) { // BlockTypeSceneGlyphItem
      parse_scene_glyph_item(b, current_version, strokes, block_body_pos,
                             block_length);
    }

    b->pos = block_body_pos + block_length;
  }

  return strokes;
}

remfmt_stroke_vec *remfmt_parse(const char *path) {
  int fd = open(path, O_RDONLY);
  if (fd < 0)
    return NULL;

  struct stat st;
  if (fstat(fd, &st) < 0) {
    close(fd);
    return NULL;
  }

  size_t size = st.st_size;
  if (size < 43) {
    close(fd);
    return NULL;
  }

  const uint8_t *data = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
  close(fd);
  if (data == MAP_FAILED)
    return NULL;

  rm_buf b = {.data = data, .size = size, .pos = 0};

  char magic[64] = {0};
  memcpy(magic, b.data, 43);
  int version = 0, num_layers = 0;
  if (sscanf(magic, rmv_magic, &version) == 0) {
    munmap((void *)data, size);
    return NULL;
  }

  if (version == 6) {
    remfmt_stroke_vec *strokes = remfmt_parse_v6(&b);
    munmap((void *)data, size);
    return strokes;
  }

  if (version != 3 && version != 5) {
    munmap((void *)data, size);
    return NULL;
  }

  b.pos = 43;
  if (b.pos + 4 > b.size) {
    munmap((void *)data, size);
    return NULL;
  }
  struct_unpack(b.data + b.pos, "<I", &num_layers);
  b.pos += 4;

  if (num_layers < 1) {
    munmap((void *)data, size);
    return NULL;
  }

  remfmt_stroke_vec *strokes = calloc(1, sizeof(remfmt_stroke_vec));
  if (strokes == NULL) {
    munmap((void *)data, size);
    return NULL;
  }

  bool read_error = false;
  for (int l = 0; l < num_layers; l++) {
    if (read_error)
      break;
    int num_strokes = 0;

    if (b.pos + 4 > b.size)
      break;
    struct_unpack(b.data + b.pos, "<I", &num_strokes);
    b.pos += 4;

    for (int i = 0; i < num_strokes; i++) {
      int num_segments = 0;
      remfmt_stroke st = {0};
      st.version = version;
      st.has_custom_color = false;
      st.custom_color = 0;
      switch (version) {
      case 3:
        if (b.pos + 20 > b.size) {
          read_error = true;
          break;
        }
        struct_unpack(b.data + b.pos, "<IIffI", &st.pen, &st.color, &st.unk1,
                      &st.width, &num_segments);
        b.pos += 20;
        break;
      case 5:
        if (b.pos + 24 > b.size) {
          read_error = true;
          break;
        }
        struct_unpack(b.data + b.pos, "<IIfffI", &st.pen, &st.color, &st.unk1,
                      &st.width, &st.unk2, &num_segments);
        b.pos += 24;
        break;
      }
      if (read_error)
        break;
      st.layer = l;
      for (int j = 0; j < num_segments; j++) {
        remfmt_seg sg = {0};
        if (b.pos + 24 > b.size) {
          read_error = true;
          break;
        }
        struct_unpack(b.data + b.pos, "<ffffff", &sg.x, &sg.y, &sg.speed,
                      &sg.tilt, &sg.width, &sg.pressure);
        b.pos += 24;
        kv_push(remfmt_seg, st.segments, sg);
      }
      if (read_error) {
        if (kv_size(st.segments) > 0)
          kv_destroy(st.segments);
        break;
      }
      kv_push(remfmt_stroke, *strokes, st);
    }
  }

  munmap((void *)data, size);
  return strokes;
}
