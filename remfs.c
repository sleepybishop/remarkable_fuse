#include <glob.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "cJSON.h"
#include "remfs.h"
#include "struct.h"

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

uuid_map_node *remfs_path_search(remfs_ctx *ctx, const char *path) {
  uuid_map_node q = {.path = (char *)path};
  return RB_FIND(uuid_rev_map, ctx->rev_map, &q);
}

uuid_map_node *remfs_uuid_search(remfs_ctx *ctx, const char *uuid) {
  remfs_file file = {0};
  uuid_map_node q = {.file = &file};
  sprintf(file.uuid, "%s", uuid);
  return RB_FIND(uuid_fwd_map, ctx->fwd_map, &q);
}

static void empty_maps(remfs_ctx *ctx) {
  uuid_map_node *n, *nxt;
  for (n = RB_MIN(uuid_fwd_map, ctx->fwd_map); n != NULL; n = nxt) {
    nxt = RB_NEXT(uuid_fwd_map, ctx->fwd_map, n);
    RB_REMOVE(uuid_fwd_map, ctx->fwd_map, n);
    kv_destroy(n->children);
    sdsfree(n->path);
    free(n);
  }
}

void remfs_destroy(remfs_ctx *ctx) {
  if (ctx) {
    empty_maps(ctx);
    free(ctx->fwd_map);
    free(ctx->rev_map);
    kv_destroy(ctx->fv);
    kv_destroy(ctx->root_children);
    free(ctx->src_dir);
    free(ctx);
  }
}

static char *gen_path(remfs_ctx *ctx, size_t idx) {
  remfs_file *file = &kv_A(ctx->fv, idx);
  kvec_t(char *) stk = {0, 0, 0};
  sds fp = sdsempty();

  if (file->parent[0] == '\0') {
    fp = sdscatprintf(fp, "/%s", file->visible_name);
  } else {
    kv_push(char *, stk, file->visible_name);
    uuid_map_node *ref = remfs_uuid_search(ctx, file->parent);
    int depth = 0;
    while (ref != NULL && depth < 64) {
      kv_push(char *, stk, ref->file->visible_name);
      ref = remfs_uuid_search(ctx, ref->file->parent);
      depth++;
    }
    while (kv_size(stk) > 0)
      fp = sdscatprintf(fp, "/%s", kv_pop(stk));
  }
  kv_destroy(stk);
  return fp;
}

static void add_member(remfs_ctx *ctx, uuid_map_node *s) {
  if (s->file->parent[0] == '\0') {
    kv_push(uuid_map_node *, ctx->root_children, s);
  } else {
    uuid_map_node *ref = remfs_uuid_search(ctx, s->file->parent);
    if (!ref) {
      kv_push(uuid_map_node *, ctx->root_children, s);
      return;
    }
    kv_push(uuid_map_node *, ref->children, s);
  }
}

remfs_ctx *remfs_init(const char *src_dir) {
  remfs_ctx *ctx = calloc(1, sizeof(remfs_ctx));
  ctx->src_dir = strdup(src_dir);
  struct stat stbuf;
  sds tmp = sdsempty();

  remfs_list(ctx->src_dir, &ctx->fv);
  ctx->fwd_map = calloc(1, sizeof(uuid_fwd_map));
  ctx->rev_map = calloc(1, sizeof(uuid_rev_map));

  RB_INIT(ctx->fwd_map);
  RB_INIT(ctx->rev_map);
  for (int i = 0; i < kv_size(ctx->fv); i++) {
    remfs_file *file = &kv_A(ctx->fv, i);
    if (file->deleted)
      continue;
    sdsclear(tmp);
    if (file->filetype == PAGE) {
      tmp = sdscatprintf(tmp, "%s/%s/%s.rm", ctx->src_dir, file->parent,
                         file->uuid);
    } else {
      tmp = sdscatprintf(tmp, "%s/%s.metadata", ctx->src_dir, file->uuid);
    }
    if (stat(tmp, &stbuf) == -1)
      continue;
    uuid_map_node *s = calloc(1, sizeof(uuid_map_node));
    s->file = file;
    if (RB_INSERT(uuid_fwd_map, ctx->fwd_map, s) != NULL) {
      free(s);
    }
  }

  for (int i = 0; i < kv_size(ctx->fv); i++) {
    remfs_file *file = &kv_A(ctx->fv, i);
    uuid_map_node *s = remfs_uuid_search(ctx, file->uuid);
    if (s == NULL)
      continue;
    if (s->path == NULL) {
      s->path = gen_path(ctx, i);
      if (RB_INSERT(uuid_rev_map, ctx->rev_map, s) != NULL) {
        sdsfree(s->path);
        s->path = NULL;
      } else {
        add_member(ctx, s);
      }
    }
  }
  sdsfree(tmp);
  return ctx;
}

