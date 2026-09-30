.. SPDX-License-Identifier: GPL-2.0

=================================
dm-pcache — Persistent Cache
=================================

*Author: Dongsheng Yang <dongsheng.yang@linux.dev>*

This document describes *dm-pcache*, a Device-Mapper target that lets a
fast block device act as a high-performance, crash-persistent cache in
front of a slower backing block device. The fast device may be either:

* a *DAX-capable* block device exposing byte-addressable persistent
  memory (e.g. ``/dev/pmem0``) — *PMEM mode*, the original target; or
* a plain block device such as an NVMe SSD — *NVMe mode*, where all
  cache-medium I/O is issued via the block layer with explicit
  data-before-metadata ordering at FLUSH boundaries.

The mode is auto-detected from the cache device's DAX capability at
construction and recorded in the on-media superblock. The code lives
in ``drivers/md/dm-pcache/``.

Quick feature summary
=====================

* *Write-back* caching (only mode currently supported).
* *16 MiB segments* allocated on the pmem device.
* *Data CRC32* verification (optional, per cache).
* Crash-safe: every metadata structure is duplicated (`PCACHE_META_INDEX_MAX
  == 2`) and protected with CRC+sequence numbers.
* *Multi-tree indexing* (indexing trees sharded by logical address) for high PMem parallelism
* Pure *DAX path* I/O – no extra BIO round-trips
* *Log-structured write-back* that preserves backend crash-consistency


Constructor
===========

::

    pcache <cache_dev> <backing_dev> [<number_of_optional_arguments> <cache_mode writeback> <data_crc true|false>]

=========================  ====================================================
``cache_dev``               Any block device. A DAX-capable device
                            (``/dev/pmem0``…) activates PMEM mode; any
                            other block device activates NVMe mode. All
                            metadata *and* cached blocks are stored here.

``backing_dev``             The slow block device to be cached.

``cache_mode``              Optional, Only ``writeback`` is accepted at the
                            moment.

``data_crc``                Optional, default to ``false``

                            * ``true``  – store CRC32 for every cached entry
			      and verify on reads
                            * ``false`` – skip CRC (faster)
=========================  ====================================================

Example
-------

.. code-block:: shell

   dmsetup create pcache_sdb --table \
     "0 $(blockdev --getsz /dev/sdb) pcache /dev/pmem0 /dev/sdb 4 cache_mode writeback data_crc true"

The first time a pmem device is used, dm-pcache formats it automatically
(super-block, cache_info, etc.).


Status line
===========

``dmsetup status <device>`` (``STATUSTYPE_INFO``) prints:

::

   <sb_flags> <seg_total> <cache_segs> <segs_used> \
   <gc_percent> <cache_flags> \
   <key_head_seg>:<key_head_off> \
   <dirty_tail_seg>:<dirty_tail_off> \
   <key_tail_seg>:<key_tail_off>

Field meanings
--------------

===============================  =============================================
``sb_flags``                     Super-block flags (e.g. endian marker).

``seg_total``                    Number of physical *pmem* segments.

``cache_segs``                   Number of segments used for cache.

``segs_used``                    Segments currently allocated (bitmap weight).

``gc_percent``                   Current GC high-water mark (0-90).

``cache_flags``                  Bit 0 – DATA_CRC enabled
                                 Bit 1 – INIT_DONE (cache initialised)
                                 Bits 2-5 – cache mode (0 == WB).

``key_head``                     Where new key-sets are being written.

``dirty_tail``                   First dirty key-set that still needs
                                 write-back to the backing device.

``key_tail``                     First key-set that may be reclaimed by GC.
===============================  =============================================


Messages
========

*Change GC trigger*

::

   dmsetup message <dev> 0 gc_percent <0-90>


Theory of operation
===================

Sub-devices
-----------

