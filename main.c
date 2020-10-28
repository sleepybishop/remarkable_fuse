#define FUSE_USE_VERSION 26

#include <assert.h>
#include <errno.h>
#include <fuse.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "cJSON.h"
#include "kstr.h"
#include "remfmt.h"
#include "remfs.h"

#define IS_SVG (1 << 0)
#define IS_ANNOT_DIR (1 << 1)
#define IS_ANNOT_PAGE (1 << 2)

#define DEFAULT_SOURCE "./xochitl"

static struct options {
  const char *src_dir;
  int show_help;
} options;

#define OPTION(t, p)                                                           \
  { t, offsetof(struct options, p), 1 }
static const struct fuse_opt option_spec[] = {
    OPTION("--source=%s", src_dir), OPTION("-h", show_help),
    OPTION("--help", show_help), FUSE_OPT_END};

static kstr munge_path(const char *path, int *flags) {
  kstr ret = {0, 0, 0};
  kstr_cat(&ret, "%s", (char *)path);
  char *svg_ext = strstr(ret.a, ".svg");
  if (svg_ext) {
    sprintf(svg_ext, "%s", ".rm");
  }
  char *annot_ext = strstr(ret.a, " Annotations");
  if (annot_ext) {
    if (annot_ext[12] == '\0') {
      annot_ext[0] = '\0';
    } else {
      memmove(annot_ext, annot_ext + 12, strlen(annot_ext + 12) + 1);
    }
  }

  if (flags) {
    *flags |= (svg_ext) ? IS_SVG : 0;
    *flags |= (annot_ext && annot_ext[12] == '\0') ? IS_ANNOT_DIR : 0;
    *flags |= (annot_ext) ? IS_ANNOT_PAGE : 0;
  }
  return ret;
}

static uuid_map_node *rewrite_path(remfs_ctx *ctx, const char *path, int *flags,
                                   kstr *newpath) {
  kstr munged = munge_path(path, flags);
  uuid_map_node *ref = remfs_path_search(ctx, munged.a);
  if (ref && newpath) {
    if (ref->file->type == COLLECTION) {
      kstr_cat(newpath, "%s", ctx->src_dir);
    } else {
      const char *exts[] = {"", "", ".epub", ".pdf", ".rm"};
      kstr_cat(newpath, "%s/", ctx->src_dir);
      if (ref->file->filetype == PAGE) {
        kstr_cat(newpath, "%s/", ref->file->parent);
      }
      if (*flags & IS_ANNOT_DIR) {
        kstr_cat(newpath, "%s", ref->file->uuid);
      } else {
        kstr_cat(newpath, "%s%s", ref->file->uuid, exts[ref->file->filetype]);
      }
    }
  }
  kv_destroy(munged);
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
    kstr newpath = {0, 0, 0};
    uuid_map_node *ref = rewrite_path(ctx, path, &flags, &newpath);
    if (newpath.a) {
      if (ref) {
        if ((flags & IS_SVG)) {
          if (ref->sh) {
            ret = fstat(fileno(ref->sh), stbuf);
          } else {
            ret = stat(newpath.a, stbuf);
            // hack for empty pages
            if (ret == 0)
              stbuf->st_size = 2 * 1024 * 1024;
          }
        } else {
          ret = stat(newpath.a, stbuf);
        }
      }
      kv_destroy(newpath);
    }
  }
  // force read only
  if (ret == 0) {
    stbuf->st_mode &= ~0200;
    stbuf->st_mode |= 0400;
  }
  // if (ret == -ENOENT) fprintf(stderr, "getattr failed: %s\n", path);
  return ret;
}

static cJSON *get_dir_entries(remfs_ctx *ctx, const char *path) {
  cJSON *n = NULL;
  if (strcmp(path, "/") == 0) {
    n = ctx->members;
  } else {
    int flags = 0;
    uuid_map_node *ref = rewrite_path(ctx, path, &flags, NULL);
    if (ref)
      n = ref->members;
  }
  return n;
}

static void fill_fake_svg(void *buf, fuse_fill_dir_t filler, remfs_file *file) {
  kstr tmp = {0, 0, 0};
  kstr_cat(&tmp, "%sXXXX", file->visible_name);
  sprintf(strstr(tmp.a, ".rm"), ".svg");
  filler(buf, tmp.a, NULL, 0);
  kv_destroy(tmp);
}

static void fill_fake_folder(void *buf, fuse_fill_dir_t filler,
                             remfs_file *file) {
  kstr tmp = {0, 0, 0};
  kstr_cat(&tmp, "%s Annotations", file->visible_name);
  filler(buf, tmp.a, NULL, 0);
  kv_destroy(tmp);
}

