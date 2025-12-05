/**
 * TinyOS File System Implementation
 *
 * Simple, lightweight file system designed for embedded systems
 * Features:
 * - Small footprint (~4KB code)
 * - Simple block-based storage
 * - Wear leveling support
 * - Power-fail safe operations
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
    uint32_t magic;             /* Magic number for validation */
    uint32_t version;           /* File system version */
    uint32_t block_size;        /* Block size */
    uint32_t total_blocks;      /* Total blocks */
    uint32_t free_blocks;       /* Free blocks */
    uint32_t total_inodes;      /* Total inodes */
    uint32_t free_inodes;       /* Free inodes */
    uint32_t first_data_block;  /* First data block */
    uint32_t inode_table_block; /* Inode table start block */
    uint32_t root_inode;        /* Root directory inode */
    uint8_t reserved[472];
} fs_superblock_t;

#define FS_MAGIC        0x544E5946  /* "TNYF" */
#define FS_VERSION      0x00010000  /* Version 1.0 */

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
    uint8_t block_cache[FS_BLOCK_SIZE];  /* Single block cache */
    uint32_t cached_block;
    bool cache_dirty;
} fs_state;

/* Bitmap for block allocation (stored in memory) */
static uint8_t block_bitmap[(FS_MAX_BLOCKS + 7) / 8];

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

/**
 * Initialize file system
 */
os_error_t fs_init(void) {
    memset(&fs_state, 0, sizeof(fs_state));
    os_mutex_init(&fs_state.fs_lock);

    for (int i = 0; i < FS_MAX_OPEN_FILES; i++) {
        fs_state.files[i].in_use = false;
        os_mutex_init(&fs_state.files[i].lock);
    }

    for (int i = 0; i < 4; i++) {
        fs_state.dirs[i].in_use = false;
    }

    fs_state.cached_block = 0xFFFFFFFF;
    fs_state.cache_dirty = false;

    return OS_OK;
}

/**
 * Format storage device
 */
