#define FUSE_USE_VERSION 26
#include <assert.h>
#include <errno.h>
#include <fuse.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "cJSON.h"
#include "deps/sds/sds.h"
#include "path_utils.h"
#include "remfuse.h"

static struct options {
  const char *config_file;
  int show_help;
} options;

#define OPTION(t, p) {t, offsetof(struct options, p), 1}
static const struct fuse_opt option_spec[] = {
    OPTION("--config=%s", config_file), OPTION("-h", show_help),
    OPTION("--help", show_help), FUSE_OPT_END};

static void load_config(const char *path) {
  FILE *f = fopen(path, "rb");
  if (!f) {
    if (options.config_file) {
      fprintf(stderr, "failed to open config file [%s]: %s\n", path,
              strerror(errno));
      exit(1);
    }
    return;
  }

  fseek(f, 0, SEEK_END);
  long size = ftell(f);
  fseek(f, 0, SEEK_SET);
  if (size <= 0) {
    fclose(f);
    return;
  }

  char *buf = malloc(size + 1);
  if (!buf) {
    fclose(f);
    return;
  }

  size_t read_bytes = fread(buf, 1, size, f);
  buf[read_bytes] = '\0';
  fclose(f);

  cJSON *root = cJSON_Parse(buf);
  free(buf);
  if (root) {
    cJSON *data_dir_cfg = cJSON_GetObjectItemCaseSensitive(root, "data_dir");
    if (data_dir_cfg && cJSON_IsString(data_dir_cfg)) {
      if (data_dir)
        free(data_dir);
      data_dir = strdup(data_dir_cfg->valuestring);
    }

    cJSON *tpl_dir = cJSON_GetObjectItemCaseSensitive(root, "template_dir");
    if (tpl_dir && cJSON_IsString(tpl_dir)) {
      if (template_dir)
        free(template_dir);
      template_dir = strdup(tpl_dir->valuestring);
    }

    cJSON *renderers = cJSON_GetObjectItemCaseSensitive(root, "renderers");
    if (cJSON_IsArray(renderers)) {
      enable_svg = false;
      enable_png = false;
      enable_pdf = false;
      enable_xoj = false;
      cJSON *item = NULL;
      cJSON_ArrayForEach(item, renderers) {
        if (cJSON_IsString(item)) {
          const char *val = cJSON_GetStringValue(item);
          if (strcmp(val, "svg") == 0)
            enable_svg = true;
          else if (strcmp(val, "png") == 0)
            enable_png = true;
          else if (strcmp(val, "pdf") == 0)
            enable_pdf = true;
          else if (strcmp(val, "xoj") == 0)
            enable_xoj = true;
        }
      }
    }

    cJSON *mutable_item = cJSON_GetObjectItem(root, "mutable");
    if (cJSON_IsBool(mutable_item))
      enable_mutable = cJSON_IsTrue(mutable_item);

    cJSON *standalone_item =
        cJSON_GetObjectItem(root, "standalone_annotations");
    if (cJSON_IsBool(standalone_item))
      enable_standalone_annotations = cJSON_IsTrue(standalone_item);

    cJSON_Delete(root);
  }
}

static void usage(const char *progname) {
  printf("usage: %s [options] <mountpoint>\n\n", progname);
  printf("File-system specific options:\n"
         "    --config=<c>        path to config JSON file\n"
         "\n");
}

static char *mountpoint = NULL;
static int opt_proc(void *data, const char *arg, int key,
                    struct fuse_args *outargs) {
  if (key == FUSE_OPT_KEY_NONOPT && mountpoint == NULL) {
    mountpoint = strdup(arg);
  }
  return 1;
}

int main(int argc, char *argv[]) {
  int ret = 1;
  struct fuse_args args = FUSE_ARGS_INIT(argc, argv);
  struct stat stbuf;

  options.config_file = NULL;

  if (fuse_opt_parse(&args, &options, option_spec, opt_proc) == -1)
    goto cleanup;

  if (options.config_file) {
    load_config(options.config_file);
  } else {
    struct stat st;
    if (stat("config.json", &st) == 0) {
      load_config("config.json");
    }
  }

  if (template_dir == NULL) {
    struct stat st;
    if (stat("./templates", &st) == 0 && S_ISDIR(st.st_mode)) {
      template_dir = strdup("./templates");
    } else if (stat("/usr/share/remarkable/templates", &st) == 0 &&
               S_ISDIR(st.st_mode)) {
      template_dir = strdup("/usr/share/remarkable/templates");
    }
  }

  if (options.show_help) {
    usage(argv[0]);
    assert(fuse_opt_add_arg(&args, "--help") == 0);
    args.argv[0][0] = '\0';
  } else {
    const char *src = data_dir ? data_dir : DEFAULT_SOURCE;
    if (stat(src, &stbuf) == -1) {
      fprintf(stderr, "failed to access data dir [%s]: %s\n", src,
              strerror(errno));
      goto cleanup;
    }
    if (!S_ISDIR(stbuf.st_mode)) {
      fprintf(stderr, "data dir [%s] is not a directory\n", src);
      goto cleanup;
    }
  }

  if (mountpoint) {
    sds cmd = sdscatprintf(sdsempty(),
                           "fusermount3 -u -q -z %s 2>/dev/null || fusermount "
                           "-u -q -z %s 2>/dev/null",
                           mountpoint, mountpoint);
    int sys_ret = system(cmd);
    (void)sys_ret;
    sdsfree(cmd);
    free(mountpoint);
  }

  fuse_opt_add_arg(&args, "-oauto_unmount");
  ret = fuse_main(args.argc, args.argv, &remfuse_ops, NULL);
cleanup:
  fuse_opt_free_args(&args);
  return ret;
}
