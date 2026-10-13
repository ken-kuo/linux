<!-- SPDX-License-Identifier: GPL-2.0 -->

# dm-pcache: NVMe Cache-Device Backend — Design

**Status:** Draft / RFC
**Component:** `drivers/md/dm-pcache/`
**Scope:** Add an NVMe (block-device) backend alongside the existing persistent-memory
(DAX) backend for the dm-pcache *cache device*. The backing device is unchanged.

---

## 1. Problem statement

dm-pcache today is architecturally a **DAX / byte-addressable** target, not merely a
"fast device" target. At start, `cache_dev_dax_init()` (`cache_dev.c`) maps the *entire*
cache device into one kernel virtual address via `dax_direct_access()` (with a `vmap`
fallback), and from then on every metadata and data access is a raw pointer dereference
into that mapping:

- addressing: `CACHE_DEV_SEGMENT()`, `cache_pos_addr()`, `get_key_head_addr()`, …
- writes: `memcpy_flushcache()` + `pmem_wmb()`; bio data via `_copy_from_iter_flushcache()`
- reads: `copy_mc_to_kernel()` / `_copy_mc_to_iter()` (machine-check-safe loads)
- zeroing: `memset()` + `dax_flush()`

NVMe is a **block device**: not byte-addressable, accessed via async bio/DMA, durable
only after bio completion + a cache flush, and atomic only at logical-block (512/4096B)
granularity. Supporting NVMe therefore means replacing the storage-access *substrate*,
while preserving the log-structured layout, key trees, GC, writeback, and crash-recovery
logic (all of which are medium-agnostic).

`Kconfig` currently has `depends on DEV_DAX`.

## 2. Goals / non-goals

**Goals**
- Use a plain NVMe block device as the dm-pcache cache device.
- Keep the on-media log-structured format and crash-recovery scheme intact.
- Avoid latency spikes; keep the write path streaming and the flush path cheap.
- Avoid read-modify-write (RMW) on the cache device.

**Non-goals**
- Changing the backing-device path (already bio-based, `backing_dev.c`).
- Supporting a logical block size **smaller** than the cache device's LBA (explicitly
  given up — see §9).
- New cache modes / eviction policies / discard (orthogonal, out of scope).

## 3. Key decisions (summary)

1. **DRAM-staged with explicit persistence domains.** Metadata is DRAM-resident and
   persisted to NVMe as whole logical blocks; cached data streams to NVMe per request.
   (Rejected: bio-per-access; full DRAM mirror.)
2. **Flush only on demand.** Persist (device flush) only when the upper layer issues a
   `REQ_PREFLUSH`/`REQ_FUA`, and at the two background durability boundaries
   (`dirty_tail` advance in writeback, `key_tail` advance in GC). No per-write flush.
3. **Data streams per request; ksets stay lazy.** Each write submits its data bio
   immediately (continuous load on the device); ksets accumulate in DRAM (their existing
   `flush_work` behavior) and are forced out only at a flush boundary. This keeps flush
   cost `O(in-flight + open-ksets)` instead of `O(dirty data)`.
