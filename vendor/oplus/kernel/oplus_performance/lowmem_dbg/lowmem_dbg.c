// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2018-2020 Oplus. All rights reserved.
 */

#define pr_fmt(fmt) "lowmem_dbg: " fmt

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/quicklist.h>
#include <linux/cma.h>
#include <linux/oom.h>
#include <linux/sched.h>
#include <linux/sched/debug.h>
#include <linux/rcupdate.h>
#include <linux/notifier.h>
#include <linux/mutex.h>
#include <linux/delay.h>
#include <linux/swap.h>
#include <linux/fs.h>
#include <linux/jiffies.h>
#include <linux/workqueue.h>
#include <linux/freezer.h>
#include <linux/ksm.h>
#include <linux/version.h>
#include <linux/dma-buf.h>
#include <linux/fdtable.h>
#include <soc/oplus/lowmem_dbg.h>
#include <linux/slub_def.h>
#include <linux/proc_fs.h>
#include <linux/vmstat.h>
#include <linux/vmalloc.h>
#ifdef CONFIG_OF_RESERVED_MEM
#include <linux/of_reserved_mem.h>
#endif /* CONFIG_OF_RESERVED_MEM */
#ifdef CONFIG_KSWAPD_DEBUG_STATISTICS
#include <linux/math64.h>
#endif

#include "../../../../mm/slab.h"

#ifdef CONFIG_MTK_ION
#include "ion_priv.h"
#endif /* CONFIG_MTK_ION */

#ifdef CONFIG_QCOM_KGSL
#include "kgsl.h"
#endif /* CONFIG_QCOM_KGSL */

#define K(x) ((x) << (PAGE_SHIFT-10))
extern unsigned long ion_total(void);

static int lowmem_dbg_ram[] = {
	0,
	768 * 1024,	/* 3GB */
	1024 * 1024,	/* 4GB */
};

static int lowmem_dbg_low[] = {
	64 * 1024,	/* 256MB */
	128 * 1024,	/* 512MB */
	256 * 1024,	/* 1024MB */
};

static void lowmem_dbg_dump(struct work_struct *work);

static DEFINE_MUTEX(lowmem_dump_mutex);
static DECLARE_WORK(lowmem_dbg_work, lowmem_dbg_dump);
static DECLARE_WORK(lowmem_dbg_critical_work, lowmem_dbg_dump);

static int dump_tasks_info(bool verbose);
static int dump_ion(bool verbose);
#if defined(CONFIG_SLUB_DEBUG) || defined(CONFIG_SLUB_STAT_DEBUG)
static int dump_slab(bool verbose);
#endif /* CONFIG_SLUB_DEBUG || CONFIG_SLUB_STAT_DEBUG */
#if IS_ENABLED(CONFIG_MTK_GPU_SUPPORT)
extern void mtk_dump_gpu_memory_usage(void) __attribute__((weak));
extern bool mtk_get_gpu_memory_usage(unsigned int *pMemUsage);
static int dump_gpu(bool verbose)
{
	mtk_dump_gpu_memory_usage();
	return 0;
}
#endif /* CONFIG_MTK_GPU_SUPPORT */

enum mem_type {
	MEM_ANON,
	MEM_SLAB_UNRECLAIMABLE,
	MEM_ION_USED,
	MEM_GPU,
	/* other stats start from here */
	MEM_TOTAL,
	MEM_FREE,
	MEM_AVAILABLE,
	MEM_FILE,
	MEM_ACTIVE_FILE,
	MEM_INACTIVE_FILE,
	MEM_ACTIVE_ANON,
	MEM_INACTIVE_ANON,
	MEM_PAGE_TABLES,
	MEM_KERNEL_STACKS,
	MEM_SLAB,
	MEM_SLAB_RECLAIMABLE,
	MEM_VMALLOC,
	MEM_ION,
	MEM_ION_CACHE,
	MEM_DT_RESERVED,
	MEM_ITEMS,
};

struct lowmem_dbg_cfg {
	unsigned int dump_interval;
	u64 last_jiffies;
	u64 wm_low;
	u64 wm_critical;
	u64 wms[MEM_TOTAL];
};

