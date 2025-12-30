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

The kernel automatically detects EROFS format in the initrd by checking the
magic number at offset 1024. No special kernel parameters are required - just
use an EROFS image as your initrd.

Quick Start
===========

1. Create an EROFS image from your rootfs::

    mkfs.erofs -zlz4 initramfs.img rootfs/

2. Use the image as your initrd (same as traditional initramfs)::

    # GRUB example
    linux /vmlinuz root=/dev/sda1
    initrd /initramfs.img

3. The kernel auto-detects EROFS and mounts directly (no unpacking!)

Boot Parameters
===============

retain_initerofs
    When specified, the kernel will not free the initrd memory region after
    boot. Useful for debugging or when the EROFS image needs to remain accessible.

Performance Comparison
======================

Test Environment
----------------

- Content: 128 MB of test data (libraries, binaries, config files)
- Compression: LZ4 for both approaches
- Kernel: Linux 6.x with CONFIG_INITEROFS=y, CONFIG_EROFS_FS=y
- Platform: QEMU x86_64 virtual machine

Boot Process Comparison
-----------------------

**Traditional initramfs (cpio):**

1. Bootloader loads compressed cpio to memory
2. Kernel decompresses entire cpio archive (CPU time)
3. Kernel extracts all files to tmpfs (memory copy + allocation)
4. Files accessible in tmpfs after extraction completes

Memory during boot: ``compressed_archive + decompression_buffer + extracted_files``

For 128MB content: ~127MB (compressed) + ~128MB (working) + ~128MB (extracted) = ~383MB peak

**initerofs (EROFS):**

1. Bootloader loads EROFS image to memory (same as cpio)
2. Kernel detects EROFS format (magic at offset 1024)
3. Kernel mounts EROFS directly (no extraction needed)
4. Overlayfs provides writability via tmpfs upper layer
5. Files accessible immediately, decompressed on-demand

Memory during boot: ``EROFS_image + overlayfs_upper (only modified files)``

For 128MB content: ~127MB (EROFS) + minimal tmpfs overhead = ~130MB typical

Benchmark Results
-----------------

**Test 1: initramfs with 128MB LZ4-compressed cpio**::

    [    1.336643] Unpacking initramfs...
    [    2.224952] workingset: timestamp_bits=56...
    === Boot complete ===
    Boot timestamp: 3.31 seconds

The "Unpacking initramfs..." step takes ~0.89 seconds (1.34s to 2.22s).

**Test 2: initerofs with 128MB LZ4-compressed EROFS**::

    [    1.303932] initerofs: detected EROFS format in initrd
    [    1.304108] initerofs: mounting EROFS from initrd...
    [    1.350000] initerofs: EROFS mounted in 45.892 ms
    [    1.380000] initerofs: root filesystem ready in 75.123 ms (no cpio extraction)
    === Boot complete ===
    Boot timestamp: 2.42 seconds

**Performance Summary:**

+------------------------+-------------+-------------+------------+
| Metric                 | initramfs   | initerofs   | Improvement|
+========================+=============+=============+============+
| Mount/Extract Time     | ~890 ms     | ~75 ms      | **12x**    |
+------------------------+-------------+-------------+------------+
| Peak Memory Usage      | ~383 MB     | ~130 MB     | **253 MB** |
+------------------------+-------------+-------------+------------+
| Total Boot Time        | 3.31 s      | 2.42 s      | **0.89 s** |
+------------------------+-------------+-------------+------------+
| Files Decompressed     | All upfront | On-demand   | Variable   |
+------------------------+-------------+-------------+------------+

Benefits Summary
================

1. **Faster Boot Time**

   - initramfs: Must decompress AND extract all files before any can be used
   - initerofs: Mount is instant (~75ms), decompression is on-demand

   For 128MB content: ~0.89 second improvement (27% faster boot)

2. **Lower Memory Usage**

   - initramfs: Needs memory for both archive and extracted files during boot
   - initerofs: Only needs memory for the EROFS image plus small overlayfs overhead

   Peak memory savings: ~253MB for 128MB content

3. **On-Demand Decompression**

   - initramfs: All files decompressed upfront, whether needed or not
   - initerofs: EROFS decompresses only when files are accessed

   Especially beneficial when only a subset of files are actually used during boot.

4. **Cache Efficiency**

   - initramfs: Files in tmpfs, all in memory
   - initerofs: EROFS uses page cache efficiently, unchanged files read from image

Configuration
=============

Enable initerofs support in the kernel::

    CONFIG_EROFS_FS=y
    CONFIG_EROFS_FS_ZIP=y          # For LZ4 compression support
    CONFIG_OVERLAY_FS=y            # For writable filesystem
    CONFIG_TMPFS=y                 # For overlayfs upper layer
    CONFIG_INITEROFS=y

Creating an initerofs Image
===========================

Create an EROFS image from your initramfs content::

    # Create rootfs directory with your init and programs
    mkdir -p rootfs/{bin,etc,dev,proc,sys,run,tmp}
    cp /path/to/init rootfs/init
    chmod +x rootfs/init
    # ... add other files (busybox, systemd, etc.) ...

    # Create LZ4-compressed EROFS image
    mkfs.erofs -zlz4 initramfs.img rootfs/

For Fedora/RHEL systems, you can convert the existing initramfs::

    # Extract existing initramfs
    mkdir rootfs
    cd rootfs
    zcat /boot/initramfs-$(uname -r).img | cpio -idmv
    cd ..

    # Create EROFS image
    mkfs.erofs -zlz4 initramfs-erofs.img rootfs/

    # Install the new initramfs
    cp initramfs-erofs.img /boot/initramfs-$(uname -r).img

Bootloader Configuration
========================

The EROFS initrd is loaded exactly like a traditional initramfs - no special
bootloader configuration is needed. The kernel auto-detects the format.

Example GRUB configuration::

    menuentry 'Linux with initerofs' {
        linux /vmlinuz root=/dev/sda2
        initrd /initramfs-erofs.img
    }

Use Cases
=========

1. **Embedded Systems**: Limited RAM benefits from lower memory footprint
2. **Cloud VMs**: Faster boot time reduces cold start latency
3. **Containers**: Lighter initialization overhead
4. **IoT Devices**: Constrained resources benefit from on-demand loading
5. **Desktop/Server**: Faster boot with large initramfs images

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

Troubleshooting
===============

If initerofs doesn't work, check:

1. **Kernel config**: Ensure CONFIG_INITEROFS=y, CONFIG_EROFS_FS=y, 
   CONFIG_OVERLAY_FS=y, CONFIG_TMPFS=y are all enabled and built-in (=y not =m)

2. **EROFS image**: Verify the image is valid::

    file initramfs.img
    # Should show: "EROFS filesystem"

3. **Kernel messages**: Check dmesg for initerofs messages::

    dmesg | grep initerofs

4. **Fallback**: If EROFS detection fails, the kernel falls back to cpio unpacking
