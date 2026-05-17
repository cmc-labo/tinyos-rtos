/**
 * TinyOS File System Implementation
 *
 * Simple, lightweight file system designed for embedded systems
 * Features:
 * - Small footprint (~4KB code)
 * - Simple block-based storage
 * - Wear leveling support
 * - Power-fail safe operations via write-ahead journaling (WAL)
 */

#include "../include/tinyos.h"
#include <string.h>

/* Internal file system structures */

/* Inode structure (32 bytes) */
typedef struct {
    uint8_t type;               /* File type */
    uint8_t flags;              /* Flags */
    uint16_t reserved;
    uint32_t size;              /* File size */
    uint32_t blocks[6];         /* Direct block pointers */
    uint32_t mtime;             /* Modification time */
    uint32_t ctime;             /* Creation time */
} fs_inode_t;

/* Directory entry (64 bytes) */
typedef struct {
    char name[FS_MAX_FILENAME_LENGTH];
    uint32_t inode;             /* Inode number */
    uint8_t type;               /* Entry type */
    uint8_t reserved[27];
} fs_dentry_t;

/* Superblock (512 bytes - one block) */
typedef struct {
    uint32_t magic;              /* Magic number for validation */
    uint32_t version;            /* File system version */
    uint32_t block_size;         /* Block size */
    uint32_t total_blocks;       /* Total blocks */
    uint32_t free_blocks;        /* Free blocks */
    uint32_t total_inodes;       /* Total inodes */
    uint32_t free_inodes;        /* Free inodes */
    uint32_t first_data_block;   /* First data block */
    uint32_t inode_table_block;  /* Inode table start block */
    uint32_t root_inode;         /* Root directory inode */
    uint32_t journal_start;      /* Journal header block */
    uint32_t journal_block_count;/* Total journal blocks (header + data) */
    uint8_t  reserved[464];
} fs_superblock_t;

#define FS_MAGIC        0x544E5946UL  /* "TNYF" */
#define FS_VERSION      0x00020000UL  /* Version 2.0 - with journaling */

/* =====================================================================
 * Journal (Write-Ahead Log)
 * ===================================================================== */

#define JOURNAL_MAGIC           0x4A524E4CUL  /* "JRNL" */
#define JOURNAL_COMMIT_MAGIC    0x4A434D54UL  /* "JCMT" */
#define JOURNAL_VERSION         1U
#define JOURNAL_TOTAL_BLOCKS    32U           /* 1 header + 31 data blocks */
#define JOURNAL_DATA_BLOCKS     (JOURNAL_TOTAL_BLOCKS - 1U)
#define JNL_MAX_ENTRIES         6U
#define JE_DESCRIPTOR           0x01U
#define JE_COMMIT               0xFFU

/* Journal header – stored at journal_start block (exactly FS_BLOCK_SIZE bytes) */
typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t data_blocks;   /* == JOURNAL_DATA_BLOCKS */
    uint32_t head;          /* next write slot [0, data_blocks) */
    uint32_t sequence;      /* last committed sequence number */
    uint8_t  reserved[FS_BLOCK_SIZE - 20];
} jnl_hdr_t;

/* Journal descriptor/commit record (exactly FS_BLOCK_SIZE bytes) */
typedef struct {
    uint32_t magic;         /* JOURNAL_MAGIC or JOURNAL_COMMIT_MAGIC */
    uint32_t sequence;
    uint8_t  type;          /* JE_DESCRIPTOR or JE_COMMIT */
    uint8_t  pad[3];
    uint32_t block_nr;      /* target FS block (descriptor only) */
    uint32_t checksum;      /* byte-sum of the associated data block */
    uint8_t  reserved[FS_BLOCK_SIZE - 20];
} jnl_entry_t;

/* File handle */
typedef struct {
    bool in_use;
    uint32_t inode;             /* Inode number */
    uint32_t flags;             /* Open flags */
    uint32_t position;          /* Current position */
    uint32_t size;              /* File size */
    mutex_t lock;               /* File lock */
} fs_file_handle_t;

/* Directory handle structure */
struct fs_dir_s {
    bool in_use;
    uint32_t inode;             /* Directory inode */
    uint32_t position;          /* Current position */
};

/* File system state */
static struct {
    bool mounted;
    fs_block_device_t *device;
    fs_superblock_t superblock;
    fs_file_handle_t files[FS_MAX_OPEN_FILES];
    struct fs_dir_s dirs[4];    /* Maximum 4 open directories */
    mutex_t fs_lock;            /* Global file system lock */
#define FS_CACHE_ENTRIES 4
    uint8_t block_cache[FS_CACHE_ENTRIES][FS_BLOCK_SIZE];
    uint32_t cached_block[FS_CACHE_ENTRIES];
    bool cache_dirty[FS_CACHE_ENTRIES];
    uint8_t cache_next;  /* Round-robin eviction index */
} fs_state;

/* Journal runtime state */
static struct {
    bool     active;
    uint32_t start_block;       /* journal header block number */
    uint32_t head;              /* next free data slot index */
    uint32_t sequence;          /* next transaction sequence */

    bool     txn_active;
    bool     bitmap_dirty;      /* bitmap modified in current txn */
    bool     superblock_dirty;  /* superblock modified in current txn */
    uint32_t txn_count;
    struct {
        uint32_t block_nr;
        uint8_t  data[FS_BLOCK_SIZE];
    } pending[JNL_MAX_ENTRIES];
} jnl;

/* Bitmap for block allocation (stored in memory) */
static uint8_t block_bitmap[(FS_MAX_BLOCKS + 7) / 8];

/* Per-block reference counts – rebuilt from inode scan on every mount.
 * A count > 1 means the block is shared (e.g. via a COW snapshot). */
static uint8_t block_refcount[FS_MAX_BLOCKS];

/* Forward declarations */
static os_error_t fs_read_block(uint32_t block, void *buffer);
static os_error_t fs_write_block(uint32_t block, const void *buffer);
static os_error_t fs_sync_cache(void);
static uint32_t fs_alloc_block(void);
static os_error_t fs_free_block(uint32_t block);
static uint32_t fs_alloc_inode(void);
static os_error_t fs_free_inode(uint32_t inode);
static os_error_t fs_read_inode(uint32_t inode, fs_inode_t *inode_data);
static os_error_t fs_write_inode(uint32_t inode, const fs_inode_t *inode_data);
static void fs_rebuild_refcounts(void);

static inline bool fd_valid(fs_file_t fd) {
    return fd >= 0 && fd < FS_MAX_OPEN_FILES && fs_state.files[fd].in_use;
}

static inline void fs_inode_location(uint32_t inode_num, uint32_t *block, uint32_t *offset) {
    uint32_t inodes_per_block = FS_BLOCK_SIZE / sizeof(fs_inode_t);
    *block  = fs_state.superblock.inode_table_block + inode_num / inodes_per_block;
    *offset = (inode_num % inodes_per_block) * sizeof(fs_inode_t);
}

/* =====================================================================
 * Journal internal helpers
 * ===================================================================== */

static uint32_t jnl_checksum(const void *data) {
    const uint8_t *p = (const uint8_t *)data;
    uint32_t sum = 0;
    for (int i = 0; i < FS_BLOCK_SIZE; i++) {
        sum += p[i];
    }
    return sum;
}

/* Remove a block from the read cache so stale data cannot be flushed back */
static void jnl_cache_invalidate(uint32_t block_nr) {
    for (int i = 0; i < FS_CACHE_ENTRIES; i++) {
        if (fs_state.cached_block[i] == block_nr) {
            fs_state.cache_dirty[i]  = false;
            fs_state.cached_block[i] = 0xFFFFFFFFU;
            return;
        }
    }
}

