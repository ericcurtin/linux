// SPDX-License-Identifier: GPL-2.0
/*
 * initerofs - Mount EROFS directly from initrd memory
 *
 * When an EROFS filesystem image is provided in the initrd memory region
 * (instead of a traditional cpio archive), this code mounts it directly
 * from memory without copying the data to a ramdisk first.
 *
 * A regular file is created in rootfs whose address_space operations
 * read on-demand from the initrd memory region.  EROFS uses its existing
 * file-backed mount support (CONFIG_EROFS_FS_BACKED_BY_FILE) to mount
 * from this file, avoiding any bulk data copy.
 *
 * Two read paths must be handled:
 *  - EROFS metadata reads use read_mapping_folio() -> a_ops->read_folio
 *  - EROFS data reads use vfs_iocb_iter_read() -> f_op->read_iter
 * Both are backed by the same initrd memory region.
 */
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/fs_struct.h>
#include <linux/pagemap.h>
#include <linux/highmem.h>
#include <linux/initrd.h>
#include <linux/init_syscalls.h>
#include <linux/uio.h>

#include "do_mounts.h"

#define INITEROFS_IMAGE_PATH "/initerofs.img"

/*
 * Initrd memory region kept as the EROFS backing store.  These are NOT
 * __initdata: initerofs_read_folio() and initerofs_read_iter() are called
 * at runtime (after __init sections are freed) whenever EROFS reads a page
 * or performs a direct data read from the mounted filesystem.
 */
static unsigned long initerofs_addr;
static u64 initerofs_len;

/*
 * Set by do_populate_rootfs() when it detects an EROFS image in the initrd
 * region, preventing initerofs_try_mount() from attempting a blind mount when
 * the initrd is a cpio archive.  __initdata is safe: both the setter and the
 * reader run during __init before these sections are freed.
 */
static bool initerofs_detected __initdata;

/*
 * Read a folio from the initrd memory region.  Called by the page cache
 * when a page is not present (first access or after eviction).
 * Used by EROFS metadata reads (superblock, inodes, directory entries).
 */
static int initerofs_read_folio(struct file *file, struct folio *folio)
{
	loff_t pos = folio_pos(folio);
	size_t len, fsize = folio_size(folio);

	if (pos >= 0 && pos < (loff_t)initerofs_len) {
		len = min_t(u64, fsize, initerofs_len - (u64)pos);
		memcpy_to_folio(folio, 0,
				(void *)(initerofs_addr + (unsigned long)pos),
				len);
	} else {
		len = 0;
	}
	if (len < fsize)
		folio_zero_range(folio, len, fsize - len);
	folio_mark_uptodate(folio);
	folio_unlock(folio);
	return 0;
}

/*
 * NOTE: initerofs_aops and initerofs_fops are NOT __initdata.  They are
 * referenced at runtime by the inode's i_mapping->a_ops and i_fop after
 * __init sections have been freed, for every page fault and read() on the
 * mounted EROFS filesystem.  Marking them __initdata would cause a UAF.
 */
static const struct address_space_operations initerofs_aops = {
	.read_folio = initerofs_read_folio,
};

/*
 * read_iter for the backing file.  Called by EROFS data reads via
 * vfs_iocb_iter_read().  Reads directly from initrd memory.
 *
 * We must provide this because the default tmpfs read_iter
 * (shmem_file_read_iter) uses shmem_get_folio() which does NOT go
 * through a_ops->read_folio, returning zero-filled pages instead.
 */
static ssize_t initerofs_read_iter(struct kiocb *iocb, struct iov_iter *to)
{
	loff_t pos = iocb->ki_pos;
	size_t count = iov_iter_count(to);
	size_t len;
	ssize_t ret;

	if (pos < 0 || (u64)pos >= initerofs_len)
		return 0;

	len = min_t(u64, count, initerofs_len - (u64)pos);
	ret = copy_to_iter((void *)(initerofs_addr + (unsigned long)pos), len,
			   to);
	if (ret > 0)
		iocb->ki_pos += ret;
	else if (len > 0)
		return -EFAULT;
	return ret;
}

static const struct file_operations initerofs_fops = {
	.read_iter = initerofs_read_iter,
	.llseek = generic_file_llseek,
};

void __init initerofs_set_detected(void)
{
	initerofs_detected = true;
}

/*
 * Try to mount EROFS directly from the initrd memory region as the initial
 * rootfs, replacing the existing tmpfs rootfs in-place.
 *
 * This mirrors what cpio initramfs does: after this function returns, the
 * process root is the EROFS image and the kernel will run /init from it
 * directly, without any further kernel-side pivot.  The init process
 * (systemd or other) is then responsible for mounting and switching to
 * whatever final root filesystem the user has configured (ext4, btrfs,
 * xfs, etc.), exactly as it would when booting from a cpio initramfs.
 *
 * Creates a regular file in rootfs whose read_folio/read_iter read from
 * initrd memory on demand.  EROFS mounts from this file using its
 * file-backed mode (CONFIG_EROFS_FS_BACKED_BY_FILE).  The EROFS mount is
 * then promoted to become the new rootfs via pivot_root(".", "."), which
 * discards the old tmpfs root, all without any bulk data copy.
 *
 * Returns true on success.
 */
