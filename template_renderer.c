#include "template_renderer.h"
#include <cJSON.h>
#include <plutovg.h>
#include <png.h>
#include <string.h>
#include <unistd.h>
#include <zlib.h>

#define MAX_EVAL_VARS 256
typedef struct {
  char name[64];
  double value;
} eval_var;

typedef struct {
  eval_var vars[MAX_EVAL_VARS];
  int num_vars;
} eval_ctx;

static void eval_add_or_set_var(eval_ctx *ctx, const char *name, double value) {
  for (int i = 0; i < ctx->num_vars; i++) {
    if (strcmp(ctx->vars[i].name, name) == 0) {
      ctx->vars[i].value = value;
      return;
    }
  }
  if (ctx->num_vars >= MAX_EVAL_VARS)
    return;
  strncpy(ctx->vars[ctx->num_vars].name, name,
          sizeof(ctx->vars[ctx->num_vars].name) - 1);
  ctx->vars[ctx->num_vars].name[sizeof(ctx->vars[ctx->num_vars].name) - 1] =
      '\0';
  ctx->vars[ctx->num_vars].value = value;
  ctx->num_vars++;
}

static double eval_lookup(eval_ctx *ctx, const char *name, int len) {
  for (int i = 0; i < ctx->num_vars; i++) {
    if (strncmp(ctx->vars[i].name, name, len) == 0 &&
        ctx->vars[i].name[len] == '\0') {
      return ctx->vars[i].value;
    }
  }
  return 0.0;
}

static void skip_ws(const char **p) {
  while (**p == ' ' || **p == '\t' || **p == '\r' || **p == '\n') {
    (*p)++;
  }
}

static double parse_expr(const char **p, eval_ctx *ctx);

static double parse_primary(const char **p, eval_ctx *ctx) {
  skip_ws(p);
  if (**p == '(') {
    (*p)++; // consume '('
    double val = parse_expr(p, ctx);
    skip_ws(p);
    if (**p == ')') {
      (*p)++; // consume ')'
    }
    return val;
  }
  if (**p == '-' || **p == '+' || **p == '!') {
    char op = **p;
    (*p)++;
    double val = parse_primary(p, ctx);
    if (op == '-')
      return -val;
    if (op == '!')
      return (val == 0.0) ? 1.0 : 0.0;
    return val;
  }
  if ((**p >= '0' && **p <= '9') || **p == '.') {
    char *end;
    double val = strtod(*p, &end);
    *p = end;
    return val;
  }
  if ((**p >= 'a' && **p <= 'z') || (**p >= 'A' && **p <= 'Z') || **p == '_') {
    const char *start = *p;
    while ((**p >= 'a' && **p <= 'z') || (**p >= 'A' && **p <= 'Z') ||
           (**p >= '0' && **p <= '9') || **p == '_') {
      (*p)++;
    }
    int len = *p - start;
    return eval_lookup(ctx, start, len);
  }
  return 0.0;
}

static double parse_multiplicative(const char **p, eval_ctx *ctx) {
  double val = parse_primary(p, ctx);
  while (1) {
    skip_ws(p);
    if (**p == '*') {
      (*p)++;
      val *= parse_primary(p, ctx);
    } else if (**p == '/') {
      (*p)++;
      double denom = parse_primary(p, ctx);
      val = (denom != 0.0) ? (val / denom) : 0.0;
    } else {
      break;
    }
  }
  return val;
}

static double parse_additive(const char **p, eval_ctx *ctx) {
  double val = parse_multiplicative(p, ctx);
  while (1) {
    skip_ws(p);
    if (**p == '+') {
      (*p)++;
      val += parse_multiplicative(p, ctx);
    } else if (**p == '-') {
      (*p)++;
      val -= parse_multiplicative(p, ctx);
    } else {
      break;
    }
  }
  return val;
}