const char * const mem_type_text[] = {
	"Anon",
	"SlabUnreclaim",
	"IONUsed",
	"GPU",
	/* other stats start from here */
	"Total",
	"Free",
	"Available",
	"File",
	"ActivFile",
	"InactiveFile",
	"AtiveAnon",
	"InativeAnon",
	"PageTable",
	"KernelStack",
	"Slab",
	"SlabReclaim",
	"Vmalloc",
	"ION",
	"IONCache",
	"DTReserved",
	"",
};

struct lowmem_dump_cfg {
	enum mem_type type;
	void (*dump)(bool);
	bool (*critical)(void);
};

#ifdef CONFIG_KSWAPD_DEBUG_STATISTICS
#define NS_PER_MS 1000000
#define KSWAPD_DEBUG_INFO_KBUF_LEN 4096
static void dump_kswapd_debug_info(void)
{
	pg_data_t *pgdat;
	int order;
	struct kswapd_debug_entry *entry;

	for_each_online_pgdat(pgdat) {
		pr_info("Node %d kswapd has run %lu ms %lu times:\n",
				pgdat->node_id,
				div_u64(atomic_long_read(&pgdat->kswapd_running_ns), NS_PER_MS),
				atomic_long_read(&pgdat->kswapd_running_times));

		entry = pgdat->kswapd_debug;
		for (order=0; order<=MAX_ORDER; order++)
			pr_info("order %4d %12lu %12lu ms\n", order,
					atomic_long_read(&entry[order].cnt),
					div_u64(atomic_long_read(&entry[order].runtime_ns), NS_PER_MS));
	}
}

ssize_t kswapd_debug_info_read(struct file *file, char __user *buffer,
		size_t count, loff_t *off)
{
	pg_data_t *pgdat;
	int order;
	struct kswapd_debug_entry *entry;
	int len = 0;
	int end;
	char* kbuf = (char*)vmalloc(KSWAPD_DEBUG_INFO_KBUF_LEN);

	if (!kbuf)
		return -ENOMEM;

	for_each_online_pgdat(pgdat) {
		len += scnprintf(kbuf + len, KSWAPD_DEBUG_INFO_KBUF_LEN - len,
				"Node %d kswapd has run %lu ms %lu times:\n",
				pgdat->node_id,
				div_u64(atomic_long_read(&pgdat->kswapd_running_ns), NS_PER_MS),
				atomic_long_read(&pgdat->kswapd_running_times));

		entry = pgdat->kswapd_debug;
		for (order=0; order<=MAX_ORDER; order++)
			len += scnprintf(kbuf + len, KSWAPD_DEBUG_INFO_KBUF_LEN - len,
					"order %3d %10lu %10lu ms\n", order,
					atomic_long_read(&entry[order].cnt),
					div_u64(atomic_long_read(&entry[order].runtime_ns), NS_PER_MS));

	}

	if (KSWAPD_DEBUG_INFO_KBUF_LEN == len)
		end = KSWAPD_DEBUG_INFO_KBUF_LEN -1;
	else
		end = len;
	if (kbuf[end] != '\n')
		kbuf[end] = '\n';

	if (len > *off)
		len -= *off;
	else
		len = 0;

	if (copy_to_user(buffer, kbuf + *off, (len < count ? len : count))) {
		vfree(kbuf);
		return -EFAULT;
	}

	vfree(kbuf);
	*off += (len < count ? len : count);
	return (len < count ? len : count);
}
#endif

