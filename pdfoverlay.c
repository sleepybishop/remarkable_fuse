#include "pdfoverlay.h"
#include "deps/sds/sds.h"
#include <errno.h>
#include <math.h>
#include <png.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

// Struct to track objects in the existing PDF
typedef struct {
  long offset;
  int gen;
  int in_object_stream;
  int stream_obj_id;
  int stream_index;
} PDFObject;

static PDFObject *objects = NULL;
static int objects_capacity = 0;
static int max_object_id = 0;
static int parse_depth = 0;

static void set_object_offset(int id, int gen, long offset) {
  if (id < 0)
    return;
  if (id >= objects_capacity) {
    int new_capacity = (id + 1) * 2;
    objects = realloc(objects, new_capacity * sizeof(PDFObject));
    memset(objects + objects_capacity, 0,
           (new_capacity - objects_capacity) * sizeof(PDFObject));
    objects_capacity = new_capacity;
  }
  // Only set if not already set by a newer xref table
  if (objects[id].offset == 0 && objects[id].in_object_stream == 0) {
    objects[id].offset = offset;
    objects[id].gen = gen;
    objects[id].in_object_stream = 0;
    if (id > max_object_id) {
      max_object_id = id;
    }
  }
}

static void set_object_in_stream(int id, int stream_obj_id, int index) {
  if (id < 0)
    return;
  if (id >= objects_capacity) {
    int new_capacity = (id + 1) * 2;
    objects = realloc(objects, new_capacity * sizeof(PDFObject));
    memset(objects + objects_capacity, 0,
           (new_capacity - objects_capacity) * sizeof(PDFObject));
    objects_capacity = new_capacity;
  }
  // Only set if not already set by a newer xref table
  if (objects[id].offset == 0 && objects[id].in_object_stream == 0) {
    objects[id].in_object_stream = 1;
    objects[id].stream_obj_id = stream_obj_id;
    objects[id].stream_index = index;
    if (id > max_object_id) {
      max_object_id = id;
    }
  }
}

static void cleanup_objects() {
  free(objects);
  objects = NULL;
  objects_capacity = 0;
  max_object_id = 0;
}

// Helper: read a file into memory
static unsigned char *read_file(const char *filename, long *out_len) {
  FILE *f = fopen(filename, "rb");
  if (!f)
    return NULL;
  fseek(f, 0, SEEK_END);
  long len = ftell(f);
  fseek(f, 0, SEEK_SET);
  if (len <= 0) {
    fclose(f);
    return NULL;
  }
  unsigned char *buf = malloc(len + 1);
  if (buf) {
    long read_bytes = fread(buf, 1, len, f);
    buf[read_bytes] = '\0';
    *out_len = read_bytes;
  }
  fclose(f);
  return buf;
}

// Find the start of a dictionary
static const char *find_dict_start(const char *obj_start, const char *obj_end) {
  const char *p = obj_start;
  while (p < obj_end) {
    if (p[0] == '<' && p[1] == '<') {
      return p;
    }
    p++;
  }
  return NULL;
}

// Find the matching end of a dictionary
static const char *find_dict_end(const char *dict_start, const char *obj_end) {
  int depth = 0;
  const char *p = dict_start;
  while (p < obj_end) {
    if (p[0] == '<' && p[1] == '<') {
      depth++;
      p += 2;
    } else if (p[0] == '>' && p[1] == '>') {
      depth--;
      p += 2;
      if (depth == 0) {
        return p;
      }
    } else {
      p++;
    }
  }
  return NULL;
}

// Find a top-level key in a dictionary (respects nested dictionaries)
static const char *find_key_in_dict(const char *dict_start,
                                    const char *dict_end, const char *key) {
  int key_len = strlen(key);
  int depth = 0;
  const char *p = dict_start;
  while (p < dict_end) {
    if (p[0] == '<' && p[1] == '<') {
      depth++;
      p += 2;
      continue;
    }
    if (p[0] == '>' && p[1] == '>') {
      depth--;
      p += 2;
      continue;
    }
    if (depth == 1) {
      if (p[0] == '/' && memcmp(p, key, key_len) == 0) {
        char next = p[key_len];
        if (next == ' ' || next == '/' || next == '[' || next == '<' ||
            next == '(' || next == '\r' || next == '\n' || next == '\t') {
          return p + key_len;
        }
      }
    }
    p++;
  }
  return NULL;
}

// Parse a reference: e.g. " 5 0 R"
static int parse_reference(const char *p, int *obj_id, int *obj_gen) {
  while (*p == ' ' || *p == '\r' || *p == '\n' || *p == '\t')
    p++;
  char *endptr;
  *obj_id = (int)strtol(p, &endptr, 10);
  if (endptr == p)
    return 0;
  p = endptr;
  while (*p == ' ' || *p == '\r' || *p == '\n' || *p == '\t')
    p++;
  *obj_gen = (int)strtol(p, &endptr, 10);
  if (endptr == p)
    return 0;
  p = endptr;
  while (*p == ' ' || *p == '\r' || *p == '\n' || *p == '\t')
    p++;
  if (*p == 'R') {
    return 1;
  }
  return 0;
}

// Find the last xref table offset from the end of the PDF
static long find_last_xref_offset(const unsigned char *buf, long len) {
  for (long i = len - 9; i >= 0; i--) {
    if (memcmp(buf + i, "startxref", 9) == 0) {
      long j = i + 9;
      while (j < len && (buf[j] == ' ' || buf[j] == '\r' || buf[j] == '\n')) {
        j++;
      }
      if (j < len) {
        return strtol((const char *)(buf + j), NULL, 10);
      }
    }
  }
  return -1;
}

// Uncompress Flate/zlib streams
static unsigned char *decompress_stream(const unsigned char *src, int src_len,
                                        int *dest_len) {
  int cap = src_len * 3 + 1024;
  unsigned char *dest = malloc(cap);
  while (1) {
    z_stream strm;
    memset(&strm, 0, sizeof(strm));
    strm.next_in = (Bytef *)src;
    strm.avail_in = src_len;
    strm.next_out = dest;
    strm.avail_out = cap;

    int ret = inflateInit(&strm);
    if (ret != Z_OK) {
      free(dest);
      return NULL;
    }

    ret = inflate(&strm, Z_FINISH);
    if (ret == Z_STREAM_END) {
      *dest_len = strm.total_out;
      inflateEnd(&strm);
      return dest;
    }

    inflateEnd(&strm);
    if (ret == Z_OK || ret == Z_BUF_ERROR) {
      cap *= 2;
      dest = realloc(dest, cap);
    } else {
      free(dest);
      return NULL;
    }
  }
}

