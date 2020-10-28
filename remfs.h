#ifndef REMFS_H
#define REMFS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "kstr.h"
#include "tree.h"

#define RM_PATH_MAX 256

typedef enum { BAD_TYPE = 0, DOCUMENT = 1, COLLECTION = 2 } remfs_type;

typedef enum {
  BAD_FILETYPE = 0,
  NOTEBOOK = 1,
  EPUB = 2,
  PDF = 3,
  PAGE = 4,
} remfs_filetype;

typedef struct {
  // from .metadata
  char visible_name[RM_PATH_MAX];
  char template_name[RM_PATH_MAX];
  char uuid[64];
  char parent[64];
  remfs_type type;
  bool deleted;
  // from .content
  remfs_filetype filetype;
  bool landscape;
  size_t page_count;
  bool dummy;
} remfs_file;

typedef kvec_t(remfs_file) remfs_file_vec;

typedef struct uuid_map_node {
  remfs_file *file;
  char *path;
  cJSON *members;
  FILE *sh;
  RB_ENTRY(uuid_map_node) fwdp;
  RB_ENTRY(uuid_map_node) revp;
} uuid_map_node;

typedef RB_HEAD(uuid_fwd_map, uuid_map_node) uuid_fwd_map;
typedef RB_HEAD(uuid_rev_map, uuid_map_node) uuid_rev_map;

typedef struct {
  char *src_dir;
  remfs_file_vec fv;
  uuid_fwd_map *fwd_map;
  uuid_rev_map *rev_map;
  cJSON *members;
} remfs_ctx;

int remfs_list(const char *path, remfs_file_vec *fv);
remfs_ctx *remfs_init(const char *src_dir);
uuid_map_node *remfs_path_search(remfs_ctx *ctx, const char *path);
uuid_map_node *remfs_uuid_search(remfs_ctx *ctx, const char *uuid);
void remfs_destroy(remfs_ctx *arg);
void remfs_print(remfs_ctx *arg, FILE *stream);

#endif
