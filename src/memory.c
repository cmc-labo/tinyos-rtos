/**
 * TinyOS Memory Management — Two-Tier Allocator
 *
 * Architecture
 * ============
 * Tier 1  Slab pools for small fixed-size requests (1–64 bytes).
 *         Four size classes (8, 16, 32, 64 bytes).  Each class is a free list
 *         threaded through a pre-allocated backing array.  O(1) alloc and free,
 *         zero external fragmentation within a class.
 *
 * Tier 2  First-fit heap for requests > 64 bytes, or when the matching slab
 *         class is exhausted.  8 KB pool, 8-byte aligned, address-sorted free
 *         list, immediate forward + backward coalescing on every os_free().
 *
 * Allocation routing
 * ------------------
 * size ≤ 64 bytes  → find tightest slab class; if full, fall through to heap.
 * size  > 64 bytes → heap first-fit directly.
 *
 * Free routing
 * ------------
 * pointer inside any slab backing array → returned to that class's free list.
 * all other pointers                     → heap free (with coalescing).
 *
 * The address-range check in os_free() is O(SLAB_CLASS_COUNT) = O(4) and
 * correctly handles slab-overflow objects that were silently served from the
 * heap.
 *
 * Why this matters for long-running IoT systems
 * ---------------------------------------------
 * A first-fit allocator serving thousands of alloc/free pairs of the same
 * small size gradually builds up a fragmented free list of equal-sized holes.
 * Even with coalescing, a mis-timed allocation of a slightly different size can
 * permanently fragment these holes.  Slab pools eliminate this pattern entirely
 * for the common fixed-size objects (queue entries, timer nodes, mutex/semaphore
 * structs, message headers) that dominate RTOS allocation traffic.
 *
 * Slab memory budget (BSS)
 * ------------------------
 *   Class 0: 16 × 8  =  128 bytes
 *   Class 1: 16 × 16 =  256 bytes
 *   Class 2:  8 × 32 =  256 bytes
 *   Class 3:  8 × 64 =  512 bytes
 *   Total slab:          1152 bytes
 *   Heap pool:           8192 bytes
 *   Grand total:         9344 bytes
 *
 * Thread safety
 * -------------
 * All public functions hold os_enter/exit_critical() around every mutation of
 * shared state (slab free lists, heap free list, counters).
 */

#include "tinyos.h"
#include <string.h>

/*===========================================================================
 * Slab pool configuration
 *===========================================================================*/

#define SLAB_CLASS_COUNT  4U

/* Object size for each class — must be a multiple of ALIGN (8) */
#define SLAB_SZ0   8U
#define SLAB_SZ1  16U
#define SLAB_SZ2  32U
#define SLAB_SZ3  64U

/* Object count per class — tune to match application allocation patterns */
#define SLAB_N0   16U
#define SLAB_N1   16U
#define SLAB_N2    8U
#define SLAB_N3    8U

/* Largest size served by the slab tier */
#define SLAB_MAX_SIZE  SLAB_SZ3

/* Backing storage — one contiguous array per class, 8-byte aligned */
static uint8_t slab_pool_0[SLAB_N0 * SLAB_SZ0] __attribute__((aligned(8)));
static uint8_t slab_pool_1[SLAB_N1 * SLAB_SZ1] __attribute__((aligned(8)));
static uint8_t slab_pool_2[SLAB_N2 * SLAB_SZ2] __attribute__((aligned(8)));
static uint8_t slab_pool_3[SLAB_N3 * SLAB_SZ3] __attribute__((aligned(8)));

/* Parallel arrays describing each class — indexed 0..SLAB_CLASS_COUNT-1 */
static uint8_t * const slab_pool_base[SLAB_CLASS_COUNT] = {
    slab_pool_0, slab_pool_1, slab_pool_2, slab_pool_3
};
static const uint32_t slab_obj_sz[SLAB_CLASS_COUNT] = {
    SLAB_SZ0, SLAB_SZ1, SLAB_SZ2, SLAB_SZ3
};
static const uint32_t slab_obj_count[SLAB_CLASS_COUNT] = {
    SLAB_N0, SLAB_N1, SLAB_N2, SLAB_N3
};

/* Runtime state (mutated under critical section) */
static uint8_t  *slab_free_head[SLAB_CLASS_COUNT]; /* free list head per class */
static uint32_t  slab_in_use[SLAB_CLASS_COUNT];    /* allocated objects per class */