// Find the binary stream data within an object
static const unsigned char *find_stream_data(const unsigned char *obj_data,
                                             const unsigned char *obj_end,
                                             int *out_stream_len) {
  const unsigned char *p = obj_data;
  while (p + 6 < obj_end) {
    if (memcmp(p, "stream", 6) == 0) {
      p += 6;
      if (*p == '\r')
        p++;
      if (*p == '\n')
        p++;
      // Find "endstream" to calculate length if needed
      const unsigned char *endstr = p;
      while (endstr + 9 < obj_end) {
        if (memcmp(endstr, "endstream", 9) == 0) {
          *out_stream_len = endstr - p;
          // Check if there is a trailing \r or \n before endstream
          if (*out_stream_len > 0 && p[*out_stream_len - 1] == '\n') {
            (*out_stream_len)--;
          }
          if (*out_stream_len > 0 && p[*out_stream_len - 1] == '\r') {
            (*out_stream_len)--;
          }
          return p;
        }
        endstr++;
      }
      return p;
    }
    p++;
  }
  return NULL;
}

// Forward declarations
static void parse_xref_at_offset(const unsigned char *file_buf, long file_len,
                                 long offset);
static const char *get_object_data(const unsigned char *file_buf, long file_len,
                                   int obj_id, int *out_len, char **to_free);

// Parse an integer object (e.g. for /Length)
static long parse_integer_object(const unsigned char *file_buf, long file_len,
                                 int obj_id) {
  if (obj_id <= 0 || obj_id > max_object_id || objects[obj_id].offset == 0) {
    return -1;
  }
  long offset = objects[obj_id].offset;
  const char *p = (const char *)(file_buf + offset);
  char *endptr;
  strtol(p, &endptr, 10);      // skip id
  strtol(endptr, &endptr, 10); // skip gen
  p = endptr;
  while (*p == ' ' || *p == '\r' || *p == '\n' || *p == '\t' || *p == 'o' ||
         *p == 'b' || *p == 'j')
    p++;
  return strtol(p, NULL, 10);
}

// Parse an xref stream object
static void parse_xref_stream(const unsigned char *file_buf, long file_len,
                              long offset) {
  const unsigned char *obj_start = file_buf + offset;
  const unsigned char *obj_end = file_buf + file_len;

  const char *dict_start =
      find_dict_start((const char *)obj_start, (const char *)obj_end);
  if (!dict_start)
    return;
  const char *dict_end = find_dict_end(dict_start, (const char *)obj_end);
  if (!dict_end)
    return;

  const char *size_ptr = find_key_in_dict(dict_start, dict_end, "/Size");
  int size = size_ptr ? atoi(size_ptr) : 0;

  const char *w_ptr = find_key_in_dict(dict_start, dict_end, "/W");
  int W[3] = {0};
  if (w_ptr) {
    while (*w_ptr != '[' && w_ptr < dict_end)
      w_ptr++;
    if (*w_ptr == '[') {
      w_ptr++;
      char *endptr;
      W[0] = strtol(w_ptr, &endptr, 10);
      w_ptr = endptr;
      W[1] = strtol(w_ptr, &endptr, 10);
      w_ptr = endptr;
      W[2] = strtol(w_ptr, &endptr, 10);
    }
  }

  const char *index_ptr = find_key_in_dict(dict_start, dict_end, "/Index");
  int *index_pairs = NULL;
  int index_count = 0;
  if (index_ptr) {
    while (*index_ptr != '[' && index_ptr < dict_end)
      index_ptr++;
    if (*index_ptr == '[') {
      index_ptr++;
      int cap = 4;
      index_pairs = malloc(cap * sizeof(int));
      while (index_ptr < dict_end) {
        while (*index_ptr == ' ' || *index_ptr == '\r' || *index_ptr == '\n' ||
               *index_ptr == '\t')
          index_ptr++;
        if (*index_ptr == ']')
          break;
        char *endptr;
        int val = strtol(index_ptr, &endptr, 10);
        if (endptr == index_ptr)
          break;
        index_ptr = endptr;
        if (index_count >= cap) {
          cap *= 2;
          int *new_pairs = realloc(index_pairs, cap * sizeof(int));
          if (!new_pairs) {
            free(index_pairs);
            return;
          }
          index_pairs = new_pairs;
        }
        index_pairs[index_count++] = val;
      }
    }
  }
  if (index_count == 0) {
    if (index_pairs)
      free(index_pairs);
    index_pairs = malloc(2 * sizeof(int));
    if (index_pairs) {
      index_pairs[0] = 0;
      index_pairs[1] = size;
      index_count = 2;
    }
  }

  const char *len_ptr = find_key_in_dict(dict_start, dict_end, "/Length");
  int length = 0;
  if (len_ptr) {
    int len_id, len_gen;
    if (parse_reference(len_ptr, &len_id, &len_gen)) {
      length = parse_integer_object(file_buf, file_len, len_id);
    } else {
      length = atoi(len_ptr);
    }
  }

  const char *prev_ptr = find_key_in_dict(dict_start, dict_end, "/Prev");
  long prev_offset = prev_ptr ? strtol(prev_ptr, NULL, 10) : -1;

  int stream_len = 0;
  const unsigned char *stream_data =
      find_stream_data(obj_start, obj_end, &stream_len);
  if (stream_data) {
    if (length <= 0)
      length = stream_len;
    int decomp_len = 0;
    unsigned char *decomp = decompress_stream(stream_data, length, &decomp_len);
    if (decomp) {
      int entry_size = W[0] + W[1] + W[2];
      unsigned char *p_decomp = decomp;

      for (int i = 0; i < index_count; i += 2) {
        int start_id = index_pairs[i];
        int count = index_pairs[i + 1];
        for (int j = 0; j < count; j++) {
          if (p_decomp + entry_size > decomp + decomp_len)
            break;

          int type = 1;
          if (W[0] > 0) {
            type = 0;
            for (int k = 0; k < W[0]; k++) {
              type = (type << 8) | p_decomp[k];
            }
          }

          long f2 = 0;
          for (int k = 0; k < W[1]; k++) {
            f2 = (f2 << 8) | p_decomp[W[0] + k];
          }

          int f3 = 0;
          for (int k = 0; k < W[2]; k++) {
            f3 = (f3 << 8) | p_decomp[W[0] + W[1] + k];
          }

          int obj_id = start_id + j;
          if (type == 1) {
            set_object_offset(obj_id, f3, f2);
          } else if (type == 2) {
            set_object_in_stream(obj_id, (int)f2, f3);
          }

          p_decomp += entry_size;
        }
      }
      free(decomp);
    }
  }

  free(index_pairs);
  if (prev_offset != -1) {
    parse_xref_at_offset(file_buf, file_len, prev_offset);
  }
}

