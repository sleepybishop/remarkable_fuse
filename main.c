#define FUSE_USE_VERSION 26

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <fuse.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "cJSON.h"
#include "deps/sds/sds.h"
#include "pdfoverlay.h"
#include "remfmt.h"
#include "remfs.h"

#define IS_SVG (1 << 0)
#define IS_ANNOT_DIR (1 << 1)
#define IS_ANNOT_PAGE (1 << 2)
#define IS_PNG (1 << 3)
#define IS_PDF (1 << 4)
#define IS_ANNOTATED_PDF (1 << 5)
#define IS_SVG_DIR (1 << 6)
#define IS_PNG_DIR (1 << 7)
#define IS_PDF_DIR (1 << 8)

#define DEFAULT_SOURCE "./xochitl"
#define CACHE_SIZE 128

typedef struct cache_entry {
  char uuid[64];
  char type[8];
  time_t mtime;
  uint8_t *data;
  size_t size;
  int refcount;
  struct cache_entry *prev;
  struct cache_entry *next;
} cache_entry;

static bool pdf_has_annotations(remfs_ctx *ctx, uuid_map_node *ref)
    __attribute__((unused));
static cache_entry *generate_annotated_pdf(remfs_ctx *ctx, uuid_map_node *ref,
                                           const char *orig_pdf_path);
static cache_entry *generate_notebook_pdf(remfs_ctx *ctx, uuid_map_node *ref);

static cache_entry *cache_head = NULL;
static cache_entry *cache_tail = NULL;
static int cache_count = 0;
static pthread_mutex_t cache_mutex = PTHREAD_MUTEX_INITIALIZER;

static cache_entry *get_cached_entry(const char *uuid, const char *type,
                                     time_t mtime) {
  pthread_mutex_lock(&cache_mutex);
  cache_entry *curr = cache_head;
  while (curr) {
    if (strcmp(curr->uuid, uuid) == 0 && strcmp(curr->type, type) == 0) {
      if (curr->mtime == mtime) {
        if (curr != cache_head) {
          curr->prev->next = curr->next;
          if (curr->next)
            curr->next->prev = curr->prev;
          else
            cache_tail = curr->prev;
          curr->next = cache_head;
          curr->prev = NULL;
          cache_head->prev = curr;
          cache_head = curr;
        }
        curr->refcount++;
        pthread_mutex_unlock(&cache_mutex);
        return curr;
      }
      if (curr->prev)
        curr->prev->next = curr->next;
      else
        cache_head = curr->next;
      if (curr->next)
        curr->next->prev = curr->prev;
      else
        cache_tail = curr->prev;
      if (curr->refcount == 0) {
        free(curr->data);
        free(curr);
      } else {
        curr->uuid[0] = '\0';
      }
      cache_count--;
      break;
    }
    curr = curr->next;
  }
  pthread_mutex_unlock(&cache_mutex);
  return NULL;
}

static cache_entry *add_to_cache(const char *uuid, const char *type,
                                 time_t mtime, uint8_t *data, size_t size) {
  cache_entry *entry = calloc(1, sizeof(cache_entry));
  strcpy(entry->uuid, uuid);
  strcpy(entry->type, type);
  entry->mtime = mtime;
  entry->data = data;
  entry->size = size;
  entry->refcount = 1;

  pthread_mutex_lock(&cache_mutex);
  entry->next = cache_head;
  if (cache_head)
    cache_head->prev = entry;
  cache_head = entry;
  if (!cache_tail)
    cache_tail = entry;
  cache_count++;

  while (cache_count > CACHE_SIZE) {
    cache_entry *tail = cache_tail;
    if (!tail)
      break;
    cache_tail = tail->prev;
    if (cache_tail)
      cache_tail->next = NULL;
    else
      cache_head = NULL;

    if (tail->refcount == 0) {
      free(tail->data);
      free(tail);
    } else {
      tail->uuid[0] = '\0';
    }
    cache_count--;
  }
  pthread_mutex_unlock(&cache_mutex);
  return entry;
}

static void release_cached_entry(cache_entry *entry) {
  pthread_mutex_lock(&cache_mutex);
  entry->refcount--;
  if (entry->refcount == 0 && entry->uuid[0] == '\0') {
    free(entry->data);
    free(entry);
  }
  pthread_mutex_unlock(&cache_mutex);
}

static bool enable_svg = true;
static bool enable_png = true;
static bool enable_pdf = true;
static bool enable_mutable = false;
static bool enable_standalone_annotations = false;
static char *template_dir = NULL;

static char *data_dir = NULL;

static struct options {
  const char *config_file;
  int show_help;
} options;

#define OPTION(t, p) {t, offsetof(struct options, p), 1}
static const struct fuse_opt option_spec[] = {
    OPTION("--config=%s", config_file), OPTION("-h", show_help),
    OPTION("--help", show_help), FUSE_OPT_END};

