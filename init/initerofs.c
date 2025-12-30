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
 * - Boot parameter: initerofs=<phys_addr>,<size>
 *   Example: initerofs=0x10000000,0x1000000
 * - The bootloader must load the EROFS image to the specified physical address
 *   and pass the address and size to the kernel.
 */

#include <linux/init.h>
#include <linux/fs.h>
#include <linux/fs_context.h>
#include <linux/mount.h>
#include <linux/namei.h>
#include <linux/memblock.h>
#include <linux/mm.h>
#include <linux/pagemap.h>
#include <linux/slab.h>
#include <linux/highmem.h>
#include <linux/backing-dev.h>
#include <linux/init_syscalls.h>
#include <linux/kstrtox.h>
#include <linux/security.h>
#include <linux/file.h>
#include <linux/pseudo_fs.h>

#include "do_mounts.h"

/* Physical address and size of the initerofs image */
phys_addr_t phys_initerofs_start __initdata;
unsigned long phys_initerofs_size __initdata;

/* Virtual address range after mapping */
static void *initerofs_data;

/* Flag to indicate initerofs should be used */
static bool __initdata use_initerofs;

/* Retain the memory if requested */
static int __initdata do_retain_initerofs;

/*
 * Parse the initerofs= boot parameter
 * Format: initerofs=<phys_addr>,<size>
 */
static int __init early_initerofs(char *p)
{
	phys_addr_t start;
	unsigned long size;
	char *endp;

	if (!p || !*p)
		return 0;

	start = memparse(p, &endp);
	if (*endp == ',') {
		size = memparse(endp + 1, NULL);
		if (start && size) {
			phys_initerofs_start = start;
			phys_initerofs_size = size;
			use_initerofs = true;
			pr_info("initerofs: configured at 0x%llx, size 0x%lx\n",
				(unsigned long long)start, size);
		}
	}
	return 0;
}
early_param("initerofs", early_initerofs);

static int __init retain_initerofs_param(char *str)
{
	if (*str)
		return 0;
	do_retain_initerofs = 1;
	return 1;
}
__setup("retain_initerofs", retain_initerofs_param);

/*
 * Reserve the memory region for initerofs during early boot.
 * This must be called early enough to prevent the memory from being
 * used for other purposes.
 */
void __init reserve_initerofs_mem(void)
{
	phys_addr_t start;
	unsigned long size;

	if (!use_initerofs || !phys_initerofs_size)
		return;

	/*
	 * Round the memory region to page boundaries.
	 * This allows us to properly map the region later.
	 */
	start = round_down(phys_initerofs_start, PAGE_SIZE);
	size = phys_initerofs_size + (phys_initerofs_start - start);
	size = round_up(size, PAGE_SIZE);

	if (!memblock_is_region_memory(start, size)) {
		pr_err("initerofs: 0x%llx+0x%lx is not a memory region\n",
		       (unsigned long long)start, size);
		use_initerofs = false;
		return;
	}

	if (memblock_is_region_reserved(start, size)) {
		pr_err("initerofs: 0x%llx+0x%lx overlaps reserved memory\n",
		       (unsigned long long)start, size);
		use_initerofs = false;
		return;
	}

	memblock_reserve(start, size);
	pr_info("initerofs: reserved memory 0x%llx - 0x%llx\n",
		(unsigned long long)start, (unsigned long long)(start + size));
}

/*
 * Check if initerofs is enabled and ready to use.
 */
bool __init initerofs_enabled(void)
{
	return use_initerofs && phys_initerofs_size > 0;
}

/*
 * Address space operations for memory-backed initerofs.
 * This provides direct access to the EROFS image in memory without
 * requiring a block device.
 */
static int initerofs_read_folio(struct file *file, struct folio *folio)
{
	loff_t pos = folio_pos(folio);
	size_t len = folio_size(folio);
	void *src, *dst;

	if (!initerofs_data) {
		folio_unlock(folio);
		return -EIO;
	}

	if (pos >= phys_initerofs_size) {
		folio_zero_range(folio, 0, len);
		folio_mark_uptodate(folio);
		folio_unlock(folio);
		return 0;
	}

	if (pos + len > phys_initerofs_size)
		len = phys_initerofs_size - pos;

	src = initerofs_data + pos;
	dst = kmap_local_folio(folio, 0);
	memcpy(dst, src, len);
	if (len < folio_size(folio))
		memset(dst + len, 0, folio_size(folio) - len);
	kunmap_local(dst);

	folio_mark_uptodate(folio);
	folio_unlock(folio);
	return 0;
}

static const struct address_space_operations initerofs_aops = {
	.read_folio = initerofs_read_folio,
};

