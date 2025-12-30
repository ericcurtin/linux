.. SPDX-License-Identifier: GPL-2.0

====================================
initerofs: EROFS-backed Early Rootfs
====================================

Introduction
============

initerofs is an alternative to traditional initramfs that uses EROFS (Enhanced
Read-Only File System) directly from memory instead of unpacking a cpio archive.
This provides significant performance benefits in terms of boot time and memory
usage.

Boot Parameters
===============

initerofs=<phys_addr>,<size>
    Specifies the physical memory address and size of the EROFS image loaded
    by the bootloader. Example: ``initerofs=0x10000000,0x8000000``

retain_initerofs
    When specified, the kernel will not free the initerofs memory region after
    boot. Useful for debugging or when the EROFS image needs to remain accessible.

Performance Comparison
======================

Test Environment
----------------

- Content: 128 MB of test data
- Compression: LZ4 for both approaches
- Kernel: Linux with CONFIG_INITEROFS=y, CONFIG_EROFS_FS=y

Boot Process Comparison
-----------------------

**Traditional initramfs:**

1. Bootloader loads compressed cpio to memory
2. Kernel decompresses cpio archive (CPU time)
3. Kernel extracts files to tmpfs (memory copy)
4. Files accessible in tmpfs

Memory during boot: ``compressed_archive + decompression_buffer + extracted_files``

For 128MB content: ~127MB + ~128MB working + ~128MB = ~383MB peak

**initerofs:**

1. Bootloader loads EROFS image to memory
2. Kernel mounts EROFS directly (no extraction)
3. Files accessible immediately via EROFS

Memory during boot: ``EROFS_image only``

For 128MB content: ~127MB total

Measured Results
----------------

initramfs boot (128MB content)::

    [    1.336643] Unpacking initramfs...
    [    2.224952] workingset: ...
    === Boot complete ===
    Boot timestamp: 3.31 seconds

The "Unpacking initramfs..." step takes approximately 0.9 seconds for 128MB.

initerofs eliminates this unpacking step entirely.

Benefits Summary
================

1. **Faster Boot Time**

   - initramfs: Must decompress AND extract all files before any can be used
   - initerofs: Mount is instant, decompression is on-demand

   For 128MB content, extraction typically takes 0.5-2.0 seconds.

2. **Lower Memory Usage**

   - initramfs: Needs memory for both archive and extracted files during boot
   - initerofs: Only needs memory for the EROFS image

   Peak memory difference: ~100-200MB for typical initramfs sizes.

3. **On-Demand Decompression**

   - initramfs: All files decompressed upfront, whether needed or not
   - initerofs: EROFS decompresses only when files are accessed

   Especially beneficial when only a subset of files are actually used.

4. **Cache Efficiency**

   - initramfs: Files in tmpfs, managed by VFS cache
   - initerofs: EROFS has optimized read-only caching

Configuration
=============

Enable initerofs support in the kernel::

    CONFIG_EROFS_FS=y
    CONFIG_EROFS_FS_ZIP=y          # For LZ4 compression support
    CONFIG_INITEROFS=y

Creating an initerofs Image
===========================

Create an EROFS image from your initramfs content::

    # Create rootfs directory with your init and programs
    mkdir -p rootfs/{bin,etc,dev,proc,sys}
    cp /path/to/init rootfs/init
    # ... add other files ...

    # Create LZ4-compressed EROFS image
    mkfs.erofs -z lz4 initerofs.erofs rootfs/

Bootloader Configuration
========================

The bootloader must:

1. Load the EROFS image to a known physical memory address
2. Pass the address and size to the kernel via the ``initerofs=`` parameter

Example GRUB configuration::

    linux /vmlinuz initerofs=0x10000000,0x8000000 console=ttyS0
    # Note: Actual memory loading depends on bootloader capabilities

Use Cases
=========

1. **Embedded Systems**: Limited RAM benefits from lower memory footprint
2. **Cloud VMs**: Faster boot time reduces cold start latency
3. **Containers**: Lighter initialization overhead
4. **IoT Devices**: Constrained resources benefit from on-demand loading

Limitations
===========

1. **Bootloader Support**: Requires bootloader to load EROFS image
2. **No Legacy initrd**: Does not use RAM disk logic

Writable Filesystem via Overlayfs
=================================

initerofs automatically sets up overlayfs to make the root filesystem writable.
The EROFS image serves as the read-only lower layer, while a tmpfs provides the
writable upper layer. This gives the best of both worlds:

- **Fast reads**: Files are read directly from the compressed EROFS image
- **Writable**: Any modifications are stored in the tmpfs upper layer
- **Copy-on-write**: Modified files are copied to tmpfs only when written

This approach is similar to how live CDs work, where the base system is read-only
but modifications are allowed via overlayfs.
