/**
 * RAM Disk Driver Header
 */

#ifndef RAMDISK_H
#define RAMDISK_H

#include "../include/tinyos.h"

/**
 * Initialize RAM disk
 */
void ramdisk_init(void);

/**
 * Get RAM disk block device interface
 * Returns pointer to block device structure for use with fs_mount()
 */
fs_block_device_t *ramdisk_get_device(void);

#endif /* RAMDISK_H */