// Parse traditional xref table
static const unsigned char *parse_xref_table(const unsigned char *buf, long len,
                                             const unsigned char *p) {
  p += 4; // skip "xref"
  while (p < buf + len) {
    while (p < buf + len && (*p == ' ' || *p == '\r' || *p == '\n')) {
      p++;
    }
    if (p >= buf + len)
      break;
    if (memcmp(p, "trailer", 7) == 0) {
      return p;
    }
    char *endptr;
    int start_id = strtol((const char *)p, &endptr, 10);
    p = (const unsigned char *)endptr;
    int count = strtol((const char *)p, &endptr, 10);
    p = (const unsigned char *)endptr;

    for (int i = 0; i < count; i++) {
      while (p < buf + len && (*p == ' ' || *p == '\r' || *p == '\n')) {
        p++;
      }
      if (p + 20 > buf + len)
        break;
      long offset = strtol((const char *)p, &endptr, 10);
      p = (const unsigned char *)endptr;
      int gen = strtol((const char *)p, &endptr, 10);
      p = (const unsigned char *)endptr;
      while (*p == ' ')
        p++;
      char type = *p;
      p++;
      while (p < buf + len && *p != '\n' && *p != '\r')
        p++;

      if (type == 'n') {
        set_object_offset(start_id + i, gen, offset);
      }
    }
  }
  return NULL;
}

static void parse_xref_at_offset(const unsigned char *file_buf, long file_len,
                                 long offset) {
  if (offset < 0 || offset >= file_len)
    return;
  if (parse_depth > 100) {
    fprintf(stderr, "Error: Max xref recursion depth exceeded.\n");
    return;
  }
  parse_depth++;

  const unsigned char *p = file_buf + offset;
  while (p < file_buf + file_len && (*p == ' ' || *p == '\r' || *p == '\n'))
    p++;
  if (p >= file_buf + file_len) {
    parse_depth--;
    return;
  }

  if (memcmp(p, "xref", 4) == 0) {
    const unsigned char *trailer_ptr = parse_xref_table(file_buf, file_len, p);
    if (trailer_ptr) {
      const char *dict_start = find_dict_start(
          (const char *)trailer_ptr, (const char *)(file_buf + file_len));
      if (dict_start) {
        const char *dict_end =
            find_dict_end(dict_start, (const char *)(file_buf + file_len));
        if (dict_end) {
          const char *prev_ptr =
              find_key_in_dict(dict_start, dict_end, "/Prev");
          if (prev_ptr) {
            long prev_offset = strtol(prev_ptr, NULL, 10);
            parse_xref_at_offset(file_buf, file_len, prev_offset);
          }
        }
      }
    }
  } else {
    parse_xref_stream(file_buf, file_len, offset);
  }
  parse_depth--;
}

// Retrieve an object's contents from file or object stream
static const char *get_object_data(const unsigned char *file_buf, long file_len,
                                   int obj_id, int *out_len, char **to_free) {
  *to_free = NULL;
  if (obj_id <= 0 || obj_id > max_object_id) {
    return NULL;
  }

  if (objects[obj_id].in_object_stream) {
    int stream_id = objects[obj_id].stream_obj_id;

    int stream_len = 0;
    char *stream_to_free = NULL;
    const char *stream_data = get_object_data(file_buf, file_len, stream_id,
                                              &stream_len, &stream_to_free);
    if (!stream_data) {
      return NULL;
    }

    const char *dict_start =
        find_dict_start(stream_data, stream_data + stream_len);
    if (!dict_start) {
      free(stream_to_free);
      return NULL;
    }
    const char *dict_end = find_dict_end(dict_start, stream_data + stream_len);
    if (!dict_end) {
      free(stream_to_free);
      return NULL;
    }

    const char *n_ptr = find_key_in_dict(dict_start, dict_end, "/N");
    int N = n_ptr ? atoi(n_ptr) : 0;
    const char *first_ptr = find_key_in_dict(dict_start, dict_end, "/First");
    int First = first_ptr ? atoi(first_ptr) : 0;

    int comp_stream_len = 0;
    const unsigned char *comp_stream = find_stream_data(
        (const unsigned char *)stream_data,
        (const unsigned char *)(stream_data + stream_len), &comp_stream_len);
    if (!comp_stream) {
      free(stream_to_free);
      return NULL;
    }

    const char *len_ptr = find_key_in_dict(dict_start, dict_end, "/Length");
    int length = 0;
    if (len_ptr) {
      int len_id, len_gen;
      if (parse_reference(len_ptr, &len_id, &len_gen)) {
        length = parse_integer_object(file_buf, file_len, len_id);
      } else {
        length = atoi(len_ptr);
      }
    }
    if (length <= 0)
      length = comp_stream_len;

    int decomp_len = 0;
    unsigned char *decomp = decompress_stream(comp_stream, length, &decomp_len);
    free(stream_to_free);
    if (!decomp) {
      return NULL;
    }

    const char *p = (const char *)decomp;
    int target_offset = -1;
    int next_offset = -1;
    for (int i = 0; i < N; i++) {
      while (p < (const char *)decomp + First &&
             (*p == ' ' || *p == '\r' || *p == '\n' || *p == '\t'))
        p++;
      char *endptr;
      int oid = strtol(p, &endptr, 10);
      p = endptr;
      while (p < (const char *)decomp + First &&
             (*p == ' ' || *p == '\r' || *p == '\n' || *p == '\t'))
        p++;
      int off = strtol(p, &endptr, 10);
      p = endptr;

      if (oid == obj_id) {
        target_offset = off;
        while (p < (const char *)decomp + First &&
               (*p == ' ' || *p == '\r' || *p == '\n' || *p == '\t'))
          p++;
        if (p < (const char *)decomp + First) {
          strtol(p, &endptr, 10);
          p = endptr;
          while (p < (const char *)decomp + First &&
                 (*p == ' ' || *p == '\r' || *p == '\n' || *p == '\t'))
            p++;
          next_offset = strtol(p, NULL, 10);
        } else {
          next_offset = decomp_len - First;
        }
        break;
      }
    }

    if (target_offset == -1) {
      free(decomp);
      return NULL;
    }

    *to_free = (char *)decomp;
    *out_len = next_offset - target_offset;
    return (const char *)decomp + First + target_offset;
  } else {
    long offset = objects[obj_id].offset;
    if (offset <= 0 || offset >= file_len) {
      return NULL;
    }
    const unsigned char *p = file_buf + offset;
    const unsigned char *end = file_buf + file_len;
    const unsigned char *endobj = p;
    while (endobj + 6 < end) {
      if (memcmp(endobj, "endobj", 6) == 0) {
        endobj += 6;
        break;
      }
      endobj++;
    }
    *out_len = endobj - p;
    return (const char *)p;
  }
}