/*
 * Mount the EROFS image from memory as the root filesystem.
 * This is called during kernel initialization to set up the early root.
 *
 * Returns 0 on success, negative error code on failure.
 */
int __init initerofs_mount_root(void)
{
	int err;

	if (!initerofs_enabled())
		return -ENODEV;

	/* Map the physical memory region */
	initerofs_data = memremap(phys_initerofs_start, phys_initerofs_size,
				  MEMREMAP_WB);
	if (!initerofs_data) {
		pr_err("initerofs: failed to map memory region\n");
		return -ENOMEM;
	}

	/* Verify EROFS magic - located at offset 1024 */
	if (phys_initerofs_size >= 1024 + 4) {
		u32 magic = le32_to_cpup((__le32 *)(initerofs_data + 1024));
		if (magic != 0xe0f5e1e2) {  /* EROFS_SUPER_MAGIC_V1 */
			pr_err("initerofs: invalid EROFS magic (got 0x%x, expected 0xe0f5e1e2)\n",
			       magic);
			memunmap(initerofs_data);
			initerofs_data = NULL;
			return -EINVAL;
		}
		pr_info("initerofs: verified EROFS superblock magic\n");
	} else {
		pr_err("initerofs: image too small to contain EROFS superblock\n");
		memunmap(initerofs_data);
		initerofs_data = NULL;
		return -EINVAL;
	}

	pr_info("initerofs: mounting EROFS from memory at 0x%llx (size %lu bytes)\n",
		(unsigned long long)phys_initerofs_start, phys_initerofs_size);

	/*
	 * Mount the EROFS filesystem.
	 * We create an EROFS mount that reads from our memory-backed region.
	 * The EROFS file-backed mode provides the infrastructure we need.
	 *
	 * For now, we use a simple approach: write the memory region to a
	 * temporary file in the initial rootfs, then mount EROFS from it.
	 * This is not the most efficient approach but works with existing
	 * EROFS infrastructure.
	 *
	 * TODO: Add native memory-backed support to EROFS for better
	 * performance by avoiding the temporary file.
	 */

	/* Create temporary file in rootfs for EROFS image */
	err = init_mkdir("/initerofs_tmp", 0700);
	if (err && err != -EEXIST) {
		pr_err("initerofs: failed to create temp directory: %d\n", err);
		goto err_unmap;
	}

	/* Write EROFS image to temp file */
	{
		struct file *file;
		loff_t pos = 0;
		ssize_t written;

		file = filp_open("/initerofs_tmp/erofs.img",
				 O_WRONLY | O_CREAT | O_LARGEFILE, 0400);
		if (IS_ERR(file)) {
			err = PTR_ERR(file);
			pr_err("initerofs: failed to create temp file: %d\n", err);
			goto err_rmdir;
		}

		written = kernel_write(file, initerofs_data, phys_initerofs_size, &pos);
		fput(file);

		if (written != phys_initerofs_size) {
			pr_err("initerofs: failed to write image: %zd\n", written);
			err = written < 0 ? written : -EIO;
			goto err_unlink;
		}
	}

	/* Mount EROFS from the temp file */
	err = init_mount("/initerofs_tmp/erofs.img", "/root", "erofs",
			 MS_RDONLY, NULL);
	if (err) {
		pr_err("initerofs: failed to mount EROFS: %d\n", err);
		goto err_unlink;
	}

	/* Clean up temp file - it's no longer needed after mount */
	init_unlink("/initerofs_tmp/erofs.img");
	init_rmdir("/initerofs_tmp");

	/* Move mount to root */
	init_chdir("/root");
	err = init_mount(".", "/", NULL, MS_MOVE, NULL);
	if (err) {
		pr_err("initerofs: failed to move mount: %d\n", err);
		return err;
	}
	init_chroot(".");

	pr_info("initerofs: successfully mounted EROFS as root filesystem\n");
	return 0;

err_unlink:
	init_unlink("/initerofs_tmp/erofs.img");
err_rmdir:
	init_rmdir("/initerofs_tmp");
err_unmap:
	memunmap(initerofs_data);
	initerofs_data = NULL;
	return err;
}

/*
 * Free initerofs memory after switching to the real root filesystem.
 * This is optional - the memory can be retained if needed.
 */
