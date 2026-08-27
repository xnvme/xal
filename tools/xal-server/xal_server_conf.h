#ifndef XAL_SERVER_CONF_H
#define XAL_SERVER_CONF_H

#include <libxal.h>

#define XAL_SERVER_URI_MAXLEN 256
#define XAL_SERVER_SHM_NAME_MAXLEN 64

struct xal_server_dev {
	char uri[XAL_SERVER_URI_MAXLEN];
	char shm_name[XAL_SERVER_SHM_NAME_MAXLEN];
	char mountpoint[XAL_PATH_MAXLEN + 1];
	char subtree[XAL_PATH_MAXLEN + 1];
};

struct xal_server_conf {
	int log_level;
	enum xal_watchmode watch_mode;
	unsigned int ndevs;
	struct xal_server_dev *devs;
};

/**
 * Parse the TOML configuration file
 *
 * Expects log_level (int), xal.watchmode (int), and devices, an array of tables with 'uri'
 * and 'shm_name' required, 'mountpoint' and 'subtree' optional. A missing file is an error;
 * with no device list there is nothing to publish. Problems are reported to syslog.
 *
 * @param path Path to the configuration file
 * @param conf Configuration to load into; on success the caller frees conf->devs
 *
 * @return On success, 0 is returned. On error, negative errno is returned to indicate the error.
 */
int
xal_server_conf_from_toml(const char *path, struct xal_server_conf *conf);

#endif /* XAL_SERVER_CONF_H */