static os_error_t jnl_write_header(void) {
    jnl_hdr_t hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.magic       = JOURNAL_MAGIC;
    hdr.version     = JOURNAL_VERSION;
    hdr.data_blocks = JOURNAL_DATA_BLOCKS;
    hdr.head        = jnl.head;
    hdr.sequence    = jnl.sequence;
    return fs_state.device->write(jnl.start_block, &hdr, 1) == 0 ? OS_OK : OS_ERROR;
}

static void jnl_init(uint32_t start_block) {
    memset(&jnl, 0, sizeof(jnl));
    jnl.active      = true;
    jnl.start_block = start_block;
}

/**
 * Replay any committed-but-unapplied journal transaction on mount.
 * Called once from fs_mount before the filesystem becomes accessible.
 */
static os_error_t jnl_recover(void) {
    if (!jnl.active || !fs_state.device) {
        return OS_ERROR;
    }

    jnl_hdr_t hdr;
    if (fs_state.device->read(jnl.start_block, &hdr, 1) != 0) {
        return OS_ERROR;
    }

    if (hdr.magic != JOURNAL_MAGIC) {
        /* Fresh (unformatted) journal – initialize it */
        jnl.head     = 0;
        jnl.sequence = 0;
        return jnl_write_header();
    }

    jnl.head     = hdr.head;
    jnl.sequence = hdr.sequence;

    if (jnl.head == 0) {
        return OS_OK;  /* Nothing to recover */
    }

    /* Scan data slots for a COMMIT record */
    uint32_t commit_slot = 0xFFFFFFFFU;
    uint8_t  slot_buf[FS_BLOCK_SIZE];

    for (uint32_t s = 0; s < jnl.head && s < JOURNAL_DATA_BLOCKS; s++) {
        if (fs_state.device->read(jnl.start_block + 1U + s, slot_buf, 1) != 0) {
            break;
        }
        const jnl_entry_t *e = (const jnl_entry_t *)slot_buf;
        if (e->magic == JOURNAL_COMMIT_MAGIC && e->type == JE_COMMIT) {
            commit_slot = s;
            break;
        }
    }

    if (commit_slot == 0xFFFFFFFFU) {
        /* No commit found – incomplete transaction, discard */
        jnl.head = 0;
        return jnl_write_header();
    }

    /* Replay all DESCRIPTOR+DATA pairs that precede the commit */
    uint8_t data_buf[FS_BLOCK_SIZE];

    for (uint32_t s = 0; s < commit_slot; ) {
        if (fs_state.device->read(jnl.start_block + 1U + s, slot_buf, 1) != 0) {
            break;
        }
        const jnl_entry_t *e = (const jnl_entry_t *)slot_buf;

        if (e->magic == JOURNAL_MAGIC && e->type == JE_DESCRIPTOR) {
            uint32_t data_slot = s + 1U;
            if (data_slot >= commit_slot) {
                break;
            }
            if (fs_state.device->read(jnl.start_block + 1U + data_slot, data_buf, 1) == 0) {
                if (jnl_checksum(data_buf) == e->checksum) {
                    fs_state.device->write(e->block_nr, data_buf, 1);
                    jnl_cache_invalidate(e->block_nr);
                }
            }
            s += 2U;  /* skip descriptor + data */
        } else {
            s++;
        }
    }

    if (fs_state.device->sync) {
        fs_state.device->sync();
    }

    /* Reset journal – recovery complete */
    jnl.head = 0;
    jnl.sequence++;
    return jnl_write_header();
}

static os_error_t jnl_begin(void) {
    if (!jnl.active || jnl.txn_active) {
        return OS_ERROR;
    }
    jnl.txn_active       = true;
    jnl.bitmap_dirty     = false;
    jnl.superblock_dirty = false;
    jnl.txn_count        = 0;
    return OS_OK;
}

static os_error_t jnl_log(uint32_t block_nr, const void *data) {
    if (!jnl.txn_active) {
        return OS_ERROR;
    }
    /* Dedup: update existing entry for the same block */
    for (uint32_t i = 0; i < jnl.txn_count; i++) {
        if (jnl.pending[i].block_nr == block_nr) {
            memcpy(jnl.pending[i].data, data, FS_BLOCK_SIZE);
            return OS_OK;
        }
    }
    if (jnl.txn_count >= JNL_MAX_ENTRIES) {
        return OS_ERROR_NO_MEMORY;
    }
    jnl.pending[jnl.txn_count].block_nr = block_nr;
    memcpy(jnl.pending[jnl.txn_count].data, data, FS_BLOCK_SIZE);
    jnl.txn_count++;
    return OS_OK;
}

/**
 * Commit the current transaction:
 *   Phase 1 – write WAL entries to journal area + flush
 *   Phase 2 – apply changes to actual FS blocks + flush
 *   Checkpoint – reset journal head
 */
static os_error_t jnl_commit(void) {
    if (!jnl.txn_active) {
        return OS_ERROR;
    }
    if (!jnl.active || !fs_state.device) {
        jnl.txn_active = false;
        return OS_ERROR;
    }

    /* Auto-include bitmap and superblock if they were touched.
     * Both must fit in the pending buffer — fail the whole commit if not,
     * so the WAL never records a partial transaction.                    */
    if (jnl.bitmap_dirty) {
        if (jnl_log(1U, block_bitmap) != OS_OK) {
            jnl.txn_active = false;
            return OS_ERROR_NO_MEMORY;
        }
    }
    if (jnl.superblock_dirty) {
        if (jnl_log(0U, &fs_state.superblock) != OS_OK) {
            jnl.txn_active = false;
            return OS_ERROR_NO_MEMORY;
        }
    }

    if (jnl.txn_count == 0) {
        jnl.txn_active = false;
        return OS_OK;
    }

    /* Make sure there is room: need (count * 2 + 1) data slots */
    uint32_t slots_needed = jnl.txn_count * 2U + 1U;
    if (jnl.head + slots_needed > JOURNAL_DATA_BLOCKS) {
        /* Journal area full – checkpoint (safe: data is in pending[]) */
        jnl.head = 0;
        jnl_write_header();
    }

    /* Phase 1: write descriptor+data pairs then commit record */
    for (uint32_t i = 0; i < jnl.txn_count; i++) {
        jnl_entry_t desc;
        memset(&desc, 0, sizeof(desc));
        desc.magic    = JOURNAL_MAGIC;
        desc.sequence = jnl.sequence;
        desc.type     = JE_DESCRIPTOR;
        desc.block_nr = jnl.pending[i].block_nr;
        desc.checksum = jnl_checksum(jnl.pending[i].data);

        fs_state.device->write(jnl.start_block + 1U + jnl.head, &desc, 1);
        jnl.head++;
        fs_state.device->write(jnl.start_block + 1U + jnl.head, jnl.pending[i].data, 1);
        jnl.head++;
    }

    jnl_entry_t commit;
    memset(&commit, 0, sizeof(commit));
    commit.magic    = JOURNAL_COMMIT_MAGIC;
    commit.sequence = jnl.sequence;
    commit.type     = JE_COMMIT;
    fs_state.device->write(jnl.start_block + 1U + jnl.head, &commit, 1);
    jnl.head++;

    /* Ensure the WAL is durable before touching FS blocks */
    if (fs_state.device->sync) {
        fs_state.device->sync();
    }

    /* Phase 2: apply to actual FS locations, bypass cache */
    for (uint32_t i = 0; i < jnl.txn_count; i++) {
        fs_state.device->write(jnl.pending[i].block_nr, jnl.pending[i].data, 1);
        jnl_cache_invalidate(jnl.pending[i].block_nr);
    }

    if (fs_state.device->sync) {
        fs_state.device->sync();
    }

    /* Checkpoint: reset journal */
    jnl.sequence++;
    jnl.head             = 0;
    jnl.txn_active       = false;
    jnl.txn_count        = 0;
    jnl.bitmap_dirty     = false;
    jnl.superblock_dirty = false;
    jnl_write_header();

    return OS_OK;
}