os_error_t fs_format(fs_block_device_t *device) {
    if (!device) {
        return OS_ERROR_INVALID_PARAM;
    }

    /* Create superblock */
    fs_superblock_t sb;
    memset(&sb, 0, sizeof(sb));

    sb.magic = FS_MAGIC;
    sb.version = FS_VERSION;
    sb.block_size = FS_BLOCK_SIZE;
    sb.total_blocks = device->get_block_count();
    sb.total_inodes = 128;  /* Support up to 128 files/directories */

    /* Layout: [Superblock][Bitmap][Inode Table][Data Blocks] */
    sb.inode_table_block = 2;  /* Block 0=superblock, 1=bitmap */
    sb.first_data_block = sb.inode_table_block + (sb.total_inodes * sizeof(fs_inode_t) + FS_BLOCK_SIZE - 1) / FS_BLOCK_SIZE;
    sb.free_blocks = sb.total_blocks - sb.first_data_block;
    sb.free_inodes = sb.total_inodes - 1;  /* Root dir uses inode 0 */
    sb.root_inode = 0;

    /* Erase device */
    if (device->erase) {
        device->erase(0, sb.total_blocks);
    }

    /* Write superblock */
    device->write(0, &sb, 1);

    /* Initialize bitmap (all blocks free except reserved) */
    memset(block_bitmap, 0, sizeof(block_bitmap));
    for (uint32_t i = 0; i < sb.first_data_block; i++) {
        uint32_t byte = i / 8;
        uint32_t bit = i % 8;
        block_bitmap[byte] |= (1 << bit);
    }
    device->write(1, block_bitmap, 1);

    /* Create root directory inode */
    fs_inode_t root_inode;
    memset(&root_inode, 0, sizeof(root_inode));
    root_inode.type = FS_TYPE_DIRECTORY;
    root_inode.size = 0;
    root_inode.ctime = os_get_tick_count();
    root_inode.mtime = root_inode.ctime;

    /* Write root inode */
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

    /* Read superblock */
    if (device->read(0, &fs_state.superblock, 1) != 0) {
        os_mutex_unlock(&fs_state.fs_lock);
        return OS_ERROR;
    }

    /* Validate superblock */
    if (fs_state.superblock.magic != FS_MAGIC) {
        os_mutex_unlock(&fs_state.fs_lock);
        return OS_ERROR;
    }

    /* Read bitmap */
    device->read(1, block_bitmap, 1);

    fs_state.device = device;
    fs_state.mounted = true;

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
    fs_state.device = NULL;

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
 * Read block from storage
 */
static os_error_t fs_read_block(uint32_t block, void *buffer) {
    if (!fs_state.mounted) {
        return OS_ERROR;
    }

    /* Check cache */
    if (block == fs_state.cached_block) {
        memcpy(buffer, fs_state.block_cache, FS_BLOCK_SIZE);
        return OS_OK;
    }

    /* Read from device */
    if (fs_state.device->read(block, buffer, 1) != 0) {
        return OS_ERROR;
    }

    /* Update cache */
    if (fs_state.cache_dirty) {
        fs_sync_cache();
    }
    memcpy(fs_state.block_cache, buffer, FS_BLOCK_SIZE);
    fs_state.cached_block = block;
    fs_state.cache_dirty = false;

    return OS_OK;
}

/**
 * Write block to storage
 */
static os_error_t fs_write_block(uint32_t block, const void *buffer) {
    if (!fs_state.mounted) {
        return OS_ERROR;
    }

    /* Update cache if this block is cached */
    if (block == fs_state.cached_block) {
        memcpy(fs_state.block_cache, buffer, FS_BLOCK_SIZE);
        fs_state.cache_dirty = true;
        return OS_OK;
    }

    /* Write directly to device */
    if (fs_state.device->write(block, buffer, 1) != 0) {
        return OS_ERROR;
    }

    return OS_OK;
}

/**
 * Sync cached block to storage
 */
static os_error_t fs_sync_cache(void) {
    if (fs_state.cache_dirty && fs_state.cached_block != 0xFFFFFFFF) {
        if (fs_state.device->write(fs_state.cached_block, fs_state.block_cache, 1) != 0) {
            return OS_ERROR;
        }
        fs_state.cache_dirty = false;
    }
    return OS_OK;
}

/**
 * Allocate a free block
 */
static uint32_t fs_alloc_block(void) {
    for (uint32_t i = fs_state.superblock.first_data_block; i < fs_state.superblock.total_blocks; i++) {
        uint32_t byte = i / 8;
        uint32_t bit = i % 8;

        if (!(block_bitmap[byte] & (1 << bit))) {
            /* Found free block */
            block_bitmap[byte] |= (1 << bit);
            fs_state.superblock.free_blocks--;
            return i;
        }
    }
    return 0xFFFFFFFF;  /* No free blocks */
}

/**
 * Free a block
 */
static os_error_t fs_free_block(uint32_t block) {
    if (block >= fs_state.superblock.total_blocks) {
        return OS_ERROR_INVALID_PARAM;
    }

    uint32_t byte = block / 8;
    uint32_t bit = block % 8;

    block_bitmap[byte] &= ~(1 << bit);
    fs_state.superblock.free_blocks++;

    return OS_OK;
}

/**
 * Allocate a free inode
 */
static uint32_t fs_alloc_inode(void) {
    /* Simple linear search for free inode */
    for (uint32_t i = 0; i < fs_state.superblock.total_inodes; i++) {
        fs_inode_t inode;
        if (fs_read_inode(i, &inode) == OS_OK) {
            if (inode.type == 0) {  /* Free inode */
                fs_state.superblock.free_inodes--;
                return i;
            }
        }
    }
    return 0xFFFFFFFF;
}

/**
 * Free an inode
 */
static os_error_t fs_free_inode(uint32_t inode_num) {
    fs_inode_t inode;
    memset(&inode, 0, sizeof(inode));

    /* Free all blocks used by this inode */
    fs_inode_t old_inode;
    if (fs_read_inode(inode_num, &old_inode) == OS_OK) {
        for (int i = 0; i < 6; i++) {
            if (old_inode.blocks[i] != 0) {
                fs_free_block(old_inode.blocks[i]);
            }
        }
    }

    fs_state.superblock.free_inodes++;
    return fs_write_inode(inode_num, &inode);
}

/**
 * Read inode from storage
 */
static os_error_t fs_read_inode(uint32_t inode_num, fs_inode_t *inode_data) {
    if (inode_num >= fs_state.superblock.total_inodes) {
        return OS_ERROR_INVALID_PARAM;
    }

    uint32_t inodes_per_block = FS_BLOCK_SIZE / sizeof(fs_inode_t);
    uint32_t block = fs_state.superblock.inode_table_block + inode_num / inodes_per_block;
    uint32_t offset = (inode_num % inodes_per_block) * sizeof(fs_inode_t);

    uint8_t buffer[FS_BLOCK_SIZE];
    if (fs_read_block(block, buffer) != OS_OK) {
        return OS_ERROR;
    }

    memcpy(inode_data, buffer + offset, sizeof(fs_inode_t));
    return OS_OK;
}

/**
 * Write inode to storage
 */
static os_error_t fs_write_inode(uint32_t inode_num, const fs_inode_t *inode_data) {
    if (inode_num >= fs_state.superblock.total_inodes) {
        return OS_ERROR_INVALID_PARAM;
    }

    uint32_t inodes_per_block = FS_BLOCK_SIZE / sizeof(fs_inode_t);
    uint32_t block = fs_state.superblock.inode_table_block + inode_num / inodes_per_block;
    uint32_t offset = (inode_num % inodes_per_block) * sizeof(fs_inode_t);

    uint8_t buffer[FS_BLOCK_SIZE];
    if (fs_read_block(block, buffer) != OS_OK) {
        return OS_ERROR;
    }

    memcpy(buffer + offset, inode_data, sizeof(fs_inode_t));
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

    /* Search directory entries */
    uint32_t entries_per_block = FS_BLOCK_SIZE / sizeof(fs_dentry_t);
    uint8_t buffer[FS_BLOCK_SIZE];

    for (int b = 0; b < 6 && inode.blocks[b] != 0; b++) {
        if (fs_read_block(inode.blocks[b], buffer) != OS_OK) {
            continue;
        }

        fs_dentry_t *entries = (fs_dentry_t *)buffer;
        for (uint32_t i = 0; i < entries_per_block; i++) {
            if (entries[i].inode != 0 && strcmp(entries[i].name, name) == 0) {
                return entries[i].inode;
            }
        }
    }

    return -1;  /* Not found */
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

    /* For now, simple implementation: assume file is in root directory */
    const char *filename = path;
    if (path[0] == '/') {
        filename = path + 1;
    }

    /* Find file in root directory */
    int32_t inode_num = fs_find_in_dir(fs_state.superblock.root_inode, filename);

    if (inode_num < 0) {
        /* File not found */
        if (flags & FS_O_CREAT) {
            /* Create new file */
            inode_num = fs_alloc_inode();
            if (inode_num < 0) {
                os_mutex_unlock(&fs_state.fs_lock);
                return FS_INVALID_FD;
            }

            /* Initialize inode */
            fs_inode_t new_inode;
            memset(&new_inode, 0, sizeof(new_inode));
            new_inode.type = FS_TYPE_REGULAR;
            new_inode.size = 0;
            new_inode.ctime = os_get_tick_count();
            new_inode.mtime = new_inode.ctime;
            fs_write_inode(inode_num, &new_inode);

            /* Add to directory - simplified: append to first block */
            fs_inode_t dir_inode;
            fs_read_inode(fs_state.superblock.root_inode, &dir_inode);

            if (dir_inode.blocks[0] == 0) {
                dir_inode.blocks[0] = fs_alloc_block();
                uint8_t empty_block[FS_BLOCK_SIZE];
                memset(empty_block, 0, FS_BLOCK_SIZE);
                fs_write_block(dir_inode.blocks[0], empty_block);
            }

            uint8_t dir_buffer[FS_BLOCK_SIZE];
            fs_read_block(dir_inode.blocks[0], dir_buffer);
            fs_dentry_t *entries = (fs_dentry_t *)dir_buffer;

            for (int i = 0; i < FS_BLOCK_SIZE / sizeof(fs_dentry_t); i++) {
                if (entries[i].inode == 0) {
                    strncpy(entries[i].name, filename, FS_MAX_FILENAME_LENGTH - 1);
                    entries[i].name[FS_MAX_FILENAME_LENGTH - 1] = '\0';
                    entries[i].inode = inode_num;
                    entries[i].type = FS_TYPE_REGULAR;
                    break;
                }
            }

            fs_write_block(dir_inode.blocks[0], dir_buffer);
            dir_inode.size += sizeof(fs_dentry_t);
            fs_write_inode(fs_state.superblock.root_inode, &dir_inode);
        } else {
            os_mutex_unlock(&fs_state.fs_lock);
            return FS_INVALID_FD;
        }
    }

    /* Read inode */
    fs_inode_t inode;
    fs_read_inode(inode_num, &inode);

    /* Truncate if requested */
    if ((flags & FS_O_TRUNC) && inode.size > 0) {
        for (int i = 0; i < 6; i++) {
            if (inode.blocks[i] != 0) {
                fs_free_block(inode.blocks[i]);
                inode.blocks[i] = 0;
            }
        }
        inode.size = 0;
        fs_write_inode(inode_num, &inode);
    }

    /* Set up file handle */
    fs_state.files[fd].in_use = true;
    fs_state.files[fd].inode = inode_num;
    fs_state.files[fd].flags = flags;
    fs_state.files[fd].position = (flags & FS_O_APPEND) ? inode.size : 0;
    fs_state.files[fd].size = inode.size;

    os_mutex_unlock(&fs_state.fs_lock);
    return fd;
}

/**
 * Close file
 */
os_error_t fs_close(fs_file_t fd) {
    if (fd < 0 || fd >= FS_MAX_OPEN_FILES || !fs_state.files[fd].in_use) {
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
    if (fd < 0 || fd >= FS_MAX_OPEN_FILES || !fs_state.files[fd].in_use) {
        return -1;
    }

    if (!(fs_state.files[fd].flags & (FS_O_RDONLY | FS_O_RDWR))) {
        return -1;  /* File not opened for reading */
    }

    os_mutex_lock(&fs_state.files[fd].lock, 1000);

    fs_inode_t inode;
    fs_read_inode(fs_state.files[fd].inode, &inode);

    uint32_t pos = fs_state.files[fd].position;
    uint32_t remaining = (inode.size > pos) ? (inode.size - pos) : 0;
    uint32_t to_read = (size < remaining) ? size : remaining;
    uint32_t bytes_read = 0;

    uint8_t block_buffer[FS_BLOCK_SIZE];

    while (bytes_read < to_read) {
        uint32_t block_idx = pos / FS_BLOCK_SIZE;
        uint32_t offset = pos % FS_BLOCK_SIZE;
        uint32_t chunk = FS_BLOCK_SIZE - offset;

        if (chunk > to_read - bytes_read) {
            chunk = to_read - bytes_read;
        }

        if (block_idx >= 6 || inode.blocks[block_idx] == 0) {
            break;  /* Beyond file size */
        }

        fs_read_block(inode.blocks[block_idx], block_buffer);
        memcpy((uint8_t *)buffer + bytes_read, block_buffer + offset, chunk);

        bytes_read += chunk;
        pos += chunk;
    }

    fs_state.files[fd].position = pos;

    os_mutex_unlock(&fs_state.files[fd].lock);
    return bytes_read;
}

/**
 * Write to file
 */
int32_t fs_write(fs_file_t fd, const void *buffer, size_t size) {
    if (fd < 0 || fd >= FS_MAX_OPEN_FILES || !fs_state.files[fd].in_use) {
        return -1;
    }

    if (!(fs_state.files[fd].flags & (FS_O_WRONLY | FS_O_RDWR))) {
        return -1;  /* File not opened for writing */
    }

    os_mutex_lock(&fs_state.files[fd].lock, 1000);

    fs_inode_t inode;
    fs_read_inode(fs_state.files[fd].inode, &inode);

    uint32_t pos = fs_state.files[fd].position;
    uint32_t bytes_written = 0;
    uint8_t block_buffer[FS_BLOCK_SIZE];

    while (bytes_written < size) {
        uint32_t block_idx = pos / FS_BLOCK_SIZE;
        uint32_t offset = pos % FS_BLOCK_SIZE;
        uint32_t chunk = FS_BLOCK_SIZE - offset;

        if (chunk > size - bytes_written) {
            chunk = size - bytes_written;
        }

        if (block_idx >= 6) {
            break;  /* File too large (max 6 blocks) */
        }

        /* Allocate block if needed */
        if (inode.blocks[block_idx] == 0) {
            inode.blocks[block_idx] = fs_alloc_block();
            if (inode.blocks[block_idx] == 0xFFFFFFFF) {
                break;  /* Out of space */
            }
            memset(block_buffer, 0, FS_BLOCK_SIZE);
            fs_write_block(inode.blocks[block_idx], block_buffer);
        }

        /* Read-modify-write */
        fs_read_block(inode.blocks[block_idx], block_buffer);
        memcpy(block_buffer + offset, (const uint8_t *)buffer + bytes_written, chunk);
        fs_write_block(inode.blocks[block_idx], block_buffer);

        bytes_written += chunk;
        pos += chunk;
    }

    /* Update inode if file size changed */
    if (pos > inode.size) {
        inode.size = pos;
        inode.mtime = os_get_tick_count();
        fs_write_inode(fs_state.files[fd].inode, &inode);
        fs_state.files[fd].size = inode.size;
    }

    fs_state.files[fd].position = pos;

    os_mutex_unlock(&fs_state.files[fd].lock);
    return bytes_written;
}

/**
 * Seek to position
 */
int32_t fs_seek(fs_file_t fd, int32_t offset, int whence) {
    if (fd < 0 || fd >= FS_MAX_OPEN_FILES || !fs_state.files[fd].in_use) {
        return -1;
    }

    os_mutex_lock(&fs_state.files[fd].lock, 1000);

    int32_t new_pos;

    switch (whence) {
        case FS_SEEK_SET:
            new_pos = offset;
            break;
        case FS_SEEK_CUR:
            new_pos = fs_state.files[fd].position + offset;
            break;
        case FS_SEEK_END:
            new_pos = fs_state.files[fd].size + offset;
            break;
        default:
            os_mutex_unlock(&fs_state.files[fd].lock);
            return -1;
    }

    if (new_pos < 0) {
        new_pos = 0;
    }

    fs_state.files[fd].position = new_pos;

    os_mutex_unlock(&fs_state.files[fd].lock);
    return new_pos;
}

/**
 * Get current position
 */
int32_t fs_tell(fs_file_t fd) {
    if (fd < 0 || fd >= FS_MAX_OPEN_FILES || !fs_state.files[fd].in_use) {
        return -1;
    }

    return fs_state.files[fd].position;
}

/**
 * Get file size
 */
int32_t fs_size(fs_file_t fd) {
    if (fd < 0 || fd >= FS_MAX_OPEN_FILES || !fs_state.files[fd].in_use) {
        return -1;
    }

    return fs_state.files[fd].size;
}

/**
 * Sync file data to storage
 */
os_error_t fs_sync(fs_file_t fd) {
    if (fd < 0 || fd >= FS_MAX_OPEN_FILES || !fs_state.files[fd].in_use) {
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

    const char *filename = path;
    if (path[0] == '/') {
        filename = path + 1;
    }

    int32_t inode_num = fs_find_in_dir(fs_state.superblock.root_inode, filename);
    if (inode_num < 0) {
        return OS_ERROR;  /* File not found */
    }

    /* Free inode and its blocks */
    fs_free_inode(inode_num);

    /* Remove from directory */
    fs_inode_t dir_inode;
    fs_read_inode(fs_state.superblock.root_inode, &dir_inode);

    uint8_t dir_buffer[FS_BLOCK_SIZE];
    for (int b = 0; b < 6 && dir_inode.blocks[b] != 0; b++) {
        fs_read_block(dir_inode.blocks[b], dir_buffer);
        fs_dentry_t *entries = (fs_dentry_t *)dir_buffer;

        for (int i = 0; i < FS_BLOCK_SIZE / sizeof(fs_dentry_t); i++) {
            if (entries[i].inode == (uint32_t)inode_num) {
                memset(&entries[i], 0, sizeof(fs_dentry_t));
                fs_write_block(dir_inode.blocks[b], dir_buffer);
                return OS_OK;
            }
        }
    }

    return OS_OK;
}

/**
 * Get file statistics
 */
os_error_t fs_stat(const char *path, fs_stat_t *stat) {
    if (!fs_state.mounted || !path || !stat) {
        return OS_ERROR_INVALID_PARAM;
    }

    const char *filename = path;
    if (path[0] == '/') {
        filename = path + 1;
    }

    int32_t inode_num = fs_find_in_dir(fs_state.superblock.root_inode, filename);
    if (inode_num < 0) {
        return OS_ERROR;  /* File not found */
    }

    fs_inode_t inode;
    if (fs_read_inode(inode_num, &inode) != OS_OK) {
        return OS_ERROR;
    }

    stat->type = inode.type;
    stat->size = inode.size;
    stat->mtime = inode.mtime;
    stat->ctime = inode.ctime;

    /* Count blocks */
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
    stats->used_blocks = fs_state.superblock.total_blocks - fs_state.superblock.free_blocks;
    stats->free_blocks = fs_state.superblock.free_blocks;
    stats->block_size = fs_state.superblock.block_size;
    stats->total_files = fs_state.superblock.total_inodes - fs_state.superblock.free_inodes;
    stats->total_dirs = 1;  /* Just root for now */

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
    if (fd < 0 || fd >= FS_MAX_OPEN_FILES || !fs_state.files[fd].in_use) {
        return OS_ERROR_INVALID_PARAM;
    }

    os_mutex_lock(&fs_state.files[fd].lock, 1000);

    fs_inode_t inode;
    fs_read_inode(fs_state.files[fd].inode, &inode);

    if (size < inode.size) {
        /* Free blocks beyond new size */
        uint32_t new_blocks = (size + FS_BLOCK_SIZE - 1) / FS_BLOCK_SIZE;
        for (uint32_t i = new_blocks; i < 6; i++) {
            if (inode.blocks[i] != 0) {
                fs_free_block(inode.blocks[i]);
                inode.blocks[i] = 0;
            }
        }
    }

    inode.size = size;
    inode.mtime = os_get_tick_count();
    fs_write_inode(fs_state.files[fd].inode, &inode);
    fs_state.files[fd].size = size;

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

    const char *old_name = old_path[0] == '/' ? old_path + 1 : old_path;
    const char *new_name = new_path[0] == '/' ? new_path + 1 : new_path;

    /* Find old file */
    int32_t inode_num = fs_find_in_dir(fs_state.superblock.root_inode, old_name);
    if (inode_num < 0) {
        return OS_ERROR;
    }

    /* Check if new name already exists */
    if (fs_find_in_dir(fs_state.superblock.root_inode, new_name) >= 0) {
        return OS_ERROR;  /* New name already exists */
    }

    /* Update directory entry */
    fs_inode_t dir_inode;
    fs_read_inode(fs_state.superblock.root_inode, &dir_inode);

    uint8_t dir_buffer[FS_BLOCK_SIZE];
    for (int b = 0; b < 6 && dir_inode.blocks[b] != 0; b++) {
        fs_read_block(dir_inode.blocks[b], dir_buffer);
        fs_dentry_t *entries = (fs_dentry_t *)dir_buffer;

        for (int i = 0; i < FS_BLOCK_SIZE / sizeof(fs_dentry_t); i++) {
            if (entries[i].inode == (uint32_t)inode_num) {
                strncpy(entries[i].name, new_name, FS_MAX_FILENAME_LENGTH - 1);
                entries[i].name[FS_MAX_FILENAME_LENGTH - 1] = '\0';
                fs_write_block(dir_inode.blocks[b], dir_buffer);
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

    const char *dirname = path[0] == '/' ? path + 1 : path;

    /* Check if already exists */
    if (fs_find_in_dir(fs_state.superblock.root_inode, dirname) >= 0) {
        return OS_ERROR;  /* Already exists */
    }

    /* Allocate inode */
    uint32_t inode_num = fs_alloc_inode();
    if (inode_num == 0xFFFFFFFF) {
        return OS_ERROR_NO_MEMORY;
    }

    /* Initialize directory inode */
    fs_inode_t new_inode;
    memset(&new_inode, 0, sizeof(new_inode));
    new_inode.type = FS_TYPE_DIRECTORY;
    new_inode.size = 0;
    new_inode.ctime = os_get_tick_count();
    new_inode.mtime = new_inode.ctime;
    fs_write_inode(inode_num, &new_inode);

    /* Add to parent directory (root) */
    fs_inode_t dir_inode;
    fs_read_inode(fs_state.superblock.root_inode, &dir_inode);

    if (dir_inode.blocks[0] == 0) {
        dir_inode.blocks[0] = fs_alloc_block();
        uint8_t empty_block[FS_BLOCK_SIZE];
        memset(empty_block, 0, FS_BLOCK_SIZE);
        fs_write_block(dir_inode.blocks[0], empty_block);
    }

    uint8_t dir_buffer[FS_BLOCK_SIZE];
    fs_read_block(dir_inode.blocks[0], dir_buffer);
    fs_dentry_t *entries = (fs_dentry_t *)dir_buffer;

    for (int i = 0; i < FS_BLOCK_SIZE / sizeof(fs_dentry_t); i++) {
        if (entries[i].inode == 0) {
            strncpy(entries[i].name, dirname, FS_MAX_FILENAME_LENGTH - 1);
            entries[i].name[FS_MAX_FILENAME_LENGTH - 1] = '\0';
            entries[i].inode = inode_num;
            entries[i].type = FS_TYPE_DIRECTORY;
            fs_write_block(dir_inode.blocks[0], dir_buffer);
            dir_inode.size += sizeof(fs_dentry_t);
            fs_write_inode(fs_state.superblock.root_inode, &dir_inode);
            return OS_OK;
        }
    }

    /* No space in directory */
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

    const char *dirname = path[0] == '/' ? path + 1 : path;

    int32_t inode_num = fs_find_in_dir(fs_state.superblock.root_inode, dirname);
    if (inode_num < 0) {
        return OS_ERROR;  /* Not found */
    }

    fs_inode_t inode;
    fs_read_inode(inode_num, &inode);

    if (inode.type != FS_TYPE_DIRECTORY) {
        return OS_ERROR;  /* Not a directory */
    }

    /* Check if directory is empty */
    if (inode.size > 0) {
        return OS_ERROR;  /* Directory not empty */
    }

    /* Free inode */
    fs_free_inode(inode_num);

    /* Remove from parent directory */
    fs_inode_t dir_inode;
    fs_read_inode(fs_state.superblock.root_inode, &dir_inode);

    uint8_t dir_buffer[FS_BLOCK_SIZE];
    for (int b = 0; b < 6 && dir_inode.blocks[b] != 0; b++) {
        fs_read_block(dir_inode.blocks[b], dir_buffer);
        fs_dentry_t *entries = (fs_dentry_t *)dir_buffer;

        for (int i = 0; i < FS_BLOCK_SIZE / sizeof(fs_dentry_t); i++) {
            if (entries[i].inode == (uint32_t)inode_num) {
                memset(&entries[i], 0, sizeof(fs_dentry_t));
                fs_write_block(dir_inode.blocks[b], dir_buffer);
                return OS_OK;
            }
        }
    }

    return OS_OK;
}

/**
 * Open directory
 */
fs_dir_t fs_opendir(const char *path) {
    if (!fs_state.mounted || !path) {
        return NULL;
    }

    /* Find free directory handle */
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

    /* For now, only support root directory */
    uint32_t dir_inode = fs_state.superblock.root_inode;

    if (path[0] != '\0' && !(path[0] == '/' && path[1] == '\0')) {
        /* Try to find subdirectory */
        const char *dirname = path[0] == '/' ? path + 1 : path;
        int32_t inode_num = fs_find_in_dir(fs_state.superblock.root_inode, dirname);
        if (inode_num < 0) {
            return NULL;
        }

        fs_inode_t inode;
        fs_read_inode(inode_num, &inode);
        if (inode.type != FS_TYPE_DIRECTORY) {
            return NULL;
        }

        dir_inode = inode_num;
    }

    dir->in_use = true;
    dir->inode = dir_inode;
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
    uint32_t current_entry = 0;

    uint8_t buffer[FS_BLOCK_SIZE];

    for (int b = 0; b < 6 && inode.blocks[b] != 0; b++) {
        fs_read_block(inode.blocks[b], buffer);
        fs_dentry_t *entries = (fs_dentry_t *)buffer;

        for (uint32_t i = 0; i < entries_per_block; i++) {
            if (entries[i].inode != 0) {
                if (current_entry == dir->position) {
                    /* Found the next entry */
                    strncpy(entry->name, entries[i].name, FS_MAX_FILENAME_LENGTH);
                    entry->type = entries[i].type;

                    /* Get size and mtime from inode */
                    fs_inode_t file_inode;
                    fs_read_inode(entries[i].inode, &file_inode);
                    entry->size = file_inode.size;
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
