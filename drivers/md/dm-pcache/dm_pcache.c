// SPDX-License-Identifier: GPL-2.0-or-later
#include <linux/module.h>
#include <linux/blkdev.h>
#include <linux/bio.h>
#include <linux/timekeeping.h>

#include "../dm-core.h"
#include "cache_dev.h"
#include "backing_dev.h"
#include "cache.h"
#include "dm_pcache.h"
#include "dm_pcache_bitmap.h"
#include "dm_pcache_registry.h"
#include "dm_pcache_net.h"

void pcache_defer_reqs_kick(struct dm_pcache *pcache)
{
	struct pcache_cache *cache = &pcache->cache;

	spin_lock(&cache->seg_map_lock);
	if (!cache->cache_full)
		queue_work(pcache->task_wq, &pcache->defered_req_work);
	spin_unlock(&cache->seg_map_lock);
}

static void defer_req(struct pcache_request *pcache_req)
{
	struct dm_pcache *pcache = pcache_req->pcache;

	BUG_ON(!list_empty(&pcache_req->list_node));

	spin_lock(&pcache->defered_req_list_lock);
	list_add(&pcache_req->list_node, &pcache->defered_req_list);
	pcache_defer_reqs_kick(pcache);
	spin_unlock(&pcache->defered_req_list_lock);
}

static void defered_req_fn(struct work_struct *work)
{
	struct dm_pcache *pcache = container_of(work, struct dm_pcache, defered_req_work);
	struct pcache_request *pcache_req;
	LIST_HEAD(tmp_list);
	int ret;

	if (pcache_is_stopping(pcache))
		return;

	spin_lock(&pcache->defered_req_list_lock);
	list_splice_init(&pcache->defered_req_list, &tmp_list);
	spin_unlock(&pcache->defered_req_list_lock);

	while (!list_empty(&tmp_list)) {
		pcache_req = list_first_entry(&tmp_list,
					    struct pcache_request, list_node);
		list_del_init(&pcache_req->list_node);
		pcache_req->ret = 0;
		ret = pcache_cache_handle_req(&pcache->cache, pcache_req);
		if (ret == -EBUSY)
			defer_req(pcache_req);
		else
			pcache_req_put(pcache_req, ret);
	}
}

void pcache_req_get(struct pcache_request *pcache_req)
{
	kref_get(&pcache_req->ref);
}

static void end_req(struct kref *ref)
{
	struct pcache_request *pcache_req = container_of(ref, struct pcache_request, ref);
	struct dm_pcache *pcache = pcache_req->pcache;
	struct bio *bio = pcache_req->bio;
	int ret = pcache_req->ret;

	if (ret == -EBUSY) {
		pcache_req_get(pcache_req);
		defer_req(pcache_req);
	} else {
		bio->bi_status = errno_to_blk_status(ret);
		bio_endio(bio);

		if (atomic_dec_and_test(&pcache->inflight_reqs))
			wake_up(&pcache->inflight_wq);
	}
}

void pcache_req_put(struct pcache_request *pcache_req, int ret)
{
	/* Set the return status if it is not already set */
	if (ret && !pcache_req->ret)
		pcache_req->ret = ret;

	kref_put(&pcache_req->ref, end_req);
}

static bool at_least_one_arg(struct dm_arg_set *as, char **error)
{
	if (!as->argc) {
		*error = "Insufficient args";
		return false;
	}

	return true;
}

static int parse_cache_dev(struct dm_pcache *pcache, struct dm_arg_set *as,
				char **error)
{
	int ret;

	if (!at_least_one_arg(as, error))
		return -EINVAL;
	ret = dm_get_device(pcache->ti, dm_shift_arg(as),
			  BLK_OPEN_READ | BLK_OPEN_WRITE,
			  &pcache->cache_dev.dm_dev);
	if (ret) {
		*error = "Error opening cache device";
		return ret;
	}

	return 0;
}

