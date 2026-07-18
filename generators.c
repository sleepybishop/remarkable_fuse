#include "generators.h"
#include "path_utils.h"
#include "pdfoverlay.h"
#include "remfmt.h"
#include "remfuse.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

bool pdf_has_annotations(remfs_ctx *ctx, uuid_map_node *ref) {
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

bool has_annotations(const char *src_dir, remfs_file *file) {
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

cache_entry *generate_annotated_pdf(remfs_ctx *ctx, uuid_map_node *ref,
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
            sds asset_dir = sdscatprintf(sdsempty(), "%s/%s", ctx->src_dir,
                                         ref->file->uuid);
            remfmt_render_params prm = {.landscape = page->file->landscape,
                                        .template_name =
                                            page->file->template_name,
                                        .template_dir = template_dir,
                                        .annotation = true,
                                        .asset_dir = asset_dir};
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
            sdsfree(asset_dir);
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

cache_entry *generate_notebook_pdf(remfs_ctx *ctx, uuid_map_node *ref) {
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
      pages_prms[idx]->asset_dir =
          sdscatprintf(sdsempty(), "%s/%s", ctx->src_dir, ref->file->uuid);
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
      if (pages_prms[i]->asset_dir) {
        sdsfree(pages_prms[i]->asset_dir);
      }
      free(pages_prms[i]);
    }
  }
  free(pages_strokes);
  free(pages_prms);

  return add_to_cache(ref->file->uuid, "pdf", latest_mtime, data, size);
}

cache_entry *generate_fake_ext(uuid_map_node *ref, const char *rmpath,
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
    char *last_slash = strrchr(rmpath, '/');
    sds asset_dir = NULL;
    if (last_slash != NULL) {
      size_t len = last_slash - rmpath;
      asset_dir = sdsnewlen(rmpath, len);
    }
    remfmt_render_params prm = {.landscape = ref->file->landscape,
                                .template_name = ref->file->template_name,
                                .template_dir = template_dir,
                                .annotation = anot,
                                .asset_dir = asset_dir};
    if (strcmp(ext, "svg") == 0) {
      remfmt_render_svg(sh, strokes, &prm);
    } else if (strcmp(ext, "pdf") == 0) {
      remfmt_render_pdf(sh, strokes, &prm);
    } else if (strcmp(ext, "xoj") == 0) {
      remfmt_render_xoj(sh, strokes, &prm);
    } else {
      remfmt_render_png(sh, strokes, &prm);
    }
    if (asset_dir) {
      sdsfree(asset_dir);
    }
    remfmt_stroke_cleanup(strokes);
  }
  fclose(sh);

  return add_to_cache(ref->file->uuid, ext, st.st_mtime, data, size);
}
