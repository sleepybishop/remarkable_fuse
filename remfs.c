#include <glob.h>
#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "remfs.h"
#include "struct.h"

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
