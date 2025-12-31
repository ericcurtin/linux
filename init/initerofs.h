/* SPDX-License-Identifier: GPL-2.0 */
/*
 * EROFS-backed early root filesystem (initerofs)
 *
 * This header provides declarations for the initerofs subsystem which
 * allows using an EROFS image directly from memory as the early root
 * filesystem, without unpacking like traditional initramfs.
 *
 * The implementation automatically detects EROFS format by checking the
 * magic number at offset 1024 in the initramfs. If detected, EROFS is mounted
 * directly instead of unpacking as cpio.
 */

#ifndef _INIT_INITEROFS_H
#define _INIT_INITEROFS_H

#include <linux/types.h>

#ifdef CONFIG_INITEROFS

/**
 * initerofs_detect - Check if initramfs contains an EROFS filesystem
 *
 * This function checks the magic number at offset 1024 in the initramfs
 * to determine if it's an EROFS image rather than a cpio archive.
 *
 * Return: true if EROFS format detected, false otherwise
 */
bool __init initerofs_detect(void);

/**
 * initerofs_mount_root - Mount the EROFS image as root filesystem
 *
 * This function mounts the EROFS filesystem from the initramfs memory region
 * with an overlayfs layer for writability.
 *
 * Return: 0 on success, negative error code on failure
 */
int __init initerofs_mount_root(void);

/**
 * initerofs_should_retain - Check if initerofs memory should be retained
 *
 * Return: true if retain_initerofs boot param was specified
 */
bool __init initerofs_should_retain(void);

/**
 * initerofs_blkdev_create - Create a memory-backed block device
 * @data: Pointer to the initramfs memory region
 * @size: Size of the initramfs in bytes
 *
 * Creates a read-only block device that serves data directly from
 * the initramfs memory region, avoiding unnecessary memory copies.
 *
 * Return: Device path string on success, NULL on failure
 */
char * __init initerofs_blkdev_create(void *data, unsigned long size);

/**
 * initerofs_blkdev_destroy - Clean up the memory-backed block device
 *
 * Called if mount fails to release resources.
 */
void __init initerofs_blkdev_destroy(void);

#else /* !CONFIG_INITEROFS */

static inline bool __init initerofs_detect(void) { return false; }
static inline int __init initerofs_mount_root(void) { return -ENODEV; }
static inline bool __init initerofs_should_retain(void) { return false; }

#endif /* CONFIG_INITEROFS */

#endif /* _INIT_INITEROFS_H */
