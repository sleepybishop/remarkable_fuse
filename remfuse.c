#ifndef FUSE_USE_VERSION
#define FUSE_USE_VERSION 26
#endif
#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <fuse.h>

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <zlib.h>

#include "cJSON.h"
#include "cache.h"
#include "deps/sds/sds.h"
#include "generators.h"
#include "path_utils.h"
#include "remfuse.h"

bool enable_svg = true;
bool enable_png = true;
bool enable_pdf = true;
bool enable_xoj = false;
bool enable_mutable = false;
bool enable_standalone_annotations = false;
char *template_dir = NULL;
char *data_dir = NULL;

pthread_mutex_t remfs_mutex = PTHREAD_MUTEX_INITIALIZER;

static int remfuse_getattr_internal(remfs_ctx *ctx, const char *path,
                                    struct stat *stbuf) {
  int ret = -ENOENT;
  memset(stbuf, 0, sizeof(struct stat));
  int flags = 0;
  size_t len = strlen(path);
  bool is_xoj = (len >= 4 && strcmp(path + len - 4, ".xoj") == 0) ||
                (len >= 5 && strcmp(path + len - 5, ".xopp") == 0);
  if (is_xoj) {
    sds parent_path = sdsempty();
    sds name = sdsempty();
    get_parent_and_name(path, &parent_path, &name);
    uuid_map_node *parent_node = NULL;
    if (strcmp(parent_path, "/") != 0) {
      parent_node = remfs_path_search(ctx, parent_path);
    }
    if (!parent_node || parent_node->file->type == COLLECTION) {
      const char *parent_uuid = parent_node ? parent_node->file->uuid : "";
      sds temp_path = sdscatprintf(sdsempty(), "%s/xojimport_%s_%s.tmp",
                                   ctx->src_dir, parent_uuid, name);
      struct stat tmp_st;
      if (stat(temp_path, &tmp_st) == 0) {
        stbuf->st_mode = S_IFREG | 0666;
        stbuf->st_nlink = 1;
        stbuf->st_size = tmp_st.st_size;
        stbuf->st_uid = getuid();
        stbuf->st_gid = getgid();
        stbuf->st_atime = tmp_st.st_atime;
        stbuf->st_mtime = tmp_st.st_mtime;
        stbuf->st_ctime = tmp_st.st_ctime;
        ret = 0;
      }
      sdsfree(temp_path);
    }
    sdsfree(parent_path);
    sdsfree(name);
  }

  if (ret == 0) {
    // Already found xoj tmp file
  } else if (strcmp(path, "/") == 0) {
    ret = stat(ctx->src_dir, stbuf);
  } else {
    sds newpath = sdsempty();
    uuid_map_node *ref = rewrite_path(ctx, path, &flags, &newpath);
    if (flags & (IS_SVG_DIR | IS_PNG_DIR | IS_PDF_DIR | IS_XOJ_DIR)) {
      bool allowed = false;
      if ((flags & IS_SVG_DIR) && enable_svg)
        allowed = true;
      if ((flags & IS_PNG_DIR) && enable_png)
        allowed = true;
      if ((flags & IS_PDF_DIR) && enable_pdf)
        allowed = true;
      if ((flags & IS_XOJ_DIR) && enable_xoj)
        allowed = true;
      if ((flags & IS_PDF_DIR) && !(flags & IS_ANNOT_DIR)) {
        allowed = false;
      }

      if (allowed && ref) {
        stbuf->st_mode = S_IFDIR | 0555;
        stbuf->st_nlink = 2;
        stbuf->st_uid = getuid();
        stbuf->st_gid = getgid();
        struct stat src_st;
        if (stat(ctx->src_dir, &src_st) == 0) {
          stbuf->st_atime = src_st.st_atime;
          stbuf->st_mtime = src_st.st_mtime;
          stbuf->st_ctime = src_st.st_ctime;
        }
        ret = 0;
      } else {
        ret = -ENOENT;
      }
    } else if (sdslen(newpath) > 0) {
      if (ref) {
        bool allowed = false;
        if ((flags & IS_SVG) && enable_svg)
          allowed = true;
        if ((flags & IS_PNG) && enable_png)
          allowed = true;
        if ((flags & IS_PDF) && enable_pdf)
          allowed = true;
        if ((flags & IS_XOJ) && enable_xoj)
          allowed = true;

        if ((flags & (IS_SVG | IS_PNG | IS_PDF | IS_XOJ)) && allowed) {
          if (ref->file->filetype == NOTEBOOK && (flags & IS_PDF)) {
            stbuf->st_mode = S_IFREG | 0444;
            stbuf->st_nlink = 1;
            sds meta_path = sdscatprintf(sdsempty(), "%s/%s.metadata",
                                         ctx->src_dir, ref->file->uuid);
            struct stat meta_st;
            time_t latest_mtime = 1;
            if (stat(meta_path, &meta_st) == 0) {
              latest_mtime = meta_st.st_mtime;
            }
            sdsfree(meta_path);

            cache_entry *cached =
                get_cached_entry(ref->file->uuid, "pdf", latest_mtime);
            if (cached) {
              stbuf->st_size = cached->size;
              stbuf->st_mtime = cached->mtime;
              release_cached_entry(cached);
            } else {
              stbuf->st_size = ref->file->page_count * 15 * 1024 * 1024;
              if (stbuf->st_size < 15 * 1024 * 1024) {
                stbuf->st_size = 15 * 1024 * 1024;
              }
              stbuf->st_mtime = latest_mtime;
            }
            ret = 0;
          } else {
            ret = stat(newpath, stbuf);
            if (ret == 0) {
              const char *type_str =
                  (flags & IS_SVG)
                      ? "svg"
                      : ((flags & IS_PDF) ? "pdf"
                                          : ((flags & IS_XOJ) ? "xoj" : "png"));
              if (ref->file->filetype == PDF && (flags & IS_PDF)) {
                if (flags & IS_ANNOTATED_PDF) {
                  cache_entry *entry =
                      generate_annotated_pdf(ctx, ref, newpath);
                  if (entry) {
                    stbuf->st_size = entry->size;
                    release_cached_entry(entry);
                  }
                }
              } else {
                time_t latest_mtime = stbuf->st_mtime;
                for (size_t i = 0; i < kv_size(ref->children); i++) {
                  uuid_map_node *page = kv_A(ref->children, i);
                  if (page && page->file->filetype == PAGE) {
                    sds rm_path =
                        sdscatprintf(sdsempty(), "%s/%s/%s.rm", ctx->src_dir,
                                     ref->file->uuid, page->file->uuid);
                    struct stat rm_st;
                    if (stat(rm_path, &rm_st) == 0) {
                      if (rm_st.st_mtime > latest_mtime) {
                        latest_mtime = rm_st.st_mtime;
                      }
                    }
                    sdsfree(rm_path);
                  }
                }
                cache_entry *cached =
                    get_cached_entry(ref->file->uuid, type_str, latest_mtime);
                if (cached) {
                  stbuf->st_size = cached->size;
                  release_cached_entry(cached);
                } else {
                  if (strcmp(type_str, "png") == 0 ||
                      strcmp(type_str, "pdf") == 0) {
                    stbuf->st_size = 15 * 1024 * 1024;
                  } else {
                    stbuf->st_size = 5 * 1024 * 1024;
                  }
                }
              }
            }
          }
        } else if ((flags & (IS_SVG | IS_PNG | IS_PDF | IS_XOJ)) && !allowed) {
          ret = -ENOENT;
        } else {
          ret = stat(newpath, stbuf);
        }
      }
    }
    sdsfree(newpath);
  }
  if (ret == 0) {
    if (enable_mutable &&
        !(flags & (IS_ANNOTATED_PDF | IS_SVG | IS_PNG | IS_PDF | IS_XOJ |
                   IS_SVG_DIR | IS_PNG_DIR | IS_PDF_DIR | IS_XOJ_DIR))) {
      stbuf->st_mode |= 0200;
    } else {
      stbuf->st_mode &= ~0200;
    }
    stbuf->st_mode |= 0400;
  }
  return ret;
}

