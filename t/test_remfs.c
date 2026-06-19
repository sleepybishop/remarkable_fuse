#include <stdio.h>
#include <string.h>
#include "remfs.h"

int main() {
    remfs_ctx *ctx = remfs_init("./t/assets/xochitl");
    if (!ctx) {
        printf("Failed to init remfs\n");
        return 1;
    }
    
    int found_page1_template = 0;
    int found_page2_template = 0;
    for (int i = 0; i < kv_size(ctx->fv); i++) {
        remfs_file *file = &kv_A(ctx->fv, i);
        if (file->filetype == PAGE) {
            if (strcmp(file->uuid, "page1111-1111-1111-1111-111111111111") == 0) {
                if (strcmp(file->template_name, "Generic") == 0) {
                    found_page1_template = 1;
                } else {
                    printf("Page 1 template expected 'Generic', got '%s'\n", file->template_name);
                }
            } else if (strcmp(file->uuid, "page2222-2222-2222-2222-222222222222") == 0) {
                if (strcmp(file->template_name, "Blank") == 0) {
                    found_page2_template = 1;
                } else {
                    printf("Page 2 template expected 'Blank', got '%s'\n", file->template_name);
                }
            }
        }
    }
    
    remfs_destroy(ctx);

    if (found_page1_template && found_page2_template) {
        printf("OK\n");
        return 0;
    } else {
        printf("FAIL: version 2 templates not correctly parsed from JSON\n");
        return 1;
    }
}
