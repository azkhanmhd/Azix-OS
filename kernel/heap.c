/*
 * heap.c — kernel heap allocator for Azix OS
 *
 * Algorithm: explicit free list with boundary-tag coalescing.
 *
 * Block layout (each block, allocated or free):
 *
 *   [ header : block_t (8 bytes) ]
 *   [ payload bytes ...          ]
 *   [ footer : block_t (8 bytes) ]  ← same value as header
 *
 * header/footer stores:
 *   - size  : total block size INCLUDING header+footer (always 8-byte aligned)
 *   - used  : 1 = allocated, 0 = free
 *
 * Minimum block size = sizeof(block_t)*2 + 8 = 24 bytes.
 *
 * Coalescing: on kfree() we merge with immediately adjacent free blocks
 * using the footer of the previous block and the header of the next block.
 *
 * This gives O(1) coalescing and O(n) first-fit allocation, which is
 * perfectly adequate for a kernel that doesn't allocate millions of objects.
 */

#include "heap.h"
#include <stdint.h>
#include <stddef.h>

/* ------------------------------------------------------------------ */
/* Block header / footer                                               */
/* ------------------------------------------------------------------ */
typedef struct {
    size_t   size; /* total block size (header + payload + footer)     */
    uint32_t used; /* 1 = allocated, 0 = free                          */
    uint32_t _pad; /* keep 8-byte aligned on 32-bit                    */
} block_t;         /* sizeof(block_t) == 12 on 32-bit — rounded to 12 */

#define BLOCK_SZ     sizeof(block_t)   /* 12 bytes on 32-bit          */
#define MIN_PAYLOAD  8                 /* smallest useful payload      */
/* Minimum total block = hdr + min_payload + ftr */
#define MIN_BLOCK    (BLOCK_SZ + MIN_PAYLOAD + BLOCK_SZ)

/* Alignment: all payloads aligned to 8 bytes */
#define ALIGN8(n)    (((n) + 7u) & ~7u)

/* ------------------------------------------------------------------ */
/* Heap state                                                          */
/* ------------------------------------------------------------------ */
static uint8_t *heap_base  = 0;
static uint8_t *heap_limit = 0;
static size_t   heap_sz    = 0;

/* ------------------------------------------------------------------ */
/* Block accessors                                                     */
/* ------------------------------------------------------------------ */
static inline block_t *hdr(void *payload)
{
    return (block_t *)((uint8_t *)payload - BLOCK_SZ);
}

static inline block_t *ftr(block_t *h)
{
    return (block_t *)((uint8_t *)h + h->size - BLOCK_SZ);
}

static inline block_t *next_blk(block_t *h)
{
    return (block_t *)((uint8_t *)h + h->size);
}

/* Footer of the physically previous block */
static inline block_t *prev_ftr(block_t *h)
{
    return (block_t *)((uint8_t *)h - BLOCK_SZ);
}

static inline block_t *prev_blk(block_t *h)
{
    block_t *pf = prev_ftr(h);
    return (block_t *)((uint8_t *)h - pf->size);
}

static inline void *payload(block_t *h)
{
    return (void *)((uint8_t *)h + BLOCK_SZ);
}

/* Write matching header and footer */
static inline void set_block(block_t *h, size_t size, uint32_t used)
{
    h->size = size; h->used = used; h->_pad = 0;
    block_t *f = ftr(h);
    f->size = size; f->used = used; f->_pad = 0;
}

/* ------------------------------------------------------------------ */
/* heap_init                                                           */
/* ------------------------------------------------------------------ */
void heap_init(void *start, size_t size)
{
    /* Align start to 8-byte boundary */
    uintptr_t s = ((uintptr_t)start + 7u) & ~7u;
    size -= (size_t)(s - (uintptr_t)start);
    size &= ~7u;

    heap_base  = (uint8_t *)s;
    heap_limit = heap_base + size;
    heap_sz    = size;

    /* Create one giant free block covering the whole heap.
       Use sentinel blocks at both ends so coalescing never walks off. */

    /* Prologue: a tiny 1-block allocated sentinel at the start.
       Size = 2*BLOCK_SZ (hdr+ftr only, no payload). */
    block_t *pro = (block_t *)heap_base;
    set_block(pro, 2 * BLOCK_SZ, 1);

    /* The single large free block */
    block_t *free_blk = next_blk(pro);
    size_t free_sz = size - 2 * BLOCK_SZ /* prologue */ - 2 * BLOCK_SZ /* epilogue */;
    free_sz &= ~7u;
    set_block(free_blk, free_sz, 0);

    /* Epilogue: a minimal allocated sentinel at the end */
    block_t *epi = next_blk(free_blk);
    set_block(epi, 2 * BLOCK_SZ, 1);
}