static void load_config(const char *path) {
  FILE *f = fopen(path, "rb");
  if (!f) {
    if (options.config_file) {
      fprintf(stderr, "warning: could not open config file %s\n", path);
    }
    return;
  }

  fseek(f, 0, SEEK_END);
  long size = ftell(f);
  fseek(f, 0, SEEK_SET);
  if (size <= 0) {
    fclose(f);
    return;
  }

  char *buf = malloc(size + 1);
  if (!buf) {
    fclose(f);
    return;
  }

  size_t read_bytes = fread(buf, 1, size, f);
  buf[read_bytes] = '\0';
  fclose(f);

  cJSON *root = cJSON_Parse(buf);
  free(buf);
  if (root) {
    cJSON *data_dir_cfg = cJSON_GetObjectItemCaseSensitive(root, "data_dir");
    if (data_dir_cfg && cJSON_IsString(data_dir_cfg)) {
      if (data_dir)
        free(data_dir);
      data_dir = strdup(data_dir_cfg->valuestring);
    }

    cJSON *tpl_dir = cJSON_GetObjectItemCaseSensitive(root, "template_dir");
    if (tpl_dir && cJSON_IsString(tpl_dir)) {
      if (template_dir)
        free(template_dir);
      template_dir = strdup(tpl_dir->valuestring);
    }

    cJSON *renderers = cJSON_GetObjectItemCaseSensitive(root, "renderers");
    if (cJSON_IsArray(renderers)) {
      enable_svg = false;
      enable_png = false;
      enable_pdf = false;
      cJSON *item = NULL;
      cJSON_ArrayForEach(item, renderers) {
        if (cJSON_IsString(item)) {
          const char *val = cJSON_GetStringValue(item);
          if (strcmp(val, "svg") == 0)
            enable_svg = true;
          else if (strcmp(val, "png") == 0)
            enable_png = true;
          else if (strcmp(val, "pdf") == 0)
            enable_pdf = true;
        }
      }
    } else {
      cJSON *svg_item = cJSON_GetObjectItem(root, "svg");
      if (cJSON_IsBool(svg_item))
        enable_svg = cJSON_IsTrue(svg_item);

      cJSON *png_item = cJSON_GetObjectItem(root, "png");
      if (cJSON_IsBool(png_item))
        enable_png = cJSON_IsTrue(png_item);

      cJSON *pdf_item = cJSON_GetObjectItem(root, "pdf");
      if (cJSON_IsBool(pdf_item))
        enable_pdf = cJSON_IsTrue(pdf_item);
    }

    cJSON *mutable_item = cJSON_GetObjectItem(root, "mutable");
    if (!mutable_item)
      mutable_item = cJSON_GetObjectItem(root, "mutability");
    if (cJSON_IsBool(mutable_item))
      enable_mutable = cJSON_IsTrue(mutable_item);

    cJSON *standalone_item =
        cJSON_GetObjectItem(root, "standalone_annotations");
    if (cJSON_IsBool(standalone_item))
      enable_standalone_annotations = cJSON_IsTrue(standalone_item);

    cJSON_Delete(root);
  }
}

static sds munge_path(const char *path, int *flags) {
  sds ret = sdsnew(path);
  size_t len = sdslen(ret);
  bool is_svg = false;
  bool is_png = false;
  bool is_pdf = false;
  bool is_annotated_pdf = false;
  bool is_svg_dir = false;
  bool is_png_dir = false;
  bool is_pdf_dir = false;

  if (len >= 4 && strcmp(ret + len - 4, "/svg") == 0) {
    ret[len - 4] = '\0';
    is_svg_dir = true;
  } else if (len >= 4 && strcmp(ret + len - 4, "/png") == 0) {
    ret[len - 4] = '\0';
    is_png_dir = true;
  } else if (len >= 4 && strcmp(ret + len - 4, "/pdf") == 0) {
    ret[len - 4] = '\0';
    is_pdf_dir = true;
  } else {
    char *p;
    if ((p = strstr(ret, "/svg/")) != NULL) {
      memmove(p + 1, p + 5, strlen(p + 5) + 1);
    } else if ((p = strstr(ret, "/png/")) != NULL) {
      memmove(p + 1, p + 5, strlen(p + 5) + 1);
    } else if ((p = strstr(ret, "/pdf/")) != NULL) {
      memmove(p + 1, p + 5, strlen(p + 5) + 1);
    }
  }

  sdsupdatelen(ret);
  len = sdslen(ret);

  if (len >= 14 && strcmp(ret + len - 14, ".annotated.pdf") == 0) {
    ret[len - 14] = '\0';
    is_pdf = true;
    is_annotated_pdf = true;
  } else if (len >= 4 && strcmp(ret + len - 4, ".svg") == 0) {
    ret[len - 4] = '\0';
    is_svg = true;
  } else if (len >= 4 && strcmp(ret + len - 4, ".png") == 0) {
    ret[len - 4] = '\0';
    is_png = true;
  } else if (len >= 4 && strcmp(ret + len - 4, ".pdf") == 0) {
    ret[len - 4] = '\0';
    is_pdf = true;
  } else if (len >= 5 && strcmp(ret + len - 5, ".epub") == 0) {
    ret[len - 5] = '\0';
  } else if (len >= 3 && strcmp(ret + len - 3, ".rm") == 0) {
    ret[len - 3] = '\0';
  }

  char *annot_ext = strstr(ret, " Annotations");
  bool is_annot_dir = false;
  bool is_annot_page = false;
  if (enable_standalone_annotations) {
    while (annot_ext) {
      if (annot_ext[12] == '\0' || annot_ext[12] == '/') {
        is_annot_page = true;
        if (annot_ext[12] == '\0') {
          is_annot_dir = true;
          annot_ext[0] = '\0';
        } else {
          memmove(annot_ext, annot_ext + 12, strlen(annot_ext + 12) + 1);
        }
        break;
      }
      annot_ext = strstr(annot_ext + 1, " Annotations");
    }
  }

  sdsupdatelen(ret);

  if (flags) {
    *flags |= is_svg ? IS_SVG : 0;
    *flags |= is_png ? IS_PNG : 0;
    *flags |= is_pdf ? IS_PDF : 0;
    *flags |= is_annotated_pdf ? IS_ANNOTATED_PDF : 0;
    *flags |= is_annot_dir ? IS_ANNOT_DIR : 0;
    *flags |= is_annot_page ? IS_ANNOT_PAGE : 0;
    *flags |= is_svg_dir ? IS_SVG_DIR : 0;
    *flags |= is_png_dir ? IS_PNG_DIR : 0;
    *flags |= is_pdf_dir ? IS_PDF_DIR : 0;
  }
  return ret;
}

