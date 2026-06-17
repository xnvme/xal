#include <errno.h>
#include <fcntl.h>
#include <khash.h>
#include <libxal.h>
#include <linux/fs.h>
#include <poll.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <bpf/bpf.h>
#include <bpf/libbpf.h>

#include <xal.h>
#include <xal_be_fiemap.h>
#include <xal_odf.h>
#include <xal_bpf_events.h>
#include <xal_bpf.h>
#include <xal_bpf_events.skel.h>

static int
handle_event(void *__ctx, void *__data, size_t len)
{
	const struct xal_bpf_event *e = __data;
	struct xal *xal = __ctx;

	if (len < sizeof(*e)) {
		XAL_DEBUG("FAILED: size of event too small; len(%lu) sizeof(e)(%lu)", len, sizeof(*e));
		return -EINVAL;
	}

	switch (e->type) {
	case XAL_FS_UNFREEZE_EVENT:
		XAL_DEBUG("WARNING: time(%ld) tgid/pid(%d/%d) cpu(%d) thawed the filesystem dev(%d,%d); unsafe to use extents",
			e->ts_ns, e->tgid, e->pid, e->cpu, e->dev_major, e->dev_minor);
		if (xal && xal->dirty) {
			atomic_store(xal->dirty, true);
			XAL_DEBUG("WARNING: setting xal->dirty to true");
		}
		break;
	default:
		break;
	}

	return 0;
}

int
xal_be_fiemap_bpf_rb_init(struct xal *xal, struct xal_bpf *bpf)
{
	struct ring_buffer *rb;
	int err;

	if (!bpf) {
		XAL_DEBUG("FAILED: No xal_bpf given");
		return -EINVAL;
	}

	// If already exist, free first for clean init
	if (bpf->rb) {
		ring_buffer__free(bpf->rb);
	}

	rb = ring_buffer__new(bpf_map__fd(bpf->skel->maps.events), handle_event, xal, NULL);
	if (!rb) {
		err = -errno;
		XAL_DEBUG("FAILD: ring_buffer__new(); err(%d)", err);
		goto out;
	}

	bpf->rb = rb;

	XAL_DEBUG("INFO: bpf ring buffer initialized");
	return 0;

out:
	ring_buffer__free(rb);
	return err;
}

int
xal_be_fiemap_bpf_init(struct xal_bpf *bpf)
{
	struct xal_bpf_events *skel = NULL;
	int err;

	if (!bpf) {
		XAL_DEBUG("FAILED: No xal_bpf given");
		return -EINVAL;
	}

	libbpf_set_strict_mode(LIBBPF_STRICT_ALL);
	libbpf_set_print(NULL);

	skel = xal_bpf_events__open();
	if (!skel) {
		err = -ENOMEM;
		XAL_DEBUG("FAILED: xal_bpf_events__open()");
		goto out;
	}

	skel->bss->ctx = bpf->ctx;

	err = xal_bpf_events__load(skel);
	if (err) {
		XAL_DEBUG("FAILED: xal_bpf_events__load(); err(%d)", err);
		goto out;
	}

	err = xal_bpf_events__attach(skel);
	if (err) {
		XAL_DEBUG("FAILED: xal_bpf_events__attach(); err(%d)", err);
		goto out;
	}

	bpf->skel = skel;

	return 0;
out:
	xal_bpf_events__destroy(skel);
	return err;
}

void
xal_be_fiemap_bpf_close(struct xal_bpf *bpf)
{
	if (!bpf) {
		XAL_DEBUG("SKIPPED: No xal_bpf given");
		return;
	}

	if (atomic_load(&bpf->running)) {
		atomic_store(&bpf->should_stop, true);
		pthread_join(bpf->bpf_poll_thread_id, NULL);
		XAL_DEBUG("INFO: xal_be_fiemap_bpf_close() joined bpf poll thread");
	}

	if (bpf->rb) {
		ring_buffer__free(bpf->rb);
	}

	if (bpf->skel) {
		xal_bpf_events__destroy(bpf->skel);
	}

	free(bpf);

	return;
}

