#include "vmlinux.h"
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_core_read.h>
#include <bpf/bpf_tracing.h>
#include <xal_bpf_events.h>

char LICENSE[] SEC("license") = "GPL";

struct {
	__uint(type, BPF_MAP_TYPE_RINGBUF);
	__uint(max_entries, 1 << 24);
} events SEC(".maps");

struct xal_bpf_ctx ctx;
struct xal_bpf_stat stats;

static __always_inline uint32_t
dev_major_from_dev(uint32_t dev)
{
	return dev >> 20;
}

static __always_inline uint32_t
dev_minor_from_dev(uint32_t dev)
{
	return dev & ((1U << 20) - 1);
}

static __always_inline bool
match_fs(uint32_t dev)
{
	if (ctx.dev_major != dev_major_from_dev(dev) ||
		ctx.dev_minor != dev_minor_from_dev(dev)) {
		__sync_fetch_and_add(&stats.ignored_events, 1);
		return false;
	}

	return true;
}

static __always_inline void
fill_event(struct xal_bpf_event *e, enum xal_event_type type, uint64_t ino,
	   uint64_t startoff, uint64_t startblock, uint64_t blockcount,
	   uint32_t state, uint32_t bmap_state)
{
	uint64_t id = bpf_get_current_pid_tgid();

	e->ts_ns = bpf_ktime_get_ns();
	e->pid = (uint32_t)id;
	e->tgid = (uint32_t)(id >> 32);
	e->cpu = bpf_get_smp_processor_id();

	e->dev_major = ctx.dev_major;
	e->dev_minor = ctx.dev_minor;
	e->ino = ino;

	e->type = type;
	e->fs_block_size = ctx.fs_block_size;

	e->startoff = startoff;
	e->startblock = startblock;
	e->blockcount = blockcount;
	e->state = state;
	e->bmap_state = bmap_state;
}

SEC("kprobe/thaw_super")
int BPF_KPROBE(on_thaw_super, struct super_block *sb)
{
	dev_t dev;
	struct xal_bpf_event *e;

	dev = BPF_CORE_READ(sb, s_dev);

	if (!match_fs(dev)) {
		return 0;
	}

	e = bpf_ringbuf_reserve(&events, sizeof(*e), 0);
	if (!e) {
		__sync_fetch_and_add(&stats.lost_events, 1);
		return 0;
	}

	fill_event(e, XAL_FS_UNFREEZE_EVENT, 0, 0, 0, 0, 0, 0);

	bpf_ringbuf_submit(e, 0);
	return 0;
}