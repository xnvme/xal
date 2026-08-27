/**
 * xal-server: publish xal indexes over POSIX shared memory
 *
 * Builds an index per configured device with the FIEMAP backend and publishes it under the
 * shm_name from the config. Readers attach with xal_from_shm(); there is no socket, so those
 * names are the whole interface. Closing unlinks the regions, so readers must be done by then.
 *
 * Usage: xal-server --config <path>
 */
#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>

#include <libxal.h>

#include <xal_server_conf.h>

static volatile sig_atomic_t g_stop = 0;

static void
on_signal(int signo __attribute__((unused)))
{
	g_stop = 1;
}

/**
 * Per-device state for the watch callback
 *
 * The callback has to remember a failed re-index across invocations, and the parsed
 * configuration it would otherwise carry is shared and const, so the flag lives here.
 */
struct xal_server_watch {
	const struct xal_server_dev *dev;
	bool index_failed;
};

/**
 * Re-index the device whose filesystem the watcher reports as changed
 *
 * Runs on the watch thread. A failure leaves the index dirty for readers to see through
 * xal_is_dirty(), and is not recoverable from here. It is also latched, so the rebuild and
 * the log line happen once instead of on every subsequent notification.
 */
static void
on_dirty(struct xal *xal, void *cb_args)
{
	struct xal_server_watch *watch = cb_args;
	int err;

	/* xal_index() advances seq_lock on its exit path whether or not it succeeded, and the
	 * watch loop fires again on any advance while the state is dirty. Returning without the
	 * latch would rebuild the pools and emit LOG_CRIT back to back with nothing to stop it. */
	if (watch->index_failed) {
		return;
	}

	err = xal_index(xal);
	if (err) {
		watch->index_failed = true;
		syslog(LOG_CRIT,
		       "FAILED: xal_index(%s); err(%d); the index is stale, restart "
		       "required",
		       watch->dev->shm_name, err);
		return;
	}

	syslog(LOG_INFO, "re-indexed %s after a filesystem change", watch->dev->uri);
}

/**
 * Open, index and, where the watch mode calls for it, watch a single device
 *
 * On failure the handle is closed, so the caller has nothing to unwind.
 */
static int
publish(const struct xal_server_conf *conf, struct xal_server_watch *watch, struct xal **out)
{
	const struct xal_server_dev *dev = watch->dev;
	struct xal_opts opts = {0};
	struct xal *xal;
	int err;

	opts.be = XAL_BACKEND_FIEMAP;
	opts.watch_mode = conf->watch_mode;
	opts.shm_name = dev->shm_name;

	if (strlen(dev->mountpoint)) {
		opts.mountpoint = dev->mountpoint;
	}
	if (strlen(dev->subtree)) {
		opts.subtree = dev->subtree;
	}

	err = xal_open_from_uri(dev->uri, &xal, &opts);
	if (err) {
		if (err == -EEXIST) {
			/* The regions are created with O_EXCL and outlive their creator, so a
			 * name left behind by a killed primary reports the same as a live one. */
			syslog(LOG_ERR,
			       "shm_name(%s) already exists in /dev/shm; a primary may hold it, "
			       "or one may have died without unlinking it",
			       dev->shm_name);
		} else {
			syslog(LOG_ERR, "FAILED: xal_open_from_uri(%s); err(%d)", dev->uri, err);
		}
		return err;
	}

	err = xal_index(xal);
	if (err) {
		syslog(LOG_ERR, "FAILED: xal_index(%s); err(%d)", dev->uri, err);
		goto failed;
	}

	/* Reflink snapshot mode pins extents with clones at index time and runs no watcher. */
	if (conf->watch_mode && (conf->watch_mode != XAL_WATCHMODE_REFLINK_SNAPSHOT)) {
		err = xal_watch_filesystem(xal, on_dirty, watch);
		if (err) {
			syslog(LOG_ERR, "FAILED: xal_watch_filesystem(%s); err(%d)", dev->uri, err);
			goto failed;
		}
	}

	syslog(LOG_NOTICE, "published %s at shm_name(%s)", dev->uri, dev->shm_name);

	*out = xal;

	return 0;

failed:
	xal_close(xal);

	return err;
}

/**
 * Close every published index
 *
 * xal_close() joins the watch thread, so there is no watcher to stop first. It also unlinks
 * the shared memory, so readers must be done by the time this runs.
 */
static void
unpublish(unsigned int nxals, struct xal **xals)
{
	for (unsigned int i = 0; i < nxals; i++) {
		xal_close(xals[i]);
	}

	free(xals);
}