static int remfuse_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
                           off_t offset, struct fuse_file_info *fi) {
  struct fuse_context *fuse_ctx = fuse_get_context();
  remfs_ctx *ctx = (remfs_ctx *)fuse_ctx->private_data;

  cJSON *n = get_dir_entries(ctx, path);
  if (!n)
    return -ENOENT;

  filler(buf, ".", NULL, 0);
  filler(buf, "..", NULL, 0);

  cJSON *p = NULL;
  cJSON_ArrayForEach(p, n) {
    char *uuid = cJSON_GetStringValue(p);
    uuid_map_node *s = remfs_uuid_search(ctx, uuid);
    if (!s)
      continue;
    if (s->file->filetype == PAGE) {
      fill_fake_svg(buf, filler, s->file);
    }
    if (s->file->filetype == PDF || s->file->filetype == EPUB) {
      fill_fake_folder(buf, filler, s->file);
    }
    filler(buf, s->file->visible_name, NULL, 0);
  }
  return 0;
}

static int generate_fake_svg(uuid_map_node *ref, const char *rmpath,
                             bool anot) {
  char svgp[] = "/tmp/remfuse_svg_XXXXXX";
  int fd = mkstemp(svgp);
  FILE *in = NULL;

  if (fd == -1)
    return -1;
  in = fopen(rmpath, "rb");
  if (!in)
    return -1;
  ref->sh = fdopen(fd, "wb");
  remfmt_stroke_vec *strokes = remfmt_parse(in);
  if (ref->sh && strokes) {
    remfmt_render_params prm = {.landscape = ref->file->landscape,
                                .template_name = ref->file->template_name,
                                .annotation = anot,
                                .hilite_color = YELLOW,
                                .note_color = BLUE};
    remfmt_render_svg(ref->sh, strokes, &prm);
  }
  remfmt_stroke_cleanup(strokes);
  fclose(in);
  fflush(ref->sh);
  return fd;
}

static int remfuse_open(const char *path, struct fuse_file_info *fi) {
  struct fuse_context *fuse_ctx = fuse_get_context();
  remfs_ctx *ctx = (remfs_ctx *)fuse_ctx->private_data;
  int fd = -1, flags = 0;
  kstr newpath = {0, 0, 0};
  uuid_map_node *ref = rewrite_path(ctx, path, &flags, &newpath);
  if (ref && (flags & IS_SVG)) {
    fd = generate_fake_svg(ref, newpath.a, flags & IS_ANNOT_PAGE);
  } else {
    if (!newpath.a)
      return -1;
    fd = open(newpath.a, fi->flags);
  }
  kv_destroy(newpath);

  if (fd == -1)
    return -errno;

  fi->fh = fd;
  return 0;
}

static int remfuse_release(const char *path, struct fuse_file_info *fi) {
  struct fuse_context *fuse_ctx = fuse_get_context();
  remfs_ctx *ctx = (remfs_ctx *)fuse_ctx->private_data;
  int flags = 0;
  uuid_map_node *ref = rewrite_path(ctx, path, &flags, NULL);
  if (ref->sh) {
    fclose(ref->sh);
    ref->sh = NULL;
  } else {
    close(fi->fh);
  }
  return 0;
}

static int remfuse_read(const char *path, char *buf, size_t size, off_t offset,
                        struct fuse_file_info *fi) {
  int ret;
  ret = pread(fi->fh, buf, size, offset);
  if (ret == -1)
    ret = -errno;
  return ret;
}

static void *remfuse_init(struct fuse_conn_info *conn) {
  remfs_ctx *ctx = remfs_init(options.src_dir);
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
         "    --source=<s>        location of data dir\n"
         "                        (default: \"%s\")\n"
         "\n",
         DEFAULT_SOURCE);
}

int main(int argc, char *argv[]) {
  int ret = 1;
  struct fuse_args args = FUSE_ARGS_INIT(argc, argv);
  struct stat stbuf;

  options.src_dir = DEFAULT_SOURCE;

  if (fuse_opt_parse(&args, &options, option_spec, NULL) == -1)
    goto cleanup;

  if (options.show_help) {
    usage(argv[0]);
    assert(fuse_opt_add_arg(&args, "--help") == 0);
    args.argv[0][0] = '\0';
  } else if (options.src_dir) {
    if (stat(options.src_dir, &stbuf) == -1) {
      fprintf(stderr, "failed to access source dir [%s]: %s\n", options.src_dir,
              strerror(errno));
      goto cleanup;
    }
    if (!S_ISDIR(stbuf.st_mode)) {
      fprintf(stderr, "source dir [%s] is not a directory\n", options.src_dir);
      goto cleanup;
    }
    /*
    if (options.src_dir && options.src_dir[0] != '/') {
      fprintf(stderr, "source dir [%s] is not an absolute path\n",
              options.src_dir);
      goto cleanup;
    }
    */
  }
  ret = fuse_main(args.argc, args.argv, &remfuse_ops, NULL);
cleanup:
  fuse_opt_free_args(&args);
  return ret;
}