/* =====================================================================
 * Block cache helpers
 * ===================================================================== */

/**
 * Initialize file system
 */
os_error_t fs_init(void) {
    memset(&fs_state, 0, sizeof(fs_state));
    memset(&jnl, 0, sizeof(jnl));
    os_mutex_init(&fs_state.fs_lock);

    for (int i = 0; i < FS_MAX_OPEN_FILES; i++) {
        fs_state.files[i].in_use = false;
        os_mutex_init(&fs_state.files[i].lock);
    }

    for (int i = 0; i < 4; i++) {
        fs_state.dirs[i].in_use = false;
    }

    for (int i = 0; i < FS_CACHE_ENTRIES; i++) {
        fs_state.cached_block[i] = 0xFFFFFFFFU;
        fs_state.cache_dirty[i]  = false;
    }
    fs_state.cache_next = 0;

    return OS_OK;
}

/**
 * Format storage device
 *
 * Disk layout:
 *   Block 0                      : Superblock
 *   Block 1                      : Block allocation bitmap
 *   Block 2                      : Journal header
 *   Blocks 3 .. 2+JOURNAL_DATA_BLOCKS : Journal data (31 blocks)
 *   Block 2+JOURNAL_TOTAL_BLOCKS : Inode table start
 *   ...                          : Data blocks
 */
os_error_t fs_format(fs_block_device_t *device) {
    if (!device) {
        return OS_ERROR_INVALID_PARAM;
    }

    fs_superblock_t sb;
    memset(&sb, 0, sizeof(sb));

    sb.magic       = FS_MAGIC;
    sb.version     = FS_VERSION;
    sb.block_size  = FS_BLOCK_SIZE;
    sb.total_blocks = device->get_block_count();
    sb.total_inodes = 128;

    /* Journal occupies blocks 2 .. 2+JOURNAL_TOTAL_BLOCKS-1 */
    sb.journal_start       = 2U;
    sb.journal_block_count = JOURNAL_TOTAL_BLOCKS;

    /* Inode table immediately after journal */
    sb.inode_table_block = sb.journal_start + sb.journal_block_count;

    uint32_t inode_blocks = (sb.total_inodes * sizeof(fs_inode_t) + FS_BLOCK_SIZE - 1U) / FS_BLOCK_SIZE;
    sb.first_data_block = sb.inode_table_block + inode_blocks;
    sb.free_blocks  = sb.total_blocks - sb.first_data_block;
    sb.free_inodes  = sb.total_inodes - 1U;  /* root dir uses inode 0 */
    sb.root_inode   = 0;

    /* Erase device */
    if (device->erase) {
        device->erase(0, sb.total_blocks);
    }

    /* Write superblock */
    device->write(0, &sb, 1);

    /* Initialize bitmap – mark reserved blocks as used */
    memset(block_bitmap, 0, sizeof(block_bitmap));
    for (uint32_t i = 0; i < sb.first_data_block; i++) {
        block_bitmap[i / 8] |= (uint8_t)(1U << (i % 8));
    }
    device->write(1, block_bitmap, 1);

    /* Initialize journal header */
    jnl_hdr_t jhdr;
    memset(&jhdr, 0, sizeof(jhdr));
    jhdr.magic       = JOURNAL_MAGIC;
    jhdr.version     = JOURNAL_VERSION;
    jhdr.data_blocks = JOURNAL_DATA_BLOCKS;
    jhdr.head        = 0;
    jhdr.sequence    = 0;
    device->write(sb.journal_start, &jhdr, 1);

    /* Create root directory inode */
    fs_inode_t root_inode;
    memset(&root_inode, 0, sizeof(root_inode));
    root_inode.type  = FS_TYPE_DIRECTORY;
    root_inode.size  = 0;
    root_inode.ctime = os_get_tick_count();
    root_inode.mtime = root_inode.ctime;

    uint8_t inode_block[FS_BLOCK_SIZE];
    memset(inode_block, 0, FS_BLOCK_SIZE);
    memcpy(inode_block, &root_inode, sizeof(fs_inode_t));
    device->write(sb.inode_table_block, inode_block, 1);

    if (device->sync) {
        device->sync();
    }

    return OS_OK;
}

/**
 * Mount file system
 */
os_error_t fs_mount(fs_block_device_t *device) {
    if (!device) {
        return OS_ERROR_INVALID_PARAM;
    }

    os_mutex_lock(&fs_state.fs_lock, 1000);

    if (fs_state.mounted) {
        os_mutex_unlock(&fs_state.fs_lock);
        return OS_ERROR;
    }

    /* Read and validate superblock */
    if (device->read(0, &fs_state.superblock, 1) != 0) {
        os_mutex_unlock(&fs_state.fs_lock);
        return OS_ERROR;
    }

    if (fs_state.superblock.magic != FS_MAGIC) {
        os_mutex_unlock(&fs_state.fs_lock);
        return OS_ERROR;
    }

    /* Read bitmap */
    device->read(1, block_bitmap, 1);

    fs_state.device  = device;
    fs_state.mounted = true;

    /* Run journal recovery if this is a v2 filesystem with a journal */
    if (fs_state.superblock.version == FS_VERSION &&
        fs_state.superblock.journal_start != 0) {
        jnl_init(fs_state.superblock.journal_start);
        jnl_recover();
        /* Re-read superblock and bitmap – recovery may have updated them */
        device->read(0, &fs_state.superblock, 1);
        device->read(1, block_bitmap, 1);
    }

    /* Rebuild per-block reference counts from live inodes */
    fs_rebuild_refcounts();

    os_mutex_unlock(&fs_state.fs_lock);
    return OS_OK;
}

/**
 * Unmount file system
 */
os_error_t fs_unmount(void) {
    os_mutex_lock(&fs_state.fs_lock, 1000);

    if (!fs_state.mounted) {
        os_mutex_unlock(&fs_state.fs_lock);
        return OS_ERROR;
    }

    /* Sync cached data */
    fs_sync_cache();

    /* Write bitmap */
    fs_state.device->write(1, block_bitmap, 1);

    /* Sync device */
    if (fs_state.device->sync) {
        fs_state.device->sync();
    }

    fs_state.mounted = false;
    fs_state.device  = NULL;
    jnl.active       = false;

    os_mutex_unlock(&fs_state.fs_lock);
    return OS_OK;
}

/**
 * Check if file system is mounted
 */
bool fs_is_mounted(void) {
    return fs_state.mounted;
}

/**
 * Read block from storage (through cache)
 */
static os_error_t fs_read_block(uint32_t block, void *buffer) {
    if (!fs_state.mounted) {
        return OS_ERROR;
    }

    /* Check cache */
    for (int i = 0; i < FS_CACHE_ENTRIES; i++) {
        if (block == fs_state.cached_block[i]) {
            memcpy(buffer, fs_state.block_cache[i], FS_BLOCK_SIZE);
            return OS_OK;
        }
    }

    /* Read from device */
    if (fs_state.device->read(block, buffer, 1) != 0) {
        return OS_ERROR;
    }

    /* Evict round-robin slot */
    uint8_t slot = fs_state.cache_next;
    if (fs_state.cache_dirty[slot]) {
        fs_state.device->write(fs_state.cached_block[slot], fs_state.block_cache[slot], 1);
        fs_state.cache_dirty[slot] = false;
    }
    memcpy(fs_state.block_cache[slot], buffer, FS_BLOCK_SIZE);
    fs_state.cached_block[slot] = block;
    fs_state.cache_next = (uint8_t)((slot + 1U) % FS_CACHE_ENTRIES);

    return OS_OK;
}