// Get page catalog object ID
static int get_catalog_id(const unsigned char *file_buf, long file_len) {
  long xref_offset = find_last_xref_offset(file_buf, file_len);
  if (xref_offset == -1)
    return -1;

  parse_xref_at_offset(file_buf, file_len, xref_offset);

  const unsigned char *p = file_buf + xref_offset;
  while (p < file_buf + file_len && (*p == ' ' || *p == '\r' || *p == '\n'))
    p++;

  const char *dict_start = NULL;
  const char *dict_end = NULL;
  if (memcmp(p, "xref", 4) == 0) {
    const unsigned char *trailer_ptr = parse_xref_table(file_buf, file_len, p);
    if (trailer_ptr) {
      dict_start = find_dict_start((const char *)trailer_ptr,
                                   (const char *)(file_buf + file_len));
      if (dict_start) {
        dict_end =
            find_dict_end(dict_start, (const char *)(file_buf + file_len));
      }
    }
  } else {
    dict_start =
        find_dict_start((const char *)p, (const char *)(file_buf + file_len));
    if (dict_start) {
      dict_end = find_dict_end(dict_start, (const char *)(file_buf + file_len));
    }
  }

  if (!dict_start || !dict_end)
    return -1;

  const char *root_ptr = find_key_in_dict(dict_start, dict_end, "/Root");
  if (!root_ptr)
    return -1;

  int root_id = 0, root_gen = 0;
  if (parse_reference(root_ptr, &root_id, &root_gen)) {
    return root_id;
  }
  return -1;
}

// Recursive helper to traverse the Pages tree
static int find_page_object_rec(const unsigned char *file_buf, long file_len,
                                int pages_node_id, int target_page_idx,
                                int *current_page_idx) {
  int node_len = 0;
  char *node_to_free = NULL;
  const char *node_data = get_object_data(file_buf, file_len, pages_node_id,
                                          &node_len, &node_to_free);
  if (!node_data)
    return -1;

  const char *dict_start = find_dict_start(node_data, node_data + node_len);
  if (!dict_start) {
    free(node_to_free);
    return -1;
  }
  const char *dict_end = find_dict_end(dict_start, node_data + node_len);
  if (!dict_end) {
    free(node_to_free);
    return -1;
  }

  const char *type_ptr = find_key_in_dict(dict_start, dict_end, "/Type");
  if (type_ptr) {
    while (*type_ptr == ' ' || *type_ptr == '\r' || *type_ptr == '\n' ||
           *type_ptr == '\t')
      type_ptr++;
    if (memcmp(type_ptr, "/Page", 5) == 0 &&
        (type_ptr[5] == ' ' || type_ptr[5] == '/' || type_ptr[5] == '>' ||
         type_ptr[5] == '\r' || type_ptr[5] == '\n')) {
      if (*current_page_idx == target_page_idx) {
        free(node_to_free);
        return pages_node_id;
      }
      (*current_page_idx)++;
      free(node_to_free);
      return -1;
    }
  }

  const char *kids_ptr = find_key_in_dict(dict_start, dict_end, "/Kids");
  if (kids_ptr) {
    while (*kids_ptr != '[' && kids_ptr < dict_end)
      kids_ptr++;
    if (*kids_ptr == '[') {
      kids_ptr++;
      while (kids_ptr < dict_end) {
        while (*kids_ptr == ' ' || *kids_ptr == '\r' || *kids_ptr == '\n' ||
               *kids_ptr == '\t')
          kids_ptr++;
        if (*kids_ptr == ']')
          break;
        int child_id = 0, child_gen = 0;
        if (parse_reference(kids_ptr, &child_id, &child_gen)) {
          char *endptr;
          strtol(kids_ptr, &endptr, 10);
          kids_ptr = endptr;
          strtol(kids_ptr, &endptr, 10);
          kids_ptr = endptr;
          while (*kids_ptr == ' ' || *kids_ptr == '\r' || *kids_ptr == '\n' ||
                 *kids_ptr == '\t')
            kids_ptr++;
          if (*kids_ptr == 'R')
            kids_ptr++;

          int res = find_page_object_rec(file_buf, file_len, child_id,
                                         target_page_idx, current_page_idx);
          if (res != -1) {
            free(node_to_free);
            return res;
          }
        } else {
          break;
        }
      }
    }
  }

  free(node_to_free);
  return -1;
}

static int find_page_object(const unsigned char *file_buf, long file_len,
                            int catalog_id, int target_page_idx) {
  int cat_len = 0;
  char *cat_to_free = NULL;
  const char *cat_data =
      get_object_data(file_buf, file_len, catalog_id, &cat_len, &cat_to_free);
  if (!cat_data)
    return -1;

  const char *dict_start = find_dict_start(cat_data, cat_data + cat_len);
  if (!dict_start) {
    free(cat_to_free);
    return -1;
  }
  const char *dict_end = find_dict_end(dict_start, cat_data + cat_len);
  if (!dict_end) {
    free(cat_to_free);
    return -1;
  }

  const char *pages_ptr = find_key_in_dict(dict_start, dict_end, "/Pages");
  if (!pages_ptr) {
    free(cat_to_free);
    return -1;
  }

  int pages_id = 0, pages_gen = 0;
  if (!parse_reference(pages_ptr, &pages_id, &pages_gen)) {
    free(cat_to_free);
    return -1;
  }
  free(cat_to_free);

  int current_page_idx = 0;
  return find_page_object_rec(file_buf, file_len, pages_id, target_page_idx,
                              &current_page_idx);
}