static int remfuse_getattr(const char *path, struct stat *stbuf) {
  struct fuse_context *fuse_ctx = fuse_get_context();
  remfs_ctx *ctx = (remfs_ctx *)fuse_ctx->private_data;
  pthread_mutex_lock(&remfs_mutex);
  int ret = remfuse_getattr_internal(ctx, path, stbuf);
  pthread_mutex_unlock(&remfs_mutex);
  return ret;
}

#if FUSE_USE_VERSION >= 30
static int my_fill_dir(remfs_ctx *ctx, const char *parent_path, void *buf,
                       fuse_fill_dir_t filler, const char *name,
                       enum fuse_readdir_flags flags) {
  if (flags & FUSE_READDIR_PLUS) {
    struct stat st;
    sds full_path;
    if (strcmp(parent_path, "/") == 0) {
      full_path = sdscatprintf(sdsempty(), "/%s", name);
    } else {
      full_path = sdscatprintf(sdsempty(), "%s/%s", parent_path, name);
    }
    if (remfuse_getattr_internal(ctx, full_path, &st) == 0) {
      sdsfree(full_path);
      return filler(buf, name, &st, 0, FUSE_FILL_DIR_PLUS);
    }
    sdsfree(full_path);
  }
  return filler(buf, name, NULL, 0, 0);
}
#define DO_FILL_DIR(name)                                                      \
  my_fill_dir(ctx, path, buf, filler, name, readdir_flags)
#else
#define DO_FILL_DIR(name) filler(buf, name, NULL, 0)
#endif

#define FILL_FAKE_EXT(file, ext_str)                                           \
  do {                                                                         \
    sds tmp = sdsnew((file)->visible_name);                                    \
    tmp = sdscat(tmp, ext_str);                                                \
    DO_FILL_DIR(tmp);                                                          \
    sdsfree(tmp);                                                              \
  } while (0)

#define FILL_FAKE_FOLDER(file)                                                 \
  do {                                                                         \
    sds tmp = sdsempty();                                                      \
    tmp = sdscatprintf(tmp, "%s Annotations", (file)->visible_name);           \
    DO_FILL_DIR(tmp);                                                          \
    sdsfree(tmp);                                                              \
  } while (0)

