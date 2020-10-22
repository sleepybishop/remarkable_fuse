#define FUSE_USE_VERSION 26

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
#include "tree.h"

typedef struct uuid_map_node {
  remfs_file *file;
  char *path;
  FILE *sh;
  int flags;
  RB_ENTRY(uuid_map_node) fwdp;
  RB_ENTRY(uuid_map_node) revp;
} uuid_map_node;

typedef RB_HEAD(uuid_fwd_map, uuid_map_node) uuid_fwd_map;
typedef RB_HEAD(uuid_rev_map, uuid_map_node) uuid_rev_map;

static int uuid_map_node_fwd_cmp(const uuid_map_node *a,
                                 const uuid_map_node *b) {
  return strcmp(a->file->uuid, b->file->uuid);
}

static int uuid_map_node_rev_cmp(const uuid_map_node *a,
                                 const uuid_map_node *b) {
  return strcmp(a->path, b->path);
}

RB_GENERATE(uuid_fwd_map, uuid_map_node, fwdp, uuid_map_node_fwd_cmp);
RB_GENERATE(uuid_rev_map, uuid_map_node, revp, uuid_map_node_rev_cmp);

typedef struct {
  char *src_dir;
  remfs_file_vec fv;
  uuid_fwd_map *fwd_map;
  uuid_rev_map *rev_map;
  cJSON *tree;
} remfs_ctx;

#define IS_SVG (1 << 0)
#define IS_ANNOT_DIR (1 << 1)
#define IS_ANNOT_PAGE (1 << 2)

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

static uuid_map_node *path_search(remfs_ctx *ctx, const char *path, kstr *ret) {
  int flags = 0;
  kstr qp = munge_path(path, &flags);
  uuid_map_node *s, q;
  q.path = qp.a;
  s = RB_FIND(uuid_rev_map, ctx->rev_map, &q);
  if (s && ret) {
    s->flags = flags;
    if (s->file->type == COLLECTION) {
      kstr_cat(ret, "%s", ctx->src_dir);
    } else {
      const char *exts[] = {"", "", ".epub", ".pdf", ".rm"};
      kstr_cat(ret, "%s/", ctx->src_dir);
      if (s->file->filetype == PAGE) {
        kstr_cat(ret, "%s/", s->file->parent);
      }
      if (flags & IS_ANNOT_DIR) {
        kstr_cat(ret, "%s", s->file->uuid);
      } else {
        kstr_cat(ret, "%s%s", s->file->uuid, exts[s->file->filetype]);
      }
    }
  }
  if (qp.a)
    kv_destroy(qp);
  return s;
}

static uuid_map_node *uuid_search(remfs_ctx *ctx, const char *uuid) {
  remfs_file file = {0};
  uuid_map_node q = {.file = &file};
  sprintf(file.uuid, "%s", uuid);
  return RB_FIND(uuid_fwd_map, ctx->fwd_map, &q);
}

