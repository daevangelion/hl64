/* SPDX-License-Identifier: MIT */
/*
 * heap_host.h -- the host build's malloc (heap_host.c, imported from the
 * old tree's n64/port/host/q64_heap_host.h, audit entry CLEAN): the
 * cart's newlib dlmalloc on a static arena the size of the cart's heap
 * after the boot carve (heap_host.c ARENA_BYTES), so the
 * engine's malloc / calloc / realloc / free (engine/mem/heap.c --wrap)
 * split, coalesce and fragment as on the cart.
 */
#ifndef HL64_HEAP_HOST_H
#define HL64_HEAP_HOST_H

#include <stddef.h>

void *heap_host_malloc(size_t size);
void heap_host_free(void *ptr);
void *heap_host_calloc(size_t nmemb, size_t size);
void *heap_host_realloc(void *ptr, size_t size);
/* memalign (the cart's malloc_uncached: 16-byte aligned, 16-byte rounded) */
void *heap_host_memalign(size_t alignment, size_t size);

/* arena bytes; free bytes (binned chunks + top + unclaimed sbrk room) */
void heap_host_stats(size_t *arena, size_t *free_bytes);

#endif