static uuid_map_node *rewrite_path(remfs_ctx *ctx, const char *path, int *flags,
                                   sds *newpath) {
  sds munged = munge_path(path, flags);
  uuid_map_node *ref = remfs_path_search(ctx, munged);
  if (!ref && (*flags & (IS_SVG | IS_PNG | IS_PDF))) {
    ref = remfs_path_search(ctx, path);
    if (ref) {
      *flags &= ~(IS_SVG | IS_PNG | IS_PDF | IS_ANNOTATED_PDF);
    }
  }
  if (ref && ref->file->filetype != PAGE) {
    if ((ref->file->filetype == PDF || ref->file->filetype == NOTEBOOK) &&
        (*flags & IS_PDF) && enable_pdf) {
      // Keep IS_PDF flag
    } else {
      *flags &= ~(IS_SVG | IS_PNG | IS_PDF | IS_ANNOTATED_PDF);
    }
  }
  if (ref && ref->file->filetype == PAGE && (*flags & IS_PDF)) {
    bool parent_is_notebook = false;
    if (ref->file->parent[0] != '\0') {
      uuid_map_node *parent_node = remfs_uuid_search(ctx, ref->file->parent);
      if (parent_node && parent_node->file &&
          parent_node->file->filetype == NOTEBOOK) {
        parent_is_notebook = true;
      }
    }
    if (parent_is_notebook) {
      sdsfree(munged);
      return NULL;
    }
  }
  if (ref && ref->file->filetype == NOTEBOOK && !(*flags & IS_PDF)) {
    if (!enable_png && !enable_svg && !enable_mutable) {
      sdsfree(munged);
      return NULL;
    }
  }
  if (ref && newpath) {
    if (ref->file->type == COLLECTION) {
      *newpath = sdscatprintf(*newpath, "%s", ctx->src_dir);
    } else {
      const char *exts[] = {"", "", ".epub", ".pdf", ".rm"};
      *newpath = sdscatprintf(*newpath, "%s/", ctx->src_dir);
      if (ref->file->filetype == PAGE) {
        *newpath = sdscatprintf(*newpath, "%s/", ref->file->parent);
      }
      if (*flags & IS_ANNOT_DIR) {
        *newpath = sdscatprintf(*newpath, "%s", ref->file->uuid);
      } else {
        *newpath = sdscatprintf(*newpath, "%s%s", ref->file->uuid,
                                exts[ref->file->filetype]);
      }
    }
  }
  sdsfree(munged);
  return ref;
}

static pthread_mutex_t remfs_mutex = PTHREAD_MUTEX_INITIALIZER;

static void gen_uuid(char *buf) {
  FILE *f = fopen("/proc/sys/kernel/random/uuid", "r");
  if (f) {
    if (fgets(buf, 37, f)) {
      buf[36] = '\0';
    }
    fclose(f);
  } else {
    static unsigned int seed = 12345;
    sprintf(buf, "%08x-%04x-%04x-%04x-%012x", rand_r(&seed),
            rand_r(&seed) & 0xFFFF, rand_r(&seed) & 0xFFFF,
            rand_r(&seed) & 0xFFFF, rand_r(&seed));
  }
}

static void get_parent_and_name(const char *path, sds *parent_path, sds *name) {
  const char *last_slash = strrchr(path, '/');
  if (!last_slash) {
    *parent_path = sdsnew("/");
    *name = sdsnew(path);
  } else if (last_slash == path) {
    *parent_path = sdsnew("/");
    *name = sdsnew(path + 1);
  } else {
    *parent_path = sdsnewlen(path, last_slash - path);
    *name = sdsnew(last_slash + 1);
  }
}

static uint8_t *slurp(const char *path) {
  uint8_t *ret = NULL;
  FILE *f = fopen(path, "rb");
  if (!f)
    return ret;
  fseek(f, 0, SEEK_END);
  long size = ftell(f);
  fseek(f, 0, SEEK_SET);
  if (size <= 0)
    goto cleanup;
  size_t meta_size = size;
  ret = malloc(meta_size + 1);
  if (!ret)
    goto cleanup;
  size_t bytes_read = fread(ret, 1, meta_size, f);
  ret[bytes_read] = '\0';
cleanup:
  fclose(f);
  return ret;
}

static bool is_path_virtual(const char *path) {
  if (strstr(path, "/svg/") != NULL || strstr(path, "/png/") != NULL ||
      strstr(path, "/pdf/") != NULL || strstr(path, " Annotations/") != NULL) {
    return true;
  }
  size_t len = strlen(path);
  if (len >= 4 && strcmp(path + len - 4, "/svg") == 0)
    return true;
  if (len >= 4 && strcmp(path + len - 4, "/png") == 0)
    return true;
  if (len >= 4 && strcmp(path + len - 4, "/pdf") == 0)
    return true;
  if (len >= 12 && strcmp(path + len - 12, " Annotations") == 0)
    return true;
  return false;
}