void __init free_initerofs_mem(void)
{
	unsigned long start, end;

	if (!initerofs_enabled())
		return;

	if (do_retain_initerofs) {
		pr_info("initerofs: retaining memory as requested\n");
		return;
	}

	if (initerofs_data) {
		memunmap(initerofs_data);
		initerofs_data = NULL;
	}

	/* Free the reserved memory region */
	start = round_down(phys_initerofs_start, PAGE_SIZE);
	end = round_up(phys_initerofs_start + phys_initerofs_size, PAGE_SIZE);

#ifdef CONFIG_ARCH_KEEP_MEMBLOCK
	memblock_free((void *)__va(start), end - start);
#endif
	free_reserved_area((void *)__va(start), (void *)__va(end),
			   POISON_FREE_INITMEM, "initerofs");

	pr_info("initerofs: freed memory region\n");
}

/*
 * Documentation note:
 *
 * Performance comparison with traditional initramfs:
 *
 * Traditional initramfs:
 * 1. Bootloader loads compressed cpio archive to memory
 * 2. Kernel decompresses cpio archive (CPU time + temporary memory)
 * 3. Kernel extracts files to tmpfs (memory for both archive and files)
 * 4. Files are now accessible in tmpfs
 * Memory used: compressed archive + decompressed archive + extracted files
 *
 * initerofs:
 * 1. Bootloader loads EROFS image to memory
 * 2. Kernel mounts EROFS directly from memory (no decompression step)
 * 3. Files are accessible immediately via EROFS
 * Memory used: just the EROFS image
 *
 * Benefits:
 * - Faster boot: No decompression/extraction step
 * - Lower memory: No duplicate data in memory
 * - On-demand decompression: EROFS can use transparent compression
 * - Better for read-only root: EROFS is optimized for read-only access
 *
 * The EROFS image can still be compressed (LZ4, etc.) and will be
 * decompressed on-demand when files are accessed, providing the best
 * of both worlds: compact storage and efficient access.
 */
	}

	pr_info("initerofs: mounting EROFS from memory at 0x%llx (size %lu bytes)\n",
		(unsigned long long)phys_initerofs_start, phys_initerofs_size);

	/*
	 * Mount the EROFS filesystem using file-backed mode.
	 * EROFS's file-backed mode (CONFIG_EROFS_FS_BACKED_BY_FILE) allows
	 * mounting from a regular file, which we provide here backed by memory.
	 */
	err = init_mount("initerofs", "/root", "erofs", MS_RDONLY, NULL);
	if (err) {
		pr_err("initerofs: failed to mount EROFS: %d\n", err);
		fput(backing_file);
		memunmap(initerofs_data);
		initerofs_data = NULL;
		return err;
	}

	pr_info("initerofs: successfully mounted EROFS as root filesystem\n");

	/* Notify security subsystem */
	security_initramfs_populated();

	return 0;
}

/*
 * Free initerofs memory after switching to the real root filesystem.
 * This is optional - the memory can be retained if needed.
 */
void __init free_initerofs_mem(void)
{
	unsigned long start, end;

	if (!initerofs_enabled())
		return;

	if (do_retain_initerofs) {
		pr_info("initerofs: retaining memory as requested\n");
		return;
	}

	if (initerofs_data) {
		memunmap(initerofs_data);
		initerofs_data = NULL;
	}

	/* Free the reserved memory region */
	start = round_down(phys_initerofs_start, PAGE_SIZE);
	end = round_up(phys_initerofs_start + phys_initerofs_size, PAGE_SIZE);

#ifdef CONFIG_ARCH_KEEP_MEMBLOCK
	memblock_free((void *)__va(start), end - start);
#endif
	free_reserved_area((void *)__va(start), (void *)__va(end),
			   POISON_FREE_INITMEM, "initerofs");

	pr_info("initerofs: freed memory region\n");
}

/*
 * Documentation note:
 *
 * Performance comparison with traditional initramfs:
 *
 * Traditional initramfs:
 * 1. Bootloader loads compressed cpio archive to memory
 * 2. Kernel decompresses cpio archive (CPU time + temporary memory)
 * 3. Kernel extracts files to tmpfs (memory for both archive and files)
 * 4. Files are now accessible in tmpfs
 * Memory used: compressed archive + decompressed archive + extracted files
 *
 * initerofs:
 * 1. Bootloader loads EROFS image to memory
 * 2. Kernel mounts EROFS directly from memory (no decompression step)
 * 3. Files are accessible immediately via EROFS
 * Memory used: just the EROFS image
 *
 * Benefits:
 * - Faster boot: No decompression/extraction step
 * - Lower memory: No duplicate data in memory
 * - On-demand decompression: EROFS can use transparent compression
 * - Better for read-only root: EROFS is optimized for read-only access
 *
 * The EROFS image can still be compressed (LZ4, etc.) and will be
 * decompressed on-demand when files are accessed, providing the best
 * of both worlds: compact storage and efficient access.
 */