/**
 * Write block to storage (through cache)
 */
static os_error_t fs_write_block(uint32_t block, const void *buffer) {
    if (!fs_state.mounted) {
        return OS_ERROR;
    }

    /* Update cache if this block is already cached */
    for (int i = 0; i < FS_CACHE_ENTRIES; i++) {
        if (block == fs_state.cached_block[i]) {
            memcpy(fs_state.block_cache[i], buffer, FS_BLOCK_SIZE);
            fs_state.cache_dirty[i] = true;
            return OS_OK;
        }
    }

    /* Write directly to device */
    if (fs_state.device->write(block, buffer, 1) != 0) {
        return OS_ERROR;
    }

    return OS_OK;
}

/**
 * Sync cached blocks to storage
 */
static os_error_t fs_sync_cache(void) {
    for (int i = 0; i < FS_CACHE_ENTRIES; i++) {
        if (fs_state.cache_dirty[i] && fs_state.cached_block[i] != 0xFFFFFFFFU) {
            if (fs_state.device->write(fs_state.cached_block[i], fs_state.block_cache[i], 1) != 0) {
                return OS_ERROR;
            }
            fs_state.cache_dirty[i] = false;
        }
    }
    return OS_OK;
}

/**
 * Allocate a free block
 */
static uint32_t fs_alloc_block(void) {
    for (uint32_t i = fs_state.superblock.first_data_block;
         i < fs_state.superblock.total_blocks && i < FS_MAX_BLOCKS; i++) {
        uint32_t byte = i / 8;
        uint32_t bit  = i % 8;

        if (!(block_bitmap[byte] & (uint8_t)(1U << bit))) {
            block_bitmap[byte] |= (uint8_t)(1U << bit);
            fs_state.superblock.free_blocks--;
            block_refcount[i] = 1;
            if (jnl.txn_active) {
                jnl.bitmap_dirty     = true;
                jnl.superblock_dirty = true;
            }
            return i;
        }
    }
    return 0xFFFFFFFFU;  /* No free blocks */
}

/**
 * Free a block.
 * With COW semantics, the block is only returned to the free pool when
 * its reference count drops to zero.
 */
static os_error_t fs_free_block(uint32_t block) {
    if (block >= fs_state.superblock.total_blocks || block >= FS_MAX_BLOCKS) {
        return OS_ERROR_INVALID_PARAM;
    }

    if (block_refcount[block] > 1) {
        /* Other inodes still share this block – only release our reference */
        block_refcount[block]--;
        return OS_OK;
    }

    /* Last reference: return block to free pool */
    block_refcount[block] = 0;
    uint32_t byte = block / 8;
    uint32_t bit  = block % 8;

    block_bitmap[byte] &= (uint8_t)~(1U << bit);
    fs_state.superblock.free_blocks++;
    if (jnl.txn_active) {
        jnl.bitmap_dirty     = true;
        jnl.superblock_dirty = true;
    }
    return OS_OK;
}

/**
 * Allocate a free inode
 */
static uint32_t fs_alloc_inode(void) {
    for (uint32_t i = 0; i < fs_state.superblock.total_inodes; i++) {
        fs_inode_t inode;
        if (fs_read_inode(i, &inode) == OS_OK) {
            if (inode.type == 0) {
                fs_state.superblock.free_inodes--;
                if (jnl.txn_active) {
                    jnl.superblock_dirty = true;
                }
                return i;
            }
        }
    }
    return 0xFFFFFFFFU;
}

/**
 * Free an inode
 */
static os_error_t fs_free_inode(uint32_t inode_num) {
    fs_inode_t old_inode;
    if (fs_read_inode(inode_num, &old_inode) == OS_OK) {
        for (int i = 0; i < 6; i++) {
            if (old_inode.blocks[i] != 0) {
                fs_free_block(old_inode.blocks[i]);
            }
        }
    }

    fs_state.superblock.free_inodes++;
    if (jnl.txn_active) {
        jnl.superblock_dirty = true;
    }

    fs_inode_t zero_inode;
    memset(&zero_inode, 0, sizeof(zero_inode));
    return fs_write_inode(inode_num, &zero_inode);
}

/**
 * Read inode from storage
 */
static os_error_t fs_read_inode(uint32_t inode_num, fs_inode_t *inode_data) {
    if (inode_num >= fs_state.superblock.total_inodes) {
        return OS_ERROR_INVALID_PARAM;
    }

    uint32_t block, offset;
    fs_inode_location(inode_num, &block, &offset);

    uint8_t buffer[FS_BLOCK_SIZE];
    if (fs_read_block(block, buffer) != OS_OK) {
        return OS_ERROR;
    }

    memcpy(inode_data, buffer + offset, sizeof(fs_inode_t));
    return OS_OK;
}

/**
 * Write inode to storage.
 * Inside an active journal transaction the write is logged; the commit
 * phase will apply it atomically together with bitmap/superblock changes.
 */
static os_error_t fs_write_inode(uint32_t inode_num, const fs_inode_t *inode_data) {
    if (inode_num >= fs_state.superblock.total_inodes) {
        return OS_ERROR_INVALID_PARAM;
    }

    uint32_t block, offset;
    fs_inode_location(inode_num, &block, &offset);

    uint8_t buffer[FS_BLOCK_SIZE];
    if (fs_read_block(block, buffer) != OS_OK) {
        return OS_ERROR;
    }

    memcpy(buffer + offset, inode_data, sizeof(fs_inode_t));

    if (jnl.txn_active) {
        return jnl_log(block, buffer);
    }
    return fs_write_block(block, buffer);
}

/**
 * Find file in directory
 */
static int32_t fs_find_in_dir(uint32_t dir_inode, const char *name) {
    fs_inode_t inode;
    if (fs_read_inode(dir_inode, &inode) != OS_OK) {
        return -1;
    }

    if (inode.type != FS_TYPE_DIRECTORY) {
        return -1;
    }

    uint32_t entries_per_block = FS_BLOCK_SIZE / sizeof(fs_dentry_t);
    uint8_t buffer[FS_BLOCK_SIZE];

    for (int b = 0; b < 6 && inode.blocks[b] != 0; b++) {
        if (fs_read_block(inode.blocks[b], buffer) != OS_OK) {
            continue;
        }

        fs_dentry_t *entries = (fs_dentry_t *)buffer;
        for (uint32_t i = 0; i < entries_per_block; i++) {
            if (entries[i].inode != 0 && strcmp(entries[i].name, name) == 0) {
                return (int32_t)entries[i].inode;
            }
        }
    }

    return -1;
}

/**
 * Open file
 */
