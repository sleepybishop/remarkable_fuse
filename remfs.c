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
  for (n = RB_MIN(uuid_rev_map, ctx->rev_map); n != NULL; n = nxt) {
    nxt = RB_NEXT(uuid_rev_map, ctx->rev_map, n);
    RB_REMOVE(uuid_rev_map, ctx->rev_map, n);
    cJSON_Delete(n->members);
    free(n->path);
    free(n);
  }
  free(ctx->rev_map);
  free(ctx->fwd_map);
}

void remfs_destroy(remfs_ctx *ctx) {
  if (ctx) {
    empty_maps(ctx);
    cJSON_Delete(ctx->members);
    kv_destroy(ctx->fv);
    free(ctx->src_dir);
    free(ctx);
  }
}

static char *gen_path(remfs_ctx *ctx, size_t idx) {
  remfs_file *file = &kv_A(ctx->fv, idx);
  kvec_t(char *) stk = {0, 0, 0};
  kstr fp = {0, 0, 0};

  if (file->parent[0] == '\0') {
    kstr_cat(&fp, "/%s", file->visible_name);
  } else {
    kv_push(char *, stk, file->visible_name);
    uuid_map_node *ref = remfs_uuid_search(ctx, file->parent);
    while (ref != NULL) {
      kv_push(char *, stk, ref->file->visible_name);
      ref = remfs_uuid_search(ctx, ref->file->parent);
    }
    while (kv_size(stk) > 0)
      kstr_cat(&fp, "/%s", kv_pop(stk));
  }
  kv_destroy(stk);
  return fp.a;
}

static void add_member(remfs_ctx *ctx, size_t idx, cJSON *root) {
  remfs_file *file = &kv_A(ctx->fv, idx);

  if (file->parent[0] == '\0') {
    cJSON *p = cJSON_CreateString(file->uuid);
    cJSON_AddItemToArray(root, p);
  } else {
    cJSON *n = NULL;
    uuid_map_node *ref = remfs_uuid_search(ctx, file->parent);
    if (!ref)
      return;
    n = ref->members;
    cJSON *p = cJSON_CreateString(file->uuid);
    if (!n) {
      n = cJSON_CreateArray();
      ref->members = n;
    }
    cJSON_AddItemToArray(n, p);
  }
}

remfs_ctx *remfs_init(const char *src_dir) {
  remfs_ctx *ctx = calloc(1, sizeof(remfs_ctx));
  ctx->src_dir = strdup(src_dir);
  struct stat stbuf;
  kstr tmp = {0, 0, 0};

  remfs_list(ctx->src_dir, &ctx->fv);
  ctx->fwd_map = calloc(1, sizeof(uuid_fwd_map));
  ctx->rev_map = calloc(1, sizeof(uuid_rev_map));

  RB_INIT(ctx->fwd_map);
  RB_INIT(ctx->rev_map);

  ctx->members = cJSON_CreateArray();
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
  }

  for (int i = 0; i < kv_size(ctx->fv); i++) {
    remfs_file *file = &kv_A(ctx->fv, i);
    uuid_map_node *s = remfs_uuid_search(ctx, file->uuid);
    if (s == NULL)
      continue;
    s->path = gen_path(ctx, i);
    RB_INSERT(uuid_rev_map, ctx->rev_map, s);
    add_member(ctx, i, ctx->members);
  }
  kv_destroy(tmp);
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
  size_t meta_size = 0;
  fseek(f, 0, SEEK_END);
  meta_size = ftell(f);
  fseek(f, 0, SEEK_SET);
  if (meta_size <= 0)
    goto cleanup;
  ret = malloc(meta_size + 1);
  if (!ret)
    goto cleanup;
  fread(ret, 1, meta_size, f);
  ret[meta_size] = '\0';
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

  snprintf(file->uuid, 36 + 1, strstr(path, ".metadata") - 36);
  p = cJSON_GetObjectItem(json, "visibleName");
  snprintf(file->visible_name, RM_PATH_MAX, "%s", cJSON_GetStringValue(p));

  p = cJSON_GetObjectItem(json, "parent");
  snprintf(file->parent, 64, cJSON_GetStringValue(p));

  p = cJSON_GetObjectItem(json, "type");
  str = cJSON_GetStringValue(p);
  if (strcmp(str, "DocumentType") == 0) {
    file->type = DOCUMENT;
  } else if (strcmp(str, "CollectionType") == 0) {
    file->type = COLLECTION;
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
  while (!feof(in)) {
    char buf[RM_PATH_MAX] = {0};
    char *line = fgets(buf, 256, in);
    if (line) {
      size_t len = strlen(line);
      if (len >= 64)
        len = 64 - 1;
      remfs_file *p = &kv_A(*fv, pg);
      snprintf(p->template_name, len, "%s", line);
      pg++;
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
  str = cJSON_GetStringValue(p);
  if (strcmp(str, "notebook") == 0) {
    file->filetype = NOTEBOOK;
  } else if (strcmp(str, "epub") == 0) {
    file->filetype = EPUB;
  } else if (strcmp(str, "pdf") == 0) {
    file->filetype = PDF;
  }

  p = cJSON_GetObjectItem(json, "dummyDocument");
  file->dummy = cJSON_IsTrue(p) ? true : false;

  p = cJSON_GetObjectItem(json, "orientation");
  str = cJSON_GetStringValue(p);
  file->landscape = (strcmp(str, "landscape") == 0);

  p = cJSON_GetObjectItem(json, "pageCount");
  file->page_count = cJSON_GetNumberValue(p);

  kv_push(remfs_file, *fv, *file);
  unsigned pn = 0;
  p = cJSON_GetObjectItem(json, "pages");
  cJSON *n = NULL;
  cJSON_ArrayForEach(n, p) {
    remfs_file page = {.type = DOCUMENT,
                       .filetype = PAGE,
                       .page_count = 1,
                       .deleted = file->deleted,
                       .dummy = file->dummy,
                       .landscape = file->landscape};
    snprintf(page.uuid, 64, "%s", cJSON_GetStringValue(n));
    snprintf(page.parent, 64, "%s", file->uuid);
    snprintf(page.visible_name, RM_PATH_MAX, "page_%06u.rm", 1 + pn++);

    kv_push(remfs_file, *fv, page);
  }

  cJSON_Delete(json);
cleanup:
  free(raw);
}

int remfs_list(const char *path, remfs_file_vec *fv) {
  glob_t globbuf;
  kstr glob_path = {0, 0, 0};
  remfs_file file;

  kstr_cat(&glob_path, "%s/*.metadata", path);
  globbuf.gl_offs = 0;
  glob(glob_path.a, GLOB_NOSORT | GLOB_DOOFFS, NULL, &globbuf);

  for (int i = 0; i < globbuf.gl_pathc; i++) {
    memset(&file, 0, sizeof(remfs_file));
    parse_meta(globbuf.gl_pathv[i], &file);
    if (file.type == DOCUMENT) {
      glob_path.n = 0;
      kstr_cat(&glob_path, "%s/%s.content", path, file.uuid);
      parse_content(glob_path.a, &file, fv);
      glob_path.n = 0;
      kstr_cat(&glob_path, "%s/%s.pagedata", path, file.uuid);
      parse_pagedata(glob_path.a, &file, fv);
    } else {
      kv_push(remfs_file, *fv, file);
    }
  }
  kv_destroy(glob_path);
  globfree(&globbuf);
  return 0;
}