static long get_mem_usage_pages(enum mem_type type)
{
	int ret = 0;
	switch(type) {
	case MEM_TOTAL:
		return totalram_pages;
	case MEM_FREE:
		ret = global_zone_page_state(NR_FREE_PAGES);
		break;
	case MEM_AVAILABLE:
		ret = si_mem_available();
		break;
	case MEM_FILE:
		ret = global_node_page_state(NR_FILE_PAGES);
		break;
	case MEM_ACTIVE_FILE:
		ret = global_node_page_state(NR_ACTIVE_FILE);
		break;
	case MEM_INACTIVE_FILE:
		ret = global_node_page_state(NR_INACTIVE_FILE);
		break;
	case MEM_ANON:
		ret = global_node_page_state(NR_ANON_MAPPED);
		break;
	case MEM_ACTIVE_ANON:
		ret = global_node_page_state(NR_ACTIVE_ANON);
		break;
	case MEM_INACTIVE_ANON:
		ret = global_node_page_state(NR_INACTIVE_ANON);
		break;
	case MEM_PAGE_TABLES:
		ret = global_zone_page_state(NR_PAGETABLE);
		break;
	case MEM_KERNEL_STACKS:
		ret = global_zone_page_state(NR_KERNEL_STACK_KB) / 4;
		break;
	case MEM_SLAB:
		ret = global_node_page_state(NR_SLAB_RECLAIMABLE) +
			global_node_page_state(NR_SLAB_UNRECLAIMABLE);
		break;
	case MEM_SLAB_RECLAIMABLE:
		ret = global_node_page_state(NR_SLAB_RECLAIMABLE);
		break;
	case MEM_SLAB_UNRECLAIMABLE:
		ret = global_node_page_state(NR_SLAB_UNRECLAIMABLE);
		break;
	case MEM_VMALLOC:
		ret = vmalloc_nr_pages();
		break;
	case MEM_ION:
#ifdef OPLUS_FEATURE_HEALTHINFO
#ifdef CONFIG_OPLUS_HEALTHINFO
		ret = (ion_total() >> PAGE_SHIFT) +
			global_zone_page_state(NR_IONCACHE_PAGES);
		break;
#endif
#endif
	case MEM_ION_USED:
#ifdef OPLUS_FEATURE_HEALTHINFO
#ifdef CONFIG_OPLUS_HEALTHINFO
		ret = ion_total() >> PAGE_SHIFT;
#endif
#endif
		break;
	case MEM_ION_CACHE:
#ifdef OPLUS_FEATURE_HEALTHINFO
#ifdef CONFIG_OPLUS_HEALTHINFO
		ret = global_zone_page_state(NR_IONCACHE_PAGES);
#endif
#endif
		break;
	case MEM_GPU:
#ifdef CONFIG_QCOM_KGSL
		ret  = atomic_long_read(&kgsl_driver.stats.page_alloc) >> PAGE_SHIFT;
#elif IS_ENABLED(CONFIG_MTK_GPU_SUPPORT)
		mtk_get_gpu_memory_usage(&ret);
		ret >>= PAGE_SHIFT;
#endif /* CONFIG_QCOM_KGSL */
		break;
	case MEM_DT_RESERVED:
#ifdef CONFIG_OF_RESERVED_MEM
		ret = dt_memory_reserved_pages();
#endif /* CONFIG_OF_RESERVED_MEM */
		break;
	case MEM_ITEMS:
		break;
	default:
		break;
	}
	return ret;
}

static inline int dump_mem_detail(enum mem_type type, bool verbose)
{
	int ret = -1;
	switch(type) {
	case MEM_SLAB:
	case MEM_SLAB_RECLAIMABLE:
	case MEM_SLAB_UNRECLAIMABLE:
#if defined(CONFIG_SLUB_DEBUG) || defined(CONFIG_SLUB_STAT_DEBUG)
		ret = dump_slab(verbose);
#endif /* CONFIG_SLUB_DEBUG || CONFIG_SLUB_STAT_DEBUG */
		break;
	case MEM_ANON:
		ret = dump_tasks_info(verbose);
		break;
	case MEM_ION_USED:
		ret = dump_ion(verbose);
		break;
	case MEM_GPU:
#if IS_ENABLED(CONFIG_MTK_GPU_SUPPORT)
		ret = dump_gpu(verbose);
#endif /* CONFIG_MTK_GPU_SUPPORT */
		break;
	default:
		break;
	}
	return ret;
}

static struct lowmem_dbg_cfg dbg_cfg;