====================  =========================================================
backing_dev             Any block device (SSD/HDD/loop/LVM, etc.).
cache_dev               Any block device. DAX capability is auto-detected:
                        present -> PMEM mode (byte-addressable mapping);
                        absent  -> NVMe mode (bio-based I/O, DRAM-staged
                        metadata, FLUSH-boundary data drain + device flush).
                        The chosen mode is recorded in the superblock
                        (``cache_type``) and validated on re-attach.
====================  =========================================================

Format
------

At format time (first attach) the superblock records:

* ``block_size`` — the logical-block granularity dm-pcache addresses,
  derived from the cache device's logical block size. Constrained to a
  power of two, no smaller than the cache device's LBA, no larger than
  the host page (bcache parity). Re-attach on a device whose LBA exceeds
  this value is rejected.
* ``cache_type`` — ``PMEM`` (0) or ``NVMe`` (1), set from the
  auto-detection above. Re-attach must agree: a PMEM-formatted image on
  a non-DAX device, or an NVMe-formatted image on a device that exposes
  DAX, is rejected.

Segments and key-sets
---------------------

* The pmem space is divided into *16 MiB segments*.
* Each write allocates space from a per-CPU *data_head* inside a segment.
* A *cache-key* records a logical range on the origin and where it lives
  inside pmem (segment + offset + generation).
* 128 keys form a *key-set* (kset); ksets are written sequentially in pmem
  and are themselves crash-safe (CRC).
* The pair *(key_tail, dirty_tail)* delimit clean/dirty and live/dead ksets.

Write-back
----------

Dirty keys are queued into a tree; a background worker copies data
back to the backing_dev and advances *dirty_tail*.  A FLUSH/FUA bio from the
upper layers forces an immediate metadata commit.

Garbage collection
------------------

GC starts when ``segs_used >= seg_total * gc_percent / 100``.  It walks
from *key_tail*, frees segments whose every key has been invalidated, and
advances *key_tail*.

CRC verification
----------------

If ``data_crc is enabled`` dm-pcache computes a CRC32 over every cached data
range when it is inserted and stores it in the on-media key.  Reads
validate the CRC before copying to the caller.


Failure handling
================

* *pmem media errors* (PMEM mode) – all metadata copies are read with
  ``copy_mc_to_kernel``; an uncorrectable error logs and aborts initialisation.
* *cache-device I/O errors* (NVMe mode) – bio errors surface as
  ``-EIO`` at the cmedium layer; metadata corruption is caught by the
  existing dual-copy + CRC + sequence scheme on replay.
* *Cache full* – if no free segment can be found, writes return ``-EBUSY``;
  dm-pcache retries internally (request deferral).
* *In-flight cache writes* (NVMe mode) – reads that hit a key whose
  cache-device data write is still in flight defer via the same ``-EBUSY``
  path; the write completion kicks the defer queue.
* *System crash* – on attach, the driver replays ksets from *key_tail* to
  rebuild the in-core trees; every segment's generation guards against
  use-after-free keys. In NVMe mode the FLUSH boundary drains in-flight
  data writes and issues ``blkdev_issue_flush`` before persisting any
  kset record, so a kset never describes data that did not reach the
  cache device.


Limitations & TODO
==================

* Only *write-back* mode; other modes planned.
* Only FIFO cache invalidate; other (LRU, ARC...) planned.
* Table reload is not supported currently.
* Discard planned.


Example workflow
================

.. code-block:: shell

   # 1.  Create devices
   dmsetup create pcache_sdb --table \
     "0 $(blockdev --getsz /dev/sdb) pcache /dev/pmem0 /dev/sdb 4 cache_mode writeback data_crc true"

   # 2.  Put a filesystem on top
   mkfs.ext4 /dev/mapper/pcache_sdb
   mount /dev/mapper/pcache_sdb /mnt

   # 3.  Tune GC threshold to 80 %
   dmsetup message pcache_sdb 0 gc_percent 80

   # 4.  Observe status
   watch -n1 'dmsetup status pcache_sdb'

   # 5.  Shutdown
   umount /mnt
   dmsetup remove pcache_sdb


``dm-pcache`` is under active development; feedback, bug reports and patches
are very welcome!