static double parse_comparison(const char **p, eval_ctx *ctx) {
  double val = parse_additive(p, ctx);
  while (1) {
    skip_ws(p);
    if (**p == '<') {
      if ((*p)[1] == '=') {
        *p += 2;
        val = (val <= parse_additive(p, ctx)) ? 1.0 : 0.0;
      } else {
        *p += 1;
        val = (val < parse_additive(p, ctx)) ? 1.0 : 0.0;
      }
    } else if (**p == '>') {
      if ((*p)[1] == '=') {
        *p += 2;
        val = (val >= parse_additive(p, ctx)) ? 1.0 : 0.0;
      } else {
        *p += 1;
        val = (val > parse_additive(p, ctx)) ? 1.0 : 0.0;
      }
    } else if (**p == '=' && (*p)[1] == '=') {
      *p += 2;
      val = (val == parse_additive(p, ctx)) ? 1.0 : 0.0;
    } else if (**p == '!' && (*p)[1] == '=') {
      *p += 2;
      val = (val != parse_additive(p, ctx)) ? 1.0 : 0.0;
    } else {
      break;
    }
  }
  return val;
}

static double parse_logical_and(const char **p, eval_ctx *ctx) {
  double val = parse_comparison(p, ctx);
  while (1) {
    skip_ws(p);
    if (**p == '&' && (*p)[1] == '&') {
      *p += 2;
      double right = parse_comparison(p, ctx);
      val = (val != 0.0 && right != 0.0) ? 1.0 : 0.0;
    } else {
      break;
    }
  }
  return val;
}

static double parse_logical_or(const char **p, eval_ctx *ctx) {
  double val = parse_logical_and(p, ctx);
  while (1) {
    skip_ws(p);
    if (**p == '|' && (*p)[1] == '|') {
      *p += 2;
      double right = parse_logical_and(p, ctx);
      val = (val != 0.0 || right != 0.0) ? 1.0 : 0.0;
    } else {
      break;
    }
  }
  return val;
}

static double parse_expr(const char **p, eval_ctx *ctx) {
  double val = parse_logical_or(p, ctx);
  skip_ws(p);
  if (**p == '?') {
    (*p)++;
    double true_val = parse_expr(p, ctx);
    skip_ws(p);
    if (**p == ':') {
      (*p)++;
      double false_val = parse_expr(p, ctx);
      val = (val != 0.0) ? true_val : false_val;
    }
  }
  return val;
}

static double eval_json_value(cJSON *item, eval_ctx *ctx, double default_val) {
  if (!item)
    return default_val;
  if (cJSON_IsNumber(item)) {
    return item->valuedouble;
  }
  if (cJSON_IsString(item)) {
    const char *expr = item->valuestring;
    const char *p = expr;
    double val = parse_expr(&p, ctx);
    return val;
  }
  return default_val;
}

static bool parse_hex_color(const char *hex, float *r, float *g, float *b,
                            float *a) {
  if (!hex || hex[0] != '#')
    return false;
  hex++;
  size_t len = strlen(hex);
  if (len == 3) {
    unsigned int ri, gi, bi;
    if (sscanf(hex, "%1x%1x%1x", &ri, &gi, &bi) == 3) {
      *r = (ri * 17) / 255.0f;
      *g = (gi * 17) / 255.0f;
      *b = (bi * 17) / 255.0f;
      *a = 1.0f;
      return true;
    }
  } else if (len == 6) {
    unsigned int ri, gi, bi;
    if (sscanf(hex, "%2x%2x%2x", &ri, &gi, &bi) == 3) {
      *r = ri / 255.0f;
      *g = gi / 255.0f;
      *b = bi / 255.0f;
      *a = 1.0f;
      return true;
    }
  } else if (len == 8) {
    unsigned int ri, gi, bi, ai;
    if (sscanf(hex, "%2x%2x%2x%2x", &ri, &gi, &bi, &ai) == 4) {
      *r = ri / 255.0f;
      *g = gi / 255.0f;
      *b = bi / 255.0f;
      *a = ai / 255.0f;
      return true;
    }
  }
  return false;
}

static plutovg_font_face_cache_t *g_font_cache = NULL;
static void init_font_cache(void) {
  if (!g_font_cache) {
    g_font_cache = plutovg_font_face_cache_create();
    if (g_font_cache) {
      plutovg_font_face_cache_load_sys(g_font_cache);
    }
  }
}