static int parse_backing_dev(struct dm_pcache *pcache, struct dm_arg_set *as,
				char **error)
{
	int ret;

	if (!at_least_one_arg(as, error))
		return -EINVAL;

	ret = dm_get_device(pcache->ti, dm_shift_arg(as),
			  BLK_OPEN_READ | BLK_OPEN_WRITE,
			  &pcache->backing_dev.dm_dev);
	if (ret) {
		*error = "Error opening backing device";
		return ret;
	}

	return 0;
}

static void pcache_init_opts(struct pcache_cache_options *opts)
{
	opts->cache_mode = PCACHE_CACHE_MODE_WRITEBACK;
	opts->data_crc = false;
}

static int parse_cache_opts(struct dm_pcache *pcache, struct dm_arg_set *as,
			    char **error)
{
	struct pcache_cache_options *opts = &pcache->opts;
	static const struct dm_arg _args[] = {
		{0, 4, "Invalid number of cache option arguments"},
	};
	unsigned int argc;
	const char *arg;
	int ret;

	pcache_init_opts(opts);
	if (!as->argc)
		return 0;

	ret = dm_read_arg_group(_args, as, &argc, error);
	if (ret)
		return -EINVAL;

	while (argc) {
		arg = dm_shift_arg(as);
		argc--;

		if (!strcmp(arg, "cache_mode")) {
			arg = dm_shift_arg(as);
			if (!strcmp(arg, "writeback")) {
				opts->cache_mode = PCACHE_CACHE_MODE_WRITEBACK;
			} else {
				*error = "Invalid cache mode parameter";
				return -EINVAL;
			}
			argc--;
		} else if (!strcmp(arg, "data_crc")) {
			arg = dm_shift_arg(as);
			if (!strcmp(arg, "true")) {
				opts->data_crc = true;
			} else if (!strcmp(arg, "false")) {
				opts->data_crc = false;
			} else {
				*error = "Invalid data crc parameter";
				return -EINVAL;
			}
			argc--;
		} else {
			*error = "Unrecognised cache option requested";
			return -EINVAL;
		}
	}

	return 0;
}

static int pcache_start(struct dm_pcache *pcache, char **error)
{
	int ret;

	ret = cache_dev_start(pcache);
	if (ret) {
		*error = "Failed to start cache dev";
		return ret;
	}

	ret = backing_dev_start(pcache);
	if (ret) {
		*error = "Failed to start backing dev";
		goto stop_cache;
	}

	ret = pcache_cache_start(pcache);
	if (ret) {
		*error = "Failed to start pcache";
		goto stop_backing;
	}

	return 0;
stop_backing:
	backing_dev_stop(pcache);
stop_cache:
	cache_dev_stop(pcache);

	return ret;
}

static void pcache_destroy_args(struct dm_pcache *pcache)
{
	if (pcache->cache_dev.dm_dev)
		dm_put_device(pcache->ti, pcache->cache_dev.dm_dev);
	if (pcache->backing_dev.dm_dev)
		dm_put_device(pcache->ti, pcache->backing_dev.dm_dev);
}

static int pcache_parse_args(struct dm_pcache *pcache, unsigned int argc, char **argv,
				char **error, unsigned int *net_argc, char ***net_argv)
{
	struct dm_arg_set as;
	int ret;

	*net_argc = 0;
	*net_argv = NULL;

	if (argc < 2) {
		*error = "Insufficient args: at least <cache_dev> <backing_dev> required";
		return -EINVAL;
	}

	as.argc = argc;
	as.argv = argv;

	ret = parse_cache_dev(pcache, &as, error);
	if (ret)
		return ret;
	ret = parse_backing_dev(pcache, &as, error);
	if (ret)
		goto out_dev;

	/*
	 * parse_cache_opts expects the first remaining arg to be the
	 * cache-opts count (a non-negative integer per dm_arg_group
	 * convention). If it isn't an integer, treat the rest as network
	 * args and use default cache options.
	 */
	pcache_init_opts(&pcache->opts);
	if (as.argc > 0) {
		unsigned int probe;

		if (kstrtouint(as.argv[0], 10, &probe) == 0) {
			ret = parse_cache_opts(pcache, &as, error);
			if (ret)
				goto out_backing;
		}
	}

	if (as.argc > 0) {
		*net_argc = as.argc;
		*net_argv = as.argv;
	}

	return 0;
out_backing:
	pcache_destroy_args(pcache);
out_dev:
	if (pcache->cache_dev.dm_dev)
		dm_put_device(pcache->ti, pcache->cache_dev.dm_dev);
	return ret;
}