static int remfuse_getattr(const char *path, struct stat *stbuf) {
  int ret = -ENOENT;
  struct fuse_context *fuse_ctx = fuse_get_context();
  remfs_ctx *ctx = (remfs_ctx *)fuse_ctx->private_data;
  memset(stbuf, 0, sizeof(struct stat));
  int flags = 0;
  pthread_mutex_lock(&remfs_mutex);
  if (strcmp(path, "/") == 0) {
    ret = stat(ctx->src_dir, stbuf);
  } else {
    sds newpath = sdsempty();
    uuid_map_node *ref = rewrite_path(ctx, path, &flags, &newpath);
    if (flags & (IS_SVG_DIR | IS_PNG_DIR | IS_PDF_DIR)) {
      bool allowed = false;
      if ((flags & IS_SVG_DIR) && enable_svg)
        allowed = true;
      if ((flags & IS_PNG_DIR) && enable_png)
        allowed = true;
      if ((flags & IS_PDF_DIR) && enable_pdf)
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

        if ((flags & (IS_SVG | IS_PNG | IS_PDF)) && allowed) {
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
              stbuf->st_size = ref->file->page_count * 1024 * 1024;
              if (stbuf->st_size < 1024 * 1024) {
                stbuf->st_size = 1024 * 1024;
              }
              stbuf->st_mtime = latest_mtime;
            }
            ret = 0;
          } else {
            ret = stat(newpath, stbuf);
            if (ret == 0) {
              const char *type_str =
                  (flags & IS_SVG) ? "svg" : ((flags & IS_PDF) ? "pdf" : "png");
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
                  stbuf->st_size = 5 * 1024 * 1024;
                }
              }
            }
          }
        } else if ((flags & (IS_SVG | IS_PNG | IS_PDF)) && !allowed) {
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
        !(flags & (IS_ANNOTATED_PDF | IS_SVG | IS_PNG | IS_PDF | IS_SVG_DIR |
                   IS_PNG_DIR | IS_PDF_DIR))) {
      stbuf->st_mode |= 0200;
    } else {
      stbuf->st_mode &= ~0200;
    }
    stbuf->st_mode |= 0400;
  }
  pthread_mutex_unlock(&remfs_mutex);
  return ret;
}

static void fill_fake_ext(void *buf, fuse_fill_dir_t filler, remfs_file *file,
                          const char *ext_str) {
  sds tmp = sdsnew(file->visible_name);
  tmp = sdscat(tmp, ext_str);
  filler(buf, tmp, NULL, 0);
  sdsfree(tmp);
}

static void fill_fake_folder(void *buf, fuse_fill_dir_t filler,
                             remfs_file *file) {
  sds tmp = sdsempty();
  tmp = sdscatprintf(tmp, "%s Annotations", file->visible_name);
  filler(buf, tmp, NULL, 0);
  sdsfree(tmp);
}

static bool has_annotations(const char *src_dir, remfs_file *file) {
  sds rmpath = sdscatprintf(sdsempty(), "%s/%s/%s.rm", src_dir, file->parent,
                            file->uuid);
  struct stat st;
  if (stat(rmpath, &st) == -1) {
    sdsfree(rmpath);
    return false;
  }
  if (st.st_size <= 43) {
    sdsfree(rmpath);
    return false;
  }

  bool has_strokes = false;
  remfmt_stroke_vec *strokes = remfmt_parse(rmpath);
  if (strokes) {
    if (kv_size(*strokes) > 0) {
      has_strokes = true;
    }
    remfmt_stroke_cleanup(strokes);
  }

  sdsfree(rmpath);
  return has_strokes;
}

static int remfuse_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
                           off_t offset, struct fuse_file_info *fi) {
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

  filler(buf, ".", NULL, 0);
  filler(buf, "..", NULL, 0);

  bool is_notebook_dir = false;
  bool is_annot_root_dir = false;
  if (strcmp(path, "/") != 0) {
    uuid_map_node *ref = rewrite_path(ctx, path, &flags, NULL);
    if (ref) {
      if (ref->file->filetype == NOTEBOOK &&
          !(flags & (IS_SVG_DIR | IS_PNG_DIR | IS_PDF_DIR))) {
        is_notebook_dir = true;
      }
      if ((flags & IS_ANNOT_DIR) &&
          !(flags & (IS_SVG_DIR | IS_PNG_DIR | IS_PDF_DIR))) {
        is_annot_root_dir = true;
      }
    }
  }

  if (is_notebook_dir) {
    if (enable_svg)
      filler(buf, "svg", NULL, 0);
    if (enable_png)
      filler(buf, "png", NULL, 0);
  }
  if (is_annot_root_dir) {
    if (enable_svg)
      filler(buf, "svg", NULL, 0);
    if (enable_png)
      filler(buf, "png", NULL, 0);
    if (enable_pdf)
      filler(buf, "pdf", NULL, 0);
  }

  for (size_t i = 0; i < kv_size(*n); i++) {
    uuid_map_node *s = kv_A(*n, i);
    if (!s)
      continue;

    if ((flags & (IS_SVG_DIR | IS_PNG_DIR | IS_PDF_DIR)) &&
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
          fill_fake_ext(buf, filler, s->file, ".svg");
        }
      } else if (flags & IS_PNG_DIR) {
        if (enable_png) {
          fill_fake_ext(buf, filler, s->file, ".png");
        }
      } else if (flags & IS_PDF_DIR) {
        if (enable_pdf) {
          fill_fake_ext(buf, filler, s->file, ".pdf");
        }
      } else {
        if (!(flags & IS_ANNOT_DIR)) {
          if (enable_mutable) {
            fill_fake_ext(buf, filler, s->file, ".rm");
          }
        }
      }
    } else if (s->file->filetype == PDF || s->file->filetype == EPUB) {
      if (enable_standalone_annotations) {
        fill_fake_folder(buf, filler, s->file);
      }
      sds tmp = sdsnew(s->file->visible_name);
      tmp = sdscat(tmp, s->file->filetype == PDF ? ".pdf" : ".epub");
      filler(buf, tmp, NULL, 0);
      sdsfree(tmp);
      if (s->file->filetype == PDF) {
        sds tmp_annot = sdsnew(s->file->visible_name);
        tmp_annot = sdscat(tmp_annot, ".annotated.pdf");
        filler(buf, tmp_annot, NULL, 0);
        sdsfree(tmp_annot);
      }
    } else {
      bool show_folder = true;
      if (s->file->filetype == NOTEBOOK) {
        if (!enable_png && !enable_svg && !enable_mutable) {
          show_folder = false;
        }
      }
      if (show_folder) {
        filler(buf, s->file->visible_name, NULL, 0);
      }
      if (s->file->filetype == NOTEBOOK && enable_pdf) {
        fill_fake_ext(buf, filler, s->file, ".pdf");
      }
    }
  }
  pthread_mutex_unlock(&remfs_mutex);
  return 0;
}