static void render_item(cJSON *item, plutovg_canvas_t *canvas, eval_ctx *ctx) {
  if (!item || !cJSON_IsObject(item))
    return;
  cJSON *type_item = cJSON_GetObjectItem(item, "type");
  if (!type_item || !cJSON_IsString(type_item))
    return;
  const char *type = type_item->valuestring;

  if (strcmp(type, "group") == 0) {
    cJSON *bb = cJSON_GetObjectItem(item, "boundingBox");
    double bx = 0, by = 0, bw = 0, bh = 0;
    if (bb) {
      bx = eval_json_value(cJSON_GetObjectItem(bb, "x"), ctx, 0.0);
      by = eval_json_value(cJSON_GetObjectItem(bb, "y"), ctx, 0.0);
      bw = eval_json_value(cJSON_GetObjectItem(bb, "width"), ctx, 0.0);
      bh = eval_json_value(cJSON_GetObjectItem(bb, "height"), ctx, 0.0);
    }

    eval_ctx child_ctx = *ctx;
    eval_add_or_set_var(&child_ctx, "parentWidth", bw);
    eval_add_or_set_var(&child_ctx, "parentHeight", bh);

    cJSON *repeat = cJSON_GetObjectItem(item, "repeat");
    cJSON *cols_item = repeat ? cJSON_GetObjectItem(repeat, "columns") : NULL;
    cJSON *rows_item = repeat ? cJSON_GetObjectItem(repeat, "rows") : NULL;

    int cols = 1;
    bool cols_infinite = false;
    if (cols_item) {
      if (cJSON_IsNumber(cols_item)) {
        cols = cols_item->valueint;
      } else if (cJSON_IsString(cols_item)) {
        const char *s = cols_item->valuestring;
        if (strcmp(s, "infinite") == 0 || strcmp(s, "down") == 0 ||
            strcmp(s, "right") == 0) {
          cols_infinite = true;
        } else {
          cols = (int)eval_json_value(cols_item, ctx, 1.0);
        }
      }
    }

    int rows = 1;
    bool rows_infinite = false;
    if (rows_item) {
      if (cJSON_IsNumber(rows_item)) {
        rows = rows_item->valueint;
      } else if (cJSON_IsString(rows_item)) {
        const char *s = rows_item->valuestring;
        if (strcmp(s, "infinite") == 0 || strcmp(s, "down") == 0 ||
            strcmp(s, "right") == 0) {
          rows_infinite = true;
        } else {
          rows = (int)eval_json_value(rows_item, ctx, 1.0);
        }
      }
    }

    int c_start = 0, c_end = cols;
    if (cols_infinite) {
      if (bw > 0) {
        double tpl_w = eval_lookup(ctx, "templateWidth", 13);
        c_start = (int)floor(-bx / bw);
        c_end = (int)ceil((tpl_w - bx) / bw);
      } else {
        c_start = 0;
        c_end = 1;
      }
    }

    int r_start = 0, r_end = rows;
    if (rows_infinite) {
      if (bh > 0) {
        double tpl_h = eval_lookup(ctx, "templateHeight", 14);
        r_start = (int)floor(-by / bh);
        r_end = (int)ceil((tpl_h - by) / bh);
      } else {
        r_start = 0;
        r_end = 1;
      }
    }

    cJSON *children = cJSON_GetObjectItem(item, "children");
    if (children && cJSON_IsArray(children)) {
      int child_count = cJSON_GetArraySize(children);
      for (int c = c_start; c < c_end; c++) {
        for (int r = r_start; r < r_end; r++) {
          plutovg_canvas_save(canvas);
          plutovg_canvas_translate(canvas, bx + c * bw, by + r * bh);
          for (int idx = 0; idx < child_count; idx++) {
            render_item(cJSON_GetArrayItem(children, idx), canvas, &child_ctx);
          }
          plutovg_canvas_restore(canvas);
        }
      }
    }
  } else if (strcmp(type, "path") == 0) {
    cJSON *data_arr = cJSON_GetObjectItem(item, "data");
    if (!data_arr || !cJSON_IsArray(data_arr))
      return;

    plutovg_canvas_new_path(canvas);

    int data_len = cJSON_GetArraySize(data_arr);
    int k = 0;
    while (k < data_len) {
      cJSON *cmd_item = cJSON_GetArrayItem(data_arr, k);
      if (cJSON_IsString(cmd_item)) {
        const char *cmd = cmd_item->valuestring;
        if (strcmp(cmd, "M") == 0) {
          if (k + 2 < data_len) {
            double x =
                eval_json_value(cJSON_GetArrayItem(data_arr, k + 1), ctx, 0.0);
            double y =
                eval_json_value(cJSON_GetArrayItem(data_arr, k + 2), ctx, 0.0);
            plutovg_canvas_move_to(canvas, x, y);
            k += 3;
          } else {
            break;
          }
        } else if (strcmp(cmd, "L") == 0) {
          if (k + 2 < data_len) {
            double x =
                eval_json_value(cJSON_GetArrayItem(data_arr, k + 1), ctx, 0.0);
            double y =
                eval_json_value(cJSON_GetArrayItem(data_arr, k + 2), ctx, 0.0);
            plutovg_canvas_line_to(canvas, x, y);
            k += 3;
          } else {
            break;
          }
        } else if (strcmp(cmd, "C") == 0) {
          if (k + 6 < data_len) {
            double x1 =
                eval_json_value(cJSON_GetArrayItem(data_arr, k + 1), ctx, 0.0);
            double y1 =
                eval_json_value(cJSON_GetArrayItem(data_arr, k + 2), ctx, 0.0);
            double x2 =
                eval_json_value(cJSON_GetArrayItem(data_arr, k + 3), ctx, 0.0);
            double y2 =
                eval_json_value(cJSON_GetArrayItem(data_arr, k + 4), ctx, 0.0);
            double x3 =
                eval_json_value(cJSON_GetArrayItem(data_arr, k + 5), ctx, 0.0);
            double y3 =
                eval_json_value(cJSON_GetArrayItem(data_arr, k + 6), ctx, 0.0);
            plutovg_canvas_cubic_to(canvas, x1, y1, x2, y2, x3, y3);
            k += 7;
          } else {
            break;
          }
        } else if (strcmp(cmd, "Q") == 0) {
          if (k + 4 < data_len) {
            double x1 =
                eval_json_value(cJSON_GetArrayItem(data_arr, k + 1), ctx, 0.0);
            double y1 =
                eval_json_value(cJSON_GetArrayItem(data_arr, k + 2), ctx, 0.0);
            double x2 =
                eval_json_value(cJSON_GetArrayItem(data_arr, k + 3), ctx, 0.0);
            double y2 =
                eval_json_value(cJSON_GetArrayItem(data_arr, k + 4), ctx, 0.0);
            plutovg_canvas_quad_to(canvas, x1, y1, x2, y2);
            k += 5;
          } else {
            break;
          }
        } else if (strcmp(cmd, "Z") == 0) {
          plutovg_canvas_close_path(canvas);
          k += 1;
        } else {
          k++;
        }
      } else {
        k++;
      }
    }

    cJSON *fill_color_item = cJSON_GetObjectItem(item, "fillColor");
    cJSON *stroke_color_item = cJSON_GetObjectItem(item, "strokeColor");
    cJSON *stroke_width_item = cJSON_GetObjectItem(item, "strokeWidth");

    bool has_fill = false;
    float fr = 0, fg = 0, fb = 0, fa = 0;
    if (fill_color_item && cJSON_IsString(fill_color_item)) {
      if (parse_hex_color(fill_color_item->valuestring, &fr, &fg, &fb, &fa)) {
        if (fa > 0)
          has_fill = true;
      }
    }

    bool has_stroke = false;
    float sr = 0, sg = 0, sb = 0, sa = 0;
    if (stroke_color_item && cJSON_IsString(stroke_color_item)) {
      if (parse_hex_color(stroke_color_item->valuestring, &sr, &sg, &sb, &sa)) {
        if (sa > 0)
          has_stroke = true;
      }
    } else if (!fill_color_item) {
      sr = 0.0f;
      sg = 0.0f;
      sb = 0.0f;
      sa = 1.0f;
      has_stroke = true;
    }

    double sw = eval_json_value(stroke_width_item, ctx, 1.0);

    if (has_fill) {
      plutovg_canvas_set_rgba(canvas, fr, fg, fb, fa);
      if (has_stroke) {
        plutovg_canvas_fill_preserve(canvas);
      } else {
        plutovg_canvas_fill(canvas);
      }
    }

    if (has_stroke) {
      plutovg_canvas_set_rgba(canvas, sr, sg, sb, sa);
      plutovg_canvas_set_line_width(canvas, sw);
      plutovg_canvas_stroke(canvas);
    }
  } else if (strcmp(type, "text") == 0) {
    cJSON *text_item = cJSON_GetObjectItem(item, "text");
    cJSON *fontSize_item = cJSON_GetObjectItem(item, "fontSize");
    cJSON *position = cJSON_GetObjectItem(item, "position");
    if (text_item && cJSON_IsString(text_item) && position &&
        cJSON_IsObject(position)) {
      double tx = eval_json_value(cJSON_GetObjectItem(position, "x"), ctx, 0.0);
      double ty = eval_json_value(cJSON_GetObjectItem(position, "y"), ctx, 0.0);
      double fs = eval_json_value(fontSize_item, ctx, 24.0);

      plutovg_font_face_t *face = NULL;
      if (g_font_cache) {
        const char *families[] = {"sans-serif",      "DejaVu Sans",
                                  "Liberation Sans", "Arial",
                                  "Helvetica",       "sans"};
        for (int i = 0; i < 6; i++) {
          face = plutovg_font_face_cache_get(g_font_cache, families[i], false,
                                             false);
          if (face)
            break;
        }
      }

      plutovg_canvas_save(canvas);
      if (face) {
        plutovg_canvas_set_font(canvas, face, fs);
      }
      plutovg_canvas_set_rgb(canvas, 0.0, 0.0, 0.0);
      plutovg_canvas_fill_text(canvas, text_item->valuestring, -1,
                               PLUTOVG_TEXT_ENCODING_UTF8, tx, ty);
      plutovg_canvas_restore(canvas);
    }
  }
}

