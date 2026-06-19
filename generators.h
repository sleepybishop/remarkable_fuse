#ifndef GENERATORS_H
#define GENERATORS_H

#include "cache.h"
#include "remfs.h"

bool pdf_has_annotations(remfs_ctx *ctx, uuid_map_node *ref);
bool has_annotations(const char *src_dir, remfs_file *file);
cache_entry *generate_annotated_pdf(remfs_ctx *ctx, uuid_map_node *ref,
                                    const char *orig_pdf_path);
cache_entry *generate_notebook_pdf(remfs_ctx *ctx, uuid_map_node *ref);
cache_entry *generate_fake_ext(uuid_map_node *ref, const char *rmpath,
                               bool anot, const char *ext);

#endif /* GENERATORS_H */