static int dm_pcache_ctr(struct dm_target *ti, unsigned int argc, char **argv)
{
	struct mapped_device *md = ti->table->md;
	struct dm_pcache *pcache;
	unsigned int net_argc = 0;
	char **net_argv = NULL;
	int ret;

	if (md->map) {
		ti->error = "Don't support table loading for live md";
		return -EOPNOTSUPP;
	}

	pcache = kzalloc_obj(struct dm_pcache);
	if (!pcache)
		return -ENOMEM;

	pcache->task_wq = alloc_workqueue("pcache-%s-wq",  WQ_UNBOUND | WQ_MEM_RECLAIM,
					  0, md->name);
	if (!pcache->task_wq) {
		ret = -ENOMEM;
		goto free_pcache;
	}

	spin_lock_init(&pcache->defered_req_list_lock);
	INIT_LIST_HEAD(&pcache->defered_req_list);
	INIT_WORK(&pcache->defered_req_work, defered_req_fn);
	pcache->ti = ti;
	pcache->net = NULL;
	pcache->is_primary = false;
	pcache->node_id = -1;

	/* Phase 3.5 — device-wide gen state. peer_bitmap_init() bumps from
	 * loaded slot headers; starts at 0 here. */
	spin_lock_init(&pcache->gen_lock);
	pcache->generation = 0;
	pcache->gen_dirty = false;

	ret = pcache_parse_args(pcache, argc, argv, &ti->error, &net_argc, &net_argv);
	if (ret)
		goto destroy_wq;

	ret = pcache_start(pcache, &ti->error);
	if (ret)
		goto destroy_args;

	/* Phase 3: peer registry sits on PMEM and must outlive bitmap teardown
	 * (slot indices reference into the same address space). Init it right
	 * after cache_dev is up, regardless of single-host vs distributed mode. */
	ret = pcache_registry_init(pcache);
	if (ret) {
		ti->error = "Failed to initialize peer registry";
		goto stop_cache;
	}

	/* Phase 3.5 — no device-level bitmap; each peer owns its bm. */
	ret = pcache_net_init(pcache, net_argc, net_argv);
	if (ret) {
		ti->error = "Failed to initialize network layer";
		goto exit_registry;
	}

	ti->num_flush_bios = 1;
	ti->flush_supported = true;
	ti->per_io_data_size = sizeof(struct pcache_request);
	ti->private = pcache;
	atomic_set(&pcache->inflight_reqs, 0);
	atomic_set(&pcache->state, PCACHE_STATE_RUNNING);
	init_waitqueue_head(&pcache->inflight_wq);

	return 0;
exit_registry:
	pcache_registry_exit(pcache);
stop_cache:
	pcache_cache_stop(pcache);
	backing_dev_stop(pcache);
	cache_dev_stop(pcache);
destroy_args:
	pcache_destroy_args(pcache);
destroy_wq:
	destroy_workqueue(pcache->task_wq);
free_pcache:
	kfree(pcache);

	return ret;
}

static void defer_req_stop(struct dm_pcache *pcache)
{
	struct pcache_request *pcache_req;
	LIST_HEAD(tmp_list);

	flush_work(&pcache->defered_req_work);

	spin_lock(&pcache->defered_req_list_lock);
	list_splice_init(&pcache->defered_req_list, &tmp_list);
	spin_unlock(&pcache->defered_req_list_lock);

	while (!list_empty(&tmp_list)) {
		pcache_req = list_first_entry(&tmp_list,
					    struct pcache_request, list_node);
		list_del_init(&pcache_req->list_node);
		pcache_req_put(pcache_req, -EIO);
	}
}

