#ifndef XAL_BPF_H
#define XAL_BPF_H

#include <pthread.h>
#include <stdatomic.h>

struct xal_bpf {
	struct xal_bpf_ctx ctx;
	struct ring_buffer *rb;
	struct xal_bpf_events *skel;
	pthread_t bpf_poll_thread_id;
	atomic_bool running;
	atomic_bool should_stop;
};

int
xal_be_fiemap_bpf_rb_init(struct xal_bpf *bpf);

int
xal_be_fiemap_bpf_init(struct xal_bpf *bpf);

void
xal_be_fiemap_bpf_close(struct xal_bpf *bpf);
#endif
