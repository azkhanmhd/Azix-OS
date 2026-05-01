#ifndef HEAP_H
#define HEAP_H

#include <stddef.h>
#include <stdint.h>

/*
 * Initialise the heap.
 *   heap_start : first usable byte (pass &_kernel_end)
 *   heap_size  : maximum heap size in bytes (e.g. 4 MB)
 */
void  heap_init(void *heap_start, size_t heap_size);

void *kmalloc(size_t size);
void *kcalloc(size_t count, size_t size);
void *krealloc(void *ptr, size_t new_size);
void  kfree(void *ptr);

/* Diagnostics */
size_t heap_used(void);
size_t heap_free_bytes(void);
size_t heap_total(void);

#endif /* HEAP_H */