static bool __attribute__((unused)) pdf_has_annotations(remfs_ctx *ctx,
                                                        uuid_map_node *ref) {
  if (!ref || !ref->file || ref->file->filetype != PDF) {
    return false;
  }
  for (size_t i = 0; i < kv_size(ref->children); i++) {
    uuid_map_node *page = kv_A(ref->children, i);
    if (!page || page->file->filetype != PAGE)
      continue;
    sds rm_path = sdscatprintf(sdsempty(), "%s/%s/%s.rm", ctx->src_dir,
                               ref->file->uuid, page->file->uuid);
    struct stat st;
    if (stat(rm_path, &st) == 0 && st.st_size > 43) {
      sdsfree(rm_path);
      return true;
    }
    sdsfree(rm_path);
  }
  return false;
}

static cache_entry *generate_annotated_pdf(remfs_ctx *ctx, uuid_map_node *ref,
                                           const char *orig_pdf_path) {
  if (!ref || !ref->file || ref->file->filetype != PDF) {
    return NULL;
  }
  struct stat st;
  if (stat(orig_pdf_path, &st) == -1) {
    return NULL;
  }

  time_t latest_mtime = st.st_mtime;
  for (size_t i = 0; i < kv_size(ref->children); i++) {
    uuid_map_node *page = kv_A(ref->children, i);
    if (!page || page->file->filetype != PAGE)
      continue;
    sds rm_path = sdscatprintf(sdsempty(), "%s/%s/%s.rm", ctx->src_dir,
                               ref->file->uuid, page->file->uuid);
    struct stat rm_st;
    if (stat(rm_path, &rm_st) == 0) {
      if (rm_st.st_mtime > latest_mtime) {
        latest_mtime = rm_st.st_mtime;
      }
    }
    sdsfree(rm_path);
  }

  cache_entry *cached = get_cached_entry(ref->file->uuid, "pdf", latest_mtime);
  if (cached) {
    return cached;
  }

  sds current_pdf = sdsnew(orig_pdf_path);
  int page_num = 0;
  for (size_t i = 0; i < kv_size(ref->children); i++) {
    uuid_map_node *page = kv_A(ref->children, i);
    if (!page || page->file->filetype != PAGE)
      continue;
    page_num++;

    int actual_page_num = page_num;
    sscanf(page->file->visible_name, "page_%u", &actual_page_num);

    sds rm_path = sdscatprintf(sdsempty(), "%s/%s/%s.rm", ctx->src_dir,
                               ref->file->uuid, page->file->uuid);
    struct stat rm_st;
    if (stat(rm_path, &rm_st) == 0 && rm_st.st_size > 43) {
      char png_tmp_path[] = "/tmp/remfs_annot_XXXXXX";
      int png_fd = mkstemp(png_tmp_path);
      if (png_fd != -1) {
        FILE *png_fp = fdopen(png_fd, "wb");
        if (png_fp) {
          int overlay_margins = 0;
          remfmt_stroke_vec *strokes = remfmt_parse(rm_path);
          if (strokes) {
            remfmt_render_params prm = {.landscape = page->file->landscape,
                                        .template_name =
                                            page->file->template_name,
                                        .template_dir = template_dir,
                                        .annotation = true};
            if (ref->file->custom_zoom_page_height > 0 &&
                ref->file->custom_zoom_page_width > 0) {
              prm.canvas_width = ref->file->custom_zoom_page_width;
              prm.canvas_height = ref->file->custom_zoom_page_height;
              overlay_margins = 0;
            } else {
              prm.canvas_width = DEV_W;
              prm.canvas_height = DEV_H;
              overlay_margins = ref->file->margins;
            }

            remfmt_render_png(png_fp, strokes, &prm);
            remfmt_stroke_cleanup(strokes);
          }
          fclose(png_fp);

          struct stat png_st;
          stat(png_tmp_path, &png_st);

          char pdf_tmp_path[] = "/tmp/remfs_pdf_XXXXXX";
          int pdf_fd = mkstemp(pdf_tmp_path);
          if (pdf_fd != -1) {
            close(pdf_fd);
            int err =
                pdf_overlay_png(current_pdf, png_tmp_path, pdf_tmp_path,
                                actual_page_num, 0, 0, 0, 0, overlay_margins);
            if (err == 0) {
              if (strcmp(current_pdf, orig_pdf_path) != 0) {
                unlink(current_pdf);
              }
              sdsfree(current_pdf);
              current_pdf = sdsnew(pdf_tmp_path);
            } else {
              unlink(pdf_tmp_path);
            }
          }
        }
        unlink(png_tmp_path);
      }
    }
    sdsfree(rm_path);
  }

  uint8_t *final_data = slurp(current_pdf);
  struct stat final_st;
  size_t final_size = 0;
  if (stat(current_pdf, &final_st) == 0) {
    final_size = final_st.st_size;
  }
  if (strcmp(current_pdf, orig_pdf_path) != 0) {
    unlink(current_pdf);
  }
  sdsfree(current_pdf);

  if (!final_data) {
    return NULL;
  }

  return add_to_cache(ref->file->uuid, "pdf", latest_mtime, final_data,
                      final_size);
}