static unsigned char *convert_argb_to_rgb(plutovg_surface_t *surface,
                                          int *out_w, int *out_h) {
  int w = plutovg_surface_get_width(surface);
  int h = plutovg_surface_get_height(surface);
  int stride = plutovg_surface_get_stride(surface);
  unsigned char *src = plutovg_surface_get_data(surface);

  unsigned char *dst = malloc((size_t)w * h * 3);
  if (!dst)
    return NULL;

  *out_w = w;
  *out_h = h;

  for (int y = 0; y < h; y++) {
    const uint32_t *src_row = (const uint32_t *)(src + stride * y);
    unsigned char *dst_row = dst + w * 3 * y;
    for (int x = 0; x < w; x++) {
      uint32_t pixel = src_row[x];
      uint32_t a = (pixel >> 24) & 0xFF;
      if (a == 0) {
        *dst_row++ = 255;
        *dst_row++ = 255;
        *dst_row++ = 255;
      } else {
        uint32_t r = (pixel >> 16) & 0xFF;
        uint32_t g = (pixel >> 8) & 0xFF;
        uint32_t b = (pixel >> 0) & 0xFF;
        if (a != 255) {
          r = (r * 255) / a;
          g = (g * 255) / a;
          b = (b * 255) / a;

          r = (r * a + 255 * (255 - a)) / 255;
          g = (g * a + 255 * (255 - a)) / 255;
          b = (b * a + 255 * (255 - a)) / 255;
        }
        *dst_row++ = (unsigned char)r;
        *dst_row++ = (unsigned char)g;
        *dst_row++ = (unsigned char)b;
      }
    }
  }
  return dst;
}

