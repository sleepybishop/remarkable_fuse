#include "cache.h"
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#define CACHE_SIZE 128

static cache_entry *cache_head = NULL;
static cache_entry *cache_tail = NULL;
static int cache_count = 0;
static pthread_mutex_t cache_mutex = PTHREAD_MUTEX_INITIALIZER;

cache_entry *get_cached_entry(const char *uuid, const char *type,
                              time_t mtime) {
  pthread_mutex_lock(&cache_mutex);
  cache_entry *curr = cache_head;
  while (curr) {
    if (strcmp(curr->uuid, uuid) == 0 && strcmp(curr->type, type) == 0) {
      if (curr->mtime == mtime) {
        if (curr != cache_head) {
          curr->prev->next = curr->next;
          if (curr->next)
            curr->next->prev = curr->prev;
          else
            cache_tail = curr->prev;
          curr->next = cache_head;
          curr->prev = NULL;
          cache_head->prev = curr;
          cache_head = curr;
        }
        curr->refcount++;
        pthread_mutex_unlock(&cache_mutex);
        return curr;
      }
      if (curr->prev)
        curr->prev->next = curr->next;
      else
        cache_head = curr->next;
      if (curr->next)
        curr->next->prev = curr->prev;
      else
        cache_tail = curr->prev;
      if (curr->refcount == 0) {
        free(curr->data);
        free(curr);
      } else {
        curr->uuid[0] = '\0';
      }
      cache_count--;
      break;
    }
    curr = curr->next;
  }
  pthread_mutex_unlock(&cache_mutex);
  return NULL;
}

cache_entry *add_to_cache(const char *uuid, const char *type, time_t mtime,
                          uint8_t *data, size_t size) {
  cache_entry *entry = calloc(1, sizeof(cache_entry));
  strcpy(entry->uuid, uuid);
  strcpy(entry->type, type);
  entry->mtime = mtime;
  entry->data = data;
  entry->size = size;
  entry->refcount = 1;

  pthread_mutex_lock(&cache_mutex);
  entry->next = cache_head;
  if (cache_head)
    cache_head->prev = entry;
  cache_head = entry;
  if (!cache_tail)
    cache_tail = entry;
  cache_count++;

  while (cache_count > CACHE_SIZE) {
    cache_entry *tail = cache_tail;
    if (!tail)
      break;
    cache_tail = tail->prev;
    if (cache_tail)
      cache_tail->next = NULL;
    else
      cache_head = NULL;

    if (tail->refcount == 0) {
      free(tail->data);
      free(tail);
    } else {
      tail->uuid[0] = '\0';
    }
    cache_count--;
  }
  pthread_mutex_unlock(&cache_mutex);
  return entry;
}

void release_cached_entry(cache_entry *entry) {
  pthread_mutex_lock(&cache_mutex);
  entry->refcount--;
  if (entry->refcount == 0 && entry->uuid[0] == '\0') {
    free(entry->data);
    free(entry);
  }
  pthread_mutex_unlock(&cache_mutex);
}