fs_file_t fs_open(const char *path, uint32_t flags) {
    if (!fs_state.mounted || !path) {
        return FS_INVALID_FD;
    }

    os_mutex_lock(&fs_state.fs_lock, 1000);

    /* Find free file handle */
    int fd = -1;
    for (int i = 0; i < FS_MAX_OPEN_FILES; i++) {
        if (!fs_state.files[i].in_use) {
            fd = i;
            break;
        }
    }

    if (fd < 0) {
        os_mutex_unlock(&fs_state.fs_lock);
        return FS_INVALID_FD;
    }

    const char *filename = (path[0] == '/') ? path + 1 : path;

    int32_t inode_num = fs_find_in_dir(fs_state.superblock.root_inode, filename);

    if (inode_num < 0) {
        if (!(flags & FS_O_CREAT)) {
            os_mutex_unlock(&fs_state.fs_lock);
            return FS_INVALID_FD;
        }

        /* Create new file – journal the entire creation atomically */
        jnl_begin();

        inode_num = (int32_t)fs_alloc_inode();
        if (inode_num < 0) {
            jnl.txn_active = false;
            os_mutex_unlock(&fs_state.fs_lock);
            return FS_INVALID_FD;
        }

        /* Initialize inode */
        fs_inode_t new_inode;
        memset(&new_inode, 0, sizeof(new_inode));
        new_inode.type  = FS_TYPE_REGULAR;
        new_inode.size  = 0;
        new_inode.ctime = os_get_tick_count();
        new_inode.mtime = new_inode.ctime;
        fs_write_inode((uint32_t)inode_num, &new_inode);  /* logged */

        /* Add dentry to root directory */
        fs_inode_t dir_inode;
        fs_read_inode(fs_state.superblock.root_inode, &dir_inode);

        if (dir_inode.blocks[0] == 0) {
            dir_inode.blocks[0] = fs_alloc_block();  /* sets bitmap/sb dirty */
            uint8_t empty_block[FS_BLOCK_SIZE];
            memset(empty_block, 0, FS_BLOCK_SIZE);
            /* Log new directory data block */
            jnl_log(dir_inode.blocks[0], empty_block);
        }

        uint8_t dir_buffer[FS_BLOCK_SIZE];
        fs_read_block(dir_inode.blocks[0], dir_buffer);
        fs_dentry_t *entries = (fs_dentry_t *)dir_buffer;

        for (size_t i = 0; i < FS_BLOCK_SIZE / sizeof(fs_dentry_t); i++) {
            if (entries[i].inode == 0) {
                strncpy(entries[i].name, filename, FS_MAX_FILENAME_LENGTH - 1);
                entries[i].name[FS_MAX_FILENAME_LENGTH - 1] = '\0';
                entries[i].inode = (uint32_t)inode_num;
                entries[i].type  = FS_TYPE_REGULAR;
                break;
            }
        }

        jnl_log(dir_inode.blocks[0], dir_buffer);  /* log updated dentry block */

        dir_inode.size += sizeof(fs_dentry_t);
        fs_write_inode(fs_state.superblock.root_inode, &dir_inode);  /* logged */

        jnl_commit();
    }

    /* Read inode */
    fs_inode_t inode;
    fs_read_inode((uint32_t)inode_num, &inode);

    /* Truncate if requested */
    if ((flags & FS_O_TRUNC) && inode.size > 0) {
        jnl_begin();
        for (int i = 0; i < 6; i++) {
            if (inode.blocks[i] != 0) {
                fs_free_block(inode.blocks[i]);
                inode.blocks[i] = 0;
            }
        }
        inode.size = 0;
        fs_write_inode((uint32_t)inode_num, &inode);
        jnl_commit();
    }

    /* Set up file handle */
    fs_state.files[fd].in_use   = true;
    fs_state.files[fd].inode    = (uint32_t)inode_num;
    fs_state.files[fd].flags    = flags;
    fs_state.files[fd].position = (flags & FS_O_APPEND) ? inode.size : 0U;
    fs_state.files[fd].size     = inode.size;

    os_mutex_unlock(&fs_state.fs_lock);
    return fd;
}

/**
 * Close file
 */
os_error_t fs_close(fs_file_t fd) {
    if (!fd_valid(fd)) {
        return OS_ERROR_INVALID_PARAM;
    }

    os_mutex_lock(&fs_state.files[fd].lock, 1000);

    fs_sync_cache();
    fs_state.files[fd].in_use = false;

    os_mutex_unlock(&fs_state.files[fd].lock);
    return OS_OK;
}

/**
 * Read from file
 */
int32_t fs_read(fs_file_t fd, void *buffer, size_t size) {
    if (!fd_valid(fd)) {
        return -1;
    }

    if (!(fs_state.files[fd].flags & FS_O_RDONLY)) {
        return -1;
    }

    os_mutex_lock(&fs_state.files[fd].lock, 1000);

    fs_inode_t inode;
    fs_read_inode(fs_state.files[fd].inode, &inode);

    uint32_t pos       = fs_state.files[fd].position;
    uint32_t remaining = (inode.size > pos) ? (inode.size - pos) : 0U;
    uint32_t to_read   = ((uint32_t)size < remaining) ? (uint32_t)size : remaining;
    uint32_t bytes_read = 0;

    uint8_t block_buffer[FS_BLOCK_SIZE];

    while (bytes_read < to_read) {
        uint32_t block_idx = pos / FS_BLOCK_SIZE;
        uint32_t offset    = pos % FS_BLOCK_SIZE;
        uint32_t chunk     = FS_BLOCK_SIZE - offset;

        if (chunk > to_read - bytes_read) {
            chunk = to_read - bytes_read;
        }

        if (block_idx >= 6 || inode.blocks[block_idx] == 0) {
            break;
        }

        fs_read_block(inode.blocks[block_idx], block_buffer);
        memcpy((uint8_t *)buffer + bytes_read, block_buffer + offset, chunk);

        bytes_read += chunk;
        pos        += chunk;
    }

    fs_state.files[fd].position = pos;

    os_mutex_unlock(&fs_state.files[fd].lock);
    return (int32_t)bytes_read;
}

/**
 * Write to file
 */
int32_t fs_write(fs_file_t fd, const void *buffer, size_t size) {
    if (!fd_valid(fd)) {
        return -1;
    }

    if (!(fs_state.files[fd].flags & FS_O_WRONLY)) {
        return -1;
    }

    os_mutex_lock(&fs_state.files[fd].lock, 1000);

    fs_inode_t inode;
    fs_read_inode(fs_state.files[fd].inode, &inode);

    uint32_t pos           = fs_state.files[fd].position;
    uint32_t bytes_written = 0;
    uint8_t  block_buffer[FS_BLOCK_SIZE];
    bool     metadata_changed = false;

    while (bytes_written < (uint32_t)size) {
        uint32_t block_idx = pos / FS_BLOCK_SIZE;
        uint32_t offset    = pos % FS_BLOCK_SIZE;
        uint32_t chunk     = FS_BLOCK_SIZE - offset;

        if (chunk > (uint32_t)size - bytes_written) {
            chunk = (uint32_t)size - bytes_written;
        }

        if (block_idx >= 6) {
            break;
        }

        /* Allocate block if needed – metadata change, needs journaling */
        if (inode.blocks[block_idx] == 0) {
            jnl_begin();
            inode.blocks[block_idx] = fs_alloc_block();  /* sets bitmap/sb dirty */
            if (inode.blocks[block_idx] == 0xFFFFFFFFU) {
                jnl.txn_active = false;
                break;
            }
            memset(block_buffer, 0, FS_BLOCK_SIZE);
            memcpy(block_buffer + offset, (const uint8_t *)buffer + bytes_written, chunk);
            /* Write data block directly (not journaled – ordered mode) */
            fs_state.device->write(inode.blocks[block_idx], block_buffer, 1);
            jnl_cache_invalidate(inode.blocks[block_idx]);
            /* Journal the updated inode (new block pointer) */
            if (pos + chunk > inode.size) {
                inode.size  = pos + chunk;
                inode.mtime = os_get_tick_count();
            }
            fs_write_inode(fs_state.files[fd].inode, &inode);  /* logged */
            jnl_commit();
            metadata_changed = true;
        } else {
            /* Block exists – read current content, apply new data */
            fs_read_block(inode.blocks[block_idx], block_buffer);
            memcpy(block_buffer + offset, (const uint8_t *)buffer + bytes_written, chunk);

            if (block_refcount[inode.blocks[block_idx]] > 1) {
                /* COW: block is shared – write to a private copy */
                jnl_begin();
                uint32_t old_block = inode.blocks[block_idx];
                uint32_t new_block = fs_alloc_block();
                if (new_block != 0xFFFFFFFFU) {
                    inode.blocks[block_idx] = new_block;
                    /* Write new data to the private block (ordered, not journaled) */
                    fs_state.device->write(new_block, block_buffer, 1);
                    jnl_cache_invalidate(new_block);
                    /* Release shared reference on old block */
                    block_refcount[old_block]--;
                    /* Journal the updated inode (new block pointer) */
                    fs_write_inode(fs_state.files[fd].inode, &inode);
                    jnl_commit();
                    metadata_changed = true;
                } else {
                    jnl.txn_active = false;
                }
            } else {
                /* Unshared block – write in-place */
                fs_write_block(inode.blocks[block_idx], block_buffer);
            }
        }

        bytes_written += chunk;
        pos           += chunk;
    }

    /* Update inode size if it grew (and we haven't already journaled it) */
    if (pos > inode.size && !metadata_changed) {
        jnl_begin();
        inode.size  = pos;
        inode.mtime = os_get_tick_count();
        fs_write_inode(fs_state.files[fd].inode, &inode);
        jnl_commit();
    }

    fs_state.files[fd].position = pos;
    fs_state.files[fd].size     = inode.size;

    os_mutex_unlock(&fs_state.files[fd].lock);
    return (int32_t)bytes_written;
}