4. **Complete-after-device-write (no early-ack).** The upper write is acked only after its
   cache-device write bio(s) complete. Zero-copy (the cache write references the upper
   bio's pages). Early-ack was rejected: it only hides per-IO latency for a single IO, does
   not raise aggregate throughput (device-bandwidth/queue-depth bound), and would force a
   data copy on the hot path.
5. **Block-granular by advertisement → no RMW.** Advertise `logical_block_size ≥ cache
   device LBA` and keep allocation a block multiple, exactly as bcache does. This removes
   all partial-block cache writes, the shared-block race, and the read bounce buffer.

## 4. Background: how dm-pcache works today

- **Layout.** Cache device split into 16MB segments. Per device: superblock (`SB_OFF=4K`,
  4K), `cache_info` (4K ×2 dual copy), `cache_ctrl` (4K), then segments. Each segment
  begins with dual-copy `seg_info` (4K ×2) + `seg_ctrl` (4K), so the **data region starts
  at a 4K-aligned `data_off = 12K`** within the segment.
- **Keys / ksets.** A write allocates space from a per-CPU `data_head` (`cache_data_alloc`,
  byte-granular advance), copies data in, inserts an in-memory `cache_key` into a sharded
  `req_key_tree` (the runtime source of truth), and appends an on-media `cache_key` to a
  *kset*. 128 keys form a kset; ksets are written sequentially and are crash-safe (CRC +
  magic + an empty-kset terminator marking log end).
- **Request flow.** `dm_pcache_map_bio()` → `pcache_cache_handle_req()`:
  `REQ_PREFLUSH` → `pcache_cache_flush()` (closes all ksets); READ → `cache_read()`
  (tree walk; sync copy for hits, async backing read for misses via `backing_dev`);
  WRITE → `cache_write()` (alloc + sync copy + insert + kset append, inline).
- **Async infra that already exists.** `backing_dev.c` implements async bio I/O to a block
  device: req mempool, submit/complete worklists, `bio_map()` (handles vmalloc'd buffers),
  `backing_dev_flush()` → `blkdev_issue_flush`. The `pcache_req` kref
  (`pcache_req_get/put` → `end_req` → `bio_endio`) already supports "fan out N sub-I/Os,
  complete on the last." Misses are already async.
- **Metadata crash-safety.** Every metadata structure has `PCACHE_META_INDEX_MAX==2`
  copies, each with CRC + 8-bit sequence; `pcache_meta_find_latest()` picks the newest
  valid copy. This is effectively an atomic update primitive and maps directly to NVMe.

## 5. Design overview

Introduce a **cache-medium access backend** with two implementations behind one internal
interface:

```
cmedium_read(cache_dev, dst, off, len)        pmem: copy_mc_to_kernel   | nvme: read bio
cmedium_write(cache_dev, off, src, len/iter)  pmem: memcpy_flushcache   | nvme: write bio
cmedium_zero(cache_dev, off, len)             pmem: memset+dax_flush    | nvme: write-zeroes
cmedium_flush(cache_dev)                      pmem: pmem_wmb (no-op)    | nvme: blkdev_issue_flush
```

The pmem backend is literally today's code; the nvme backend is a `cache_dev_req` path
mirroring `backing_dev.c`. Callers stop doing raw `cache_dev->mapping + off` arithmetic.

**Persistence domains:**
- *Metadata* (superblock, cache_info, cache_ctrl, seg_info, seg_ctrl/gen, ksets, tail
  positions): DRAM-resident; persisted as whole LBA-aligned blocks from the DRAM image.
- *Data*: streamed to NVMe per write request; read back per cache-hit read.

## 6. Detailed design

### 6.1 New plumbing

```
struct cache_dev_req        mirror of pcache_backing_dev_req: req mempool,
                            submit/complete worklists, bio_end cb, bio_map()
cache_dev->inflight_writes  atomic_t — in-flight DATA write bios (flush drain)
PCACHE_CACHE_KEY_FLAGS_WRITING   key is in the tree but its data bio is not yet complete
seg_to_lba(pos) = (SEGMENTS_OFF + seg_id*PCACHE_SEG_SIZE + seg_off) >> SECTOR_SHIFT
pcache_sb.block_size        on-media record of the format-time block size (see §9)
sb flag: cache-medium mode (pmem vs nvme/volatile-cache) for replay assumptions
```

### 6.2 Write flow (`cache_write`)

Per sub-key (split at subtree / segment boundaries, as today):

```
1. cache_key_alloc()
2. cache_data_alloc()                 reserve cache_pos in per-CPU data_head  [unchanged]
3. build cache-device WRITE bio:
       src  = upper bio pages @ io_done       (zero-copy; upper bio pinned by kref)
       dest = seg_to_lba(key->cache_pos)
       pcache_req_get(); atomic_inc(inflight_writes); submit (async)
4. lock subtree:
       key->flags |= CLEAN | WRITING
       cache_key_insert(req_key_tree, key, fixup=true)   ARRIVAL ORDER preserved
       cache_key_append(kset, FUA?)                      kset stays in DRAM (lazy)
   unlock subtree
5. io_done += key->len
```

Data-write bio completion:

```
   clear key->WRITING
   atomic_dec(inflight_writes)
   pcache_defer_reqs_kick()     wake reads that deferred on this key
   pcache_req_put()             last put across all sub-bios → bio_endio(upper write)
```

The key is inserted inline in arrival order (correctness of overwrites); the data bio may
complete later, hence the `WRITING` flag. `data_crc` (if enabled) is computed over the bio
source at submission.

### 6.3 Read flow (`cache_read`)

The walk handlers (`read_overlap_*`) change their **hit** branch from a synchronous
`cache_copy_to_req_bio` into an async cache-device read bio that joins the kref fan-out:

```
per overlapping key_tmp:
  EMPTY   (miss placeholder)  → backing read bio                       [unchanged]
  WRITING (data bio in flight)→ DEFER (see §6.4)
  hit:
     if key->seg_gen < seg->gen → stale → delete + research            [existing check]
     cache_seg_get(seg)         pin seg so GC can't invalidate mid-read
     build cache-device READ bio: src=seg_to_lba(pos), dst=upper bio pages
     pcache_req_get(); submit (async)
```

Read-bio completion: `cache_seg_put(seg)`; `pcache_req_put()` → last put → `bio_endio`.
Because placement is block-aligned (§9), reads DMA directly into the upper bio — no bounce.

### 6.4 Read-after-write coherence

The only new coherence point: a read may hit a `WRITING` key (in the tree for ordering,
data not yet durable). It defers via the existing mechanism:

```
read walk hits WRITING key → return -EBUSY → defer_req() → defered_req_list
   ...write-bio completion calls pcache_defer_reqs_kick()
   ...defered_req_fn re-runs the read; WRITING now clear → normal hit path
```

This reuses the `-EBUSY` / `defered_req_work` path that cache-full already uses. (Possible
later optimization: targeted wakeup, or serve the overlap from the still-pinned upper write
bio pages. Not needed for v1.)

### 6.5 Flush flow (`pcache_cache_flush`, on `REQ_PREFLUSH`)

The block-layer FLUSH contract orders only writes **acked before** the FLUSH was issued.
New writes arriving while a FLUSH is in progress belong to a later epoch and the upper
layer will FLUSH them separately when it cares. So FLUSH does not stop accepting new
I/O — it captures a snapshot of "work to be made durable" and waits only on that.

```
1. e = atomic_inc_return(&flush_epoch) - 1   snapshot epoch; new data bios are tagged
                                              with the new epoch and are NOT in this FLUSH

2. for each kset:                            FREEZE open ksets — snapshot, do not submit
       lock kset->kset_lock                       (the atomicity point; fine-grained per kset)
       memcpy kset_onmedia -> per-epoch persist buffer
       kset->kset_onmedia.key_num = 0             new keys append to a fresh image
       unlock kset->kset_lock

3. wait_event(inflight_data[e] == 0)         drain ONLY epoch-e data writes
                                              (new writes are in epoch e+1, run concurrently)

4. cmedium_flush(cache_dev)                  blkdev_issue_flush — every data write that
                                              was acked before this FLUSH is now durable

5. submit kset write bios from step 2        with REQ_FUA (or follow with cmedium_flush)
                                              ksets land strictly AFTER referenced data

6. wait kset bios + bio_endio(upper FLUSH)
```

#### Answering the three concerns

Because data streams continuously (§6.2), step 3 drains only the last handful of bios —
**no synchronous burst**. The same ordered sequence (snapshot → drain → flush → write
the durability-frontier metadata with FUA → flush) precedes `dirty_tail` advance
(writeback) and `key_tail` advance (GC).

#### FUA writes

A `REQ_FUA` write must be crash-recoverable the moment it is acked — both its cached data
**and** the kset entry mapping its logical range to the cache location must be on media
before completion. On **DAX** this is automatic: the data copy and the force-closed kset
are made durable synchronously (`memcpy_flushcache` + `pmem_wmb`).

On **BIO** the data alone would be durable — `cache_data_write_submit` clones the upper
bio, so the cache-device write inherits `REQ_FUA` and the device flushes it before
completing — but the kset mapping is only *staged*. So a FUA write on BIO is deferred to
the workqueue (like `REQ_PREFLUSH`; it must wait, so it can't run in the submitter
context), and after `cache_write`, `pcache_cache_handle_req` runs `pcache_cache_flush()`
to drain + device-flush the data and the staged kset before the upper bio is acked.
Without this, a crash right after the ack would lose the mapping and replay would serve
stale data from the backing device.

### 6.6 Promote-on-miss (`miss_read_end_req`)

On **DAX** the backing-read completion synchronously copies the data into the cache (an inline
memcpy into the mapped medium, under the subtree lock) and appends the key.

On **BIO** the medium write can't be done inline under `cache_subtree->tree_lock` (it would sleep
on a bio), so the promote is **asynchronous**:

  1. Before taking the lock, `miss_read_end_req` pre-allocates a bounce buffer sized to the miss
     length (`GFP_NOIO`, reliable at any size). A `NULL` here just skips the (best-effort) promote.
  2. Under the lock: `cache_data_alloc()`; snapshot the just-read bytes from the upper bio into
     the buffer (`pcache_bio_range_copy`); compute `data_crc` over the buffer; clear `EMPTY`, set
     `CLEAN | WRITING`; submit an async `CACHE_DEV_REQ_TYPE_KMEM` write of the buffer to the medium
     (`count_data = true`, so a FLUSH drains it before the referencing kset); `cache_key_append()`.
  3. The completion (`cache_promote_write_done`) frees the buffer, clears `WRITING`, and drops the
     key reference.

The write reads from the *independent buffer*, not the upper bio, so the upper read completes
immediately — no `pcache_req` reference is held across the promote. A fresh KMEM bio is used
rather than re-issuing the upper read bio as a write (no bio-direction aliasing). Promote is
best-effort; if the buffer or request can't be obtained, the placeholder is dropped and the range
simply re-misses later.

### 6.7 Metadata persistence (DRAM-staged, RMW-free)

Metadata lives in DRAM and is persisted by writing the **whole enclosing LBA-aligned
block** from the DRAM image — never a device read-modify-write, because DRAM holds the full
block. The existing dual-copy slots are already 4K-sized (`SEG_INFO_SIZE`,
`CACHE_INFO_SIZE`, `CTRL`), so each slot write is already a clean LBA multiple. Small
metadata updates cost up to one block of write-amp, but they are batched to the flush
boundary, so the cost is negligible relative to data traffic.

### 6.8 Crash recovery

Replay walks ksets from `key_tail`, validating CRC + sequence and stopping at the
empty-kset terminator (log end) — unchanged. Two NVMe-specific notes:
- The un-flushed DRAM tail is legitimately lost on crash (allowed: it was never behind a
  completed FLUSH). The terminator + CRC scheme already treats a torn/missing trailing kset
  as end-of-log.
- To prevent a device from writing a kset to media *before* its referenced data, the flush
  barrier is ordered (data drain → flush → kset FUA). Keeping `data_crc` enabled on NVMe is
  cheap insurance (no pmem ECC) and lets replay reject any key whose data did not land. A
  superblock flag records "nvme/volatile-cache mode" so replay does not assume eADR-style
  persistence of the last writes.

## 7. Alignment / no-RMW (the bcache approach)

### 7.1 Why unalignment would otherwise occur

`cache_data_alloc` advances `head_pos` by the exact byte `key->len`, byte-granular (a
pmem-ism). On a block device that manufactures partial-block writes and, because allocation
is contiguous, makes adjacent extents *share* a device block — a write-write hazard under
async I/O. RMW + per-block locking would be required to fix it.

### 7.2 Decision: avoid RMW by construction (as bcache does)

bcache never sub-block-packs: it fixes a `block_size` at format, **rejects** a cache device
whose LBA exceeds it (`super.c`: `block_size << 9 < bdev_logical_block_size(bdev)`),
advertises that `block_size` as the device's logical/physical/io_min, requires the backing
block size ≥ the cache set's, and stores data in block units within buckets. Result: all
cache I/O is block-aligned; no RMW.

dm-pcache adopts the same approach. It is nearly free in the existing format:
- segment data region already starts 4K-aligned (`data_off = 12K`);
- subtree split is 4MB, segment `data_size` is 4K-based — all LBA multiples;
- DM core stacks `logical_block_size = max(cache LBA, backing LBA)` (plus the
  other queue limits) via `blk_stack_limits` over both `dm_get_device`'d
  devices — **but only because the target implements `.iterate_devices`**.
  `dm_calculate_queue_limits` invokes `dm_set_device_limits` (which does the
  `blk_stack_limits` call) *solely* for targets that expose their devices
  through that callout; without it the pcache device silently keeps the
  512-byte stacking default and can emit sub-LBA bios a 4Kn medium rejects.
  `dm_pcache_iterate_devices` iterates the cache and backing devices so the
  stacking actually happens (mirrors `cache_iterate_devices` in dm-cache).

So once upper I/O is an LBA multiple, the byte-granular `head_pos` advance lands on LBA
boundaries on its own (it only ever adds LBA-multiple lengths to an already-aligned base),
and `cache_key_cutfront` trims by LBA multiples, keeping surviving fragments aligned.

### 7.3 Accepted trade-off

dm-pcache **cannot present a logical block size smaller than the cache device LBA** (e.g.
512-logical filesystem in front of a 4Kn cache without reformatting). This flexibility is
deliberately given up; the per-`data_head` open-block buffer + RMW FSM that would restore
it are explicitly **not** implemented.

### 7.4 Guardrails to add

1. Add `block_size` to `pcache_sb`; set at format; on attach reject if
   `block_size << 9 < bdev_logical_block_size(cache bdev)` (bcache parity).
2. ctr-time guard: reject (or round `data_off` up) if cache LBA exceeds the 4K layout unit.
   Not reachable with real NVMe (≤4Kn; bcache caps block at PAGE_SECTORS), but assert it.
3. Debug `WARN` invariant in `cache_data_alloc` that `seg_off`/`len` stay LBA-aligned.

## 8. Persistence & ordering semantics (correctness core)

- **Durability contract.** A bio is durable on completion + a subsequent flush; `REQ_FUA`
  makes that write durable on completion; everything acked before a `REQ_PREFLUSH` is
  durable when the flush completes. Between flushes, a crash loses the un-flushed tail —
  exactly what any write-back block device already tolerates.
- **The one invariant:** *a kset must not become durable before the data it references.*
  Enforced at the flush boundary (data drain → flush → kset FUA), not per write. Data being
  written eagerly makes the first phase nearly free.
- **Atomicity.** Sector/LBA-atomic on NVMe vs 8-byte on pmem; the 4K-aligned dual-copy slots
  + CRC + sequence remain the atomic-update primitive and are LBA-safe.

## 9. Module-by-module change map

| File | Today (pmem) | NVMe change |
|---|---|---|
| `cache_dev.c` | DAX whole-device vmap; sb via mapping | open bdev; query LBA; sb via cmedium; drop vmap path; store/validate `block_size` |
| `segment.c` | `_copy_*_flushcache` / `_copy_mc_to_iter` on `->data` | route through `cmedium_*` (pmem-sync or nvme-bio) |
| `cache_segment.c` | in-place seg_info/gen writes | whole-block writes from DRAM image |
| `cache.c` | cache_info / pos in-place writes | same; ordered at flush boundary |
| `cache_key.c` | kset + terminator byte-offset writes | kset bios; lazy; FUA at boundary |
| `cache_writeback.c`, `cache_gc.c` | `copy_mc_to_kernel` kset reads | cmedium reads into DRAM + error check; flush before tail advance |
| `cache_req.c` | sync data copy inline | per-request async data write; async hit reads; `WRITING`+defer; promote async |
| `dm_pcache.c` | — | `cache_dev_req` machinery; (optional) `io_hints`; mode arg or auto-detect |
| `Kconfig` / `.rst` | `depends on DEV_DAX` | make DAX optional / separate symbol; update docs |

## 10. Performance notes

- The metadata-flush amplification that would make a log-structured cache "flush-bound" is
  avoided by the on-demand flush model: gen bumps and kset fills coalesce in DRAM and ride
  one flush. (bcache pays the same journal/btree amplification.)
- dm-pcache's per-CPU `data_head` and sharded multi-subtree avoid bcache's open-bucket
  allocator contention and btree-root lock contention, so it is plausibly *better* on
  many-core write workloads — independent of medium.
- Genuine regressions are vs the **pmem** baseline only, and are intrinsic to block media
  (also paid by bcache/dm-cache): read hits cost a device read; writes cost a device write;
  GC adds read+write amplification on the cache device, competing with foreground +
  writeback bandwidth → budget/throttle GC.
- Conclusion: with an async, block-aligned data path, dm-pcache-on-NVMe is competitive
  with, and better-scaling than, bcache; it is not inherently flush-bound.

## 11. Configuration / format / compat

- `Kconfig`: drop the hard `DEV_DAX` dependency for the NVMe backend.
- Constructor: NVMe mode validates a writable block device of sufficient size; the
  `dm_dev->dax_dev` requirement and `cache_dev_dax_init()` go away for that mode.
- Mode selection: auto-detect (DAX present?) or an explicit table arg
  (`cache_type pmem|nvme`); record the chosen mode + `block_size` in `pcache_sb`.
- Format compatibility: a superblock flag prevents attaching a pmem-formatted image in
  NVMe mode (and vice-versa) where persistence assumptions differ.

## 12. Implementation plan (phases)

1. **Refactor (behavior-preserving):** introduce the `cmedium_*` interface with only the
   pmem backend wired up. No functional change; reviewable in isolation.
2. **`cache_dev_req` backend:** bio machinery mirroring `backing_dev.c` (req pool,
   submit/complete worklists, `bio_map`, `cmedium_flush`).
3. **Async data path:** per-request data writes, complete-after-device-write, `WRITING`
   flag + `-EBUSY` defer, async hit reads. (Async *promote*-on-miss is deferred — see §14.)
4. **Flush/ordering:** drain `inflight_writes` + `blkdev_issue_flush`; FUA kset barrier;
   flush before `dirty_tail` / `key_tail` advance.
5. **Format/limits:** `block_size` in `pcache_sb` + validation; mode flag; Kconfig + docs;
   `io_hints` if needed.

## 13. Open questions

- Mode selection: explicit table arg vs auto-detect from DAX capability.
- Optional DRAM hot-data read cache on top of the NVMe data path (extra memory; could beat
  bcache on hit latency) — deferred.
- GC bandwidth budgeting / throttling policy on NVMe.

## 14. TODO / deferred work

- Whether to submit kset bios eagerly on close (warm the device) or hold until the flush
  boundary (fewer small writes); tunable.
