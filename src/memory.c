/**
 * TinyOS Memory Management
 *
 * Simple, deterministic memory allocator for embedded systems
 * Uses fixed-size block allocation for predictable behavior
 */

#include "tinyos.h"
#include <string.h>

/* Memory pool configuration */
#define MEMORY_POOL_SIZE    4096  /* 4KB total heap */
#define BLOCK_SIZE          32    /* 32-byte blocks */
#define NUM_BLOCKS          (MEMORY_POOL_SIZE / BLOCK_SIZE)

/* Memory block structure */
typedef struct memory_block {
    struct memory_block *next;
    bool allocated;
    size_t size;
} memory_block_t;

/* Memory pool */
static struct {
    uint8_t pool[MEMORY_POOL_SIZE] __attribute__((aligned(4)));
    memory_block_t *free_list;
    size_t free_bytes;
    size_t allocated_bytes;
    uint32_t allocation_count;
    uint32_t free_count;
} mem;

/**
 * Initialize memory management system
 */
void os_mem_init(void) {
    memset(&mem, 0, sizeof(mem));
    mem.free_bytes = MEMORY_POOL_SIZE;

    /* Initialize free list */
    mem.free_list = (memory_block_t *)mem.pool;
    memory_block_t *current = mem.free_list;

    for (size_t i = 0; i < NUM_BLOCKS - 1; i++) {
        current->allocated = false;
        current->size = BLOCK_SIZE;
        current->next = (memory_block_t *)((uint8_t *)current + BLOCK_SIZE);
        current = current->next;
    }

    /* Last block */
    current->allocated = false;
    current->size = BLOCK_SIZE;
    current->next = NULL;
}

/**
 * Allocate memory
 */
void *os_malloc(size_t size) {
    if (size == 0 || size > MEMORY_POOL_SIZE) {
        return NULL;
    }

    uint32_t state = os_enter_critical();

    /* Calculate required blocks */
    size_t blocks_needed = (size + sizeof(memory_block_t) + BLOCK_SIZE - 1) / BLOCK_SIZE;
    size_t total_size = blocks_needed * BLOCK_SIZE;

    /* Search free list for suitable block */
    memory_block_t **indirect = &mem.free_list;
    memory_block_t *block = mem.free_list;
    memory_block_t *prev = NULL;

    while (block != NULL) {
        if (!block->allocated) {
            /* Check if we have enough contiguous blocks */
            size_t available = 0;
            memory_block_t *check = block;

            while (check != NULL && !check->allocated && available < total_size) {
                available += BLOCK_SIZE;
                check = (memory_block_t *)((uint8_t *)check + BLOCK_SIZE);
            }

            if (available >= total_size) {
                /* Found suitable block */
                block->allocated = true;
                block->size = total_size;

                /* Remove from free list */
                if (prev == NULL) {
                    mem.free_list = (memory_block_t *)((uint8_t *)block + total_size);
                } else {
                    prev->next = (memory_block_t *)((uint8_t *)block + total_size);
                }

                mem.free_bytes -= total_size;
                mem.allocated_bytes += total_size;
                mem.allocation_count++;

                os_exit_critical(state);

                /* Return pointer after block header */
                return (void *)((uint8_t *)block + sizeof(memory_block_t));
            }
        }

        prev = block;
        block = block->next;
    }

    os_exit_critical(state);
    return NULL;  /* Out of memory */
}

/**
 * Free memory
 */
void os_free(void *ptr) {
    if (ptr == NULL) {
        return;
    }

    uint32_t state = os_enter_critical();

    /* Get block header */
    memory_block_t *block = (memory_block_t *)((uint8_t *)ptr - sizeof(memory_block_t));

    if (!block->allocated) {
        os_exit_critical(state);
        return;  /* Double free */
    }

    /* Mark as free */
    block->allocated = false;
    mem.free_bytes += block->size;
    mem.allocated_bytes -= block->size;
    mem.free_count++;

    /* Add back to free list */
    block->next = mem.free_list;
    mem.free_list = block;

    /* TODO: Coalesce adjacent free blocks */

    os_exit_critical(state);
}

/**
 * Get free memory
 */
size_t os_get_free_memory(void) {
    return mem.free_bytes;
}

/**
 * Get memory statistics
 */
void os_get_memory_stats(size_t *free, size_t *used, uint32_t *allocs, uint32_t *frees) {
    if (free) *free = mem.free_bytes;
    if (used) *used = mem.allocated_bytes;
    if (allocs) *allocs = mem.allocation_count;
    if (frees) *frees = mem.free_count;
}