static void oplus_show_mem(void)
{
	pg_data_t *pgdat;
	unsigned long total = 0, reserved = 0, highmem = 0;
	long free, slab_rec, slab_unrec, vmalloc, anon, file, pagetbl, kernel_stack;
	long ion_used, ion_cache, gpu, dt_reserved, unaccounted;

	show_free_areas(SHOW_MEM_FILTER_NODES, NULL);

	for_each_online_pgdat(pgdat) {
		unsigned long flags;
		int zoneid;

		pgdat_resize_lock(pgdat, &flags);
		for (zoneid = 0; zoneid < MAX_NR_ZONES; zoneid++) {
			struct zone *zone = &pgdat->node_zones[zoneid];
			if (!populated_zone(zone))
				continue;

			total += zone->present_pages;
			reserved += zone->present_pages - zone->managed_pages;

			if (is_highmem_idx(zoneid))
				highmem += zone->present_pages;
		}
		pgdat_resize_unlock(pgdat, &flags);
	}

	printk("%lu pages RAM\n", total);
	printk("%lu pages HighMem/MovableOnly\n", highmem);
	printk("%lu pages reserved\n", reserved);
#ifdef CONFIG_CMA
	printk("%lu pages cma reserved\n", totalcma_pages);
#endif
#ifdef CONFIG_QUICKLIST
	printk("%lu pages in pagetable cache\n",
	       quicklist_total_size());
#endif
#ifdef CONFIG_MEMORY_FAILURE
	printk("%lu pages hwpoisoned\n", atomic_long_read(&num_poisoned_pages));
#endif
	total = get_mem_usage_pages(MEM_TOTAL);
	free = get_mem_usage_pages(MEM_FREE);
	slab_rec = get_mem_usage_pages(MEM_SLAB_RECLAIMABLE);
	slab_unrec = get_mem_usage_pages(MEM_SLAB_UNRECLAIMABLE);
	vmalloc = get_mem_usage_pages(MEM_VMALLOC);
	anon = get_mem_usage_pages(MEM_ANON);
	file = get_mem_usage_pages(MEM_FILE);
	pagetbl = get_mem_usage_pages(MEM_PAGE_TABLES);
	kernel_stack = get_mem_usage_pages(MEM_KERNEL_STACKS);
	ion_used = get_mem_usage_pages(MEM_ION_USED);
	ion_cache = get_mem_usage_pages(MEM_ION_CACHE);
	gpu = get_mem_usage_pages(MEM_GPU);
	dt_reserved = get_mem_usage_pages(MEM_DT_RESERVED);

	unaccounted = total - free - slab_rec - slab_unrec - vmalloc -anon -
		file - pagetbl - kernel_stack - ion_used - ion_cache - gpu;

	pr_info("%s:%lukB %s:%lukB %s:%lukB %s:%lukB %s:%lukB "
		"%s:%lukB %s:%lukB %s:%lukB %s:%lukB %s:%lukB "
		"%s:%lukB %s:%lukB %s:%lukB Unaccounted:%lukB",
		mem_type_text[MEM_TOTAL], K(total),
		mem_type_text[MEM_FREE], K(free),
		mem_type_text[MEM_SLAB_RECLAIMABLE], K(slab_rec),
		mem_type_text[MEM_SLAB_UNRECLAIMABLE], K(slab_unrec),
		mem_type_text[MEM_VMALLOC], K(vmalloc),
		mem_type_text[MEM_ANON], K(anon),
		mem_type_text[MEM_FILE], K(file),
		mem_type_text[MEM_PAGE_TABLES], K(pagetbl),
		mem_type_text[MEM_KERNEL_STACKS], K(kernel_stack),
		mem_type_text[MEM_ION_USED], K(ion_used),
		mem_type_text[MEM_ION_CACHE], K(ion_cache),
		mem_type_text[MEM_GPU], K(gpu),
		mem_type_text[MEM_DT_RESERVED], K(dt_reserved),
		K(unaccounted));
}