static void dm_pcache_dtr(struct dm_target *ti)
{
	struct dm_pcache *pcache;

	pcache = ti->private;
	atomic_set(&pcache->state, PCACHE_STATE_STOPPING);
	defer_req_stop(pcache);

	/* Drop pending network refs before waiting for inflight to drain. */
	pcache_net_stop(pcache);

	wait_event(pcache->inflight_wq,
			atomic_read(&pcache->inflight_reqs) == 0);

	/* Phase 3.5 — per-peer bitmaps were freed by pcache_net_stop above. */
	pcache_registry_exit(pcache);
	pcache_cache_stop(pcache);
	backing_dev_stop(pcache);
	cache_dev_stop(pcache);

	pcache_destroy_args(pcache);
	drain_workqueue(pcache->task_wq);
	destroy_workqueue(pcache->task_wq);

	kfree(pcache);
}

static int dm_pcache_map_bio(struct dm_target *ti, struct bio *bio)
{
	struct pcache_request *pcache_req = dm_per_bio_data(bio, sizeof(struct pcache_request));
	struct dm_pcache *pcache = ti->private;
	int ret;

	/* Phase 3 Step 8 — Secondary write protection.
	 * In distributed mode, only the Primary may serve WRITEs. Allowing a
	 * Secondary to write locally creates split-brain on the next reconnect.
	 * Standalone mode (no pcache->net) is unaffected. */
	if (pcache->net && !pcache->is_primary &&
	    bio_data_dir(bio) == WRITE) {
		bio_io_error(bio);
		return DM_MAPIO_SUBMITTED;
	}

	/* Phase 3 Step 10 — Quorum-based write fencing.
	 * Primary refuses WRITEs once it loses quorum (default: off=always ok;
	 * majority: needs (active+1)/2 + 1 connected peers including self). */
	if (pcache->net && pcache->is_primary &&
	    bio_data_dir(bio) == WRITE && !pcache_has_quorum(pcache)) {
		bio_io_error(bio);
		return DM_MAPIO_SUBMITTED;
	}

#if PCACHE_LAT_TRACE
	pcache_req->t_map = ktime_get_ns();
#endif
	pcache_req->pcache = pcache;
	kref_init(&pcache_req->ref);
	pcache_req->ret = 0;
	pcache_req->bio = bio;
	pcache_req->off = (u64)bio->bi_iter.bi_sector << SECTOR_SHIFT;
	pcache_req->data_len = bio->bi_iter.bi_size;
	INIT_LIST_HEAD(&pcache_req->list_node);
	atomic_inc(&pcache->inflight_reqs);

	ret = pcache_cache_handle_req(&pcache->cache, pcache_req);
	if (ret == -EBUSY) {
		defer_req(pcache_req);
		return DM_MAPIO_SUBMITTED;
	}

	/*
	 * Local write succeeded -- mirror to the peer if we're Primary.
	 * Hold an extra reference on pcache_req across the network ACK so
	 * the bio survives the unconditional put below; the ACK/NACK
	 * handler performs the final put which triggers bio_endio.
	 */
	if (ret == 0 && pcache->is_primary && bio_data_dir(bio) == WRITE &&
	    bio->bi_iter.bi_size > 0) {
		/* Phase 3.5 C1 — skip replication for empty bios (REQ_PREFLUSH,
		 * barrier-only). Previously these went through the full fan-out
		 * path including 30 s ACK timeout, which manifested as
		 * "fsync failed: Connection timed out" even on healthy clusters.
		 * The flush semantics on peers are covered by per-write
		 * pcache_net_write_to_cache calls, which already persist data
		 * before ACKing. */
		if (pcache_peer_connected(pcache)) {
			int net_ret;

			pcache_req_get(pcache_req);
			net_ret = pcache_net_submit_write(pcache, pcache_req,
					bio->bi_iter.bi_sector,
					bio->bi_iter.bi_size);
			if (net_ret) {
				/* Enqueue failed: mark all peers' bitmaps dirty
				 * so their next resync covers this region. */
				pcache_mark_all_peers_dirty(pcache,
					bio->bi_iter.bi_sector,
					bio->bi_iter.bi_size);
				pcache_req_put(pcache_req, net_ret);
			}
		} else {
			/* No peer connected: mark every active peer's per-peer
			 * bitmap so each catches up when it reconnects. */
			pcache_mark_all_peers_dirty(pcache,
				bio->bi_iter.bi_sector,
				bio->bi_iter.bi_size);
		}
	}

	pcache_req_put(pcache_req, ret);
	return DM_MAPIO_SUBMITTED;
}