static int remfs_getattr(const char *path, struct stat *stbuf) {
  int ret = -ENOENT;
  struct fuse_context *fuse_ctx = fuse_get_context();
  remfs_ctx *ctx = (remfs_ctx *)fuse_ctx->private_data;
  memset(stbuf, 0, sizeof(struct stat));
  if (strcmp(path, "/") == 0) {
    ret = stat(ctx->src_dir, stbuf);
  } else {
    kstr munged = {0, 0, 0};
    uuid_map_node *ref = path_search(ctx, path, &munged);
    if (munged.a) {
      if (ref) {
        if ((ref->flags & IS_SVG)) {
          if (ref->sh) {
            ret = fstat(fileno(ref->sh), stbuf);
          } else {
            ret = stat(munged.a, stbuf);
            // hack for empty pages
            if (ret == 0)
              stbuf->st_size = 2 * 1024 * 1024;
          }
        } else {
          ret = stat(munged.a, stbuf);
        }
      }
      kv_destroy(munged);
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
    n = cJSON_GetObjectItem(ctx->tree, "root");
  } else {
    uuid_map_node *ref = path_search(ctx, path, NULL);
    if (ref)
      n = cJSON_GetObjectItem(ctx->tree, ref->file->uuid);
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

static int remfs_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
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
    uuid_map_node *s = uuid_search(ctx, uuid);
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
  char svgp[] = "/tmp/remfs_svg_XXXXXX";
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
    remfmt_render(ref->sh, strokes, &prm);
  }
  remfmt_stroke_cleanup(strokes);
  fclose(in);
  fflush(ref->sh);
  return fd;
}

static int remfs_open(const char *path, struct fuse_file_info *fi) {
  struct fuse_context *fuse_ctx = fuse_get_context();
  remfs_ctx *ctx = (remfs_ctx *)fuse_ctx->private_data;
  int fd = -1;
  kstr munged = {0, 0, 0};
  uuid_map_node *ref = path_search(ctx, path, &munged);
  if (ref && (ref->flags & IS_SVG)) {
    fd = generate_fake_svg(ref, munged.a, ref->flags & IS_ANNOT_PAGE);
  } else {
    if (!munged.a)
      return -1;
    fd = open(munged.a, fi->flags);
  }
  kv_destroy(munged);

  if (fd == -1)
    return -errno;

  fi->fh = fd;
  return 0;
}

static int remfs_release(const char *path, struct fuse_file_info *fi) {
  struct fuse_context *fuse_ctx = fuse_get_context();
  remfs_ctx *ctx = (remfs_ctx *)fuse_ctx->private_data;
  uuid_map_node *ref = path_search(ctx, path, NULL);
  if (ref->sh) {
    fclose(ref->sh);
    ref->sh = NULL;
  } else {
    close(fi->fh);
  }
  return 0;
}

static int remfs_read(const char *path, char *buf, size_t size, off_t offset,
                      struct fuse_file_info *fi) {
  int ret;
  ret = pread(fi->fh, buf, size, offset);
  if (ret == -1)
    ret = -errno;
  return ret;
}

static char *gen_path(remfs_ctx *ctx, size_t idx) {
  remfs_file *file = &kv_A(ctx->fv, idx);
  kvec_t(char *) stk = {0, 0, 0};
  kstr fp = {0, 0, 0};

  if (file->parent[0] == '\0') {
    kstr_cat(&fp, "/%s", file->visible_name);
  } else {
    kv_push(char *, stk, file->visible_name);
    uuid_map_node *ref = uuid_search(ctx, file->parent);
    while (ref != NULL) {
      kv_push(char *, stk, ref->file->visible_name);
      ref = uuid_search(ctx, ref->file->parent);
    }
    while (kv_size(stk) > 0)
      kstr_cat(&fp, "/%s", kv_pop(stk));
  }
  kv_destroy(stk);
  return fp.a;
}

static void add_to_tree(remfs_ctx *ctx, size_t idx, cJSON *root) {
  remfs_file *file = &kv_A(ctx->fv, idx);

  cJSON *p = cJSON_CreateString(file->uuid);
  if (file->parent[0] == '\0') {
    cJSON_AddItemToArray(root, p);
  } else {
    cJSON *n = cJSON_GetObjectItem(ctx->tree, file->parent);
    if (!n) {
      n = cJSON_CreateArray();
      cJSON_AddItemToObject(ctx->tree, file->parent, n);
    }
    cJSON_AddItemToArray(n, p);
  }
}

static void *remfs_init(struct fuse_conn_info *conn) {
  remfs_ctx *ctx = calloc(1, sizeof(remfs_ctx));
  ctx->src_dir = strdup("./xochitl"); // FIXME CONFIG
  struct stat stbuf;
  kstr tmp = {0, 0, 0};

  remfs_list(ctx->src_dir, &ctx->fv);
  ctx->fwd_map = calloc(1, sizeof(uuid_fwd_map));
  ctx->rev_map = calloc(1, sizeof(uuid_rev_map));

  RB_INIT(ctx->fwd_map);
  RB_INIT(ctx->rev_map);

  ctx->tree = cJSON_CreateObject();

  cJSON *root = cJSON_CreateArray();
  cJSON_AddItemToObject(ctx->tree, "root", root);
  for (int i = 0; i < kv_size(ctx->fv); i++) {
    remfs_file *file = &kv_A(ctx->fv, i);
    if (file->deleted)
      continue;
    tmp.n = 0;
    if (file->filetype == PAGE) {
      kstr_cat(&tmp, "%s/%s/%s.rm", ctx->src_dir, file->parent, file->uuid);
    } else {
      kstr_cat(&tmp, "%s/%s.metadata", ctx->src_dir, file->uuid);
    }
    if (stat(tmp.a, &stbuf) == -1)
      continue;
    uuid_map_node *s = calloc(1, sizeof(uuid_map_node));
    s->file = file;
    RB_INSERT(uuid_fwd_map, ctx->fwd_map, s);
    add_to_tree(ctx, i, root);
  }

  for (int i = 0; i < kv_size(ctx->fv); i++) {
    remfs_file *file = &kv_A(ctx->fv, i);
    uuid_map_node *s = uuid_search(ctx, file->uuid);
    if (s == NULL)
      continue;
    s->path = gen_path(ctx, i);
    RB_INSERT(uuid_rev_map, ctx->rev_map, s);
  }
  kv_destroy(tmp);
  /*
  uuid_map_node *n;
  RB_FOREACH(n, uuid_rev_map, ctx->rev_map)
  fprintf(stderr, "%s->%s\n", n->path, n->file->uuid);
  */
  return ctx;
}

static void destroy_maps(remfs_ctx *ctx) {
  uuid_map_node *n, *nxt;
  for (n = RB_MIN(uuid_rev_map, ctx->rev_map); n != NULL; n = nxt) {
    nxt = RB_NEXT(uuid_rev_map, ctx->rev_map, n);
    RB_REMOVE(uuid_rev_map, ctx->rev_map, n);
    free(n->path);
    free(n);
  }
  free(ctx->rev_map);
  free(ctx->fwd_map);
}

static void remfs_destroy(void *arg) {
  remfs_ctx *ctx = (remfs_ctx *)arg;
  if (ctx) {
    destroy_maps(ctx);
    cJSON_Delete(ctx->tree);
    kv_destroy(ctx->fv);
    free(ctx->src_dir);
    free(ctx);
  }
}

static struct fuse_operations remfs_ops = {
    .getattr = remfs_getattr,
    .readdir = remfs_readdir,
    .open = remfs_open,
    .release = remfs_release,
    .read = remfs_read,
    .init = remfs_init,
    .destroy = remfs_destroy,
};

int main(int argc, char *argv[]) {
  int ret;
  struct fuse_args args = FUSE_ARGS_INIT(argc, argv);
  ret = fuse_main(argc, argv, &remfs_ops, NULL);
  fuse_opt_free_args(&args);
  return ret;
}