/**
 * Seek to position
 */
int32_t fs_seek(fs_file_t fd, int32_t offset, int whence) {
    if (!fd_valid(fd)) {
        return -1;
    }

    os_mutex_lock(&fs_state.files[fd].lock, 1000);

    int32_t new_pos;

    switch (whence) {
        case FS_SEEK_SET:
            new_pos = offset;
            break;
        case FS_SEEK_CUR:
            new_pos = (int32_t)fs_state.files[fd].position + offset;
            break;
        case FS_SEEK_END:
            new_pos = (int32_t)fs_state.files[fd].size + offset;
            break;
        default:
            os_mutex_unlock(&fs_state.files[fd].lock);
            return -1;
    }

    if (new_pos < 0) {
        new_pos = 0;
    }

    fs_state.files[fd].position = (uint32_t)new_pos;

    os_mutex_unlock(&fs_state.files[fd].lock);
    return new_pos;
}

/**
 * Get current position
 */
int32_t fs_tell(fs_file_t fd) {
    if (!fd_valid(fd)) {
        return -1;
    }
    return (int32_t)fs_state.files[fd].position;
}

/**
 * Get file size
 */
int32_t fs_size(fs_file_t fd) {
    if (!fd_valid(fd)) {
        return -1;
    }
    return (int32_t)fs_state.files[fd].size;
}

/**
 * Sync file data to storage
 */
os_error_t fs_sync(fs_file_t fd) {
    if (!fd_valid(fd)) {
        return OS_ERROR_INVALID_PARAM;
    }

    fs_sync_cache();

    if (fs_state.device && fs_state.device->sync) {
        fs_state.device->sync();
    }

    return OS_OK;
}

/**
 * Remove file
 */
os_error_t fs_remove(const char *path) {
    if (!fs_state.mounted || !path) {
        return OS_ERROR_INVALID_PARAM;
    }

    const char *filename = (path[0] == '/') ? path + 1 : path;

    int32_t inode_num = fs_find_in_dir(fs_state.superblock.root_inode, filename);
    if (inode_num < 0) {
        return OS_ERROR;
    }

    jnl_begin();

    /* Free inode and its blocks (sets bitmap/sb dirty) */
    fs_free_inode((uint32_t)inode_num);

    /* Remove dentry from directory */
    fs_inode_t dir_inode;
    fs_read_inode(fs_state.superblock.root_inode, &dir_inode);

    uint8_t dir_buffer[FS_BLOCK_SIZE];
    bool dentry_removed = false;
    for (int b = 0; b < 6 && dir_inode.blocks[b] != 0 && !dentry_removed; b++) {
        fs_read_block(dir_inode.blocks[b], dir_buffer);
        fs_dentry_t *entries = (fs_dentry_t *)dir_buffer;

        for (size_t i = 0; i < FS_BLOCK_SIZE / sizeof(fs_dentry_t); i++) {
            if (entries[i].inode == (uint32_t)inode_num) {
                memset(&entries[i], 0, sizeof(fs_dentry_t));
                jnl_log(dir_inode.blocks[b], dir_buffer);
                dentry_removed = true;
                break;
            }
        }
    }

    jnl_commit();
    return OS_OK;
}

/**
 * Get file statistics
 */
os_error_t fs_stat(const char *path, fs_stat_t *stat) {
    if (!fs_state.mounted || !path || !stat) {
        return OS_ERROR_INVALID_PARAM;
    }

    const char *filename = (path[0] == '/') ? path + 1 : path;

    int32_t inode_num = fs_find_in_dir(fs_state.superblock.root_inode, filename);
    if (inode_num < 0) {
        return OS_ERROR;
    }

    fs_inode_t inode;
    if (fs_read_inode((uint32_t)inode_num, &inode) != OS_OK) {
        return OS_ERROR;
    }

    stat->type  = inode.type;
    stat->size  = inode.size;
    stat->mtime = inode.mtime;
    stat->ctime = inode.ctime;

    stat->blocks = 0;
    for (int i = 0; i < 6; i++) {
        if (inode.blocks[i] != 0) {
            stat->blocks++;
        }
    }

    return OS_OK;
}

/**
 * Get file system statistics
 */
os_error_t fs_get_stats(fs_stats_t *stats) {
    if (!fs_state.mounted || !stats) {
        return OS_ERROR_INVALID_PARAM;
    }

    stats->total_blocks = fs_state.superblock.total_blocks;
    stats->used_blocks  = fs_state.superblock.total_blocks - fs_state.superblock.free_blocks;
    stats->free_blocks  = fs_state.superblock.free_blocks;
    stats->block_size   = fs_state.superblock.block_size;
    stats->total_files  = fs_state.superblock.total_inodes - fs_state.superblock.free_inodes;
    stats->total_dirs   = 1;

    return OS_OK;
}

/**
 * Get free space in bytes
 */
uint32_t fs_get_free_space(void) {
    if (!fs_state.mounted) {
        return 0;
    }
    return fs_state.superblock.free_blocks * fs_state.superblock.block_size;
}

/**
 * Get total space in bytes
 */
uint32_t fs_get_total_space(void) {
    if (!fs_state.mounted) {
        return 0;
    }
    return fs_state.superblock.total_blocks * fs_state.superblock.block_size;
}

/**
 * Truncate file
 */
os_error_t fs_truncate(fs_file_t fd, uint32_t size) {
    if (!fd_valid(fd)) {
        return OS_ERROR_INVALID_PARAM;
    }

    os_mutex_lock(&fs_state.files[fd].lock, 1000);

    fs_inode_t inode;
    fs_read_inode(fs_state.files[fd].inode, &inode);

    jnl_begin();

    if (size < inode.size) {
        uint32_t new_blocks = (size + FS_BLOCK_SIZE - 1U) / FS_BLOCK_SIZE;
        for (uint32_t i = new_blocks; i < 6; i++) {
            if (inode.blocks[i] != 0) {
                fs_free_block(inode.blocks[i]);
                inode.blocks[i] = 0;
            }
        }
    }

    inode.size  = size;
    inode.mtime = os_get_tick_count();
    fs_write_inode(fs_state.files[fd].inode, &inode);
    fs_state.files[fd].size = size;

    jnl_commit();

    os_mutex_unlock(&fs_state.files[fd].lock);
    return OS_OK;
}