static unsigned char *render_json_template_to_rgb(const char *json_str,
                                                  int *out_w, int *out_h) {
  cJSON *json = cJSON_Parse(json_str);
  if (!json)
    return NULL;

  int width = DEV_W;
  int height = DEV_H;
  cJSON *orient_item = cJSON_GetObjectItem(json, "orientation");
  if (orient_item && cJSON_IsString(orient_item) &&
      strcmp(orient_item->valuestring, "landscape") == 0) {
    width = DEV_H;
    height = DEV_W;
  }

  eval_ctx ctx;
  ctx.num_vars = 0;
  eval_add_or_set_var(&ctx, "templateWidth", width);
  eval_add_or_set_var(&ctx, "templateHeight", height);
  eval_add_or_set_var(&ctx, "paperOriginX", width / 2.0);
  eval_add_or_set_var(&ctx, "paperOriginY", height / 2.0);
  eval_add_or_set_var(&ctx, "parentWidth", width);
  eval_add_or_set_var(&ctx, "parentHeight", height);

  cJSON *constants = cJSON_GetObjectItem(json, "constants");
  if (constants && cJSON_IsArray(constants)) {
    int count = cJSON_GetArraySize(constants);
    for (int i = 0; i < count; i++) {
      cJSON *c_item = cJSON_GetArrayItem(constants, i);
      if (cJSON_IsObject(c_item)) {
        cJSON *child = c_item->child;
        while (child) {
          double val = eval_json_value(child, &ctx, 0.0);
          eval_add_or_set_var(&ctx, child->string, val);
          child = child->next;
        }
      }
    }
  }

  plutovg_surface_t *surface = plutovg_surface_create(width, height);
  if (!surface) {
    cJSON_Delete(json);
    return NULL;
  }
  plutovg_canvas_t *canvas = plutovg_canvas_create(surface);
  if (!canvas) {
    plutovg_surface_destroy(surface);
    cJSON_Delete(json);
    return NULL;
  }

  init_font_cache();
  if (g_font_cache) {
    plutovg_canvas_set_font_face_cache(canvas, g_font_cache);
  }

  plutovg_canvas_set_rgb(canvas, 1.0, 1.0, 1.0);
  plutovg_canvas_paint(canvas);

  cJSON *items = cJSON_GetObjectItem(json, "items");
  if (items && cJSON_IsArray(items)) {
    int count = cJSON_GetArraySize(items);
    for (int i = 0; i < count; i++) {
      render_item(cJSON_GetArrayItem(items, i), canvas, &ctx);
    }
  }

  unsigned char *rgb_data = convert_argb_to_rgb(surface, out_w, out_h);

  plutovg_canvas_destroy(canvas);
  plutovg_surface_destroy(surface);
  cJSON_Delete(json);

  return rgb_data;
}

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