static int dump_tasks_info(bool unused)
{
	struct task_struct *p;
	struct task_struct *tsk;
	char task_state = ' ';
	char frozen_mark = ' ';
	unsigned long tsk_nr_ptes = 0;
	pid_t ppid = 0;

	pr_info("comm             32   uid s f   pid  ppid   oom       vss    anon    file   shmem    swap\n");

	rcu_read_lock();
	for_each_process(p) {
		tsk = find_lock_task_mm(p);
		if (!tsk)
			continue;
#if (LINUX_VERSION_CODE < KERNEL_VERSION(4, 19, 0))
		tsk_nr_ptes = PTRS_PER_PTE * sizeof(pte_t) * atomic_long_read(&tsk->mm->nr_ptes);
#else
		tsk_nr_ptes = mm_pgtables_bytes(tsk->mm);
#endif
		task_state = task_state_to_char(tsk);
		/* check whether we have freezed a task. */
		frozen_mark = frozen(tsk) ? '*' : ' ';
		ppid = task_pid_nr(rcu_dereference(tsk->real_parent));

		pr_info("%-16s %2d %5d %c %c %5d %5d %5d %9lu %7lu %7lu %7lu %7lu\n",
			tsk->comm, test_ti_thread_flag(task_thread_info(tsk), TIF_32BIT) != 0,
			from_kuid(&init_user_ns, task_uid(tsk)),
			task_state, frozen_mark,
			tsk->pid, ppid, tsk->signal->oom_score_adj,
			tsk->mm->total_vm,
			get_mm_counter(tsk->mm, MM_ANONPAGES),
			get_mm_counter(tsk->mm, MM_FILEPAGES),
			get_mm_counter(tsk->mm, MM_SHMEMPAGES),
			get_mm_counter(tsk->mm, MM_SWAPENTS));
		task_unlock(tsk);
	}
	rcu_read_unlock();

	return 0;
}

