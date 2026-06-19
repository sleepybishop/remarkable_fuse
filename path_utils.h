#ifndef PATH_UTILS_H
#define PATH_UTILS_H

#include "deps/sds/sds.h"
#include "remfs.h"
#include "remfuse.h"

sds munge_path(const char *path, int *flags);
uuid_map_node *rewrite_path(remfs_ctx *ctx, const char *path, int *flags,
                            sds *newpath);
void gen_uuid(char *buf);
void get_parent_and_name(const char *path, sds *parent_path, sds *name);
uint8_t *slurp(const char *path);
bool is_path_virtual(const char *path);

#endif /* PATH_UTILS_H */