#if FUSE_USE_VERSION >= 30
static int remfuse_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
                           off_t offset, struct fuse_file_info *fi,
                           enum fuse_readdir_flags readdir_flags) {
  (void)readdir_flags;
#else
static int remfuse_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
                           off_t offset, struct fuse_file_info *fi) {
#endif
  struct fuse_context *fuse_ctx = fuse_get_context();
  remfs_ctx *ctx = (remfs_ctx *)fuse_ctx->private_data;

  pthread_mutex_lock(&remfs_mutex);
  children_vec *n = NULL;
  int flags = 0;
  if (strcmp(path, "/") == 0) {
    n = &ctx->root_children;
  } else {
    uuid_map_node *ref = rewrite_path(ctx, path, &flags, NULL);
    if (ref)
      n = &ref->children;
  }

  if (!n) {
    pthread_mutex_unlock(&remfs_mutex);
    return -ENOENT;
  }

  DO_FILL_DIR(".");
  DO_FILL_DIR("..");

  bool is_notebook_dir = false;
  bool is_annot_root_dir = false;
  if (strcmp(path, "/") != 0) {
    uuid_map_node *ref = rewrite_path(ctx, path, &flags, NULL);
    if (ref) {
      if (ref->file->filetype == NOTEBOOK &&
          !(flags & (IS_SVG_DIR | IS_PNG_DIR | IS_PDF_DIR | IS_XOJ_DIR))) {
        is_notebook_dir = true;
      }
      if ((flags & IS_ANNOT_DIR) &&
          !(flags & (IS_SVG_DIR | IS_PNG_DIR | IS_PDF_DIR | IS_XOJ_DIR))) {
        is_annot_root_dir = true;
      }
    }
  }

  if (is_notebook_dir) {
    if (enable_svg)
      DO_FILL_DIR("svg");
    if (enable_png)
      DO_FILL_DIR("png");
    if (enable_xoj)
      DO_FILL_DIR("xoj");
  }
  if (is_annot_root_dir) {
    if (enable_svg)
      DO_FILL_DIR("svg");
    if (enable_png)
      DO_FILL_DIR("png");
    if (enable_pdf)
      DO_FILL_DIR("pdf");
  }

  for (size_t i = 0; i < kv_size(*n); i++) {
    uuid_map_node *s = kv_A(*n, i);
    if (!s)
      continue;

    if ((flags & (IS_SVG_DIR | IS_PNG_DIR | IS_PDF_DIR | IS_XOJ_DIR)) &&
        s->file->filetype != PAGE) {
      continue;
    }

    if (s->file->filetype == PAGE) {
      if (flags & IS_ANNOT_DIR) {
        if (!has_annotations(ctx->src_dir, s->file)) {
          continue;
        }
      }
      if (flags & IS_SVG_DIR) {
        if (enable_svg) {
          FILL_FAKE_EXT(s->file, ".svg");
        }
      } else if (flags & IS_PNG_DIR) {
        if (enable_png) {
          FILL_FAKE_EXT(s->file, ".png");
        }
      } else if (flags & IS_PDF_DIR) {
        if (enable_pdf) {
          FILL_FAKE_EXT(s->file, ".pdf");
        }
      } else if (flags & IS_XOJ_DIR) {
        if (enable_xoj) {
          FILL_FAKE_EXT(s->file, ".xoj");
        }
      } else {
        if (!(flags & IS_ANNOT_DIR)) {
          if (enable_mutable) {
            FILL_FAKE_EXT(s->file, ".rm");
          }
        }
      }
    } else if (s->file->filetype == PDF || s->file->filetype == EPUB) {
      if (enable_standalone_annotations) {
        FILL_FAKE_FOLDER(s->file);
      }
      sds tmp = sdsnew(s->file->visible_name);
      tmp = sdscat(tmp, s->file->filetype == PDF ? ".pdf" : ".epub");
      DO_FILL_DIR(tmp);
      sdsfree(tmp);
      if (s->file->filetype == PDF) {
        sds tmp_annot = sdsnew(s->file->visible_name);
        tmp_annot = sdscat(tmp_annot, ".annotated.pdf");
        DO_FILL_DIR(tmp_annot);
        sdsfree(tmp_annot);
      }
    } else {
      bool show_folder = true;
      if (s->file->filetype == NOTEBOOK) {
        if (!enable_png && !enable_svg && !enable_mutable && !enable_xoj) {
          show_folder = false;
        }
      }
      if (show_folder) {
        DO_FILL_DIR(s->file->visible_name);
      }
      if (s->file->filetype == NOTEBOOK && enable_pdf) {
        FILL_FAKE_EXT(s->file, ".pdf");
      }
    }
  }
  pthread_mutex_unlock(&remfs_mutex);
  return 0;
}

#define CACHE_PTR_FLAG (1ULL << 63)
#define MAKE_CACHE_PTR(ptr) ((uint64_t)(uintptr_t)(ptr) | CACHE_PTR_FLAG)
#define GET_CACHE_PTR(fh) ((cache_entry *)(uintptr_t)((fh) & ~CACHE_PTR_FLAG))
#define IS_CACHE_PTR(fh) (((fh) & CACHE_PTR_FLAG) != 0)

