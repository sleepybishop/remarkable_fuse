#ifndef REMFS_H
#define REMFS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "kstr.h"

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

int remfs_list(const char *path, remfs_file_vec *fv);

#endif