bool __init initerofs_try_mount(void)
{
	struct file *file;
	struct inode *inode;
	int err;

	/*
	 * Only proceed if do_populate_rootfs() confirmed EROFS magic.
	 * Without this guard, a cpio initrd with CONFIG_INITEROFS=y would
	 * trigger a spurious init_mount() failure before falling through to
	 * the ramdisk path.
	 *
	 * Also guard against being called twice: if initerofs_addr is already
	 * set then a previous call succeeded and the EROFS image is already
	 * live as rootfs.
	 */
	if (!initerofs_detected || initerofs_addr)
		return false;

	if (!initrd_start || initrd_end <= initrd_start)
		return false;

	/* Save initrd region info (persistent - must not be __initdata) */
	initerofs_addr = initrd_start;
	initerofs_len = (u64)(initrd_end - initrd_start);

	/*
	 * Create a regular file in rootfs backed by initrd memory.
	 * O_RDWR is used so that EROFS's file-backed mount path, which opens
	 * the file with O_RDONLY | O_LARGEFILE, sees a regular file rather
	 * than a write-only one.  Nothing is ever written to this file; all
	 * reads go through initerofs_read_folio / initerofs_read_iter.
	 */
	file = filp_open(INITEROFS_IMAGE_PATH, O_CREAT | O_RDWR | O_LARGEFILE,
			 0600);
	if (IS_ERR(file)) {
		pr_err("INITEROFS: cannot create %s: %ld\n",
		       INITEROFS_IMAGE_PATH, PTR_ERR(file));
		return false;
	}

	inode = file_inode(file);
	i_size_write(inode, initerofs_len);
	inode->i_mapping->a_ops = &initerofs_aops;
	/*
	 * Replace i_fop before releasing the file and before any other
	 * opener can see the inode.  This is safe here because:
	 *  1. The inode was just created by filp_open(O_CREAT) above; no
	 *     other file descriptor references it yet.
	 *  2. We hold the only struct file* (via filp_open), so there are
	 *     no concurrent f_op->read_iter callers on this inode.
	 *  3. The subsequent init_mount() will open a new struct file for
	 *     this inode, at which point i_fop is already initerofs_fops,
	 *     so the EROFS mount sees our read_iter from the first open.
	 *
	 * We must override i_fop because tmpfs's shmem_file_read_iter()
	 * bypasses a_ops->read_folio (using shmem_get_folio instead),
	 * returning zero-filled pages that cause EROFS to report EUCLEAN.
	 */
	inode->i_fop = &initerofs_fops;
	fput(file);
	init_flush_fput();

	/* Mount EROFS from the memory-backed file at the staging point */
	err = init_mount(INITEROFS_IMAGE_PATH, "/root", "erofs", SB_RDONLY,
			 NULL);
	if (err) {
		pr_err("INITEROFS: mount failed: %d\n", err);
		goto fail;
	}

	/*
	 * Remove the backing file now that EROFS holds its own reference to
	 * the inode via the superblock.  The dentry is unlinked but the inode
	 * (and the initrd memory backing store) stays live for as long as the
	 * EROFS superblock holds its file reference.
	 */
	init_unlink(INITEROFS_IMAGE_PATH);

	/*
	 * Promote the EROFS mount to become the new root, discarding the
	 * temporary tmpfs rootfs.  This is identical to what prepare_namespace()
	 * does after mount_root(): pivot to cwd (the EROFS mount at /root),
	 * then detach and discard the old tmpfs root.
	 *
	 * After this the kernel sees the EROFS image as / and will find /init
	 * on it, running it directly with no further kernel-side pivot.  The
	 * init process (systemd or other) is then responsible for mounting and
	 * switching to whatever final root filesystem the user has configured
	 * (ext4, btrfs, xfs, etc.), exactly as it would from a cpio initramfs.
	 */
	err = init_chdir("/root");
	if (err) {
		pr_err("INITEROFS: chdir to /root failed: %d\n", err);
		init_umount("/root", MNT_DETACH);
		goto fail;
	}

	if (init_pivot_root(".", ".")) {
		pr_err("INITEROFS: pivot_root failed\n");
		init_chdir("/");
		init_umount("/root", MNT_DETACH);
		goto fail;
	}

	if (init_umount(".", MNT_DETACH))
		pr_warn("INITEROFS: failed to unmount old rootfs\n");

	pr_info("INITEROFS: mounted EROFS from initrd memory as rootfs.\n");
	return true;

fail:
	initerofs_addr = 0;
	initerofs_len = 0;
	return false;
}