static int
parse_args(int argc, char *argv[], const char **config_file_path)
{
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--config") == 0) {
			if (i + 1 >= argc) {
				syslog(LOG_CRIT, "--config must be followed by a path");
				return -EINVAL;
			}
			*config_file_path = argv[++i];
		} else {
			syslog(LOG_CRIT, "unexpected argument: %s", argv[i]);
			return -EINVAL;
		}
	}

	if (!*config_file_path) {
		syslog(LOG_CRIT, "no configuration file given, see --config");
		return -EINVAL;
	}

	return 0;
}

int
main(int argc, char **argv)
{
	struct xal_server_conf conf = {0};
	const char *config_file_path = NULL;
	struct xal_server_watch *watches = NULL;
	struct xal **xals = NULL;
	unsigned int npublished = 0;
	struct sigaction sa = {0};
	sigset_t block, waitmask;
	int logopt = LOG_PID;
	int err;

	/* Under systemd stderr already reaches the journal, so LOG_PERROR there would log every
	 * line twice. */
	if (isatty(STDERR_FILENO)) {
		logopt |= LOG_PERROR;
	}

	openlog("xal-server", logopt, LOG_DAEMON);

	err = parse_args(argc, argv, &config_file_path);
	if (err) {
		goto exit;
	}

	err = xal_server_conf_from_toml(config_file_path, &conf);
	if (err) {
		goto exit;
	}
	setlogmask(LOG_UPTO(conf.log_level));

	xals = calloc(conf.ndevs, sizeof(*xals));
	if (!xals) {
		err = -errno;
		syslog(LOG_CRIT, "FAILED: calloc(); err(%d)", err);
		goto exit;
	}

	watches = calloc(conf.ndevs, sizeof(*watches));
	if (!watches) {
		err = -errno;
		syslog(LOG_CRIT, "FAILED: calloc(); err(%d)", err);
		goto exit;
	}

	/* Installed before the first publish: a watcher thread inherits the mask from
	 * pthread_create(), and a signal delivered to one of those would set g_stop while this
	 * thread stayed asleep in sigsuspend(). Blocking early also keeps a mid-publish signal from
	 * killing the process and stranding the shm regions. */
	sa.sa_handler = on_signal;
	sigemptyset(&sa.sa_mask);

	err = sigaction(SIGTERM, &sa, NULL);
	if (err) {
		err = -errno;
		syslog(LOG_CRIT, "FAILED: sigaction(SIGTERM); err(%d)", err);
		goto exit;
	}

	err = sigaction(SIGINT, &sa, NULL);
	if (err) {
		err = -errno;
		syslog(LOG_CRIT, "FAILED: sigaction(SIGINT); err(%d)", err);
		goto exit;
	}

	/* SIGHUP's default action terminates without unwinding, so a dropped terminal on the
	 * foreground invocation would strand every published region. */
	err = sigaction(SIGHUP, &sa, NULL);
	if (err) {
		err = -errno;
		syslog(LOG_CRIT, "FAILED: sigaction(SIGHUP); err(%d)", err);
		goto exit;
	}

	sigemptyset(&block);
	sigaddset(&block, SIGTERM);
	sigaddset(&block, SIGINT);
	sigaddset(&block, SIGHUP);

	err = sigprocmask(SIG_BLOCK, &block, NULL);
	if (err) {
		err = -errno;
		syslog(LOG_CRIT, "FAILED: sigprocmask(SIG_BLOCK); err(%d)", err);
		goto exit;
	}

	for (npublished = 0; npublished < conf.ndevs; npublished++) {
		watches[npublished].dev = &conf.devs[npublished];

		err = publish(&conf, &watches[npublished], &xals[npublished]);
		if (err) {
			goto exit;
		}
	}

	syslog(LOG_NOTICE, "serving %u device(s)", conf.ndevs);

	/* An explicitly empty mask rather than the one inherited at startup: a parent that had
	 * already blocked SIGTERM would otherwise leave this unstoppable short of SIGKILL. */
	sigemptyset(&waitmask);

	/* sigsuspend() installs the given mask and waits atomically, so a signal arriving just
	 * before the wait cannot be missed. One pending from the publish loop lands here. */
	while (!g_stop) {
		sigsuspend(&waitmask);
	}

	syslog(LOG_NOTICE, "terminating");

exit:
	/* Left blocked: a second SIGTERM here would skip the unlink and strand the regions. */
	unpublish(npublished, xals);
	free(watches);
	free(conf.devs);

	/* The log mask comes from the config, which may be what failed, so this path does not rely
	 * on syslog. */
	if (err) {
		fprintf(stderr, "xal-server: exiting; err(%d) %s\n", err, strerror(abs(err)));
	}

	closelog();

	return err ? EXIT_FAILURE : EXIT_SUCCESS;
}