// Get the MediaBox array for a page (climbs tree to parent if inherited)
static int get_page_mediabox(const unsigned char *file_buf, long file_len,
                             int page_id, double mediabox[4]) {
  mediabox[0] = 0;
  mediabox[1] = 0;
  mediabox[2] = 612;
  mediabox[3] = 792;

  int page_len = 0;
  char *page_to_free = NULL;
  const char *page_data =
      get_object_data(file_buf, file_len, page_id, &page_len, &page_to_free);
  if (!page_data)
    return 0;

  const char *dict_start = find_dict_start(page_data, page_data + page_len);
  if (!dict_start) {
    free(page_to_free);
    return 0;
  }
  const char *dict_end = find_dict_end(dict_start, page_data + page_len);
  if (!dict_end) {
    free(page_to_free);
    return 0;
  }

  const char *mb_ptr = find_key_in_dict(dict_start, dict_end, "/MediaBox");
  if (mb_ptr) {
    while (*mb_ptr != '[' && mb_ptr < dict_end)
      mb_ptr++;
    if (*mb_ptr == '[') {
      mb_ptr++;
      char *endptr;
      mediabox[0] = strtod(mb_ptr, &endptr);
      mb_ptr = endptr;
      mediabox[1] = strtod(mb_ptr, &endptr);
      mb_ptr = endptr;
      mediabox[2] = strtod(mb_ptr, &endptr);
      mb_ptr = endptr;
      mediabox[3] = strtod(mb_ptr, &endptr);
      free(page_to_free);
      return 1;
    }
  }

  const char *parent_ptr = find_key_in_dict(dict_start, dict_end, "/Parent");
  if (parent_ptr) {
    int parent_id = 0, parent_gen = 0;
    if (parse_reference(parent_ptr, &parent_id, &parent_gen)) {
      free(page_to_free);
      return get_page_mediabox(file_buf, file_len, parent_id, mediabox);
    }
  }

  free(page_to_free);
  return 0;
}

// Load PNG via libpng
static unsigned char *load_png(const char *filename, int *out_width,
                               int *out_height, int *out_has_alpha) {
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

  int width = png_get_image_width(png, info);
  int height = png_get_image_height(png, info);
  png_byte color_type = png_get_color_type(png, info);
  png_byte bit_depth = png_get_bit_depth(png, info);

  if (bit_depth == 16) {
    png_set_strip_16(png);
  }
  if (color_type == PNG_COLOR_TYPE_PALETTE) {
    png_set_palette_to_rgb(png);
  }
  if (color_type == PNG_COLOR_TYPE_GRAY && bit_depth < 8) {
    png_set_expand_gray_1_2_4_to_8(png);
  }
  if (png_get_valid(png, info, PNG_INFO_tRNS)) {
    png_set_tRNS_to_alpha(png);
  }
  if (color_type == PNG_COLOR_TYPE_GRAY ||
      color_type == PNG_COLOR_TYPE_GRAY_ALPHA) {
    png_set_gray_to_rgb(png);
  }

  png_read_update_info(png, info);

  int row_bytes = png_get_rowbytes(png, info);
  unsigned char *data = malloc(height * row_bytes);
  png_bytep *row_pointers = malloc(height * sizeof(png_bytep));
  for (int i = 0; i < height; i++) {
    row_pointers[i] = data + i * row_bytes;
  }

  png_read_image(png, row_pointers);

  free(row_pointers);
  png_destroy_read_struct(&png, &info, NULL);
  fclose(fp);

  *out_width = width;
  *out_height = height;
  *out_has_alpha = (color_type & PNG_COLOR_MASK_ALPHA) ||
                   png_get_valid(png, info, PNG_INFO_tRNS);
  return data;
}

// Compress data with zlib deflate
static unsigned char *compress_data(const unsigned char *src, int src_len,
                                    int *dest_len) {
  int cap = src_len + src_len / 100 + 20;
  unsigned char *dest = malloc(cap);
  z_stream strm;
  memset(&strm, 0, sizeof(strm));
  strm.next_in = (Bytef *)src;
  strm.avail_in = src_len;
  strm.next_out = dest;
  strm.avail_out = cap;

  int ret = deflateInit(&strm, Z_DEFAULT_COMPRESSION);
  if (ret != Z_OK) {
    free(dest);
    return NULL;
  }

  ret = deflate(&strm, Z_FINISH);
  if (ret == Z_STREAM_END) {
    *dest_len = strm.total_out;
    deflateEnd(&strm);
    return dest;
  }

  deflateEnd(&strm);
  free(dest);
  return NULL;
}

// Merge an XObject dictionary/reference with our new image /Im1
static sds merge_xobject_dict(const unsigned char *file_buf, long file_len,
                              const char *xobj_val, int img_obj_id) {
  int x_id = 0, x_gen = 0;
  const char *dict_start = NULL;
  const char *dict_end = NULL;
  char *to_free = NULL;

  if (parse_reference(xobj_val, &x_id, &x_gen)) {
    int len = 0;
    const char *data =
        get_object_data(file_buf, file_len, x_id, &len, &to_free);
    if (data) {
      dict_start = find_dict_start(data, data + len);
      if (dict_start) {
        dict_end = find_dict_end(dict_start, data + len);
      }
    }
  } else {
    dict_start = find_dict_start(xobj_val, xobj_val + 1000);
    if (dict_start) {
      dict_end = find_dict_end(dict_start, xobj_val + 5000);
    }
  }

  sds res = sdsempty();
  res = sdscat(res, "<< ");
  if (dict_start && dict_end) {
    const char *p = dict_start + 2;
    while (p < dict_end - 2) {
      res = sdscatlen(res, p, 1);
      p++;
    }
  }
  res = sdscatprintf(res, " /Im1 %d 0 R >>", img_obj_id);

  free(to_free);
  return res;
}

