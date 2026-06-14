#include <stdio.h>
#include <string.h>
#include "remfs.h"

int main() {
    remfs_ctx *ctx = remfs_init("./t/assets/xochitl");
    if (!ctx) {
        printf("Failed to init remfs\n");
        return 1;
    }
    
    int found_template = 0;
    for (int i = 0; i < kv_size(ctx->fv); i++) {
        remfs_file *file = &kv_A(ctx->fv, i);
        if (file->filetype == PAGE && file->template_name[0] != '\0') {
            found_template = 1;
            break;
        }
    }
    
    remfs_destroy(ctx);

    if (found_template) {
        printf("OK\n");
        return 0;
    } else {
        printf("FAIL: no templates found\n");
        return 1;
    }
}