static int
read_stats(struct xal_bpf_events *skel, struct xal_bpf_stat *out)
{
	*out = skel->bss->stats;
	return 0;
}

static void *
background_bpf_poll(void *arg)
{
	struct xal *xal = arg;
	struct xal_be_fiemap *be = (struct xal_be_fiemap *)xal->be;
	struct xal_bpf *bpf = be->bpf;
	struct xal_bpf_stat st;
	int err = 0;

	XAL_DEBUG("INFO: starting background bpf poll thread");

	while (!atomic_load(&bpf->should_stop)) {
		err = ring_buffer__poll(bpf->rb, 200);
		if (err < 0 && err != -EINTR) {
			XAL_DEBUG("FAILED: ring_buffer__poll(); err(%d)", err);
			break;
		}

		err = 0;
		if (read_stats(bpf->skel, &st) == 0) {
			// check stats for lost events
		}
	}

	pthread_exit((void *)(intptr_t)err);
}

int
xal_bpf_start_poll_thread(struct xal *xal)
{
	struct xal_backend_base *base;
	struct xal_be_fiemap *be;
	struct xal_bpf *bpf;
	int err;

	if (!xal) {
		XAL_DEBUG("FAILED: no xal given");
		return -EINVAL;
	}

	base = (struct xal_backend_base *)xal->be;
	if (base->type != XAL_BACKEND_FIEMAP) {
		XAL_DEBUG("FAILED: Invalid backend type(%d)", base->type);
		return -ENOTSUP;
	}

	be = (struct xal_be_fiemap *)base;
	if (!be->bpf) {
		XAL_DEBUG("FAILED: xal opened without bpf");
		return -ENODEV;
	}

	bpf = be->bpf;
	if (!bpf->skel) {
		XAL_DEBUG("FAILED: xal_bpf_events skel not initialized");
		return -ENOTCONN;
	}

	if (!bpf->rb) {
		XAL_DEBUG("FAILED: xal bpf ring buffer not initialized");
		return -ENOBUFS;
	}

	if (atomic_load(&bpf->running)) {
		XAL_DEBUG("SKIPPED: bpf thread already running");
		return 0;
	}

	atomic_store(&bpf->should_stop, false);
	atomic_store(&bpf->running, true);

	err = pthread_create(&bpf->bpf_poll_thread_id, NULL, &background_bpf_poll, xal);
	if (err) {
		atomic_store(&bpf->running, false);
		XAL_DEBUG("FAILED: pthread_create(); err(%d)", err);
		return -err;
	}

	return 0;
}

int
xal_bpf_stop_poll_thread(struct xal *xal)
{
	struct xal_backend_base *base;
	struct xal_be_fiemap *be;
	struct xal_bpf *bpf;
	int err;

	if (!xal) {
		XAL_DEBUG("FAILED: no xal given");
		return -EINVAL;
	}

	base = (struct xal_backend_base *)xal->be;
	if (base->type != XAL_BACKEND_FIEMAP) {
		XAL_DEBUG("FAILED: Invalid backend type(%d)", base->type);
		return -ENOTSUP;
	}

	be = (struct xal_be_fiemap *)base;
	if (!be->bpf) {
		XAL_DEBUG("FAILED: xal opened without bpf");
		return -ENODEV;
	}

	bpf = be->bpf;
	if (!bpf->skel) {
		XAL_DEBUG("FAILED: xal_bpf_events skel not initialized");
		return -ENOTCONN;
	}

	if (!atomic_load(&bpf->running)) {
		XAL_DEBUG("FAILED: bpf thread is not running");
		return -ESRCH;
	}

	atomic_store(&bpf->should_stop, true);
	err = pthread_join(bpf->bpf_poll_thread_id, NULL);
	if (err) {
		XAL_DEBUG("FAILED: pthread_join(); err(%d)", err);
		return -err;
	}
	XAL_DEBUG("INFO: xal_bpf_stop_poll_thread() joined bpf poll thread");

	atomic_store(&bpf->running, false);

	return 0;
}
