#ifndef XAL_BPF_EVENTS_H
#define XAL_BPF_EVENTS_H

enum xal_event_type {
	XAL_FS_UNFREEZE_EVENT = 1,
	// if needed, add more types here
};

struct xal_bpf_event {
	uint32_t type;

	uint64_t ts_ns;
	uint32_t pid;
	uint32_t tgid;
	uint32_t cpu;

	uint32_t dev_major;
	uint32_t dev_minor;
	uint64_t ino;

	uint32_t fs_block_size;

	uint64_t startoff;
	uint64_t startblock;
	uint64_t blockcount;
	uint32_t state;
	uint32_t bmap_state;
};

struct xal_bpf_stat {
	uint64_t lost_events;
	uint64_t ignored_events;
};

struct xal_bpf_ctx {
	uint32_t dev_major;
	uint32_t dev_minor;
	uint32_t fs_block_size;
};
#endif