/* ------------------------------------------------------------------ */
/* Coalesce adjacent free blocks                                       */
/* ------------------------------------------------------------------ */
static block_t *coalesce(block_t *h)
{
    block_t *nx = next_blk(h);
    int prev_free = 0, next_free = 0;

    /* Check next block (stay within heap) */
    if ((uint8_t *)nx < heap_limit && nx->used == 0)
        next_free = 1;

    /* Check previous block (don't go before heap_base) */
    if ((uint8_t *)h > heap_base + 2 * BLOCK_SZ) {
        block_t *pf = prev_ftr(h);
        if (pf->used == 0)
            prev_free = 1;
    }

    if (!prev_free && !next_free) {
        /* Nothing to merge */
    } else if (!prev_free && next_free) {
        size_t new_sz = h->size + nx->size;
        set_block(h, new_sz, 0);
    } else if (prev_free && !next_free) {
        block_t *ph = prev_blk(h);
        size_t new_sz = ph->size + h->size;
        set_block(ph, new_sz, 0);
        h = ph;
    } else { /* both free */
        block_t *ph = prev_blk(h);
        size_t new_sz = ph->size + h->size + nx->size;
        set_block(ph, new_sz, 0);
        h = ph;
    }
    return h;
}

/* ------------------------------------------------------------------ */
/* kmalloc                                                             */
/* ------------------------------------------------------------------ */
void *kmalloc(size_t size)
{
    if (!heap_base || size == 0) return 0;

    /* Round up payload + header + footer */
    size_t need = ALIGN8(size) + 2 * BLOCK_SZ;
    if (need < MIN_BLOCK) need = MIN_BLOCK;

    /* First-fit search (skip prologue) */
    block_t *h = next_blk((block_t *)heap_base); /* first real block */
    while ((uint8_t *)h < heap_limit) {
        if (!h->used && h->size >= need) {
            /* Split if the leftover is large enough to be its own block */
            size_t leftover = h->size - need;
            if (leftover >= MIN_BLOCK) {
                set_block(h, need, 1);
                set_block(next_blk(h), leftover, 0);
            } else {
                set_block(h, h->size, 1); /* use the whole block */
            }
            return payload(h);
        }
        h = next_blk(h);
    }
    return 0; /* out of heap */
}

/* ------------------------------------------------------------------ */
/* kcalloc                                                             */
/* ------------------------------------------------------------------ */
void *kcalloc(size_t count, size_t size)
{
    size_t total = count * size;
    void *p = kmalloc(total);
    if (p) {
        uint8_t *b = (uint8_t *)p;
        for (size_t i = 0; i < total; i++) b[i] = 0;
    }
    return p;
}

/* ------------------------------------------------------------------ */
/* krealloc                                                            */
/* ------------------------------------------------------------------ */
void *krealloc(void *ptr, size_t new_size)
{
    if (!ptr)       return kmalloc(new_size);
    if (!new_size)  { kfree(ptr); return 0; }

    block_t *h = hdr(ptr);
    size_t old_payload = h->size - 2 * BLOCK_SZ;

    void *np = kmalloc(new_size);
    if (!np) return 0;

    /* Copy old content */
    size_t copy = (new_size < old_payload) ? new_size : old_payload;
    uint8_t *src = (uint8_t *)ptr;
    uint8_t *dst = (uint8_t *)np;
    for (size_t i = 0; i < copy; i++) dst[i] = src[i];

    kfree(ptr);
    return np;
}

/* ------------------------------------------------------------------ */
/* kfree                                                               */
/* ------------------------------------------------------------------ */
void kfree(void *ptr)
{
    if (!ptr || !heap_base) return;
    block_t *h = hdr(ptr);
    if (!h->used) return; /* double-free guard */
    set_block(h, h->size, 0);
    coalesce(h);
}

/* ------------------------------------------------------------------ */
/* Diagnostics                                                         */
/* ------------------------------------------------------------------ */
size_t heap_used(void)
{
    if (!heap_base) return 0;
    size_t used = 0;
    block_t *h = next_blk((block_t *)heap_base);
    while ((uint8_t *)h < heap_limit) {
        if (h->used && h->size > 2 * BLOCK_SZ)
            used += h->size - 2 * BLOCK_SZ; /* payload only */
        h = next_blk(h);
    }
    return used;
}

size_t heap_free_bytes(void)
{
    if (!heap_base) return 0;
    size_t free = 0;
    block_t *h = next_blk((block_t *)heap_base);
    while ((uint8_t *)h < heap_limit) {
        if (!h->used && h->size > 2 * BLOCK_SZ)
            free += h->size - 2 * BLOCK_SZ;
        h = next_blk(h);
    }
    return free;
}

size_t heap_total(void) { return heap_sz; }