static int remfuse_open(const char *path, struct fuse_file_info *fi) {
  struct fuse_context *fuse_ctx = fuse_get_context();
  remfs_ctx *ctx = (remfs_ctx *)fuse_ctx->private_data;

  pthread_mutex_lock(&remfs_mutex);
  int flags = 0;
  sds newpath = sdsempty();
  uuid_map_node *ref = rewrite_path(ctx, path, &flags, &newpath);

  if ((fi->flags & (O_WRONLY | O_RDWR)) &&
      (!enable_mutable ||
       (flags & (IS_ANNOTATED_PDF | IS_SVG | IS_PNG | IS_PDF | IS_XOJ)))) {
    sdsfree(newpath);
    pthread_mutex_unlock(&remfs_mutex);
    return -EROFS;
  }

  if (flags & (IS_SVG | IS_PNG | IS_PDF | IS_XOJ)) {
    bool allowed = false;
    if ((flags & IS_SVG) && enable_svg)
      allowed = true;
    if ((flags & IS_PNG) && enable_png)
      allowed = true;
    if ((flags & IS_PDF) && enable_pdf)
      allowed = true;
    if ((flags & IS_XOJ) && enable_xoj)
      allowed = true;
    if (!allowed) {
      sdsfree(newpath);
      pthread_mutex_unlock(&remfs_mutex);
      return -ENOENT;
    }
  }

  if (ref && (flags & IS_SVG) && enable_svg) {
    cache_entry *entry =
        generate_fake_ext(ref, newpath, flags & IS_ANNOT_PAGE, "svg");
    sdsfree(newpath);
    if (!entry) {
      pthread_mutex_unlock(&remfs_mutex);
      return -ENOENT;
    }
    fi->fh = MAKE_CACHE_PTR(entry);
    pthread_mutex_unlock(&remfs_mutex);
    return 0;
  } else if (ref && (flags & IS_PDF) && enable_pdf) {
    cache_entry *entry = NULL;
    if (flags & IS_ANNOTATED_PDF) {
      entry = generate_annotated_pdf(ctx, ref, newpath);
      if (entry) {
        sdsfree(newpath);
        fi->fh = MAKE_CACHE_PTR(entry);
        pthread_mutex_unlock(&remfs_mutex);
        return 0;
      }
      sdsfree(newpath);
      pthread_mutex_unlock(&remfs_mutex);
      return -ENOENT;
    } else {
      if (ref->file->filetype == PDF) {
        int fd = open(newpath, fi->flags);
        sdsfree(newpath);
        if (fd == -1) {
          pthread_mutex_unlock(&remfs_mutex);
          return -errno;
        }
        fi->fh = fd;
        pthread_mutex_unlock(&remfs_mutex);
        return 0;
      } else if (ref->file->filetype == NOTEBOOK) {
        entry = generate_notebook_pdf(ctx, ref);
      } else {
        entry = generate_fake_ext(ref, newpath, flags & IS_ANNOT_PAGE, "pdf");
      }
      if (entry) {
        sdsfree(newpath);
        fi->fh = MAKE_CACHE_PTR(entry);
        pthread_mutex_unlock(&remfs_mutex);
        return 0;
      }
      sdsfree(newpath);
      pthread_mutex_unlock(&remfs_mutex);
      return -ENOENT;
    }
  } else if (ref && (flags & IS_XOJ) && enable_xoj) {
    cache_entry *entry = generate_fake_ext(ref, newpath, false, "xoj");
    if (entry) {
      fi->fh = MAKE_CACHE_PTR(entry);
      sdsfree(newpath);
      pthread_mutex_unlock(&remfs_mutex);
      return 0;
    }
    sdsfree(newpath);
    pthread_mutex_unlock(&remfs_mutex);
    return -ENOENT;
  } else if (ref && (flags & IS_PNG) && enable_png) {
    cache_entry *entry =
        generate_fake_ext(ref, newpath, flags & IS_ANNOT_PAGE, "png");
    sdsfree(newpath);
    if (!entry) {
      pthread_mutex_unlock(&remfs_mutex);
      return -ENOENT;
    }
    fi->fh = MAKE_CACHE_PTR(entry);
    pthread_mutex_unlock(&remfs_mutex);
    return 0;
  } else {
    if (sdslen(newpath) == 0) {
      sdsfree(newpath);
      pthread_mutex_unlock(&remfs_mutex);
      return -1;
    }
    int fd = open(newpath, fi->flags);
    sdsfree(newpath);
    if (fd == -1) {
      pthread_mutex_unlock(&remfs_mutex);
      return -errno;
    }
    fi->fh = fd;
    pthread_mutex_unlock(&remfs_mutex);
    return 0;
  }
}

static sds sds_append_varuint(sds s, uint32_t val) {
  uint8_t buf[10];
  int len = 0;
  while (1) {
    uint8_t b = val & 0x7F;
    val >>= 7;
    if (val > 0) {
      buf[len++] = b | 0x80;
    } else {
      buf[len++] = b;
      break;
    }
  }
  return sdscatlen(s, buf, len);
}

static sds sds_append_crdt_id(sds s, uint8_t p1, uint32_t p2) {
  s = sdscatlen(s, &p1, 1);
  return sds_append_varuint(s, p2);
}

static sds sds_append_tag(sds s, uint32_t index, uint8_t typ) {
  uint32_t val = (index << 4) | typ;
  return sds_append_varuint(s, val);
}

typedef struct {
  float x;
  float y;
} xoj_point;

typedef struct {
  float width;
  xoj_point *points;
  int num_points;
} xoj_stroke;

typedef struct {
  xoj_stroke *strokes;
  int num_strokes;
} xoj_page;

static sds generate_rm_data(xoj_stroke *strokes, int num_strokes) {
  sds out = sdsnewlen("reMarkable .lines file, version=6          ", 43);
  uint32_t crdt_seq = 1;

  for (int i = 0; i < num_strokes; i++) {
    xoj_stroke *st = &strokes[i];
    sds block_body = sdsempty();

    for (int tag = 1; tag <= 4; tag++) {
      block_body = sds_append_tag(block_body, tag, 0xF);
      block_body = sds_append_crdt_id(block_body, 0, crdt_seq++);
    }

    block_body = sds_append_tag(block_body, 5, 0x4);
    uint32_t val_zero = 0;
    block_body = sdscatlen(block_body, &val_zero, 4);

    sds subblock = sdsempty();
    uint8_t item_type = 0x03; // line item
    subblock = sdscatlen(subblock, &item_type, 1);

    uint32_t tool_id = 1; // Pen
    subblock = sds_append_tag(subblock, 1, 0x4);
    subblock = sdscatlen(subblock, &tool_id, 4);

    uint32_t color_id = 0; // Black
    subblock = sds_append_tag(subblock, 2, 0x4);
    subblock = sdscatlen(subblock, &color_id, 4);

    double stroke_width = (double)st->width;
    subblock = sds_append_tag(subblock, 3, 0x8);
    subblock = sdscatlen(subblock, &stroke_width, 8);

    float val_float_zero = 0.0f;
    subblock = sds_append_tag(subblock, 4, 0x4);
    subblock = sdscatlen(subblock, &val_float_zero, 4);

    sds points_data = sdsempty();
    for (int p = 0; p < st->num_points; p++) {
      float pt_x = st->points[p].x;
      float pt_y = st->points[p].y;
      uint16_t speed = 0;
      uint16_t direction = 4;
      uint8_t width = 0;
      uint8_t pressure = 127;
      points_data = sdscatlen(points_data, &pt_x, 4);
      points_data = sdscatlen(points_data, &pt_y, 4);
      points_data = sdscatlen(points_data, &speed, 2);
      points_data = sdscatlen(points_data, &direction, 2);
      points_data = sdscatlen(points_data, &width, 1);
      points_data = sdscatlen(points_data, &pressure, 1);
    }

    subblock = sds_append_tag(subblock, 5, 0xC);
    uint32_t points_len = (uint32_t)sdslen(points_data);
    subblock = sdscatlen(subblock, &points_len, 4);
    subblock = sdscatsds(subblock, points_data);
    sdsfree(points_data);

    subblock = sds_append_tag(subblock, 6, 0xF);
    subblock = sds_append_crdt_id(subblock, 0, crdt_seq++);

    block_body = sds_append_tag(block_body, 6, 0xC);
    uint32_t subblock_len = (uint32_t)sdslen(subblock);
    block_body = sdscatlen(block_body, &subblock_len, 4);
    block_body = sdscatsds(block_body, subblock);
    sdsfree(subblock);

    uint32_t body_len = (uint32_t)sdslen(block_body);
    uint8_t hdr_bytes[4] = {0, 2, 2, 0x05};
    out = sdscatlen(out, &body_len, 4);
    out = sdscatlen(out, hdr_bytes, 4);
    out = sdscatsds(out, block_body);
    sdsfree(block_body);
  }

  return out;
}

