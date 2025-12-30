// SPDX-License-Identifier: GPL-2.0-only
/*
 * init/initerofs.c - EROFS-backed early root filesystem (initerofs)
 *
 * Copyright (C) 2024
 *
 * This implements "initerofs" - a mechanism to use an EROFS (Enhanced Read-Only
 * File System) image directly from memory as the early root filesystem, without
 * the need to unpack a cpio archive like traditional initramfs.
 *
 * The implementation automatically detects EROFS format by checking the magic
 * number at offset 1024. If the initramfs is in EROFS format, it mounts it
 * directly instead of unpacking as cpio. This uses the existing initramfs
 * memory reservation infrastructure.
 *
 * Performance benefits vs. traditional initramfs:
 * - No double-buffering: Traditional initramfs requires both the compressed
 *   archive and the unpacked files in memory simultaneously during boot.
 * - No decompression/unpacking step: EROFS can be used directly from memory,
 *   eliminating the CPU time spent on decompression.
 * - Reduced memory footprint: Only the EROFS image needs to be in memory,
 *   not an extracted copy of all files.
 * - EROFS native compression: EROFS supports transparent compression (LZ4, etc.)
 *   which is decompressed on-demand during file access, further saving memory.
 *
 * Usage:
 * - Create an EROFS image: mkfs.erofs -zlz4 initramfs.img rootfs/
 * - Use initramfs.img as your initrd (bootloader loads it as usual)
 * - The kernel automatically detects EROFS format and mounts directly
 */

#include <linux/init.h>
#include <linux/fs.h>
#include <linux/fs_context.h>
#include <linux/mount.h>
#include <linux/namei.h>
#include <linux/memblock.h>
#include <linux/mm.h>
#include <linux/io.h>
#include <linux/pagemap.h>
#include <linux/slab.h>
#include <linux/highmem.h>
#include <linux/backing-dev.h>
#include <linux/init_syscalls.h>
#include <linux/kstrtox.h>
#include <linux/security.h>
#include <linux/file.h>
#include <linux/magic.h>
#include <linux/ktime.h>
#include <linux/initrd.h>
#include <uapi/linux/mount.h>

#include "do_mounts.h"
#include "initerofs.h"

/* EROFS superblock offset from fs/erofs/erofs_fs.h */
#define INITEROFS_SB_OFFSET	1024

/* Retain the memory if requested */
static int __initdata do_retain_initerofs;

static int __init retain_initerofs_param(char *str)
{
	if (*str)
		return 0;
	do_retain_initerofs = 1;
	return 1;
}
__setup("retain_initerofs", retain_initerofs_param);

/*
 * Check if the initrd contains an EROFS filesystem.
 * EROFS magic is at offset 1024 in the superblock.
 */
bool __init initerofs_detect(void)
{
	u32 magic;

	if (!initrd_start || !initrd_end)
		return false;

	/* Need at least superblock offset + magic size */
	if (initrd_end - initrd_start < INITEROFS_SB_OFFSET + sizeof(u32))
		return false;

	magic = le32_to_cpup((__le32 *)((void *)initrd_start + INITEROFS_SB_OFFSET));
	if (magic == EROFS_SUPER_MAGIC_V1) {
		pr_info("initerofs: detected EROFS format in initrd\n");
		return true;
	}

	return false;
}

/*
 * Mount the EROFS image from initrd memory as the root filesystem.
 * This is called during kernel initialization to set up the early root.
 *
 * Returns 0 on success, negative error code on failure.
 */