// Merge page resources dictionary to include /Im1
static sds merge_resources(const unsigned char *file_buf, long file_len,
                           const char *orig_res_val, int img_obj_id) {
  if (!orig_res_val) {
    sds res = sdsempty();
    res = sdscatprintf(res, "<< /XObject << /Im1 %d 0 R >> >>", img_obj_id);
    return res;
  }

  int res_id = 0, res_gen = 0;
  const char *dict_start = NULL;
  const char *dict_end = NULL;
  char *to_free = NULL;

  if (parse_reference(orig_res_val, &res_id, &res_gen)) {
    int len = 0;
    const char *data =
        get_object_data(file_buf, file_len, res_id, &len, &to_free);
    if (data) {
      dict_start = find_dict_start(data, data + len);
      if (dict_start) {
        dict_end = find_dict_end(dict_start, data + len);
      }
    }
  } else {
    dict_start = find_dict_start(orig_res_val, orig_res_val + 1000);
    if (dict_start) {
      dict_end = find_dict_end(dict_start, orig_res_val + 5000);
    }
  }

  sds res = sdsempty();
  res = sdscat(res, "<<\n");

  int has_xobject = 0;
  if (dict_start && dict_end) {
    const char *p = dict_start;
    int depth = 0;
    while (p < dict_end) {
      if (p[0] == '<' && p[1] == '<') {
        depth++;
        p += 2;
        continue;
      }
      if (p[0] == '>' && p[1] == '>') {
        depth--;
        p += 2;
        continue;
      }
      if (p[0] == '[') {
        depth++;
        p++;
        continue;
      }
      if (p[0] == ']') {
        depth--;
        p++;
        continue;
      }
      if (depth == 1) {
        if (p[0] == '/') {
          const char *key_start = p;
          p++; // Skip leading '/'
          while (p < dict_end && *p != ' ' && *p != '/' && *p != '[' &&
                 *p != '<' && *p != '(' && *p != '\r' && *p != '\n' &&
                 *p != '\t') {
            p++;
          }
          int key_len = p - key_start;
          sds key = sdsnewlen(key_start, key_len);

          while (p < dict_end &&
                 (*p == ' ' || *p == '\r' || *p == '\n' || *p == '\t'))
            p++;

          const char *val_start = p;
          int val_depth = 0;
          while (p < dict_end) {
            if (p[0] == '<' && p[1] == '<') {
              val_depth++;
              p += 2;
            } else if (p[0] == '>' && p[1] == '>') {
              if (val_depth == 0) {
                break;
              }
              val_depth--;
              p += 2;
            } else if (p[0] == '[') {
              val_depth++;
              p++;
            } else if (p[0] == ']') {
              if (val_depth == 0) {
                break;
              }
              val_depth--;
              p++;
            } else if (val_depth == 0 && p[0] == '/') {
              break;
            } else {
              p++;
            }
          }
          int val_len = p - val_start;
          sds val = sdsnewlen(val_start, val_len);

          if (strcmp(key, "/XObject") == 0) {
            has_xobject = 1;
            sds merged_x =
                merge_xobject_dict(file_buf, file_len, val, img_obj_id);
            res = sdscatprintf(res, "  /XObject %s\n", merged_x);
            sdsfree(merged_x);
          } else {
            res = sdscatprintf(res, "  %s %s\n", key, val);
          }
          sdsfree(key);
          sdsfree(val);
          continue;
        }
      }
      p++;
    }
  }

  if (!has_xobject) {
    res = sdscatprintf(res, "  /XObject << /Im1 %d 0 R >>\n", img_obj_id);
  }

  res = sdscat(res, ">>");
  free(to_free);
  return res;
}

// Append new content stream object ID to existing /Contents
static sds merge_contents(const char *orig_contents_val,
                          int new_content_obj_id) {
  sds res = sdsempty();
  if (!orig_contents_val) {
    res = sdscatprintf(res, "%d 0 R", new_content_obj_id);
    return res;
  }

  const char *p = orig_contents_val;
  while (*p == ' ' || *p == '\r' || *p == '\n' || *p == '\t')
    p++;
  if (*p == '[') {
    const char *end = strrchr(orig_contents_val, ']');
    if (end) {
      res = sdscatlen(res, orig_contents_val, end - orig_contents_val);
      res = sdscatprintf(res, " %d 0 R ]", new_content_obj_id);
    } else {
      res = sdscatprintf(res, "[ %s %d 0 R ]", orig_contents_val,
                         new_content_obj_id);
    }
  } else {
    res = sdscatprintf(res, "[ %s %d 0 R ]", orig_contents_val,
                       new_content_obj_id);
  }
  return res;
}

// Rebuild Page dictionary object to include updated resources and contents
static sds rebuild_page_object(const unsigned char *file_buf, long file_len,
                               int page_id, int img_obj_id,
                               int new_content_obj_id, int has_new_bounds,
                               double new_bounds[4]) {
  int page_len = 0;
  char *page_to_free = NULL;
  const char *page_data =
      get_object_data(file_buf, file_len, page_id, &page_len, &page_to_free);
  if (!page_data)
    return NULL;

  const char *dict_start = find_dict_start(page_data, page_data + page_len);
  if (!dict_start) {
    free(page_to_free);
    return NULL;
  }
  const char *dict_end = find_dict_end(dict_start, page_data + page_len);
  if (!dict_end) {
    free(page_to_free);
    return NULL;
  }

  const char *orig_res_ptr =
      find_key_in_dict(dict_start, dict_end, "/Resources");
  sds orig_res_val = NULL;
  if (orig_res_ptr) {
    const char *p = orig_res_ptr;
    while (*p == ' ' || *p == '\r' || *p == '\n' || *p == '\t')
      p++;
    const char *val_start = p;
    if (p[0] == '<' && p[1] == '<') {
      const char *end = find_dict_end(p, dict_end);
      if (end)
        p = end;
    } else {
      while (p < dict_end) {
        if (p[0] == '/' || (p[0] == '>' && p[1] == '>'))
          break;
        p++;
      }
    }
    orig_res_val = sdsnewlen(val_start, p - val_start);
  }

  const char *orig_contents_ptr =
      find_key_in_dict(dict_start, dict_end, "/Contents");
  sds orig_contents_val = NULL;
  if (orig_contents_ptr) {
    const char *p = orig_contents_ptr;
    while (*p == ' ' || *p == '\r' || *p == '\n' || *p == '\t')
      p++;
    const char *val_start = p;
    if (*p == '[') {
      while (p < dict_end && *p != ']')
        p++;
      if (p < dict_end)
        p++;
    } else {
      while (p < dict_end) {
        if (p[0] == '/' || (p[0] == '>' && p[1] == '>'))
          break;
        p++;
      }
    }
    orig_contents_val = sdsnewlen(val_start, p - val_start);
  }

  sds new_res = merge_resources(file_buf, file_len, orig_res_val, img_obj_id);
  sds new_contents = merge_contents(orig_contents_val, new_content_obj_id);

  sds page_obj = sdsempty();
  page_obj = sdscatprintf(page_obj, "%d 0 obj\n<<\n", page_id);

  const char *p = dict_start;
  int depth = 0;
  while (p < dict_end) {
    if (p[0] == '<' && p[1] == '<') {
      depth++;
      p += 2;
      continue;
    }
    if (p[0] == '>' && p[1] == '>') {
      depth--;
      p += 2;
      continue;
    }
    if (p[0] == '[') {
      depth++;
      p++;
      continue;
    }
    if (p[0] == ']') {
      depth--;
      p++;
      continue;
    }
    if (depth == 1) {
      if (p[0] == '/') {
        const char *key_start = p;
        p++; // Skip leading '/'
        while (p < dict_end && *p != ' ' && *p != '/' && *p != '[' &&
               *p != '<' && *p != '(' && *p != '\r' && *p != '\n' &&
               *p != '\t') {
          p++;
        }
        int key_len = p - key_start;
        sds key = sdsnewlen(key_start, key_len);

        while (p < dict_end &&
               (*p == ' ' || *p == '\r' || *p == '\n' || *p == '\t'))
          p++;

        const char *val_start = p;
        int val_depth = 0;
        while (p < dict_end) {
          if (p[0] == '<' && p[1] == '<') {
            val_depth++;
            p += 2;
          } else if (p[0] == '>' && p[1] == '>') {
            if (val_depth == 0) {
              break;
            }
            val_depth--;
            p += 2;
          } else if (p[0] == '[') {
            val_depth++;
            p++;
          } else if (p[0] == ']') {
            if (val_depth == 0) {
              break;
            }
            val_depth--;
            p++;
          } else if (val_depth == 0 && p[0] == '/') {
            break;
          } else {
            p++;
          }
        }
        int val_len = p - val_start;
        sds val = sdsnewlen(val_start, val_len);

        if (strcmp(key, "/Resources") == 0) {
          page_obj = sdscatprintf(page_obj, "  /Resources %s\n", new_res);
        } else if (strcmp(key, "/Contents") == 0) {
          page_obj = sdscatprintf(page_obj, "  /Contents %s\n", new_contents);
        } else if (has_new_bounds && (strcmp(key, "/MediaBox") == 0 ||
                                      strcmp(key, "/CropBox") == 0 ||
                                      strcmp(key, "/BleedBox") == 0 ||
                                      strcmp(key, "/TrimBox") == 0 ||
                                      strcmp(key, "/ArtBox") == 0)) {
          // Skip original bounds
        } else {
          page_obj = sdscatprintf(page_obj, "  %s %s\n", key, val);
        }

        sdsfree(key);
        sdsfree(val);
        continue;
      }
    }
    p++;
  }

  if (!orig_res_ptr) {
    page_obj = sdscatprintf(page_obj, "  /Resources %s\n", new_res);
  }
  if (!orig_contents_ptr) {
    page_obj = sdscatprintf(page_obj, "  /Contents %s\n", new_contents);
  }

  if (has_new_bounds) {
    page_obj = sdscatprintf(page_obj, "  /MediaBox [ %.4f %.4f %.4f %.4f ]\n",
                            new_bounds[0], new_bounds[1], new_bounds[2],
                            new_bounds[3]);
    page_obj = sdscatprintf(page_obj, "  /CropBox [ %.4f %.4f %.4f %.4f ]\n",
                            new_bounds[0], new_bounds[1], new_bounds[2],
                            new_bounds[3]);
  }

  page_obj = sdscat(page_obj, ">>\nendobj\n");

  sdsfree(orig_res_val);
  sdsfree(orig_contents_val);
  sdsfree(new_res);
  sdsfree(new_contents);
  free(page_to_free);
  return page_obj;
}