static cache_entry *generate_notebook_pdf(remfs_ctx *ctx, uuid_map_node *ref) {
  int page_count = 0;
  for (size_t i = 0; i < kv_size(ref->children); i++) {
    uuid_map_node *page = kv_A(ref->children, i);
    if (page && page->file->filetype == PAGE) {
      page_count++;
    }
  }

  sds meta_path =
      sdscatprintf(sdsempty(), "%s/%s.metadata", ctx->src_dir, ref->file->uuid);
  struct stat meta_st;
  time_t latest_mtime = 1;
  if (stat(meta_path, &meta_st) == 0) {
    latest_mtime = meta_st.st_mtime;
  }
  sdsfree(meta_path);

  cache_entry *cached = get_cached_entry(ref->file->uuid, "pdf", latest_mtime);
  if (cached) {
    return cached;
  }

  uint8_t *data = NULL;
  size_t size = 0;
  FILE *sh = open_memstream((char **)&data, &size);
  if (!sh)
    return NULL;

  remfmt_stroke_vec **pages_strokes =
      calloc(page_count, sizeof(remfmt_stroke_vec *));
  remfmt_render_params **pages_prms =
      calloc(page_count, sizeof(remfmt_render_params *));

  int idx = 0;
  for (size_t i = 0; i < kv_size(ref->children); i++) {
    uuid_map_node *page = kv_A(ref->children, i);
    if (page && page->file->filetype == PAGE) {
      sds rm_path = sdscatprintf(sdsempty(), "%s/%s/%s.rm", ctx->src_dir,
                                 ref->file->uuid, page->file->uuid);
      pages_strokes[idx] = remfmt_parse(rm_path);
      sdsfree(rm_path);

      pages_prms[idx] = malloc(sizeof(remfmt_render_params));
      pages_prms[idx]->landscape = page->file->landscape;
      pages_prms[idx]->template_name = page->file->template_name;
      pages_prms[idx]->template_dir = template_dir;
      pages_prms[idx]->annotation = false;
      idx++;
    }
  }

  remfmt_render_notebook_pdf(sh, page_count, pages_strokes, pages_prms);
  fclose(sh);

  for (int i = 0; i < page_count; i++) {
    if (pages_strokes[i]) {
      remfmt_stroke_cleanup(pages_strokes[i]);
    }
    if (pages_prms[i]) {
      free(pages_prms[i]);
    }
  }
  free(pages_strokes);
  free(pages_prms);

  return add_to_cache(ref->file->uuid, "pdf", latest_mtime, data, size);
}