#ifdef CONFIG_MTK_ION
extern struct ion_device *g_ion_device;
static int dump_ion(bool verbose)
{
	struct ion_device *dev = g_ion_device;
	struct rb_node *n, *m;
	unsigned int buffer_size = 0;
	size_t total_orphaned_size = 0;
	size_t total_size = 0;

#ifdef OPLUS_FEATURE_MTK_ION_SEPARATE_LOCK
/* Hailong.Liu@BSP.Kernel.MM, 2020-09-07, use two separate locks for heaps and
 * clients in ion_device */
	if (!down_read_trylock(&dev->client_lock)) {
#else /* OPLUS_FEATURE_MTK_ION_SEPARATE_LOCK */
	if (!down_read_trylock(&dev->lock)) {
#endif /* OPLUS_FEATURE_MTK_ION_SEPARATE_LOCK */
		return -1;
	}
	for (n = rb_first(&dev->clients); n; n = rb_next(n)) {
		struct ion_client *client =
			rb_entry(n, struct ion_client, node);
		char task_comm[TASK_COMM_LEN];

		if (client->task) {
			get_task_comm(task_comm, client->task);
		}

		mutex_lock(&client->lock);
		for (m = rb_first(&client->handles); m; m = rb_next(m)) {
			struct ion_handle *handle = rb_entry(m, struct ion_handle,
							     node);
			buffer_size += (unsigned int)(handle->buffer->size);
		}
		if (!buffer_size) {
			mutex_unlock(&client->lock);
			continue;
		}
		pr_info("[%-5d] %-8d %-16s %-16s\n",
			client->pid,
			buffer_size / SZ_1K,
			client->task ? task_comm : "from_kernel",
			(*client->dbg_name) ? client->dbg_name : client->name);
		buffer_size = 0;
		mutex_unlock(&client->lock);
	}

	pr_info("orphaned allocation (info is from last known client):\n");
	mutex_lock(&dev->buffer_lock);
	for (n = rb_first(&dev->buffers); n; n = rb_next(n)) {
		struct ion_buffer *buffer = rb_entry(n, struct ion_buffer,
						     node);
		total_size += buffer->size;
		if (!buffer->handle_count) {
			pr_info("[%-5d] %-8d %-16s 0x%p %d %d\n",
				buffer->pid,
				buffer->size / SZ_1K,
				buffer->task_comm,
				buffer,
				buffer->kmap_cnt,
				atomic_read(&buffer->ref.refcount.refs));
			total_orphaned_size += buffer->size;
		}
	}
	mutex_unlock(&dev->buffer_lock);
	pr_info("orphaned: %zu total: %zu\n",
		total_orphaned_size, total_size);

#ifdef OPLUS_FEATURE_MTK_ION_SEPARATE_LOCK
/* Hailong.Liu@BSP.Kernel.MM, 2020-09-07, use two separate locks for heaps and
 * clients in ion_device */
	up_read(&dev->client_lock);
#else /* OPLUS_FEATURE_MTK_ION_SEPARATE_LOCK */
	up_read(&dev->lock);
#endif /* OPLUS_FEATURE_MTK_ION_SEPARATE_LOCK */
	return 0;
}
#else /* CONFIG_MTK_ION */
struct dma_info {
	struct task_struct *tsk;
	bool verbose;
	size_t sz;
};

static int acct_dma_dize(const void *data, struct file *file,
			 unsigned int n)
{
	struct dma_info *dmainfo = (struct dma_info *)data;
	struct dma_buf *dbuf;

	if (!oplus_is_dma_buf_file(file)) {
		return 0;
	}

	dbuf = file->private_data;
	if (dbuf->size && dmainfo->verbose) {
		pr_info("%s:%ldkB\n",
			dbuf->buf_name,
			dbuf->size / SZ_1K);
	}

	dmainfo->sz += dbuf->size;
	return 0;
}

static int dump_ion(bool verbose)
{
	struct task_struct *task, *thread;
	struct files_struct *files;
	int ret = 0;

	rcu_read_lock();
	for_each_process(task) {
		struct files_struct *group_leader_files = NULL;
		struct dma_info dmainfo = {
			.verbose = verbose,
			.sz = 0,
		};

		for_each_thread(task, thread) {
			task_lock(thread);
			if (unlikely(!group_leader_files)) {
				group_leader_files = task->group_leader->files;
			}

			files = thread->files;
			if (files && (group_leader_files != files ||
				      thread == task->group_leader)) {
				dmainfo.tsk = thread;
				ret = iterate_fd(files, 0, acct_dma_dize,
						 &dmainfo);
			}
			task_unlock(thread);
		}

		if (ret || !dmainfo.sz) {
			continue;
		}

		pr_info("%s (PID:%d) size:%lukB\n",
			task->comm, task->pid, dmainfo.sz / SZ_1K);
	}
	rcu_read_unlock();
	return 0;
}
#endif /* CONFIG_MTK_ION */

#if defined(CONFIG_SLUB_DEBUG) || defined(CONFIG_SLUB_STAT_DEBUG)
static int dump_slab(bool verbose)
{
	if (likely(!verbose)) {
		unsigned long slab_pages = 0;
		struct kmem_cache *cachep = NULL;
		struct kmem_cache *max_cachep = NULL;
		struct kmem_cache *prev_max_cachep = NULL;

		mutex_lock(&slab_mutex);
		list_for_each_entry(cachep, &slab_caches, list) {
			struct slabinfo sinfo;
			unsigned long scratch;

			memset(&sinfo, 0, sizeof(sinfo));
			get_slabinfo(cachep, &sinfo);
			scratch = sinfo.num_slabs << sinfo.cache_order;

			if (slab_pages < scratch) {
				slab_pages = scratch;
				prev_max_cachep = max_cachep;
				max_cachep = cachep;
			}
		}

		if (max_cachep || prev_max_cachep) {
			pr_info("name            <active_objs> <num_objs> <objsize> <objperslab> <pagesperslab> : tunables <limit> <batchcount> <sharedfactor> : slabdata <active_slabs> <num_slabs> <sharedavail>");
		}

		if (max_cachep) {
			struct slabinfo sinfo;

			memset(&sinfo, 0, sizeof(sinfo));
			/* TODO maybe we can cache slabinfo to achieve
			 * better performance */
			get_slabinfo(max_cachep, &sinfo);

			pr_info("%-17s %6lu %6lu %6u %4u %4d : tunables %4u %4u %4u : slabdata %6lu %6lu %6lu\n",
				max_cachep->name, sinfo.active_objs,
				sinfo.num_objs, max_cachep->size,
				sinfo.objects_per_slab,
				(1 << sinfo.cache_order),
				sinfo.limit, sinfo.batchcount, sinfo.shared,
				sinfo.active_slabs, sinfo.num_slabs,
				sinfo.shared_avail);
		}

		if (prev_max_cachep) {
			struct slabinfo sinfo;

			memset(&sinfo, 0, sizeof(sinfo));
			/* TODO maybe we can cache slabinfo to achieve
			 * better performance */
			get_slabinfo(prev_max_cachep, &sinfo);

			pr_info("%-17s %6lu %6lu %6u %4u %4d : tunables %4u %4u %4u : slabdata %6lu %6lu %6lu\n",
				prev_max_cachep->name, sinfo.active_objs,
				sinfo.num_objs, prev_max_cachep->size,
				sinfo.objects_per_slab,
				(1 << sinfo.cache_order),
				sinfo.limit, sinfo.batchcount, sinfo.shared,
				sinfo.active_slabs, sinfo.num_slabs,
				sinfo.shared_avail);
		}
		mutex_unlock(&slab_mutex);

	} else {
		struct kmem_cache *cachep = NULL;

		pr_info("# name            <active_objs> <num_objs> <objsize> <objperslab> <pagesperslab> : tunables <limit> <batchcount> <sharedfactor> : slabdata <active_slabs> <num_slabs> <sharedavail>");

		mutex_lock(&slab_mutex);
		list_for_each_entry(cachep, &slab_caches, list) {
			struct slabinfo sinfo;

			memset(&sinfo, 0, sizeof(sinfo));
			get_slabinfo(cachep, &sinfo);

			pr_info("%-17s %6lu %6lu %6u %4u %4d : tunables %4u %4u %4u : slabdata %6lu %6lu %6lu\n",
				cachep->name, sinfo.active_objs,
				sinfo.num_objs, cachep->size,
				sinfo.objects_per_slab,
				(1 << sinfo.cache_order),
				sinfo.limit, sinfo.batchcount, sinfo.shared,
				sinfo.active_slabs, sinfo.num_slabs,
				sinfo.shared_avail);
		}
		mutex_unlock(&slab_mutex);
	}
	return 0;
}
#endif /* CONFIG_SLUB_DEBUG && CONFIG_SLUB_STAT_DEBUG */


static void lowmem_dbg_dump(struct work_struct *work)
{
	bool critical = (work == &lowmem_dbg_critical_work);
	int i = 0;
	struct lowmem_dbg_cfg *pcfg = &dbg_cfg;

	mutex_lock(&lowmem_dump_mutex);
	pr_info("dump start avail:%lukB critical:%d\n",
		K(get_mem_usage_pages(MEM_AVAILABLE)), critical);
	oplus_show_mem();
	for (i = MEM_ANON; i < MEM_TOTAL; i++) {
		long usage = get_mem_usage_pages(i);
		const char *text = mem_type_text[i];
		if (usage <= 0) {
			pr_warn("not suport %s stats", text);
			continue;
		}
		pr_info("dump [%s] usage:%lukB above_watermark:%d\n",
			text, K(usage), pcfg->wms[i] < usage);
		dump_mem_detail(i, critical ? true : pcfg->wms[i] < usage);
		pr_info("dump [%s] end.\n", text);
	}
#ifdef CONFIG_KSWAPD_DEBUG_STATISTICS
	dump_kswapd_debug_info();
#endif
	pr_info("dump end\n");

#ifdef CONFIG_MTK_ION
	if (K(get_mem_usage_pages(MEM_ION_USED)) > 3670016)
		ion_mm_heap_memory_detail();
#endif

	mutex_unlock(&lowmem_dump_mutex);
}

void oplus_lowmem_dbg(bool critical)
{
	u64 now = get_jiffies_64();
	struct lowmem_dbg_cfg *pcfg = &dbg_cfg;

	if (time_before64(now, (pcfg->last_jiffies + pcfg->dump_interval)))
		return;

	pcfg->last_jiffies = now;
	if (unlikely(critical)) {
		schedule_work(&lowmem_dbg_critical_work);
	} else {
		schedule_work(&lowmem_dbg_work);
	}
}

static unsigned long lowmem_count(struct shrinker *s,
				  struct shrink_control *sc)
{
	return global_node_page_state(NR_ACTIVE_ANON) +
		global_node_page_state(NR_ACTIVE_FILE) +
		global_node_page_state(NR_INACTIVE_ANON) +
		global_node_page_state(NR_INACTIVE_FILE);
}

static unsigned long lowmem_scan(struct shrinker *s, struct shrink_control *sc)
{
	static atomic_t atomic_lmk = ATOMIC_INIT(0);
	struct lowmem_dbg_cfg *pcfg = &dbg_cfg;
	long avail = get_mem_usage_pages(MEM_AVAILABLE);

	if (avail > pcfg->wm_low)
		return 0;

	if (atomic_inc_return(&atomic_lmk) > 1) {
		atomic_dec(&atomic_lmk);
		return 0;
	}

	oplus_lowmem_dbg(avail <= pcfg->wm_critical);
	atomic_dec(&atomic_lmk);
	return 0;
}

static struct shrinker lowmem_shrinker = {
	.scan_objects = lowmem_scan,
	.count_objects = lowmem_count,
	.seeks = DEFAULT_SEEKS
};

static __init int oplus_lowmem_dbg_init(void)
{
	int ret, i;
	struct lowmem_dbg_cfg *pcfg = &dbg_cfg;

	/* This is a process holding an application service */
	pcfg->dump_interval = 15 * HZ;

	/* init watermark */
	pcfg->wms[MEM_ION_USED] = SZ_2G >> PAGE_SHIFT;
	pcfg->wms[MEM_ANON] = totalram_pages / 2;
	pcfg->wms[MEM_SLAB_UNRECLAIMABLE] = SZ_1G >> PAGE_SHIFT;
	pcfg->wms[MEM_GPU] = SZ_2G >> PAGE_SHIFT;
	pcfg->wm_low = lowmem_dbg_low[0];
	for (i = ARRAY_SIZE(lowmem_dbg_ram) - 1; i >= 0; i--) {
		if (totalram_pages >= lowmem_dbg_ram[i]) {
			pcfg->wm_low = lowmem_dbg_low[i];
			break;
		}
	}
	pcfg->wm_critical = pcfg->wm_low / 2;

	if (totalram_pages < lowmem_dbg_ram[1])
		pcfg->wm_critical = pcfg->wm_low;

	ret = register_shrinker(&lowmem_shrinker);

	pr_info("init watermark %s:%lukB %s:%lukB %s:%lukB %s:%lukB "
		"Low %lukB Critical %lukB",
		mem_type_text[MEM_ANON], K(pcfg->wms[MEM_ANON]),
		mem_type_text[MEM_SLAB_UNRECLAIMABLE],
		K(pcfg->wms[MEM_SLAB_UNRECLAIMABLE]),
		mem_type_text[MEM_ION_USED], K(pcfg->wms[MEM_ION_USED]),
		mem_type_text[MEM_GPU], K(pcfg->wms[MEM_GPU]),
		K(pcfg->wm_low), K(pcfg->wm_critical));
	return 0;
}
device_initcall(oplus_lowmem_dbg_init);

#ifdef LOWMEM_DBG_DEBUG
static ssize_t oplus_lowmem_dbg_test_write(struct file *file,
					   const char __user *buf,
					   size_t count, loff_t *ppos)
{
	char buffer[13];
	int err, critical;

	memset(buffer, 0, sizeof(buffer));
	if (count > sizeof(buffer) - 1)
		count = sizeof(buffer) - 1;
	if (copy_from_user(buffer, buf, count))
		return -EFAULT;

	err = kstrtoint(strstrip(buffer), 0, &critical);
	if(err)
		return err;

	oplus_lowmem_dbg(critical);
	return count;
}

static int oplus_lowmem_dbg_test_open(struct inode *inode, struct file *filp)
{
	return 0;
}

static const struct file_operations lowmem_dbg_test_operations = {
	.open = oplus_lowmem_dbg_test_open,
	.write = oplus_lowmem_dbg_test_write,
};

static __init int oplus_lowmem_dbg_test_init(void)
{
	proc_create("lowmem_dbg_test", S_IWUGO, NULL,
		    &lowmem_dbg_test_operations);
	return 0;
}
fs_initcall(oplus_lowmem_dbg_test_init);
#endif

#ifdef CONFIG_KSWAPD_DEBUG_STATISTICS
static const struct file_operations kswapd_debug_operations = {
	.read = kswapd_debug_info_read,
};

int kswapd_debug_init(struct proc_dir_entry *parent)
{
	struct proc_dir_entry *entry;

	if (!parent) {
		pr_warn("%s: parent is NULL\n", __func__);
		return -EINVAL;
	}

	entry = proc_create("kswapd_debug", S_IRUGO, parent, &kswapd_debug_operations);
	if (!entry) {
		pr_warn("%s: create kswapd_debug failed.\n", __func__);
		return -ENOMEM;
	}

	return 0;
}
#endif