static char *get_attr_value(const char *tag, const char *attr_name) {
  char search[128];
  snprintf(search, sizeof(search), "%s=\"", attr_name);
  char *p = strstr(tag, search);
  if (!p) {
    snprintf(search, sizeof(search), "%s='", attr_name);
    p = strstr(tag, search);
  }
  if (!p)
    return NULL;
  p += strlen(search);
  char *end = strchr(p, p[-1]);
  if (!end)
    return NULL;
  size_t len = end - p;
  char *val = malloc(len + 1);
  memcpy(val, p, len);
  val[len] = '\0';
  return val;
}

static int parse_xoj_file(const char *filepath, xoj_page **pages_out,
                          int *num_pages_out) {
  gzFile f = gzopen(filepath, "rb");
  if (!f)
    return -1;

  xoj_page *pages = NULL;
  int num_pages = 0;

  sds current_tag = sdsempty();
  sds stroke_text = sdsempty();
  bool in_tag = false;
  bool in_stroke_text = false;

  float page_w = 1404.0f;
  float page_h = 1872.0f;
  float scale_x = 1.0f;
  float scale_y = 1.0f;
  float stroke_width = 2.0f;

  char c;
  while (gzread(f, &c, 1) == 1) {
    if (c == '<') {
      in_tag = true;
      sdsclear(current_tag);
      current_tag = sdscatlen(current_tag, &c, 1);
    } else if (c == '>') {
      if (in_tag) {
        current_tag = sdscatlen(current_tag, &c, 1);
        in_tag = false;

        if (strncmp(current_tag, "<page", 5) == 0) {
          pages = realloc(pages, (num_pages + 1) * sizeof(xoj_page));
          pages[num_pages].strokes = NULL;
          pages[num_pages].num_strokes = 0;
          num_pages++;

          char *w_str = get_attr_value(current_tag, "width");
          char *h_str = get_attr_value(current_tag, "height");
          if (w_str) {
            page_w = strtof(w_str, NULL);
            free(w_str);
          } else {
            page_w = 1404.0f;
          }
          if (h_str) {
            page_h = strtof(h_str, NULL);
            free(h_str);
          } else {
            page_h = 1872.0f;
          }
          scale_x = 1404.0f / page_w;
          scale_y = 1872.0f / page_h;
        } else if (strncmp(current_tag, "<stroke", 7) == 0) {
          in_stroke_text = true;
          sdsclear(stroke_text);

          char *w_str = get_attr_value(current_tag, "width");
          if (w_str) {
            stroke_width = strtof(w_str, NULL) * scale_x / 2.0f;
            free(w_str);
          } else {
            stroke_width = 2.0f * scale_x / 2.0f;
          }
        } else if (strcmp(current_tag, "</stroke>") == 0) {
          in_stroke_text = false;
          if (num_pages > 0) {
            xoj_page *curr_page = &pages[num_pages - 1];
            curr_page->strokes =
                realloc(curr_page->strokes,
                        (curr_page->num_strokes + 1) * sizeof(xoj_stroke));
            xoj_stroke *curr_stroke =
                &curr_page->strokes[curr_page->num_strokes];
            curr_stroke->width = stroke_width;
            curr_stroke->points = NULL;
            curr_stroke->num_points = 0;

            char *p = stroke_text;
            while (*p) {
              while (*p && isspace((unsigned char)*p))
                p++;
              if (!*p)
                break;
              char *next;
              float x_val = strtof(p, &next);
              if (next == p)
                break;
              p = next;

              while (*p && isspace((unsigned char)*p))
                p++;
              if (!*p)
                break;
              float y_val = strtof(p, &next);
              if (next == p)
                break;
              p = next;

              curr_stroke->points =
                  realloc(curr_stroke->points,
                          (curr_stroke->num_points + 1) * sizeof(xoj_point));
              curr_stroke->points[curr_stroke->num_points].x =
                  (x_val * scale_x) - 702.0f;
              curr_stroke->points[curr_stroke->num_points].y = y_val * scale_y;
              curr_stroke->num_points++;
            }
            curr_page->num_strokes++;
          }
        }
      }
    } else {
      if (in_tag) {
        current_tag = sdscatlen(current_tag, &c, 1);
      } else if (in_stroke_text) {
        stroke_text = sdscatlen(stroke_text, &c, 1);
      }
    }
  }

  sdsfree(current_tag);
  sdsfree(stroke_text);
  gzclose(f);

  *pages_out = pages;
  *num_pages_out = num_pages;
  return 0;
}

static void free_xoj_pages(xoj_page *pages, int num_pages) {
  if (!pages)
    return;
  for (int i = 0; i < num_pages; i++) {
    for (int j = 0; j < pages[i].num_strokes; j++) {
      free(pages[i].strokes[j].points);
    }
    free(pages[i].strokes);
  }
  free(pages);
}

