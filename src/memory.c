/**
 * TinyOS Memory Management
 *
 * First-fit allocator with immediate coalescing.
 *
 * Design
 * ------
 * The heap pool is a flat byte array.  Every region (free or allocated)
 * begins with a blk_t header that records the payload size and whether the
 * block is in use.
 *
 * Free blocks are linked in a singly-linked list sorted by address.
 * Address-sorted order is the key invariant that makes O(1) coalescing
 * possible: after inserting a freed block we can immediately check both its
 * physical predecessor (the previous list entry) and its physical successor
 * (blk->next) without an extra scan.
 *
 * Why the old allocator was broken
 * ---------------------------------
 * The previous implementation inserted freed blocks at the head of the free
 * list (LIFO), producing an address-unsorted list.  The coalescing loop then
 * tried to merge a block with its physical neighbor by following list->next,
 * but list->next was an arbitrary address-unsorted block — not the physically
 * adjacent one — so merges silently failed and fragmentation was unbounded.
 *
 * Alignment
 * ---------
 * All payload sizes are rounded up to ALIGN (8 bytes) so returned pointers
 * satisfy the strictest C alignment requirement on 32-bit ARM.  The header
 * itself is padded to the same alignment, so the payload pointer that follows
 * it is also naturally aligned.
 *
 * Thread safety
 * -------------
 * Every public function wraps its critical work in os_enter/exit_critical()
 * except os_get_memory_stats() which reads atomically.
 */

#include "tinyos.h"
#include <string.h>

/* ── Heap configuration ─────────────────────────────────────────────────── */

#define MEMORY_POOL_SIZE  8192U    /* 8 KB — configurable at build time       */
#define ALIGN             8U       /* must be a power of two                  */
#define ALIGN_UP(n)       (((n) + ALIGN - 1U) & ~(ALIGN - 1U))

/* ── Block header ───────────────────────────────────────────────────────── */

typedef struct blk {
    uint32_t    size;   /* payload size (bytes, always a multiple of ALIGN)   */
    uint32_t    used;   /* 1 = allocated, 0 = free                            */
    struct blk *next;   /* next free block in address-sorted list (free only) */
} blk_t;

#define BLK_HDR  ALIGN_UP(sizeof(blk_t))   /* header size, aligned (16 B)   */
#define MIN_SPLIT (BLK_HDR + ALIGN)         /* minimum useful remainder size  */

/* ── Heap state ─────────────────────────────────────────────────────────── */

static uint8_t pool[MEMORY_POOL_SIZE] __attribute__((aligned(8)));
static blk_t  *free_head;      /* first free block (lowest address)          */
static size_t  free_payload;   /* sum of free block payload sizes             */
static size_t  used_payload;   /* sum of allocated block payload sizes        */
static uint32_t alloc_count;
static uint32_t free_count;

/* ── Initialization ─────────────────────────────────────────────────────── */

void os_mem_init(void) {
    memset(pool, 0, MEMORY_POOL_SIZE);

    blk_t *root = (blk_t *)pool;
    root->size = MEMORY_POOL_SIZE - BLK_HDR;
    root->used = 0;
    root->next = NULL;

    free_head    = root;
    free_payload = root->size;
    used_payload = 0;
    alloc_count  = 0;
    free_count   = 0;
}

/* ── Allocation (first-fit) ─────────────────────────────────────────────── */

void *os_malloc(size_t size) {
    if (size == 0) return NULL;
    size = ALIGN_UP(size);

    uint32_t cs = os_enter_critical();

    blk_t **pp = &free_head;
    while (*pp) {
        blk_t *blk = *pp;
        if (blk->size >= size) {
            if (blk->size >= size + MIN_SPLIT) {
                /* ── Split: carve the requested size from the front ──────── */
                blk_t *remainder = (blk_t *)((uint8_t *)blk + BLK_HDR + size);
                remainder->size  = blk->size - BLK_HDR - size;
                remainder->used  = 0;
                remainder->next  = blk->next;
                *pp              = remainder;
                blk->size        = size;
                /* Splitting consumes BLK_HDR bytes for the new header */
                free_payload    -= BLK_HDR + size;
            } else {
                /* ── Use entire block without splitting ──────────────────── */
                *pp           = blk->next;
                free_payload -= blk->size;
            }
            blk->used  = 1;
            blk->next  = NULL;
            used_payload += blk->size;
            alloc_count++;
            os_exit_critical(cs);
            return (uint8_t *)blk + BLK_HDR;
        }
        pp = &(*pp)->next;
    }

    os_exit_critical(cs);
    return NULL;   /* out of memory */
}

/* ── Free (with immediate coalescing) ───────────────────────────────────── */

void os_free(void *ptr) {
    if (!ptr) return;

    blk_t *blk = (blk_t *)((uint8_t *)ptr - BLK_HDR);

    uint32_t cs = os_enter_critical();

    if (!blk->used) {
        /* Double-free detected — ignore to prevent heap corruption */
        os_exit_critical(cs);
        return;
    }

    blk->used     = 0;
    used_payload -= blk->size;
    free_payload += blk->size;
    free_count++;

    /*
     * Insert into the address-sorted free list.
     * After insertion: ...[prev] → [blk] → [cur]...
     * with prev < blk < cur in address space (or NULL at boundaries).
     */
    blk_t *prev = NULL;
    blk_t *cur  = free_head;
    while (cur && cur < blk) { prev = cur; cur = cur->next; }
    blk->next = cur;
    if (prev) prev->next = blk; else free_head = blk;

    /*
     * Coalesce forward: merge blk with its immediate physical successor
     * if that successor is also the next entry in the free list (i.e., they
     * are physically adjacent and both free).
     */
    if (blk->next &&
        (uint8_t *)blk + BLK_HDR + blk->size == (uint8_t *)blk->next) {
        blk->size    += BLK_HDR + blk->next->size;
        free_payload += BLK_HDR;           /* reclaim merged header           */
        blk->next     = blk->next->next;
    }

    /*
     * Coalesce backward: merge the predecessor (prev) with blk if they are
     * physically adjacent.  Do this AFTER forward coalescing so prev absorbs
     * the already-enlarged blk in one step.
     */
    if (prev &&
        (uint8_t *)prev + BLK_HDR + prev->size == (uint8_t *)blk) {
        prev->size   += BLK_HDR + blk->size;
        free_payload += BLK_HDR;           /* reclaim merged header           */
        prev->next    = blk->next;
    }

    os_exit_critical(cs);
}

/* ── Query functions ────────────────────────────────────────────────────── */

size_t os_get_free_memory(void) {
    return free_payload;
}

void os_get_memory_stats(size_t *free, size_t *used,
                         uint32_t *allocs, uint32_t *frees) {
    if (free)   *free   = free_payload;
    if (used)   *used   = used_payload;
    if (allocs) *allocs = alloc_count;
    if (frees)  *frees  = free_count;
}