static cache_entry *generate_fake_ext(uuid_map_node *ref, const char *rmpath,
                                      bool anot, const char *ext) {
  struct stat st;
  if (stat(rmpath, &st) == -1)
    return NULL;

  cache_entry *cached = get_cached_entry(ref->file->uuid, ext, st.st_mtime);
  if (cached)
    return cached;

  uint8_t *data = NULL;
  size_t size = 0;
  FILE *sh = open_memstream((char **)&data, &size);
  if (!sh)
    return NULL;

  remfmt_stroke_vec *strokes = remfmt_parse(rmpath);
  if (strokes) {
    remfmt_render_params prm = {.landscape = ref->file->landscape,
                                .template_name = ref->file->template_name,
                                .template_dir = template_dir,
                                .annotation = anot};
    if (strcmp(ext, "svg") == 0) {
      remfmt_render_svg(sh, strokes, &prm);
    } else if (strcmp(ext, "pdf") == 0) {
      remfmt_render_pdf(sh, strokes, &prm);
    } else {
      remfmt_render_png(sh, strokes, &prm);
    }
    remfmt_stroke_cleanup(strokes);
  }
  fclose(sh);

  return add_to_cache(ref->file->uuid, ext, st.st_mtime, data, size);
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
       (flags & (IS_ANNOTATED_PDF | IS_SVG | IS_PNG | IS_PDF)))) {
    sdsfree(newpath);
    pthread_mutex_unlock(&remfs_mutex);
    return -EROFS;
  }

  if (flags & (IS_SVG | IS_PNG | IS_PDF)) {
    bool allowed = false;
    if ((flags & IS_SVG) && enable_svg)
      allowed = true;
    if ((flags & IS_PNG) && enable_png)
      allowed = true;
    if ((flags & IS_PDF) && enable_pdf)
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

static int remfuse_release(const char *path, struct fuse_file_info *fi) {
  if (IS_CACHE_PTR(fi->fh)) {
    release_cached_entry(GET_CACHE_PTR(fi->fh));
  } else {
    close(fi->fh);
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

static int remfuse_mkdir(const char *path, mode_t mode) {
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
    if (!parent_node || parent_node->file->type != COLLECTION) {
      sdsfree(parent_path);
      sdsfree(name);
      pthread_mutex_unlock(&remfs_mutex);
      return -ENOENT;
    }
  }

  bool is_notebook = false;
  size_t name_len = sdslen(name);
  if (name_len >= 9 && strcmp(name + name_len - 9, ".notebook") == 0) {
    is_notebook = true;
  }

  char uuid[64];
  gen_uuid(uuid);

  cJSON *meta = cJSON_CreateObject();
  cJSON_AddBoolToObject(meta, "deleted", false);
  char last_mod[32];
  sprintf(last_mod, "%llu", (unsigned long long)time(NULL) * 1000);
  cJSON_AddStringToObject(meta, "lastModified", last_mod);
  cJSON_AddStringToObject(meta, "parent",
                          parent_node ? parent_node->file->uuid : "");
  cJSON_AddBoolToObject(meta, "pinned", false);
  cJSON_AddStringToObject(meta, "type",
                          is_notebook ? "DocumentType" : "CollectionType");
  cJSON_AddStringToObject(meta, "visibleName", name);

  sds meta_path =
      sdscatprintf(sdsempty(), "%s/%s.metadata", ctx->src_dir, uuid);
  FILE *f = fopen(meta_path, "w");
  if (f) {
    char *str = cJSON_Print(meta);
    fputs(str, f);
    free(str);
    fclose(f);
  }
  sdsfree(meta_path);
  cJSON_Delete(meta);

  if (is_notebook) {
    cJSON *content = cJSON_CreateObject();
    cJSON_AddObjectToObject(content, "extraMetadata");
    cJSON_AddStringToObject(content, "fileType", "notebook");
    cJSON_AddStringToObject(content, "orientation", "portrait");
    cJSON_AddArrayToObject(content, "pages");

    sds content_path =
        sdscatprintf(sdsempty(), "%s/%s.content", ctx->src_dir, uuid);
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
        sdscatprintf(sdsempty(), "%s/%s.pagedata", ctx->src_dir, uuid);
    FILE *f_p = fopen(pagedata_path, "w");
    if (f_p)
      fclose(f_p);
    sdsfree(pagedata_path);

    sds dir_path = sdscatprintf(sdsempty(), "%s/%s", ctx->src_dir, uuid);
    mkdir(dir_path, 0755);
    sdsfree(dir_path);
  }

  sdsfree(parent_path);
  sdsfree(name);
  pthread_mutex_unlock(&remfs_mutex);

  remfs_reload(ctx);
  return 0;
}

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

  if (!ref || ref->file->type != COLLECTION) {
    pthread_mutex_unlock(&remfs_mutex);
    return -ENOENT;
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

  if (ref->file->filetype == PAGE) {
    uuid_map_node *notebook_node = remfs_uuid_search(ctx, ref->file->parent);
    if (notebook_node) {
      sds content_path = sdscatprintf(sdsempty(), "%s/%s.content", ctx->src_dir,
                                      notebook_node->file->uuid);
      uint8_t *raw_content = slurp(content_path);
      if (raw_content) {
        cJSON *json = cJSON_Parse((const char *)raw_content);
        if (json) {
          cJSON *pages = cJSON_GetObjectItem(json, "pages");
          if (!pages) {
            cJSON *cPages = cJSON_GetObjectItem(json, "cPages");
            if (cPages) {
              pages = cJSON_GetObjectItem(cPages, "pages");
            }
          }
          if (pages && cJSON_IsArray(pages)) {
            int idx = -1;
            for (int i = 0; i < cJSON_GetArraySize(pages); i++) {
              cJSON *item = cJSON_GetArrayItem(pages, i);
              if (cJSON_IsString(item) &&
                  strcmp(item->valuestring, ref->file->uuid) == 0) {
                idx = i;
                break;
              }
            }
            if (idx != -1) {
              cJSON_DeleteItemFromArray(pages, idx);
              char *str = cJSON_Print(json);
              FILE *f_c = fopen(content_path, "w");
              if (f_c) {
                fputs(str, f_c);
                fclose(f_c);
              }
              free(str);

              sds pagedata_path =
                  sdscatprintf(sdsempty(), "%s/%s.pagedata", ctx->src_dir,
                               notebook_node->file->uuid);
              FILE *f_p = fopen(pagedata_path, "r");
              if (f_p) {
                kvec_t(sds) lines = {0};
                char buf[RM_PATH_MAX];
                while (fgets(buf, sizeof(buf), f_p)) {
                  size_t len = strlen(buf);
                  while (len > 0 &&
                         (buf[len - 1] == '\n' || buf[len - 1] == '\r')) {
                    buf[len - 1] = '\0';
                    len--;
                  }
                  kv_push(sds, lines, sdsnew(buf));
                }
                fclose(f_p);

                f_p = fopen(pagedata_path, "w");
                if (f_p) {
                  for (int i = 0; i < kv_size(lines); i++) {
                    if (i != idx) {
                      fprintf(f_p, "%s\n", kv_A(lines, i));
                    }
                    sdsfree(kv_A(lines, i));
                  }
                  fclose(f_p);
                }
                kv_destroy(lines);
              }
              sdsfree(pagedata_path);
            }
          }
          cJSON_Delete(json);
        }
        free(raw_content);
      }
      sdsfree(content_path);
    }
  } else {
    sds meta_path = sdscatprintf(sdsempty(), "%s/%s.metadata", ctx->src_dir,
                                 ref->file->uuid);
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
  }
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
  bool is_rm = (name_len >= 3 && strcmp(name + name_len - 3, ".rm") == 0);

  if (is_rm) {
    if (!parent_node || parent_node->file->type != DOCUMENT ||
        parent_node->file->filetype != NOTEBOOK) {
      sdsfree(parent_path);
      sdsfree(name);
      pthread_mutex_unlock(&remfs_mutex);
      return -ENOENT;
    }

    char page_uuid[64];
    gen_uuid(page_uuid);

    sds page_path = sdscatprintf(sdsempty(), "%s/%s/%s.rm", ctx->src_dir,
                                 parent_node->file->uuid, page_uuid);
    FILE *f_rm = fopen(page_path, "wb");
    if (!f_rm) {
      sdsfree(page_path);
      sdsfree(parent_path);
      sdsfree(name);
      pthread_mutex_unlock(&remfs_mutex);
      return -EIO;
    }
    fwrite("reMarkable .lines file, version=6          ", 1, 43, f_rm);
    fclose(f_rm);

    sds content_path = sdscatprintf(sdsempty(), "%s/%s.content", ctx->src_dir,
                                    parent_node->file->uuid);
    uint8_t *raw_content = slurp(content_path);
    if (raw_content) {
      cJSON *json = cJSON_Parse((const char *)raw_content);
      if (json) {
        cJSON *pages = cJSON_GetObjectItem(json, "pages");
        if (!pages) {
          cJSON *cPages = cJSON_GetObjectItem(json, "cPages");
          if (cPages) {
            pages = cJSON_GetObjectItem(cPages, "pages");
          }
        }
        if (pages && cJSON_IsArray(pages)) {
          cJSON_AddItemToArray(pages, cJSON_CreateString(page_uuid));
          char *str = cJSON_Print(json);
          FILE *f_c = fopen(content_path, "w");
          if (f_c) {
            fputs(str, f_c);
            fclose(f_c);
          }
          free(str);
        }
        cJSON_Delete(json);
      }
      free(raw_content);
    }
    sdsfree(content_path);

    sds pagedata_path = sdscatprintf(sdsempty(), "%s/%s.pagedata", ctx->src_dir,
                                     parent_node->file->uuid);
    FILE *f_p = fopen(pagedata_path, "a");
    if (f_p) {
      fputs("Blank\n", f_p);
      fclose(f_p);
    }
    sdsfree(pagedata_path);

    sdsfree(parent_path);
    sdsfree(name);

    int fd = open(page_path, fi->flags, mode);
    sdsfree(page_path);
    if (fd == -1) {
      pthread_mutex_unlock(&remfs_mutex);
      return -errno;
    }
    fi->fh = fd;
    pthread_mutex_unlock(&remfs_mutex);

    remfs_reload(ctx);
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
static void *remfuse_init(struct fuse_conn_info *conn) {
  const char *src = data_dir ? data_dir : DEFAULT_SOURCE;
  remfs_ctx *ctx = remfs_init(src);
  return ctx;
}

static void remfuse_destroy(void *arg) {
  remfs_ctx *ctx = (remfs_ctx *)arg;
  remfs_destroy(ctx);
}

static struct fuse_operations remfuse_ops = {
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

static void usage(const char *progname) {
  printf("usage: %s [options] <mountpoint>\n\n", progname);
  printf("File-system specific options:\n"
         "    --config=<c>        path to config JSON file\n"
         "\n");
}

static char *mountpoint = NULL;
static int opt_proc(void *data, const char *arg, int key,
                    struct fuse_args *outargs) {
  if (key == FUSE_OPT_KEY_NONOPT && mountpoint == NULL) {
    mountpoint = strdup(arg);
  }
  return 1;
}

int main(int argc, char *argv[]) {
  int ret = 1;
  struct fuse_args args = FUSE_ARGS_INIT(argc, argv);
  struct stat stbuf;

  options.config_file = NULL;

  if (fuse_opt_parse(&args, &options, option_spec, opt_proc) == -1)
    goto cleanup;

  if (options.config_file) {
    load_config(options.config_file);
  } else {
    struct stat st;
    if (stat("config.json", &st) == 0) {
      load_config("config.json");
    }
  }

  if (options.show_help) {
    usage(argv[0]);
    assert(fuse_opt_add_arg(&args, "--help") == 0);
    args.argv[0][0] = '\0';
  } else {
    const char *src = data_dir ? data_dir : DEFAULT_SOURCE;
    if (stat(src, &stbuf) == -1) {
      fprintf(stderr, "failed to access data dir [%s]: %s\n", src,
              strerror(errno));
      goto cleanup;
    }
    if (!S_ISDIR(stbuf.st_mode)) {
      fprintf(stderr, "data dir [%s] is not a directory\n", src);
      goto cleanup;
    }
  }

  if (mountpoint) {
    sds cmd = sdscatprintf(sdsempty(),
                           "fusermount3 -u -q -z %s 2>/dev/null || fusermount "
                           "-u -q -z %s 2>/dev/null",
                           mountpoint, mountpoint);
    int sys_ret = system(cmd);
    (void)sys_ret;
    sdsfree(cmd);
    free(mountpoint);
  }

  fuse_opt_add_arg(&args, "-oauto_unmount");
  ret = fuse_main(args.argc, args.argv, &remfuse_ops, NULL);
cleanup:
  fuse_opt_free_args(&args);
  return ret;
}
