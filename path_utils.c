#include "path_utils.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

sds munge_path(const char *path, int *flags) {
  sds ret = sdsnew(path);
  size_t len = sdslen(ret);
  bool is_svg = false;
  bool is_png = false;
  bool is_pdf = false;
  bool is_annotated_pdf = false;
  bool is_svg_dir = false;
  bool is_png_dir = false;
  bool is_pdf_dir = false;
  bool is_xoj = false;
  bool is_xoj_dir = false;

  if (len >= 4 && strcmp(ret + len - 4, "/svg") == 0) {
    ret[len - 4] = '\0';
    is_svg_dir = true;
  } else if (len >= 4 && strcmp(ret + len - 4, "/png") == 0) {
    ret[len - 4] = '\0';
    is_png_dir = true;
  } else if (len >= 4 && strcmp(ret + len - 4, "/pdf") == 0) {
    ret[len - 4] = '\0';
    is_pdf_dir = true;
  } else if (len >= 4 && strcmp(ret + len - 4, "/xoj") == 0) {
    ret[len - 4] = '\0';
    is_xoj_dir = true;
  } else {
    char *p;
    if ((p = strstr(ret, "/svg/")) != NULL) {
      memmove(p + 1, p + 5, strlen(p + 5) + 1);
    } else if ((p = strstr(ret, "/png/")) != NULL) {
      memmove(p + 1, p + 5, strlen(p + 5) + 1);
    } else if ((p = strstr(ret, "/pdf/")) != NULL) {
      memmove(p + 1, p + 5, strlen(p + 5) + 1);
    } else if ((p = strstr(ret, "/xoj/")) != NULL) {
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
  } else if (len >= 4 && strcmp(ret + len - 4, ".xoj") == 0) {
    ret[len - 4] = '\0';
    is_xoj = true;
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
    *flags |= is_xoj ? IS_XOJ : 0;
    *flags |= is_annotated_pdf ? IS_ANNOTATED_PDF : 0;
    *flags |= is_annot_dir ? IS_ANNOT_DIR : 0;
    *flags |= is_annot_page ? IS_ANNOT_PAGE : 0;
    *flags |= is_svg_dir ? IS_SVG_DIR : 0;
    *flags |= is_png_dir ? IS_PNG_DIR : 0;
    *flags |= is_pdf_dir ? IS_PDF_DIR : 0;
    *flags |= is_xoj_dir ? IS_XOJ_DIR : 0;
  }
  return ret;
}

uuid_map_node *rewrite_path(remfs_ctx *ctx, const char *path, int *flags,
                            sds *newpath) {
  sds munged = munge_path(path, flags);
  uuid_map_node *ref = remfs_path_search(ctx, munged);
  if (!ref && (*flags & (IS_SVG | IS_PNG | IS_PDF | IS_XOJ))) {
    ref = remfs_path_search(ctx, path);
    if (ref) {
      *flags &= ~(IS_SVG | IS_PNG | IS_PDF | IS_XOJ | IS_ANNOTATED_PDF);
    }
  }
  if (ref && ref->file->filetype != PAGE) {
    if ((ref->file->filetype == PDF || ref->file->filetype == NOTEBOOK) &&
        (*flags & IS_PDF) && enable_pdf) {
      // Keep IS_PDF flag
    } else {
      *flags &= ~(IS_SVG | IS_PNG | IS_PDF | IS_XOJ | IS_ANNOTATED_PDF);
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
    if (!enable_png && !enable_svg && !enable_mutable && !enable_xoj) {
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

void gen_uuid(char *buf) {
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

void get_parent_and_name(const char *path, sds *parent_path, sds *name) {
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

uint8_t *slurp(const char *path) {
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

bool is_path_virtual(const char *path) {
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