/**
 * Rename file
 */
os_error_t fs_rename(const char *old_path, const char *new_path) {
    if (!fs_state.mounted || !old_path || !new_path) {
        return OS_ERROR_INVALID_PARAM;
    }

    const char *old_name = (old_path[0] == '/') ? old_path + 1 : old_path;
    const char *new_name = (new_path[0] == '/') ? new_path + 1 : new_path;

    int32_t inode_num = fs_find_in_dir(fs_state.superblock.root_inode, old_name);
    if (inode_num < 0) {
        return OS_ERROR;
    }

    if (fs_find_in_dir(fs_state.superblock.root_inode, new_name) >= 0) {
        return OS_ERROR;
    }

    fs_inode_t dir_inode;
    fs_read_inode(fs_state.superblock.root_inode, &dir_inode);

    uint8_t dir_buffer[FS_BLOCK_SIZE];
    for (int b = 0; b < 6 && dir_inode.blocks[b] != 0; b++) {
        fs_read_block(dir_inode.blocks[b], dir_buffer);
        fs_dentry_t *entries = (fs_dentry_t *)dir_buffer;

        for (size_t i = 0; i < FS_BLOCK_SIZE / sizeof(fs_dentry_t); i++) {
            if (entries[i].inode == (uint32_t)inode_num) {
                strncpy(entries[i].name, new_name, FS_MAX_FILENAME_LENGTH - 1);
                entries[i].name[FS_MAX_FILENAME_LENGTH - 1] = '\0';

                jnl_begin();
                jnl_log(dir_inode.blocks[b], dir_buffer);
                jnl_commit();
                return OS_OK;
            }
        }
    }

    return OS_ERROR;
}

/**
 * Create directory
 */
os_error_t fs_mkdir(const char *path) {
    if (!fs_state.mounted || !path) {
        return OS_ERROR_INVALID_PARAM;
    }

    const char *dirname = (path[0] == '/') ? path + 1 : path;

    if (fs_find_in_dir(fs_state.superblock.root_inode, dirname) >= 0) {
        return OS_ERROR;
    }

    jnl_begin();

    uint32_t inode_num = fs_alloc_inode();
    if (inode_num == 0xFFFFFFFFU) {
        jnl.txn_active = false;
        return OS_ERROR_NO_MEMORY;
    }

    fs_inode_t new_inode;
    memset(&new_inode, 0, sizeof(new_inode));
    new_inode.type  = FS_TYPE_DIRECTORY;
    new_inode.size  = 0;
    new_inode.ctime = os_get_tick_count();
    new_inode.mtime = new_inode.ctime;
    fs_write_inode(inode_num, &new_inode);

    fs_inode_t dir_inode;
    fs_read_inode(fs_state.superblock.root_inode, &dir_inode);

    if (dir_inode.blocks[0] == 0) {
        dir_inode.blocks[0] = fs_alloc_block();
        uint8_t empty_block[FS_BLOCK_SIZE];
        memset(empty_block, 0, FS_BLOCK_SIZE);
        jnl_log(dir_inode.blocks[0], empty_block);
    }

    uint8_t dir_buffer[FS_BLOCK_SIZE];
    fs_read_block(dir_inode.blocks[0], dir_buffer);
    fs_dentry_t *entries = (fs_dentry_t *)dir_buffer;

    for (size_t i = 0; i < FS_BLOCK_SIZE / sizeof(fs_dentry_t); i++) {
        if (entries[i].inode == 0) {
            strncpy(entries[i].name, dirname, FS_MAX_FILENAME_LENGTH - 1);
            entries[i].name[FS_MAX_FILENAME_LENGTH - 1] = '\0';
            entries[i].inode = inode_num;
            entries[i].type  = FS_TYPE_DIRECTORY;

            jnl_log(dir_inode.blocks[0], dir_buffer);

            dir_inode.size += sizeof(fs_dentry_t);
            fs_write_inode(fs_state.superblock.root_inode, &dir_inode);

            jnl_commit();
            return OS_OK;
        }
    }

    /* No space in directory */
    jnl.txn_active = false;
    fs_free_inode(inode_num);
    return OS_ERROR_NO_MEMORY;
}

/**
 * Remove directory
 */
os_error_t fs_rmdir(const char *path) {
    if (!fs_state.mounted || !path) {
        return OS_ERROR_INVALID_PARAM;
    }

    const char *dirname = (path[0] == '/') ? path + 1 : path;

    int32_t inode_num = fs_find_in_dir(fs_state.superblock.root_inode, dirname);
    if (inode_num < 0) {
        return OS_ERROR;
    }

    fs_inode_t inode;
    fs_read_inode((uint32_t)inode_num, &inode);

    if (inode.type != FS_TYPE_DIRECTORY) {
        return OS_ERROR;
    }

    if (inode.size > 0) {
        return OS_ERROR;  /* Directory not empty */
    }

    jnl_begin();

    fs_free_inode((uint32_t)inode_num);

    fs_inode_t dir_inode;
    fs_read_inode(fs_state.superblock.root_inode, &dir_inode);

    uint8_t dir_buffer[FS_BLOCK_SIZE];
    for (int b = 0; b < 6 && dir_inode.blocks[b] != 0; b++) {
        fs_read_block(dir_inode.blocks[b], dir_buffer);
        fs_dentry_t *entries = (fs_dentry_t *)dir_buffer;

        for (size_t i = 0; i < FS_BLOCK_SIZE / sizeof(fs_dentry_t); i++) {
            if (entries[i].inode == (uint32_t)inode_num) {
                memset(&entries[i], 0, sizeof(fs_dentry_t));
                jnl_log(dir_inode.blocks[b], dir_buffer);
                jnl_commit();
                return OS_OK;
            }
        }
    }

    jnl_commit();
    return OS_OK;
}

/**
 * Open directory
 */
fs_dir_t fs_opendir(const char *path) {
    if (!fs_state.mounted || !path) {
        return NULL;
    }

    struct fs_dir_s *dir = NULL;
    for (int i = 0; i < 4; i++) {
        if (!fs_state.dirs[i].in_use) {
            dir = &fs_state.dirs[i];
            break;
        }
    }

    if (!dir) {
        return NULL;
    }

    uint32_t dir_inode = fs_state.superblock.root_inode;

    if (path[0] != '\0' && !(path[0] == '/' && path[1] == '\0')) {
        const char *dirname = (path[0] == '/') ? path + 1 : path;
        int32_t inode_num   = fs_find_in_dir(fs_state.superblock.root_inode, dirname);
        if (inode_num < 0) {
            return NULL;
        }

        fs_inode_t inode;
        fs_read_inode((uint32_t)inode_num, &inode);
        if (inode.type != FS_TYPE_DIRECTORY) {
            return NULL;
        }

        dir_inode = (uint32_t)inode_num;
    }

    dir->in_use   = true;
    dir->inode    = dir_inode;
    dir->position = 0;

    return dir;
}

/**
 * Read directory entry
 */
