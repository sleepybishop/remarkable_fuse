#include "rm_parser.h"
#include "struct.h"
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#define TAG_TYPE_ID 0xF
#define TAG_TYPE_LENGTH4 0xC
#define TAG_TYPE_BYTE8 0x8
#define TAG_TYPE_BYTE4 0x4
#define TAG_TYPE_BYTE1 0x1

typedef struct {
  const uint8_t *data;
  size_t size;
  size_t pos;
} rm_buf;

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