static int remfuse_release(const char *path, struct fuse_file_info *fi) {
  if (IS_CACHE_PTR(fi->fh)) {
    release_cached_entry(GET_CACHE_PTR(fi->fh));
    return 0;
  }

  close(fi->fh);

  size_t len = strlen(path);
  bool is_xoj = (len >= 4 && strcmp(path + len - 4, ".xoj") == 0) ||
                (len >= 5 && strcmp(path + len - 5, ".xopp") == 0);

  if (is_xoj && enable_mutable) {
    pthread_mutex_lock(&remfs_mutex);
    struct fuse_context *fuse_ctx = fuse_get_context();
    remfs_ctx *ctx = (remfs_ctx *)fuse_ctx->private_data;

    sds parent_path = sdsempty();
    sds name = sdsempty();
    get_parent_and_name(path, &parent_path, &name);

    uuid_map_node *parent_node = NULL;
    if (strcmp(parent_path, "/") != 0) {
      parent_node = remfs_path_search(ctx, parent_path);
    }

    if (!parent_node || parent_node->file->type == COLLECTION) {
      const char *parent_uuid = parent_node ? parent_node->file->uuid : "";
      sds temp_path = sdscatprintf(sdsempty(), "%s/xojimport_%s_%s.tmp",
                                   ctx->src_dir, parent_uuid, name);

      xoj_page *pages = NULL;
      int num_pages = 0;
      int parse_ret = parse_xoj_file(temp_path, &pages, &num_pages);

      if (parse_ret == 0 && num_pages > 0) {
        char doc_uuid[64];
        gen_uuid(doc_uuid);

        sds dir_path =
            sdscatprintf(sdsempty(), "%s/%s", ctx->src_dir, doc_uuid);
        mkdir(dir_path, 0755);
        sdsfree(dir_path);

        sds vis_name = sdsnew(name);
        size_t v_len = sdslen(vis_name);
        if (v_len >= 4 && strcmp(vis_name + v_len - 4, ".xoj") == 0) {
          vis_name[v_len - 4] = '\0';
        } else if (v_len >= 5 && strcmp(vis_name + v_len - 5, ".xopp") == 0) {
          vis_name[v_len - 5] = '\0';
        }
        sdsupdatelen(vis_name);

        cJSON *meta = cJSON_CreateObject();
        cJSON_AddBoolToObject(meta, "deleted", false);
        char last_mod[32];
        sprintf(last_mod, "%llu", (unsigned long long)time(NULL) * 1000);
        cJSON_AddStringToObject(meta, "lastModified", last_mod);
        cJSON_AddStringToObject(meta, "parent", parent_uuid);
        cJSON_AddBoolToObject(meta, "pinned", false);
        cJSON_AddStringToObject(meta, "type", "DocumentType");
        cJSON_AddStringToObject(meta, "visibleName", vis_name);
        sdsfree(vis_name);

        sds meta_path =
            sdscatprintf(sdsempty(), "%s/%s.metadata", ctx->src_dir, doc_uuid);
        FILE *f_m = fopen(meta_path, "w");
        if (f_m) {
          char *str = cJSON_Print(meta);
          fputs(str, f_m);
          free(str);
          fclose(f_m);
        }
        sdsfree(meta_path);
        cJSON_Delete(meta);

        cJSON *content = cJSON_CreateObject();
        cJSON_AddObjectToObject(content, "extraMetadata");
        cJSON_AddStringToObject(content, "fileType", "notebook");
        cJSON_AddStringToObject(content, "orientation", "portrait");
        cJSON_AddArrayToObject(content, "pages");
        cJSON *pages_arr = cJSON_GetObjectItem(content, "pages");

        sds pagedata_path =
            sdscatprintf(sdsempty(), "%s/%s.pagedata", ctx->src_dir, doc_uuid);
        FILE *f_p = fopen(pagedata_path, "w");

        for (int p = 0; p < num_pages; p++) {
          char page_uuid[64];
          gen_uuid(page_uuid);

          sds page_path = sdscatprintf(sdsempty(), "%s/%s/%s.rm", ctx->src_dir,
                                       doc_uuid, page_uuid);
          sds rm_data =
              generate_rm_data(pages[p].strokes, pages[p].num_strokes);
          FILE *f_rm = fopen(page_path, "wb");
          if (f_rm) {
            fwrite(rm_data, 1, sdslen(rm_data), f_rm);
            fclose(f_rm);
          }
          sdsfree(page_path);
          sdsfree(rm_data);

          cJSON_AddItemToArray(pages_arr, cJSON_CreateString(page_uuid));

          if (f_p) {
            fputs("Blank\n", f_p);
          }
        }

        if (f_p) {
          fclose(f_p);
        }
        sdsfree(pagedata_path);

        sds content_path =
            sdscatprintf(sdsempty(), "%s/%s.content", ctx->src_dir, doc_uuid);
        FILE *f_c = fopen(content_path, "w");
        if (f_c) {
          char *str = cJSON_Print(content);
          fputs(str, f_c);
          free(str);
          fclose(f_c);
        }
        sdsfree(content_path);
        cJSON_Delete(content);
      }
      free_xoj_pages(pages, num_pages);

      unlink(temp_path);
      sdsfree(temp_path);
    }

    sdsfree(parent_path);
    sdsfree(name);
    pthread_mutex_unlock(&remfs_mutex);

    remfs_reload(ctx);
  }

  return 0;
}

static int remfuse_read(const char *path, char *buf, size_t size, off_t offset,
                        struct fuse_file_info *fi) {
  if (IS_CACHE_PTR(fi->fh)) {
    cache_entry *entry = GET_CACHE_PTR(fi->fh);
    if (offset >= entry->size)
      return 0;
    if (offset + size > entry->size)
      size = entry->size - offset;
    memcpy(buf, entry->data + offset, size);
    return size;
  } else {
    int ret = pread(fi->fh, buf, size, offset);
    if (ret == -1)
      return -errno;
    return ret;
  }
}

