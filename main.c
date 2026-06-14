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
#include <unistd.h>

#include "cJSON.h"
#include "deps/sds/sds.h"
#include "remfmt.h"
#include "remfs.h"

#define IS_SVG (1 << 0)
#define IS_ANNOT_DIR (1 << 1)
#define IS_ANNOT_PAGE (1 << 2)
#define IS_PNG (1 << 3)
#define IS_PDF (1 << 4)

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

    cJSON_Delete(root);
  }
}

static sds munge_path(const char *path, int *flags) {
  sds ret = sdsnew(path);

  size_t len = sdslen(ret);
  bool is_svg = false;
  bool is_png = false;
  bool is_pdf = false;
  if (len >= 4 && strcmp(ret + len - 4, ".svg") == 0) {
    ret[len - 4] = '\0';
    is_svg = true;
  } else if (len >= 4 && strcmp(ret + len - 4, ".png") == 0) {
    ret[len - 4] = '\0';
    is_png = true;
  } else if (len >= 4 && strcmp(ret + len - 4, ".pdf") == 0) {
    ret[len - 4] = '\0';
    is_pdf = true;
  }

  char *annot_ext = strstr(ret, " Annotations");
  bool is_annot_dir = false;
  bool is_annot_page = false;
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

  sdsupdatelen(ret);

  if (flags) {
    *flags |= is_svg ? IS_SVG : 0;
    *flags |= is_png ? IS_PNG : 0;
    *flags |= is_pdf ? IS_PDF : 0;
    *flags |= is_annot_dir ? IS_ANNOT_DIR : 0;
    *flags |= is_annot_page ? IS_ANNOT_PAGE : 0;
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
      *flags &= ~(IS_SVG | IS_PNG | IS_PDF);
    }
  }
  if (ref && ref->file->filetype != PAGE) {
    *flags &= ~(IS_SVG | IS_PNG | IS_PDF);
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

static int remfuse_getattr(const char *path, struct stat *stbuf) {
  int ret = -ENOENT;
  struct fuse_context *fuse_ctx = fuse_get_context();
  remfs_ctx *ctx = (remfs_ctx *)fuse_ctx->private_data;
  memset(stbuf, 0, sizeof(struct stat));
  if (strcmp(path, "/") == 0) {
    ret = stat(ctx->src_dir, stbuf);
  } else {
    int flags = 0;
    sds newpath = sdsempty();
    uuid_map_node *ref = rewrite_path(ctx, path, &flags, &newpath);
    if (sdslen(newpath) > 0) {
      if (ref) {
        bool allowed = false;
        if ((flags & IS_SVG) && enable_svg)
          allowed = true;
        if ((flags & IS_PNG) && enable_png)
          allowed = true;
        if ((flags & IS_PDF) && enable_pdf)
          allowed = true;

        if ((flags & (IS_SVG | IS_PNG | IS_PDF)) && allowed) {
          ret = stat(newpath, stbuf);
          if (ret == 0) {
            const char *type_str =
                (flags & IS_SVG) ? "svg" : ((flags & IS_PDF) ? "pdf" : "png");
            cache_entry *cached =
                get_cached_entry(ref->file->uuid, type_str, stbuf->st_mtime);
            if (cached) {
              stbuf->st_size = cached->size;
              release_cached_entry(cached);
            } else {
              stbuf->st_size = 5 * 1024 * 1024;
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
    stbuf->st_mode &= ~0200;
    stbuf->st_mode |= 0400;
  }
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

  children_vec *n = NULL;
  int flags = 0;
  if (strcmp(path, "/") == 0) {
    n = &ctx->root_children;
  } else {
    uuid_map_node *ref = rewrite_path(ctx, path, &flags, NULL);
    if (ref)
      n = &ref->children;
  }

  if (!n)
    return -ENOENT;

  filler(buf, ".", NULL, 0);
  filler(buf, "..", NULL, 0);

  for (size_t i = 0; i < kv_size(*n); i++) {
    uuid_map_node *s = kv_A(*n, i);
    if (!s)
      continue;
    if (s->file->filetype == PAGE) {
      if (flags & IS_ANNOT_DIR) {
        if (!has_annotations(ctx->src_dir, s->file)) {
          continue;
        }
      }
      if (enable_svg)
        fill_fake_ext(buf, filler, s->file, ".svg");
      if (enable_png)
        fill_fake_ext(buf, filler, s->file, ".png");
      if (enable_pdf)
        fill_fake_ext(buf, filler, s->file, ".pdf");
    } else if (s->file->filetype == PDF || s->file->filetype == EPUB) {
      fill_fake_folder(buf, filler, s->file);
      filler(buf, s->file->visible_name, NULL, 0);
    } else {
      filler(buf, s->file->visible_name, NULL, 0);
    }
  }
  return 0;
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
  int flags = 0;
  sds newpath = sdsempty();
  uuid_map_node *ref = rewrite_path(ctx, path, &flags, &newpath);

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
      return -ENOENT;
    }
  }

  if (ref && (flags & IS_SVG) && enable_svg) {
    cache_entry *entry =
        generate_fake_ext(ref, newpath, flags & IS_ANNOT_PAGE, "svg");
    sdsfree(newpath);
    if (!entry)
      return -ENOENT;
    fi->fh = MAKE_CACHE_PTR(entry);
    return 0;
  } else if (ref && (flags & IS_PDF) && enable_pdf) {
    cache_entry *entry =
        generate_fake_ext(ref, newpath, flags & IS_ANNOT_PAGE, "pdf");
    sdsfree(newpath);
    if (!entry)
      return -ENOENT;
    fi->fh = MAKE_CACHE_PTR(entry);
    return 0;
  } else if (ref && (flags & IS_PNG) && enable_png) {
    cache_entry *entry =
        generate_fake_ext(ref, newpath, flags & IS_ANNOT_PAGE, "png");
    sdsfree(newpath);
    if (!entry)
      return -ENOENT;
    fi->fh = MAKE_CACHE_PTR(entry);
    return 0;
  } else {
    if (sdslen(newpath) == 0) {
      sdsfree(newpath);
      return -1;
    }
    int fd = open(newpath, fi->flags);
    sdsfree(newpath);
    if (fd == -1)
      return -errno;
    fi->fh = fd;
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

static void *remfuse_init(struct fuse_conn_info *conn) {
  const char *src = data_dir ? data_dir : DEFAULT_SOURCE;
  remfs_ctx *ctx = remfs_init(src);
  // remfs_print(ctx, stderr);
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
    .init = remfuse_init,
    .destroy = remfuse_destroy,
};

static void usage(const char *progname) {
  printf("usage: %s [options] <mountpoint>\n\n", progname);
  printf("File-system specific options:\n"
         "    --config=<c>        path to config JSON file\n"
         "\n");
}

int main(int argc, char *argv[]) {
  int ret = 1;
  struct fuse_args args = FUSE_ARGS_INIT(argc, argv);
  struct stat stbuf;

  options.config_file = NULL;

  if (fuse_opt_parse(&args, &options, option_spec, NULL) == -1)
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
  ret = fuse_main(args.argc, args.argv, &remfuse_ops, NULL);
cleanup:
  fuse_opt_free_args(&args);
  return ret;
}
