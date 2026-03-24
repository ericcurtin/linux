/* SPDX-License-Identifier: MIT */
/*
 * EROFS (Enhanced ROM File System) on-disk superblock definitions.
 * Exposed for use outside fs/erofs/ (e.g. initrd image detection in init/).
 *
 * Copyright (C) 2017-2018 HUAWEI, Inc.
 *             https://www.huawei.com/
 * Copyright (C) 2021, Alibaba Cloud
 */
#ifndef _LINUX_EROFS_FS_H
#define _LINUX_EROFS_FS_H

#include <linux/types.h>
#include <uapi/linux/magic.h>

/* to allow for x86 boot sectors and other oddities. */
#define EROFS_SUPER_OFFSET      1024

/* erofs on-disk super block (currently 144 bytes at maximum) */
struct erofs_super_block {
	__le32 magic;           /* file system magic number */
	__le32 checksum;        /* crc32c to avoid unexpected on-disk overlap */
	__le32 feature_compat;
	__u8 blkszbits;         /* filesystem block size in bit shift */
	__u8 sb_extslots;	/* superblock size = 128 + sb_extslots * 16 */
	union {
		__le16 rootnid_2b;	/* nid of root directory */
		__le16 blocks_hi;	/* (48BIT on) blocks count MSB */
	} __packed rb;
	__le64 inos;            /* total valid ino # (== f_files - f_favail) */
	__le64 epoch;		/* base seconds used for compact inodes */
	__le32 fixed_nsec;	/* fixed nanoseconds for compact inodes */
	__le32 blocks_lo;	/* blocks count LSB */
	__le32 meta_blkaddr;	/* start block address of metadata area */
	__le32 xattr_blkaddr;	/* start block address of shared xattr area */
	__u8 uuid[16];          /* 128-bit uuid for volume */
	__u8 volume_name[16];   /* volume name */
	__le32 feature_incompat;
	union {
		/* bitmap for available compression algorithms */
		__le16 available_compr_algs;
		/* customized sliding window size instead of 64k by default */
		__le16 lz4_max_distance;
	} __packed u1;
	__le16 extra_devices;	/* # of devices besides the primary device */
	__le16 devt_slotoff;	/* startoff = devt_slotoff * devt_slotsize */
	__u8 dirblkbits;	/* directory block size in bit shift */
	__u8 xattr_prefix_count;	/* # of long xattr name prefixes */
	__le32 xattr_prefix_start;	/* start of long xattr prefixes */
	__le64 packed_nid;	/* nid of the special packed inode */
	__u8 xattr_filter_reserved; /* reserved for xattr name filter */
	__u8 ishare_xattr_prefix_id;
	__u8 reserved[2];
	__le32 build_time;	/* seconds added to epoch for mkfs time */
	__le64 rootnid_8b;	/* (48BIT on) nid of root directory */
	__le64 reserved2;
	__le64 metabox_nid;     /* (METABOX on) nid of the metabox inode */
	__le64 reserved3;	/* [align to extslot 1] */
};

#endif /* _LINUX_EROFS_FS_H */