static void dm_pcache_status(struct dm_target *ti, status_type_t type,
			     unsigned int status_flags, char *result,
			     unsigned int maxlen)
{
	struct dm_pcache *pcache = ti->private;
	struct pcache_cache_dev *cache_dev = &pcache->cache_dev;
	struct pcache_backing_dev *backing_dev = &pcache->backing_dev;
	struct pcache_cache *cache = &pcache->cache;
	unsigned int sz = 0;

	switch (type) {
	case STATUSTYPE_INFO:
		DMEMIT("%x %u %u %u %u %x %u:%u %u:%u %u:%u",
		       cache_dev->sb_flags,
		       cache_dev->seg_num,
		       cache->n_segs,
		       bitmap_weight(cache->seg_map, cache->n_segs),
		       pcache_cache_get_gc_percent(cache),
		       cache->cache_info.flags,
		       cache->key_head.cache_seg->cache_seg_id,
		       cache->key_head.seg_off,
		       cache->dirty_tail.cache_seg->cache_seg_id,
		       cache->dirty_tail.seg_off,
		       cache->key_tail.cache_seg->cache_seg_id,
		       cache->key_tail.seg_off);
		/* Phase 3.5 — gen is device-wide. Sum dirty bits across all
		 * active peers' bitmaps (worst-case "total replication debt"). */
		if (pcache->net) {
			u64 total_dirty = 0, max_bits = 0;
			struct pcache_peer *_p;
			int _slot;

			for_each_active_peer(pcache->net, _p, _slot) {
				if (_p->bm) {
					total_dirty += pcache_bm_dirty_count(_p->bm);
					if (_p->bm->nr_bits > max_bits)
						max_bits = _p->bm->nr_bits;
				}
			}
			DMEMIT(" bm_gen=%llu bm_dirty=%llu/%llu",
			       pcache->generation, total_dirty, max_bits);
		}
		break;
	case STATUSTYPE_TABLE:
		DMEMIT("%s %s 4 cache_mode writeback crc %s",
		       cache_dev->dm_dev->name,
		       backing_dev->dm_dev->name,
		       cache_data_crc_on(cache) ? "true" : "false");
		break;
	case STATUSTYPE_IMA:
		*result = '\0';
		break;
	}
}

