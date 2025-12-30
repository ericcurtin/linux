/* SPDX-License-Identifier: GPL-2.0 */
/*
 * init/initerofs.h - EROFS-backed early root filesystem (initerofs)
 *
 * This header provides declarations for the initerofs subsystem which
 * allows using an EROFS image directly from memory as the early root
 * filesystem, without unpacking like traditional initramfs.
 */

#ifndef _INIT_INITEROFS_H
#define _INIT_INITEROFS_H

#include <linux/types.h>

#ifdef CONFIG_INITEROFS

/* Physical address and size of the initerofs image (set by boot param) */
extern phys_addr_t phys_initerofs_start;
extern unsigned long phys_initerofs_size;

/**
 * reserve_initerofs_mem - Reserve memory for the initerofs image
 *
 * This function must be called early during boot to reserve the memory
 * region containing the EROFS image. It prevents the memory from being
 * used for other purposes before the filesystem is mounted.
 *
 * The physical address and size are obtained from the "initerofs=" boot
 * parameter which should be in the format: initerofs=<phys_addr>,<size>
 */
void __init reserve_initerofs_mem(void);

/**
 * initerofs_enabled - Check if initerofs is configured and ready
 *
 * Returns true if the initerofs boot parameter was provided and the
 * memory region was successfully reserved.
 *
 * Return: true if initerofs is enabled, false otherwise
 */
bool __init initerofs_enabled(void);

/**
 * initerofs_mount_root - Mount the EROFS image as root filesystem
 *
 * This function maps the reserved physical memory region, verifies the
 * EROFS superblock magic, and mounts the EROFS filesystem as the root.
 *
 * This should be called after the memory subsystem is initialized but
 * before attempting to access the root filesystem.
 *
 * Return: 0 on success, negative error code on failure
 */
int __init initerofs_mount_root(void);

/**
 * free_initerofs_mem - Free initerofs memory region
 *
 * This function unmaps and optionally frees the memory region used by
 * initerofs. It should be called after switching to the real root
 * filesystem if the initerofs memory is no longer needed.
 *
 * Note: If the system continues to use initerofs as the root filesystem,
 * this function should not be called. Use "retain_initerofs" boot param
 * to prevent automatic memory release.
 */
void __init free_initerofs_mem(void);

#else /* !CONFIG_INITEROFS */

static inline void __init reserve_initerofs_mem(void) {}
static inline bool __init initerofs_enabled(void) { return false; }
static inline int __init initerofs_mount_root(void) { return -ENODEV; }
static inline void __init free_initerofs_mem(void) {}

#endif /* CONFIG_INITEROFS */

#endif /* _INIT_INITEROFS_H */