unsigned char *load_template_data(const char *template_dir,
                                  const char *template_name, int *w, int *h) {
  if (!template_dir || !template_name || template_name[0] == '\0') {
    return NULL;
  }

  sds json_path =
      sdscatprintf(sdsempty(), "%s/%s.template", template_dir, template_name);
  FILE *f = fopen(json_path, "r");
  sdsfree(json_path);
  if (f) {
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc(size + 1);
    if (buf) {
      size_t read_bytes = fread(buf, 1, size, f);
      buf[read_bytes] = '\0';
      fclose(f);

      unsigned char *rgb_data = render_json_template_to_rgb(buf, w, h);
      free(buf);
      if (rgb_data) {
        return rgb_data;
      }
    } else {
      fclose(f);
    }
  }

  sds png_path =
      sdscatprintf(sdsempty(), "%s/%s.png", template_dir, template_name);
  unsigned char *png_data = load_png_template(png_path, w, h);
  sdsfree(png_path);
  return png_data;
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

sds load_template_svg_background(const char *template_dir,
                                 const char *template_name) {
  sds b64_img = sdsempty();
  if (!template_dir || !template_name || template_name[0] == '\0') {
    return b64_img;
  }

  sds json_path =
      sdscatprintf(sdsempty(), "%s/%s.template", template_dir, template_name);
  FILE *json_f = fopen(json_path, "r");
  sdsfree(json_path);
  if (json_f) {
    fseek(json_f, 0, SEEK_END);
    long size = ftell(json_f);
    fseek(json_f, 0, SEEK_SET);
    char *buf = malloc(size + 1);
    if (buf) {
      size_t read_bytes = fread(buf, 1, size, json_f);
      buf[read_bytes] = '\0';
      fclose(json_f);

      cJSON *json = cJSON_Parse(buf);
      free(buf);
      if (json) {
        int width = DEV_W;
        int height = DEV_H;
        cJSON *orient_item = cJSON_GetObjectItem(json, "orientation");
        if (orient_item && cJSON_IsString(orient_item) &&
            strcmp(orient_item->valuestring, "landscape") == 0) {
          width = DEV_H;
          height = DEV_W;
        }

        eval_ctx ctx;
        ctx.num_vars = 0;
        eval_add_or_set_var(&ctx, "templateWidth", width);
        eval_add_or_set_var(&ctx, "templateHeight", height);
        eval_add_or_set_var(&ctx, "paperOriginX", width / 2.0);
        eval_add_or_set_var(&ctx, "paperOriginY", height / 2.0);
        eval_add_or_set_var(&ctx, "parentWidth", width);
        eval_add_or_set_var(&ctx, "parentHeight", height);

        cJSON *constants = cJSON_GetObjectItem(json, "constants");
        if (constants && cJSON_IsArray(constants)) {
          int count = cJSON_GetArraySize(constants);
          for (int i = 0; i < count; i++) {
            cJSON *c_item = cJSON_GetArrayItem(constants, i);
            if (cJSON_IsObject(c_item)) {
              cJSON *child = c_item->child;
              while (child) {
                double val = eval_json_value(child, &ctx, 0.0);
                eval_add_or_set_var(&ctx, child->string, val);
                child = child->next;
              }
            }
          }
        }

        plutovg_surface_t *surface = plutovg_surface_create(width, height);
        if (surface) {
          plutovg_canvas_t *canvas = plutovg_canvas_create(surface);
          if (canvas) {
            init_font_cache();
            if (g_font_cache) {
              plutovg_canvas_set_font_face_cache(canvas, g_font_cache);
            }
            plutovg_canvas_set_rgb(canvas, 1.0, 1.0, 1.0);
            plutovg_canvas_paint(canvas);

            cJSON *items = cJSON_GetObjectItem(json, "items");
            if (items && cJSON_IsArray(items)) {
              int count = cJSON_GetArraySize(items);
              for (int i = 0; i < count; i++) {
                render_item(cJSON_GetArrayItem(items, i), canvas, &ctx);
              }
            }

            char tmp_path[] = "/tmp/remfs_tpl_XXXXXX";
            int tmp_fd = mkstemp(tmp_path);
            if (tmp_fd != -1) {
              close(tmp_fd);
              if (plutovg_surface_write_to_png(surface, tmp_path)) {
                FILE *pf = fopen(tmp_path, "rb");
                if (pf) {
                  fseek(pf, 0, SEEK_END);
                  long psize = ftell(pf);
                  fseek(pf, 0, SEEK_SET);
                  unsigned char *pbuf = malloc(psize);
                  if (pbuf) {
                    if (fread(pbuf, 1, psize, pf) == psize) {
                      sds b64 = base64_encode(pbuf, psize);
                      b64_img = sdscatprintf(
                          b64_img,
                          "<image x=\"0\" y=\"0\" width=\"%d\" height=\"%d\" "
                          "href=\"data:image/png;base64,%s\"></image>\n",
                          width, height, b64);
                      sdsfree(b64);
                    }
                    free(pbuf);
                  }
                  fclose(pf);
                }
              }
              unlink(tmp_path);
            }
            plutovg_canvas_destroy(canvas);
          }
          plutovg_surface_destroy(surface);
        }
        cJSON_Delete(json);
      }
    }
  } else {
    sds tpl_path =
        sdscatprintf(sdsempty(), "%s/%s.png", template_dir, template_name);
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
  return b64_img;
}