static int dm_pcache_message(struct dm_target *ti, unsigned int argc,
			     char **argv, char *result, unsigned int maxlen)
{
	struct dm_pcache *pcache = ti->private;
	unsigned long val;
	unsigned int sz = 0;

	if (argc < 1)
		goto err;

	if (argc == 2 && !strcasecmp(argv[0], "gc_percent")) {
		if (kstrtoul(argv[1], 10, &val))
			goto err;
		return pcache_cache_set_gc_percent(&pcache->cache, val);
	}

	/* Phase 3.5 — bitmap-status now lists per-peer rows. */
	if (argc == 1 && !strcasecmp(argv[0], "bitmap-status")) {
		struct pcache_peer *_p;
		int _slot;

		if (!pcache->net) {
			DMEMIT("no network (single-host mode)");
			return 0;
		}
		DMEMIT("device gen=%llu gen_dirty=%d", pcache->generation,
		       pcache->gen_dirty ? 1 : 0);
		for_each_active_peer(pcache->net, _p, _slot) {
			if (_p->bm) {
				DMEMIT("; peer[%d node_id=%d] peer_gen_at_sync=%llu dirty=%llu/%llu flags=0x%x",
				       _slot, _p->node_id,
				       _p->bm->peer_gen_at_sync,
				       pcache_bm_dirty_count(_p->bm),
				       _p->bm->nr_bits, _p->bm->flags);
			}
		}
		return 0;
	}

	if (argc == 1 && !strcasecmp(argv[0], "force-resync"))
		return pcache_net_force_resync(pcache);

	/* Phase 3 Step 4 — runtime peer management. */
	if (argc == 3 && !strcasecmp(argv[0], "add-peer")) {
		u32 pid;

		if (kstrtouint(argv[1], 10, &pid))
			goto err;
		return pcache_add_peer(pcache, pid, argv[2]);
	}
	if (argc == 2 && !strcasecmp(argv[0], "remove-peer")) {
		u32 pid;

		if (kstrtouint(argv[1], 10, &pid))
			goto err;
		return pcache_remove_peer(pcache, pid);
	}
	if (argc == 1 && !strcasecmp(argv[0], "list-peers")) {
		int n = pcache_list_peers(pcache, result, maxlen);

		if (n < 0)
			return n;
		sz = (unsigned int)n;
		(void)sz;
		return 0;
	}

	/* Phase 3 Step 9 — runtime role switch.
	 *   set-role primary [force]
	 *   set-role secondary [force]
	 */
	if ((argc == 2 || argc == 3) && !strcasecmp(argv[0], "set-role")) {
		bool primary;
		bool force = (argc == 3 && !strcasecmp(argv[2], "force"));

		if (!strcasecmp(argv[1], "primary"))
			primary = true;
		else if (!strcasecmp(argv[1], "secondary"))
			primary = false;
		else
			goto err;
		return pcache_set_role(pcache, primary, force);
	}

	/* Phase 3 Step 13 — split-brain & resync control. */
	if (argc == 3 && !strcasecmp(argv[0], "resolve-split-brain")) {
		u32 pid;
		bool victim;

		if (kstrtouint(argv[1], 10, &pid))
			goto err;
		if (!strcasecmp(argv[2], "victim"))
			victim = true;
		else if (!strcasecmp(argv[2], "survivor"))
			victim = false;
		else
			goto err;
		return pcache_resolve_split_brain(pcache, pid, victim);
	}
	if (argc == 1 && !strcasecmp(argv[0], "abort-resync"))
		return pcache_abort_resync(pcache);
	if (argc == 1 && !strcasecmp(argv[0], "reset-generation"))
		return pcache_reset_generation(pcache);

err:
	return -EINVAL;
}

static struct target_type dm_pcache_target = {
	.name		= "pcache-dist",
	.version	= {0, 1, 0},
	.module		= THIS_MODULE,
	.features	= DM_TARGET_SINGLETON,
	.ctr		= dm_pcache_ctr,
	.dtr		= dm_pcache_dtr,
	.map		= dm_pcache_map_bio,
	.status		= dm_pcache_status,
	.message	= dm_pcache_message,
};

static int __init dm_pcache_init(void)
{
	int ret;

	ret = pcache_backing_init();
	if (ret)
		goto err;

	ret = pcache_cache_init();
	if (ret)
		goto backing_exit;

	ret = dm_register_target(&dm_pcache_target);
	if (ret)
		goto cache_exit;
	return 0;

cache_exit:
	pcache_cache_exit();
backing_exit:
	pcache_backing_exit();
err:
	return ret;
}
module_init(dm_pcache_init);

static void __exit dm_pcache_exit(void)
{
	dm_unregister_target(&dm_pcache_target);
	pcache_cache_exit();
	pcache_backing_exit();
}
module_exit(dm_pcache_exit);

MODULE_DESCRIPTION("dm-pcache Persistent Cache for block device");
MODULE_AUTHOR("Dongsheng Yang <dongsheng.yang@linux.dev>");
MODULE_LICENSE("GPL");