os_error_t fs_readdir(fs_dir_t dir, fs_dirent_t *entry) {
    if (!dir || !dir->in_use || !entry) {
        return OS_ERROR_INVALID_PARAM;
    }

    fs_inode_t inode;
    fs_read_inode(dir->inode, &inode);

    if (inode.type != FS_TYPE_DIRECTORY) {
        return OS_ERROR;
    }

    uint32_t entries_per_block = FS_BLOCK_SIZE / sizeof(fs_dentry_t);
    uint32_t current_entry     = 0;
    uint8_t  buffer[FS_BLOCK_SIZE];

    for (int b = 0; b < 6 && inode.blocks[b] != 0; b++) {
        fs_read_block(inode.blocks[b], buffer);
        fs_dentry_t *entries = (fs_dentry_t *)buffer;

        for (uint32_t i = 0; i < entries_per_block; i++) {
            if (entries[i].inode != 0) {
                if (current_entry == dir->position) {
                    strncpy(entry->name, entries[i].name, FS_MAX_FILENAME_LENGTH - 1);
                    entry->name[FS_MAX_FILENAME_LENGTH - 1] = '\0';
                    entry->type = entries[i].type;

                    fs_inode_t file_inode;
                    fs_read_inode(entries[i].inode, &file_inode);
                    entry->size  = file_inode.size;
                    entry->mtime = file_inode.mtime;

                    dir->position++;
                    return OS_OK;
                }
                current_entry++;
            }
        }
    }

    return OS_ERROR;  /* No more entries */
}

/**
 * Close directory
 */
os_error_t fs_closedir(fs_dir_t dir) {
    if (!dir || !dir->in_use) {
        return OS_ERROR_INVALID_PARAM;
    }

    dir->in_use = false;
    return OS_OK;
}

/* =====================================================================
 * Copy-on-Write (COW) support
 * ===================================================================== */

/**
 * Rebuild block_refcount[] by scanning all live inodes.
 *
 * Called once from fs_mount after journal recovery.  Every block pointer
 * found in a valid inode increments that block's reference count, so
 * blocks shared by two or more inodes (e.g. after fs_snapshot) end up
 * with count > 1 and will trigger COW on the next write.
 */
static void fs_rebuild_refcounts(void) {
    memset(block_refcount, 0, sizeof(block_refcount));

    for (uint32_t i = 0; i < fs_state.superblock.total_inodes; i++) {
        fs_inode_t inode;
        if (fs_read_inode(i, &inode) != OS_OK) {
            continue;
        }
        if (inode.type == 0) {
            continue;  /* Free inode */
        }
        for (int b = 0; b < 6; b++) {
            if (inode.blocks[b] != 0 && inode.blocks[b] < FS_MAX_BLOCKS) {
                block_refcount[inode.blocks[b]]++;
            }
        }
    }
}

/**
 * Create a copy-on-write snapshot of a file.
 *
 * The snapshot inode initially points to the same data blocks as the
 * source.  Each shared block gets its reference count incremented.
 * The first write to either file (source or snapshot) will trigger a
 * COW allocation in fs_write, preserving the other copy unchanged.
 *
 * The entire operation is wrapped in a single journal transaction so
 * that a power failure leaves the filesystem either fully snapshotted
 * or completely unmodified.
 */
os_error_t fs_snapshot(const char *source, const char *snapshot_name) {
    if (!fs_state.mounted || !source || !snapshot_name) {
        return OS_ERROR_INVALID_PARAM;
    }

    os_mutex_lock(&fs_state.fs_lock, 1000);

    const char *src_name  = (source[0]        == '/') ? source        + 1 : source;
    const char *snap_name = (snapshot_name[0] == '/') ? snapshot_name + 1 : snapshot_name;

    /* Locate source */
    int32_t src_inode_num = fs_find_in_dir(fs_state.superblock.root_inode, src_name);
    if (src_inode_num < 0) {
        os_mutex_unlock(&fs_state.fs_lock);
        return OS_ERROR;
    }

    /* Snapshot name must not already exist */
    if (fs_find_in_dir(fs_state.superblock.root_inode, snap_name) >= 0) {
        os_mutex_unlock(&fs_state.fs_lock);
        return OS_ERROR;
    }

    fs_inode_t src_inode;
    if (fs_read_inode((uint32_t)src_inode_num, &src_inode) != OS_OK) {
        os_mutex_unlock(&fs_state.fs_lock);
        return OS_ERROR;
    }

    if (src_inode.type != FS_TYPE_REGULAR) {
        os_mutex_unlock(&fs_state.fs_lock);
        return OS_ERROR_INVALID_PARAM;
    }

    /* Pre-check: find a free dentry slot before opening the transaction */
    fs_inode_t dir_inode;
    fs_read_inode(fs_state.superblock.root_inode, &dir_inode);

    uint8_t dir_buffer[FS_BLOCK_SIZE];
    size_t free_slot = (size_t)-1;

    if (dir_inode.blocks[0] != 0) {
        fs_read_block(dir_inode.blocks[0], dir_buffer);
        fs_dentry_t *entries = (fs_dentry_t *)dir_buffer;
        for (size_t i = 0; i < FS_BLOCK_SIZE / sizeof(fs_dentry_t); i++) {
            if (entries[i].inode == 0) {
                free_slot = i;
                break;
            }
        }
    }

    /* If the dir block is absent or full we need to allocate – allow for that */
    bool need_new_dir_block = (dir_inode.blocks[0] == 0 || free_slot == (size_t)-1);
    if (need_new_dir_block && dir_inode.blocks[0] != 0) {
        /* All slots in block[0] used – we would need block[1] etc.; bail out */
        os_mutex_unlock(&fs_state.fs_lock);
        return OS_ERROR_NO_MEMORY;
    }

    /* ---- Begin atomic transaction ---- */
    jnl_begin();

    uint32_t snap_inode_num = fs_alloc_inode();
    if (snap_inode_num == 0xFFFFFFFFU) {
        jnl.txn_active = false;
        os_mutex_unlock(&fs_state.fs_lock);
        return OS_ERROR_NO_MEMORY;
    }

    /* Snapshot inode is a structural copy of source; both share data blocks */
    fs_inode_t snap_inode = src_inode;
    snap_inode.ctime = os_get_tick_count();
    fs_write_inode(snap_inode_num, &snap_inode);  /* journaled */

    /* Increment reference count on every shared data block */
    for (int b = 0; b < 6; b++) {
        if (src_inode.blocks[b] != 0 && src_inode.blocks[b] < FS_MAX_BLOCKS) {
            block_refcount[src_inode.blocks[b]]++;
        }
    }

    /* Install dentry in root directory */
    if (need_new_dir_block) {
        dir_inode.blocks[0] = fs_alloc_block();
        memset(dir_buffer, 0, FS_BLOCK_SIZE);
        free_slot = 0;
    }

    fs_dentry_t *entries = (fs_dentry_t *)dir_buffer;
    strncpy(entries[free_slot].name, snap_name, FS_MAX_FILENAME_LENGTH - 1);
    entries[free_slot].name[FS_MAX_FILENAME_LENGTH - 1] = '\0';
    entries[free_slot].inode = snap_inode_num;
    entries[free_slot].type  = FS_TYPE_REGULAR;
    jnl_log(dir_inode.blocks[0], dir_buffer);

    dir_inode.size += sizeof(fs_dentry_t);
    fs_write_inode(fs_state.superblock.root_inode, &dir_inode);  /* journaled */

    jnl_commit();

    os_mutex_unlock(&fs_state.fs_lock);
    return OS_OK;
}

/**
 * Return the reference count of a block (diagnostic helper).
 * Returns 0 for unallocated blocks, 1 for exclusively-owned blocks,
 * and >1 for blocks shared between COW snapshots.
 */
uint8_t fs_get_block_refcount(uint32_t block_nr) {
    if (block_nr >= FS_MAX_BLOCKS) {
        return 0;
    }
    return block_refcount[block_nr];
}