static int remfuse_write(const char *path, const char *buf, size_t size,
                         off_t offset, struct fuse_file_info *fi) {
  if (!enable_mutable)
    return -EROFS;

  if (IS_CACHE_PTR(fi->fh)) {
    return -EROFS;
  }

  int ret = pwrite(fi->fh, buf, size, offset);
  if (ret == -1)
    return -errno;
  return ret;
}

static int remfuse_truncate(const char *path, off_t size) {
  if (!enable_mutable)
    return -EROFS;

  size_t len = strlen(path);
  if (is_path_virtual(path) ||
      (len >= 14 && strcmp(path + len - 14, ".annotated.pdf") == 0)) {
    return -EROFS;
  }

  pthread_mutex_lock(&remfs_mutex);
  struct fuse_context *fuse_ctx = fuse_get_context();
  remfs_ctx *ctx = (remfs_ctx *)fuse_ctx->private_data;

  int flags = 0;
  sds newpath = sdsempty();
  rewrite_path(ctx, path, &flags, &newpath);
  if (sdslen(newpath) == 0) {
    sdsfree(newpath);
    pthread_mutex_unlock(&remfs_mutex);
    return -ENOENT;
  }

  int ret = truncate(newpath, size);
  sdsfree(newpath);
  pthread_mutex_unlock(&remfs_mutex);

  if (ret == -1)
    return -errno;
  return 0;
}

static int remfuse_mkdir(const char *path, mode_t mode) { return -EROFS; }

static int remfuse_rmdir(const char *path) {
  if (!enable_mutable)
    return -EROFS;

  if (is_path_virtual(path)) {
    return -EROFS;
  }

  pthread_mutex_lock(&remfs_mutex);
  struct fuse_context *fuse_ctx = fuse_get_context();
  remfs_ctx *ctx = (remfs_ctx *)fuse_ctx->private_data;

  int flags = 0;
  sds newpath = sdsempty();
  uuid_map_node *ref = rewrite_path(ctx, path, &flags, &newpath);
  sdsfree(newpath);

  if (!ref) {
    pthread_mutex_unlock(&remfs_mutex);
    return -ENOENT;
  }

  if (ref->file->type != DOCUMENT || ref->file->filetype != NOTEBOOK) {
    pthread_mutex_unlock(&remfs_mutex);
    return -EPERM;
  }

  sds meta_path =
      sdscatprintf(sdsempty(), "%s/%s.metadata", ctx->src_dir, ref->file->uuid);
  uint8_t *raw_meta = slurp(meta_path);
  if (raw_meta) {
    cJSON *json = cJSON_Parse((const char *)raw_meta);
    if (json) {
      cJSON *del_item = cJSON_GetObjectItem(json, "deleted");
      if (del_item) {
        cJSON_ReplaceItemInObject(json, "deleted", cJSON_CreateBool(true));
      } else {
        cJSON_AddBoolToObject(json, "deleted", true);
      }
      char *str = cJSON_Print(json);
      FILE *f_m = fopen(meta_path, "w");
      if (f_m) {
        fputs(str, f_m);
        fclose(f_m);
      }
      free(str);
      cJSON_Delete(json);
    }
    free(raw_meta);
  }
  sdsfree(meta_path);
  pthread_mutex_unlock(&remfs_mutex);

  remfs_reload(ctx);
  return 0;
}

static int remfuse_unlink(const char *path) {
  if (!enable_mutable)
    return -EROFS;

  size_t len = strlen(path);
  if (is_path_virtual(path) ||
      (len >= 14 && strcmp(path + len - 14, ".annotated.pdf") == 0)) {
    return -EROFS;
  }

  pthread_mutex_lock(&remfs_mutex);
  struct fuse_context *fuse_ctx = fuse_get_context();
  remfs_ctx *ctx = (remfs_ctx *)fuse_ctx->private_data;

  int flags = 0;
  sds newpath = sdsempty();
  uuid_map_node *ref = rewrite_path(ctx, path, &flags, &newpath);
  sdsfree(newpath);

  if (!ref) {
    pthread_mutex_unlock(&remfs_mutex);
    return -ENOENT;
  }

  if (ref->file->type != DOCUMENT) {
    pthread_mutex_unlock(&remfs_mutex);
    return -EPERM;
  }

  sds meta_path =
      sdscatprintf(sdsempty(), "%s/%s.metadata", ctx->src_dir, ref->file->uuid);
  uint8_t *raw_meta = slurp(meta_path);
  if (raw_meta) {
    cJSON *json = cJSON_Parse((const char *)raw_meta);
    if (json) {
      cJSON *del_item = cJSON_GetObjectItem(json, "deleted");
      if (del_item) {
        cJSON_ReplaceItemInObject(json, "deleted", cJSON_CreateBool(true));
      } else {
        cJSON_AddBoolToObject(json, "deleted", true);
      }
      char *str = cJSON_Print(json);
      FILE *f_m = fopen(meta_path, "w");
      if (f_m) {
        fputs(str, f_m);
        fclose(f_m);
      }
      free(str);
      cJSON_Delete(json);
    }
    free(raw_meta);
  }
  sdsfree(meta_path);
  pthread_mutex_unlock(&remfs_mutex);

  remfs_reload(ctx);
  return 0;
}