/*===========================================================================
 * Heap configuration
 *===========================================================================*/

#define MEMORY_POOL_SIZE  8192U
#define ALIGN             8U
#define ALIGN_UP(n)       (((n) + ALIGN - 1U) & ~(ALIGN - 1U))

typedef struct blk {
    uint32_t    size;    /* payload size (bytes, always a multiple of ALIGN)  */
    uint32_t    used;    /* 1 = allocated, 0 = free                           */
    struct blk *next;    /* next free block in address-sorted list (free only) */
} blk_t;

#define BLK_HDR    ALIGN_UP(sizeof(blk_t))  /* aligned header size (16 B)    */
#define MIN_SPLIT  (BLK_HDR + ALIGN)        /* minimum useful remainder      */

static uint8_t pool[MEMORY_POOL_SIZE] __attribute__((aligned(8)));
static blk_t  *free_head;

/* Combined allocation counters (slab + heap) */
static size_t   heap_free_bytes;
static size_t   heap_used_bytes;
static uint32_t alloc_count;
static uint32_t free_count;

/*===========================================================================
 * Slab helpers (called under critical section)
 *===========================================================================*/

/* Thread the free list for one slab class.
 * Each free object's first sizeof(uint8_t *) bytes hold a pointer to the
 * next free object; the last object's link is NULL. */
static void slab_class_init(uint32_t c) {
    uint8_t  *base  = slab_pool_base[c];
    uint32_t  sz    = slab_obj_sz[c];
    uint32_t  n     = slab_obj_count[c];

    for (uint32_t i = 0; i < n - 1U; i++) {
        *(uint8_t **)(base + i * sz) = base + (i + 1U) * sz;
    }
    *(uint8_t **)(base + (n - 1U) * sz) = NULL;

    slab_free_head[c] = base;
    slab_in_use[c]    = 0U;
}

/* Pop the head of the free list — O(1).  Returns NULL when class is empty. */
static void *slab_alloc(uint32_t c) {
    uint8_t *obj = slab_free_head[c];
    if (obj == NULL) return NULL;
    slab_free_head[c] = *(uint8_t **)obj;
    slab_in_use[c]++;
    return obj;
}

/* Push an object back onto the free list — O(1). */
static void slab_free_obj(uint32_t c, void *obj) {
    *(uint8_t **)obj  = slab_free_head[c];
    slab_free_head[c] = (uint8_t *)obj;
    slab_in_use[c]--;
}

/* Return the slab class index that owns ptr, or -1 if it is a heap pointer. */
static int slab_class_of(const void *ptr) {
    for (uint32_t c = 0; c < SLAB_CLASS_COUNT; c++) {
        const uint8_t *base = slab_pool_base[c];
        if ((const uint8_t *)ptr >= base &&
            (const uint8_t *)ptr <  base + slab_obj_count[c] * slab_obj_sz[c]) {
            return (int)c;
        }
    }
    return -1;
}

/*===========================================================================
 * Initialization
 *===========================================================================*/

void os_mem_init(void) {
    /* Zero slab pools for clean initial state */
    for (uint32_t c = 0; c < SLAB_CLASS_COUNT; c++) {
        memset(slab_pool_base[c], 0, slab_obj_count[c] * slab_obj_sz[c]);
        slab_class_init(c);
    }

    /* Initialise heap as a single free block */
    memset(pool, 0, MEMORY_POOL_SIZE);
    blk_t *root  = (blk_t *)pool;
    root->size   = MEMORY_POOL_SIZE - BLK_HDR;
    root->used   = 0;
    root->next   = NULL;

    free_head       = root;
    heap_free_bytes = root->size;
    heap_used_bytes = 0U;
    alloc_count     = 0U;
    free_count      = 0U;
}

/*===========================================================================
 * Allocation
 *===========================================================================*/

