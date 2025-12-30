// SPDX-License-Identifier: GPL-2.0-only
/*
 * init/initerofs_blkdev.c - Memory-backed block device for initerofs
 *
 * This provides a simple read-only block device that serves data directly
 * from the initrd memory region, avoiding unnecessary memory copies.
 */

#include <linux/init.h>
#include <linux/module.h>
#include <linux/blkdev.h>
#include <linux/bio.h>
#include <linux/highmem.h>
#include <linux/initrd.h>
#include <linux/init_syscalls.h>

#include "initerofs.h"
#include "do_mounts.h"

#define INITEROFS_BLKDEV_NAME	"initerofs"
#define INITEROFS_SECTOR_SIZE	512

static int initerofs_major;
static struct gendisk *initerofs_disk;
static void *initerofs_data;
static unsigned long initerofs_size;

/*
 * Handle a bio by copying data directly from the initrd memory region.
 * This is a simple synchronous implementation - reads are served directly
 * from the memory-mapped initrd.
 */
static void initerofs_submit_bio(struct bio *bio)
{
	struct bio_vec bvec;
	struct bvec_iter iter;
	sector_t sector = bio->bi_iter.bi_sector;
	unsigned long offset;

	/* We only support reads */
	if (bio_op(bio) != REQ_OP_READ) {
		bio_io_error(bio);
		return;
	}

	bio_for_each_segment(bvec, bio, iter) {
		void *dst;
		unsigned int len = bvec.bv_len;

		offset = sector * INITEROFS_SECTOR_SIZE;

		/* Bounds check */
		if (offset + len > initerofs_size) {
			bio_io_error(bio);
			return;
		}

		/* Direct memory copy - no intermediate buffers needed */
		dst = bvec_kmap_local(&bvec);
		memcpy(dst, initerofs_data + offset, len);
		kunmap_local(dst);

		sector += len >> 9;
	}

	bio_endio(bio);
}

static const struct block_device_operations initerofs_fops = {
	.owner = THIS_MODULE,
	.submit_bio = initerofs_submit_bio,
};

/*
 * Create and register the memory-backed block device.
 * Returns the device path on success, NULL on failure.
 */
char * __init initerofs_blkdev_create(void *data, unsigned long size)
{
	struct queue_limits lim = {
		.logical_block_size = INITEROFS_SECTOR_SIZE,
		.physical_block_size = INITEROFS_SECTOR_SIZE,
		.max_hw_sectors = UINT_MAX,
		.max_segments = BLK_MAX_SEGMENTS,
	};
	int err;

	if (!data || !size)
		return NULL;

	initerofs_data = data;
	initerofs_size = size;

	/* Register block device major number */
	initerofs_major = register_blkdev(0, INITEROFS_BLKDEV_NAME);
	if (initerofs_major < 0) {
		pr_err("initerofs: failed to register block device\n");
		return NULL;
	}

	/* Allocate and configure the gendisk */
	initerofs_disk = blk_alloc_disk(&lim, NUMA_NO_NODE);
	if (IS_ERR(initerofs_disk)) {
		pr_err("initerofs: failed to allocate disk\n");
		unregister_blkdev(initerofs_major, INITEROFS_BLKDEV_NAME);
		return NULL;
	}

	initerofs_disk->major = initerofs_major;
	initerofs_disk->first_minor = 0;
	initerofs_disk->minors = 1;
	initerofs_disk->fops = &initerofs_fops;
	snprintf(initerofs_disk->disk_name, sizeof(initerofs_disk->disk_name),
		 "%s", INITEROFS_BLKDEV_NAME);

	/* Set capacity in sectors */
	set_capacity(initerofs_disk, size / INITEROFS_SECTOR_SIZE);

	/* Mark as read-only */
	set_disk_ro(initerofs_disk, true);

	/* Add the disk to the system */
	err = add_disk(initerofs_disk);
	if (err) {
		pr_err("initerofs: failed to add disk: %d\n", err);
		put_disk(initerofs_disk);
		unregister_blkdev(initerofs_major, INITEROFS_BLKDEV_NAME);
		return NULL;
	}

	pr_info("initerofs: registered block device major %d (%lu bytes)\n",
		initerofs_major, size);

	/* Create /dev directory if it doesn't exist */
	err = init_mkdir("/dev", 0755);
	if (err && err != -EEXIST) {
		pr_err("initerofs: failed to create /dev: %d\n", err);
		del_gendisk(initerofs_disk);
		put_disk(initerofs_disk);
		unregister_blkdev(initerofs_major, INITEROFS_BLKDEV_NAME);
		return NULL;
	}

	/* Create the device node */
	err = create_dev("/dev/" INITEROFS_BLKDEV_NAME,
			 MKDEV(initerofs_major, 0));
	if (err) {
		pr_err("initerofs: failed to create device node: %d\n", err);
		del_gendisk(initerofs_disk);
		put_disk(initerofs_disk);
		unregister_blkdev(initerofs_major, INITEROFS_BLKDEV_NAME);
		return NULL;
	}

	pr_info("initerofs: created /dev/%s (major %d)\n",
		INITEROFS_BLKDEV_NAME, initerofs_major);

	return "/dev/" INITEROFS_BLKDEV_NAME;
}

/*
 * Clean up the block device (called if mount fails).
 */
void __init initerofs_blkdev_destroy(void)
{
	init_unlink("/dev/" INITEROFS_BLKDEV_NAME);
	if (initerofs_disk) {
		del_gendisk(initerofs_disk);
		put_disk(initerofs_disk);
		initerofs_disk = NULL;
	}
	if (initerofs_major > 0) {
		unregister_blkdev(initerofs_major, INITEROFS_BLKDEV_NAME);
		initerofs_major = 0;
	}
}