static int remfuse_create(const char *path, mode_t mode,
                          struct fuse_file_info *fi) {
  if (!enable_mutable)
    return -EROFS;

  if (is_path_virtual(path)) {
    return -EROFS;
  }

  pthread_mutex_lock(&remfs_mutex);
  struct fuse_context *fuse_ctx = fuse_get_context();
  remfs_ctx *ctx = (remfs_ctx *)fuse_ctx->private_data;
  sds parent_path = sdsempty();
  sds name = sdsempty();
  get_parent_and_name(path, &parent_path, &name);

  uuid_map_node *parent_node = NULL;
  if (strcmp(parent_path, "/") != 0) {
    parent_node = remfs_path_search(ctx, parent_path);
  }

  size_t name_len = sdslen(name);
  bool is_pdf = (name_len >= 4 && strcmp(name + name_len - 4, ".pdf") == 0);
  bool is_epub = (name_len >= 5 && strcmp(name + name_len - 5, ".epub") == 0);
  bool is_xoj = (name_len >= 4 && strcmp(name + name_len - 4, ".xoj") == 0) ||
                (name_len >= 5 && strcmp(name + name_len - 5, ".xopp") == 0);

  if (is_xoj) {
    if (parent_node && parent_node->file->type != COLLECTION) {
      sdsfree(parent_path);
      sdsfree(name);
      pthread_mutex_unlock(&remfs_mutex);
      return -ENOTDIR;
    }

    const char *parent_uuid = parent_node ? parent_node->file->uuid : "";
    sds temp_path = sdscatprintf(sdsempty(), "%s/xojimport_%s_%s.tmp",
                                 ctx->src_dir, parent_uuid, name);
    int fd = open(temp_path, fi->flags | O_CREAT, mode);
    sdsfree(temp_path);
    sdsfree(parent_path);
    sdsfree(name);

    if (fd == -1) {
      pthread_mutex_unlock(&remfs_mutex);
      return -errno;
    }
    fi->fh = fd;
    pthread_mutex_unlock(&remfs_mutex);
    return 0;
  } else if (is_pdf || is_epub) {
    if (parent_node && parent_node->file->type != COLLECTION) {
      sdsfree(parent_path);
      sdsfree(name);
      pthread_mutex_unlock(&remfs_mutex);
      return -ENOTDIR;
    }

    char doc_uuid[64];
    gen_uuid(doc_uuid);

    sds vis_name = sdsnew(name);
    if (is_pdf) {
      vis_name[sdslen(vis_name) - 4] = '\0';
    } else {
      vis_name[sdslen(vis_name) - 5] = '\0';
    }
    sdsupdatelen(vis_name);

    cJSON *meta = cJSON_CreateObject();
    cJSON_AddBoolToObject(meta, "deleted", false);
    char last_mod[32];
    sprintf(last_mod, "%llu", (unsigned long long)time(NULL) * 1000);
    cJSON_AddStringToObject(meta, "lastModified", last_mod);
    cJSON_AddStringToObject(meta, "parent",
                            parent_node ? parent_node->file->uuid : "");
    cJSON_AddBoolToObject(meta, "pinned", false);
    cJSON_AddStringToObject(meta, "type", "DocumentType");
    cJSON_AddStringToObject(meta, "visibleName", vis_name);
    sdsfree(vis_name);

    sds meta_path =
        sdscatprintf(sdsempty(), "%s/%s.metadata", ctx->src_dir, doc_uuid);
    FILE *f_m = fopen(meta_path, "w");
    if (f_m) {
      char *str = cJSON_Print(meta);
      fputs(str, f_m);
      free(str);
      fclose(f_m);
    }
    sdsfree(meta_path);
    cJSON_Delete(meta);

    cJSON *content = cJSON_CreateObject();
    cJSON_AddObjectToObject(content, "extraMetadata");
    cJSON_AddStringToObject(content, "fileType", is_pdf ? "pdf" : "epub");
    cJSON_AddArrayToObject(content, "pages");

    sds content_path =
        sdscatprintf(sdsempty(), "%s/%s.content", ctx->src_dir, doc_uuid);
    FILE *f_c = fopen(content_path, "w");
    if (f_c) {
      char *str = cJSON_Print(content);
      fputs(str, f_c);
      free(str);
      fclose(f_c);
    }
    sdsfree(content_path);
    cJSON_Delete(content);

    sds pagedata_path =
        sdscatprintf(sdsempty(), "%s/%s.pagedata", ctx->src_dir, doc_uuid);
    FILE *f_p = fopen(pagedata_path, "w");
    if (f_p)
      fclose(f_p);
    sdsfree(pagedata_path);

    sds doc_path = sdscatprintf(sdsempty(), "%s/%s%s", ctx->src_dir, doc_uuid,
                                is_pdf ? ".pdf" : ".epub");
    int fd = open(doc_path, fi->flags, mode);
    sdsfree(doc_path);

    sdsfree(parent_path);
    sdsfree(name);

    if (fd == -1) {
      pthread_mutex_unlock(&remfs_mutex);
      return -errno;
    }
    fi->fh = fd;
    pthread_mutex_unlock(&remfs_mutex);

    remfs_reload(ctx);
    return 0;
  } else {
    sdsfree(parent_path);
    sdsfree(name);
    pthread_mutex_unlock(&remfs_mutex);
    return -EINVAL;
  }
}

static int remfuse_utimens(const char *path, const struct timespec tv[2]) {
  return 0;
}

#if FUSE_USE_VERSION >= 30
static void *remfuse_init(struct fuse_conn_info *conn,
                          struct fuse_config *cfg) {
  (void)cfg;
#else
static void *remfuse_init(struct fuse_conn_info *conn) {
#endif
  const char *src = data_dir ? data_dir : DEFAULT_SOURCE;
  remfs_ctx *ctx = remfs_init(src);
  return ctx;
}

static void remfuse_destroy(void *arg) {
  remfs_ctx *ctx = (remfs_ctx *)arg;
  remfs_destroy(ctx);
}

struct fuse_operations remfuse_ops = {
    .getattr = remfuse_getattr,
    .readdir = remfuse_readdir,
    .open = remfuse_open,
    .release = remfuse_release,
    .read = remfuse_read,
    .write = remfuse_write,
    .truncate = remfuse_truncate,
    .mkdir = remfuse_mkdir,
    .rmdir = remfuse_rmdir,
    .unlink = remfuse_unlink,
    .create = remfuse_create,
    .utimens = remfuse_utimens,
    .init = remfuse_init,
    .destroy = remfuse_destroy,
};
