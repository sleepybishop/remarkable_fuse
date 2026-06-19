#ifndef REMFUSE_H
#define REMFUSE_H

#include "remfs.h"
#include <pthread.h>
#include <stdbool.h>
#include <sys/types.h>

#define IS_SVG (1 << 0)
#define IS_ANNOT_DIR (1 << 1)
#define IS_ANNOT_PAGE (1 << 2)
#define IS_PNG (1 << 3)
#define IS_PDF (1 << 4)
#define IS_ANNOTATED_PDF (1 << 5)
#define IS_SVG_DIR (1 << 6)
#define IS_PNG_DIR (1 << 7)
#define IS_PDF_DIR (1 << 8)
#define IS_XOJ (1 << 9)
#define IS_XOJ_DIR (1 << 10)

#define DEFAULT_SOURCE "./xochitl"

extern bool enable_svg;
extern bool enable_png;
extern bool enable_pdf;
extern bool enable_xoj;
extern bool enable_mutable;
extern bool enable_standalone_annotations;
extern char *template_dir;
extern char *data_dir;

extern pthread_mutex_t remfs_mutex;

struct fuse_operations;
extern struct fuse_operations remfuse_ops;

#endif /* REMFUSE_H */