// Primary Library API
int pdf_overlay_png(const char *pdf_path, const char *png_path,
                    const char *out_pdf_path, int page_num, double x, double y,
                    double width, double height, int margins) {
  long pdf_len = 0;
  unsigned char *pdf_buf = read_file(pdf_path, &pdf_len);
  if (!pdf_buf) {
    fprintf(stderr, "Error: Could not read input PDF: %s\n", pdf_path);
    return -1;
  }

  int png_w = 0, png_h = 0, has_alpha = 0;
  unsigned char *png_raw = load_png(png_path, &png_w, &png_h, &has_alpha);
  if (!png_raw) {
    fprintf(stderr, "Error: Could not read PNG: %s\n", png_path);
    free(pdf_buf);
    return -1;
  }

  int catalog_id = get_catalog_id(pdf_buf, pdf_len);
  if (catalog_id == -1) {
    fprintf(stderr, "Error: Could not find catalog (/Root) in PDF.\n");
    free(pdf_buf);
    free(png_raw);
    cleanup_objects();
    return -1;
  }

  int target_page_id =
      find_page_object(pdf_buf, pdf_len, catalog_id, page_num - 1);
  if (target_page_id == -1) {
    fprintf(stderr, "Error: Could not find page %d in PDF.\n", page_num);
    free(pdf_buf);
    free(png_raw);
    cleanup_objects();
    return -1;
  }

  double mediabox[4];
  get_page_mediabox(pdf_buf, pdf_len, target_page_id, mediabox);

  double draw_x = x;
  double draw_y = y;
  double draw_w = width;
  double draw_h = height;
  if (draw_w <= 0 && draw_h <= 0) {
    double page_w = mediabox[2] - mediabox[0];
    double page_h = mediabox[3] - mediabox[1];

    double W_view = (double)png_w;
    double H_view = (double)png_h - margins;
    if (W_view < 100.0)
      W_view = (double)png_w;
    if (H_view < 100.0)
      H_view = (double)png_h;

    double scale = page_w / W_view;
    if (page_h / H_view > scale) {
      scale = page_h / H_view;
    }
    draw_w = (double)png_w * scale;
    draw_h = (double)png_h * scale;

    draw_x = mediabox[0] + (page_w - draw_w) / 2.0;

    // In Xochitl, the PDF is fit within the viewable area (W_view x H_view).
    // If the PDF is fit to height, it spans exactly from top margin to bottom.
    // So the top of the PDF (mediabox[3]) aligns with the top margin
    // (y=margins). This means the top of the PNG (y=0) is `margins * scale`
    // above the PDF.
    draw_y = mediabox[3] - draw_h + margins * scale;
  } else if (draw_w <= 0) {
    draw_w = draw_h * ((double)png_w / png_h);
  } else if (draw_h <= 0) {
    draw_h = draw_w * ((double)png_h / png_w);
  }

  unsigned char *rgb_buf = malloc(png_w * png_h * 3);
  unsigned char *alpha_buf = has_alpha ? malloc(png_w * png_h) : NULL;
  int stride = has_alpha ? 4 : 3;
  for (int i = 0; i < png_w * png_h; i++) {
    rgb_buf[i * 3 + 0] = png_raw[i * stride + 0];
    rgb_buf[i * 3 + 1] = png_raw[i * stride + 1];
    rgb_buf[i * 3 + 2] = png_raw[i * stride + 2];
    if (has_alpha) {
      alpha_buf[i] = png_raw[i * stride + 3];
    }
  }
  free(png_raw);

  int rgb_comp_len = 0;
  unsigned char *rgb_comp =
      compress_data(rgb_buf, png_w * png_h * 3, &rgb_comp_len);
  free(rgb_buf);

  int alpha_comp_len = 0;
  unsigned char *alpha_comp = NULL;
  if (has_alpha) {
    alpha_comp = compress_data(alpha_buf, png_w * png_h, &alpha_comp_len);
    free(alpha_buf);
  }

  int smask_obj_id = max_object_id + 1;
  int img_obj_id = has_alpha ? max_object_id + 2 : max_object_id + 1;
  int content_obj_id = img_obj_id + 1;
  int new_max_id = content_obj_id;

  FILE *out = fopen(out_pdf_path, "wb");
  if (!out) {
    fprintf(stderr, "Error: Could not open output file %s: %s\n", out_pdf_path,
            strerror(errno));
    free(pdf_buf);
    free(rgb_comp);
    free(alpha_comp);
    cleanup_objects();
    return -1;
  }

  fwrite(pdf_buf, 1, pdf_len, out);
  long current_offset = pdf_len;

  long smask_offset = 0;
  long img_offset = 0;
  long content_offset = 0;
  long page_offset = 0;

  if (has_alpha && alpha_comp) {
    smask_offset = current_offset;
    fprintf(out, "%d 0 obj\n", smask_obj_id);
    fprintf(out, "<< /Type /XObject /Subtype /Image /Width %d /Height %d\n",
            png_w, png_h);
    fprintf(out,
            "   /ColorSpace /DeviceGray /BitsPerComponent 8 /Filter "
            "/FlateDecode /Length %d >>\n",
            alpha_comp_len);
    fprintf(out, "stream\n");
    fwrite(alpha_comp, 1, alpha_comp_len, out);
    fprintf(out, "\nendstream\nendobj\n");
    current_offset = ftell(out);
    free(alpha_comp);
  }

  img_offset = current_offset;
  fprintf(out, "%d 0 obj\n", img_obj_id);
  fprintf(out, "<< /Type /XObject /Subtype /Image /Width %d /Height %d\n",
          png_w, png_h);
  fprintf(out,
          "   /ColorSpace /DeviceRGB /BitsPerComponent 8 /Filter /FlateDecode "
          "/Length %d\n",
          rgb_comp_len);
  if (has_alpha) {
    fprintf(out, "   /SMask %d 0 R\n", smask_obj_id);
  }
  fprintf(out, " >>\nstream\n");
  fwrite(rgb_comp, 1, rgb_comp_len, out);
  fprintf(out, "\nendstream\nendobj\n");
  current_offset = ftell(out);
  free(rgb_comp);

  sds draw_cmd = sdsempty();
  draw_cmd = sdscatprintf(draw_cmd, "q %.4f 0 0 %.4f %.4f %.4f cm /Im1 Do Q\n",
                          draw_w, draw_h, draw_x, draw_y);

  content_offset = current_offset;
  fprintf(out, "%d 0 obj\n", content_obj_id);
  fprintf(out, "<< /Length %ld >>\nstream\n%s\nendstream\nendobj\n",
          (long)sdslen(draw_cmd), draw_cmd);
  current_offset = ftell(out);
  sdsfree(draw_cmd);

  int has_new_bounds = 0; // (margins > 0);
  double new_bounds[4] = {draw_x, draw_y, draw_x + draw_w, draw_y + draw_h};
  sds page_dict =
      rebuild_page_object(pdf_buf, pdf_len, target_page_id, img_obj_id,
                          content_obj_id, has_new_bounds, new_bounds);
  if (!page_dict) {
    fprintf(stderr, "Error: Rebuilding page object failed.\n");
    fclose(out);
    free(pdf_buf);
    cleanup_objects();
    return -1;
  }

  page_offset = current_offset;
  fwrite(page_dict, 1, sdslen(page_dict), out);
  current_offset = ftell(out);
  sdsfree(page_dict);

  long new_xref_offset = current_offset;
  fprintf(out, "xref\n");
  fprintf(out, "%d 1\n", target_page_id);
  fprintf(out, "%010ld 00000 n \n", page_offset);

  if (has_alpha) {
    fprintf(out, "%d 3\n", smask_obj_id);
    fprintf(out, "%010ld 00000 n \n", smask_offset);
    fprintf(out, "%010ld 00000 n \n", img_offset);
    fprintf(out, "%010ld 00000 n \n", content_offset);
  } else {
    fprintf(out, "%d 2\n", img_obj_id);
    fprintf(out, "%010ld 00000 n \n", img_offset);
    fprintf(out, "%010ld 00000 n \n", content_offset);
  }

  long orig_xref_offset = find_last_xref_offset(pdf_buf, pdf_len);

  fprintf(out, "trailer\n");
  fprintf(out, "<< /Size %d\n", new_max_id + 1);
  fprintf(out, "   /Root %d 0 R\n", catalog_id);
  if (orig_xref_offset != -1) {
    fprintf(out, "   /Prev %ld\n", orig_xref_offset);
  }
  fprintf(out, ">>\n");
  fprintf(out, "startxref\n");
  fprintf(out, "%ld\n", new_xref_offset);
  fprintf(out, "%%%%EOF\n");

  fclose(out);
  free(pdf_buf);
  cleanup_objects();
  return 0;
}

