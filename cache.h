#ifndef CACHE_H
#define CACHE_H

#include <stddef.h>
#include <stdint.h>
#include <time.h>

typedef struct cache_entry {
  char uuid[64];
  char type[8];
  time_t mtime;
  uint8_t *data;
  size_t size;
  int refcount;
  struct cache_entry *prev;
  struct cache_entry *next;
} cache_entry;

cache_entry *get_cached_entry(const char *uuid, const char *type, time_t mtime);
cache_entry *add_to_cache(const char *uuid, const char *type, time_t mtime,
                          uint8_t *data, size_t size);
void release_cached_entry(cache_entry *entry);

#endif /* CACHE_H */