void remfs_print(remfs_ctx *ctx, FILE *stream) {
  uuid_map_node *n;
  RB_FOREACH(n, uuid_rev_map, ctx->rev_map)
  fprintf(stream, "%s->%s\n", n->path, n->file->uuid);
  fflush(stream);
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

static void parse_meta(const char *path, remfs_file *file) {
  cJSON *json, *p;
  char *str;
  uint8_t *raw = slurp(path);
  if (!raw)
    return;
  json = cJSON_Parse((const char *)raw);
  if (!json)
    goto cleanup;

  const char *filename = strrchr(path, '/');
  if (filename) {
    filename++;
  } else {
    filename = path;
  }
  snprintf(file->uuid, sizeof(file->uuid), "%.36s", filename);

  p = cJSON_GetObjectItem(json, "visibleName");
  if (cJSON_IsString(p)) {
    snprintf(file->visible_name, RM_PATH_MAX, "%s", cJSON_GetStringValue(p));
    for (char *c = file->visible_name; *c; c++) {
      if (*c == '/')
        *c = '_';
    }
  } else {
    snprintf(file->visible_name, RM_PATH_MAX, "Untitled");
  }

  p = cJSON_GetObjectItem(json, "parent");
  if (cJSON_IsString(p)) {
    snprintf(file->parent, sizeof(file->parent), "%s", cJSON_GetStringValue(p));
  } else {
    file->parent[0] = '\0';
  }

  p = cJSON_GetObjectItem(json, "type");
  if (cJSON_IsString(p)) {
    str = cJSON_GetStringValue(p);
    if (strcmp(str, "DocumentType") == 0) {
      file->type = DOCUMENT;
    } else if (strcmp(str, "CollectionType") == 0) {
      file->type = COLLECTION;
    }
  }

  p = cJSON_GetObjectItem(json, "deleted");
  file->deleted = cJSON_IsTrue(p) ? true : false;

  cJSON_Delete(json);
cleanup:
  free(raw);
}

static void parse_pagedata(const char *path, remfs_file *file,
                           remfs_file_vec *fv) {
  FILE *in = fopen(path, "r");
  if (!in)
    return;
  int pg = kv_size(*fv) - file->page_count;
  if (pg < 0 || pg >= kv_size(*fv)) {
    fclose(in);
    return;
  }
  while (!feof(in)) {
    char buf[RM_PATH_MAX] = {0};
    char *line = fgets(buf, sizeof(buf), in);
    if (!line)
      break;
    if (line) {
      size_t len = strlen(line);
      while (len > 0 && (line[len - 1] == '\r' || line[len - 1] == '\n' ||
                         line[len - 1] == ' ' || line[len - 1] == '\t')) {
        line[len - 1] = '\0';
        len--;
      }
      if (len > 0) {
        if (pg >= kv_size(*fv)) {
          break;
        }
        remfs_file *p = &kv_A(*fv, pg);
        snprintf(p->template_name, sizeof(p->template_name), "%s", line);
        pg++;
      }
    }
  }
  fclose(in);
}

static void parse_content(const char *path, remfs_file *file,
                          remfs_file_vec *fv) {
  cJSON *json, *p;
  char *str;
  uint8_t *raw = slurp(path);
  if (!raw)
    return;
  json = cJSON_Parse((const char *)raw);
  if (!json)
    goto cleanup;

  p = cJSON_GetObjectItem(json, "fileType");
  if (cJSON_IsString(p)) {
    str = cJSON_GetStringValue(p);
    if (strcmp(str, "notebook") == 0) {
      file->filetype = NOTEBOOK;
    } else if (strcmp(str, "epub") == 0) {
      file->filetype = EPUB;
    } else if (strcmp(str, "pdf") == 0) {
      file->filetype = PDF;
    }
  }

  p = cJSON_GetObjectItem(json, "dummyDocument");
  file->dummy = cJSON_IsTrue(p) ? true : false;

  p = cJSON_GetObjectItem(json, "orientation");
  if (cJSON_IsString(p)) {
    str = cJSON_GetStringValue(p);
    file->landscape = (strcmp(str, "landscape") == 0);
  }

  p = cJSON_GetObjectItem(json, "margins");
  if (cJSON_IsNumber(p)) {
    file->margins = p->valueint;
  } else {
    file->margins = 0;
  }

  p = cJSON_GetObjectItem(json, "customZoomScale");
  if (cJSON_IsNumber(p)) {
    file->custom_zoom_scale = p->valuedouble;
  } else {
    file->custom_zoom_scale = 0.0;
  }

  p = cJSON_GetObjectItem(json, "customZoomPageHeight");
  if (cJSON_IsNumber(p)) {
    file->custom_zoom_page_height = p->valueint;
  } else {
    file->custom_zoom_page_height = 0;
  }

  p = cJSON_GetObjectItem(json, "customZoomPageWidth");
  if (cJSON_IsNumber(p)) {
    file->custom_zoom_page_width = p->valueint;
  } else {
    file->custom_zoom_page_width = 0;
  }

  size_t doc_idx = kv_size(*fv);
  kv_push(remfs_file, *fv, *file);

  unsigned pn = 0;
  p = cJSON_GetObjectItem(json, "pages");
  if (!p) {
    cJSON *cPages = cJSON_GetObjectItem(json, "cPages");
    if (cPages) {
      p = cJSON_GetObjectItem(cPages, "pages");
    }
  }
  if (cJSON_IsArray(p)) {
    cJSON *n = NULL;
    cJSON_ArrayForEach(n, p) {
      const char *page_uuid = NULL;
      if (cJSON_IsString(n)) {
        page_uuid = cJSON_GetStringValue(n);
      } else if (cJSON_IsObject(n)) {
        cJSON *id_obj = cJSON_GetObjectItem(n, "id");
        if (cJSON_IsString(id_obj)) {
          page_uuid = cJSON_GetStringValue(id_obj);
        }
      }
      if (page_uuid) {
        remfs_file page = {.type = DOCUMENT,
                           .filetype = PAGE,
                           .page_count = 1,
                           .deleted = file->deleted,
                           .dummy = file->dummy,
                           .landscape = file->landscape};
        snprintf(page.uuid, sizeof(page.uuid), "%s", page_uuid);
        snprintf(page.parent, sizeof(page.parent), "%s", file->uuid);
        snprintf(page.visible_name, RM_PATH_MAX, "page_%06u", 1 + pn++);

        kv_push(remfs_file, *fv, page);
      }
    }
  }

  kv_A(*fv, doc_idx).page_count = pn;
  file->page_count = pn;

  cJSON_Delete(json);
cleanup:
  free(raw);
}

int remfs_list(const char *path, remfs_file_vec *fv) {
  glob_t globbuf;
  sds glob_path = sdsempty();
  remfs_file file;

  glob_path = sdscatprintf(glob_path, "%s/*.metadata", path);
  globbuf.gl_offs = 0;
  glob(glob_path, GLOB_NOSORT | GLOB_DOOFFS, NULL, &globbuf);

  for (int i = 0; i < globbuf.gl_pathc; i++) {
    memset(&file, 0, sizeof(remfs_file));
    parse_meta(globbuf.gl_pathv[i], &file);
    if (file.type == DOCUMENT) {
      sdsclear(glob_path);
      glob_path = sdscatprintf(glob_path, "%s/%s.content", path, file.uuid);
      parse_content(glob_path, &file, fv);
      sdsclear(glob_path);
      glob_path = sdscatprintf(glob_path, "%s/%s.pagedata", path, file.uuid);
      parse_pagedata(glob_path, &file, fv);
    } else {
      kv_push(remfs_file, *fv, file);
    }
  }
  sdsfree(glob_path);
  globfree(&globbuf);
  return 0;
}

void remfs_reload(remfs_ctx *ctx) {
  empty_maps(ctx);
  kv_destroy(ctx->fv);
  kv_destroy(ctx->root_children);

  memset(&ctx->fv, 0, sizeof(ctx->fv));
  memset(&ctx->root_children, 0, sizeof(ctx->root_children));
  RB_INIT(ctx->fwd_map);
  RB_INIT(ctx->rev_map);

  sds tmp = sdsempty();
  struct stat stbuf;
  remfs_list(ctx->src_dir, &ctx->fv);

  for (int i = 0; i < kv_size(ctx->fv); i++) {
    remfs_file *file = &kv_A(ctx->fv, i);
    if (file->deleted)
      continue;
    sdsclear(tmp);
    if (file->filetype == PAGE) {
      tmp = sdscatprintf(tmp, "%s/%s/%s.rm", ctx->src_dir, file->parent,
                         file->uuid);
    } else {
      tmp = sdscatprintf(tmp, "%s/%s.metadata", ctx->src_dir, file->uuid);
    }
    if (stat(tmp, &stbuf) == -1)
      continue;
    uuid_map_node *s = calloc(1, sizeof(uuid_map_node));
    s->file = file;
    if (RB_INSERT(uuid_fwd_map, ctx->fwd_map, s) != NULL) {
      free(s);
    }
  }

  for (int i = 0; i < kv_size(ctx->fv); i++) {
    remfs_file *file = &kv_A(ctx->fv, i);
    uuid_map_node *s = remfs_uuid_search(ctx, file->uuid);
    if (s == NULL)
      continue;
    if (s->path == NULL) {
      s->path = gen_path(ctx, i);
      if (RB_INSERT(uuid_rev_map, ctx->rev_map, s) != NULL) {
        sdsfree(s->path);
        s->path = NULL;
      } else {
        add_member(ctx, s);
      }
    }
  }
  sdsfree(tmp);
}