void *os_malloc(size_t size) {
    if (size == 0) return NULL;

    uint32_t cs = os_enter_critical();

    /* ── Tier 1: slab pool ──────────────────────────────────────────────── */
    if (size <= SLAB_MAX_SIZE) {
        /* Find the tightest class that satisfies the request */
        for (uint32_t c = 0; c < SLAB_CLASS_COUNT; c++) {
            if (size <= slab_obj_sz[c]) {
                void *p = slab_alloc(c);
                if (p != NULL) {
                    alloc_count++;
                    os_exit_critical(cs);
                    return p;
                }
                /* Class exhausted — fall through to the heap rather than
                 * using a larger class (that wastes memory). */
                break;
            }
        }
    }

    /* ── Tier 2: first-fit heap ─────────────────────────────────────────── */
    size = ALIGN_UP(size);

    blk_t **pp = &free_head;
    while (*pp) {
        blk_t *blk = *pp;
        if (blk->size >= size) {
            if (blk->size >= size + MIN_SPLIT) {
                /* Split: carve the requested size from the front */
                blk_t *rem  = (blk_t *)((uint8_t *)blk + BLK_HDR + size);
                rem->size   = blk->size - BLK_HDR - size;
                rem->used   = 0;
                rem->next   = blk->next;
                *pp         = rem;
                blk->size   = size;
                heap_free_bytes -= BLK_HDR + size;
            } else {
                /* Use entire block */
                *pp              = blk->next;
                heap_free_bytes -= blk->size;
            }
            blk->used     = 1;
            blk->next     = NULL;
            heap_used_bytes += blk->size;
            alloc_count++;
            os_exit_critical(cs);
            return (uint8_t *)blk + BLK_HDR;
        }
        pp = &(*pp)->next;
    }

    os_exit_critical(cs);
    return NULL;   /* out of memory */
}

/*===========================================================================
 * Free
 *===========================================================================*/

void os_free(void *ptr) {
    if (ptr == NULL) return;

    uint32_t cs = os_enter_critical();

    /* ── Tier 1: slab return ────────────────────────────────────────────── */
    int c = slab_class_of(ptr);
    if (c >= 0) {
        slab_free_obj((uint32_t)c, ptr);
        free_count++;
        os_exit_critical(cs);
        return;
    }

    /* ── Tier 2: heap free with immediate coalescing ────────────────────── */
    blk_t *blk = (blk_t *)((uint8_t *)ptr - BLK_HDR);

    if (!blk->used) {
        /* Double-free detected — ignore to prevent heap corruption */
        os_exit_critical(cs);
        return;
    }

    blk->used        = 0;
    heap_used_bytes -= blk->size;
    heap_free_bytes += blk->size;
    free_count++;

    /* Insert into address-sorted free list: ...[prev] → [blk] → [cur]... */
    blk_t *prev = NULL;
    blk_t *cur  = free_head;
    while (cur && cur < blk) { prev = cur; cur = cur->next; }
    blk->next = cur;
    if (prev) prev->next = blk; else free_head = blk;

    /* Coalesce forward: merge with physically adjacent successor */
    if (blk->next &&
        (uint8_t *)blk + BLK_HDR + blk->size == (uint8_t *)blk->next) {
        blk->size       += BLK_HDR + blk->next->size;
        heap_free_bytes += BLK_HDR;
        blk->next        = blk->next->next;
    }

    /* Coalesce backward: merge predecessor into the (already-enlarged) blk */
    if (prev &&
        (uint8_t *)prev + BLK_HDR + prev->size == (uint8_t *)blk) {
        prev->size      += BLK_HDR + blk->size;
        heap_free_bytes += BLK_HDR;
        prev->next       = blk->next;
    }

    os_exit_critical(cs);
}

/*===========================================================================
 * Query
 *===========================================================================*/

/* Free bytes: heap free + all unallocated slab object payload bytes */
size_t os_get_free_memory(void) {
    size_t slab_free = 0;
    for (uint32_t c = 0; c < SLAB_CLASS_COUNT; c++) {
        uint32_t free_objs = slab_obj_count[c] - slab_in_use[c];
        slab_free += (size_t)(free_objs * slab_obj_sz[c]);
    }
    return heap_free_bytes + slab_free;
}

/* Combined heap + slab statistics */
void os_get_memory_stats(size_t *free_out, size_t *used_out,
                         uint32_t *allocs, uint32_t *frees) {
    size_t slab_free_bytes = 0;
    size_t slab_used_bytes = 0;
    for (uint32_t c = 0; c < SLAB_CLASS_COUNT; c++) {
        slab_used_bytes += (size_t)(slab_in_use[c]                          * slab_obj_sz[c]);
        slab_free_bytes += (size_t)((slab_obj_count[c] - slab_in_use[c])   * slab_obj_sz[c]);
    }
    if (free_out) *free_out = heap_free_bytes + slab_free_bytes;
    if (used_out) *used_out = heap_used_bytes + slab_used_bytes;
    if (allocs)   *allocs   = alloc_count;
    if (frees)    *frees    = free_count;
}