#ifdef PDFOVERLAY_CLI
// Standalone CLI implementation
int main(int argc, char **argv) {
  if (argc < 4) {
    printf("PDFOverlay - Overlay a PNG image onto an existing PDF page.\n\n");
    printf("Usage: %s <input.pdf> <overlay.png> <output.pdf> [page_num] [x] "
           "[y] [width] [height]\n\n",
           argv[0]);
    printf("Arguments:\n");
    printf("  input.pdf   : Path to input PDF file\n");
    printf("  overlay.png : Path to PNG image to overlay\n");
    printf("  output.pdf  : Path to output PDF file\n");
    printf("  page_num    : 1-based page number to overlay on (default: 1)\n");
    printf("  x, y        : Coordinates on the PDF page in points (default: 0, "
           "0)\n");
    printf("  width       : Target width in points (default: fits aspect ratio "
           "/ page size)\n");
    printf("  height      : Target height in points (default: fits aspect "
           "ratio / page size)\n");
    return 1;
  }

  const char *input_pdf = argv[1];
  const char *overlay_png = argv[2];
  const char *output_pdf = argv[3];
  int page_num = (argc > 4) ? atoi(argv[4]) : 1;
  double x = (argc > 5) ? atof(argv[5]) : 0.0;
  double y = (argc > 6) ? atof(argv[6]) : 0.0;
  double width = (argc > 7) ? atof(argv[7]) : 0.0;
  double height = (argc > 8) ? atof(argv[8]) : 0.0;

  if (page_num < 1) {
    fprintf(stderr, "Error: page_num must be >= 1\n");
    return 1;
  }

  int ret = pdf_overlay_png(input_pdf, overlay_png, output_pdf, page_num, x, y,
                            width, height, 0);
  if (ret == 0) {
    printf("Successfully overlaid %s onto page %d of %s and saved to %s\n",
           overlay_png, page_num, input_pdf, output_pdf);
    return 0;
  } else {
    fprintf(stderr, "Error: Failed to overlay image.\n");
    return 1;
  }
}
#endif