int __init initerofs_mount_root(void)
{
	char *blkdev_path;
	unsigned long size;
	int err;
	ktime_t start_time, end_time;
	s64 elapsed_ns;

	if (!initrd_start || !initrd_end)
		return -ENODEV;

	size = initrd_end - initrd_start;

	start_time = ktime_get();

	pr_info("initerofs: mounting EROFS from initrd at 0x%lx (size %lu bytes)\n",
		initrd_start, size);

	/*
	 * Mount the EROFS filesystem using direct memory-backed block device.
	 *
	 * We create a simple block device that serves reads directly from the
	 * initrd memory region. This avoids any memory copy - EROFS reads the
	 * data directly from where the bootloader placed it.
	 *
	 * Benefits over file-backed approach:
	 * 1. Zero-copy: No need to write initrd to a backing file
	 * 2. Immediate availability: Block device is ready instantly
	 * 3. Lower memory pressure: No page cache duplication
	 */

	/* Create the memory-backed block device */
	blkdev_path = initerofs_blkdev_create((void *)initrd_start, size);
	if (!blkdev_path) {
		pr_err("initerofs: failed to create block device\n");
		return -ENOMEM;
	}

	/* Create mount point */
	err = init_mkdir("/root", 0755);
	if (err && err != -EEXIST) {
		pr_err("initerofs: failed to create /root directory: %d\n", err);
		goto err_blkdev;
	}

	/* Mount EROFS from the memory-backed block device */
	err = init_mount(blkdev_path, "/root", "erofs", MS_RDONLY, NULL);
	if (err) {
		pr_err("initerofs: failed to mount EROFS from %s: %d\n",
		       blkdev_path, err);
		goto err_blkdev;
	}

	end_time = ktime_get();
	elapsed_ns = ktime_to_ns(ktime_sub(end_time, start_time));
	pr_info("initerofs: EROFS mounted in %lld.%06lld ms (zero-copy)\n",
		elapsed_ns / 1000000, (elapsed_ns % 1000000));

	/*
	 * Set up overlayfs to make the filesystem writable.
	 * EROFS (lower/read-only) + tmpfs (upper/writable) = overlayfs (merged)
	 */

	/* Create directories for overlayfs */
	err = init_mkdir("/overlay_upper", 0755);
	if (err && err != -EEXIST) {
		pr_err("initerofs: failed to create overlay upper dir: %d\n", err);
		goto err_unmount_erofs;
	}

	err = init_mkdir("/overlay_work", 0755);
	if (err && err != -EEXIST) {
		pr_err("initerofs: failed to create overlay work dir: %d\n", err);
		goto err_rmdir_upper;
	}

	err = init_mkdir("/overlay_merged", 0755);
	if (err && err != -EEXIST) {
		pr_err("initerofs: failed to create overlay merged dir: %d\n", err);
		goto err_rmdir_work;
	}

	/* Mount tmpfs for the writable upper layer */
	err = init_mount("tmpfs", "/overlay_upper", "tmpfs", 0, "mode=0755");
	if (err) {
		pr_err("initerofs: failed to mount tmpfs for upper layer: %d\n", err);
		goto err_rmdir_merged;
	}

	/* Create work and upper directories inside tmpfs */
	err = init_mkdir("/overlay_upper/work", 0755);
	if (err && err != -EEXIST) {
		pr_err("initerofs: failed to create work subdir: %d\n", err);
		goto err_unmount_tmpfs;
	}

	err = init_mkdir("/overlay_upper/upper", 0755);
	if (err && err != -EEXIST) {
		pr_err("initerofs: failed to create upper subdir: %d\n", err);
		goto err_unmount_tmpfs;
	}

	/* Mount overlayfs combining EROFS (lower) and tmpfs (upper) */
	err = init_mount("overlay", "/overlay_merged", "overlay", 0,
			 "lowerdir=/root,upperdir=/overlay_upper/upper,workdir=/overlay_upper/work");
	if (err) {
		pr_err("initerofs: failed to mount overlayfs: %d\n", err);
		goto err_unmount_tmpfs;
	}

	/* Move overlayfs mount to root */
	init_chdir("/overlay_merged");
	err = init_mount(".", "/", NULL, MS_MOVE, NULL);
	if (err) {
		pr_err("initerofs: failed to move mount: %d\n", err);
		return err;
	}
	init_chroot(".");

	end_time = ktime_get();
	elapsed_ns = ktime_to_ns(ktime_sub(end_time, start_time));
	pr_info("initerofs: root filesystem ready in %lld.%06lld ms (no cpio extraction)\n",
		elapsed_ns / 1000000, (elapsed_ns % 1000000));

	return 0;

err_unmount_tmpfs:
	init_umount("/overlay_upper", 0);
err_rmdir_merged:
	init_rmdir("/overlay_merged");
err_rmdir_work:
	init_rmdir("/overlay_work");
err_rmdir_upper:
	init_rmdir("/overlay_upper");
err_unmount_erofs:
	init_umount("/root", 0);

err_blkdev:
	initerofs_blkdev_destroy();
	return err;
}

/*
 * Check if initerofs should retain memory (via retain_initerofs param)
 */
bool __init initerofs_should_retain(void)
{
	return do_retain_initerofs;
}
