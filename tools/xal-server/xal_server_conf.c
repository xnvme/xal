#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <tomlc17.h>

#include <libxal.h>

#include <xal_server_conf.h>

/**
 * Copy a string datum into a fixed-size field, refusing anything that would not fit
 *
 * Truncating would silently yield a different path or name than the operator wrote.
 */
static int
copy_str(const char *key, unsigned int idx, toml_datum_t datum, char *dst, size_t dst_nbytes)
{
	size_t len;

	if (datum.type != TOML_STRING) {
		syslog(LOG_ERR, "device %u: missing or invalid '%s'", idx, key);
		return -EINVAL;
	}

	/* An escaped NUL is a well-formed TOML string, so u.s can stop short of u.str.len. Copying
	 * by strlen() would then publish a silently shorter name than the operator wrote, which is
	 * the truncation this refuses to do. */
	len = strlen(datum.u.s);
	if (len != (size_t)datum.u.str.len) {
		syslog(LOG_ERR, "device %u: '%s' contains an embedded NUL", idx, key);
		return -EINVAL;
	}

	if (len >= dst_nbytes) {
		syslog(LOG_ERR, "device %u: '%s' is %zu bytes, at most %zu fit", idx, key, len,
		       dst_nbytes - 1);
		return -ENAMETOOLONG;
	}

	memcpy(dst, datum.u.s, len + 1);

	return 0;
}

static int
parse_devices(toml_datum_t devices, struct xal_server_conf *conf)
{
	int err;

	if (devices.type != TOML_ARRAY) {
		syslog(LOG_ERR, "missing or invalid 'devices'");
		return -EINVAL;
	}

	if (devices.u.arr.size < 1) {
		syslog(LOG_ERR, "'devices' is empty, nothing to publish");
		return -EINVAL;
	}

	conf->devs = calloc(devices.u.arr.size, sizeof(*conf->devs));
	if (!conf->devs) {
		err = -errno;
		syslog(LOG_CRIT, "FAILED: calloc(); err(%d)", err);
		return err;
	}
	conf->ndevs = devices.u.arr.size;

	for (unsigned int i = 0; i < conf->ndevs; i++) {
		struct xal_server_dev *dev = &conf->devs[i];
		toml_datum_t elem = devices.u.arr.elem[i];
		toml_datum_t datum;

		if (elem.type != TOML_TABLE) {
			syslog(LOG_ERR, "device %u: expected a table with 'uri' and 'shm_name'", i);
			return -EINVAL;
		}

		err = copy_str("uri", i, toml_get(elem, "uri"), dev->uri, sizeof(dev->uri));
		if (err) {
			return err;
		}

		/* Readers address devices by this name, so it is spelled out rather than derived
		 * from a list position a later edit could shift. */
		err = copy_str("shm_name", i, toml_get(elem, "shm_name"), dev->shm_name,
			       sizeof(dev->shm_name));
		if (err) {
			return err;
		}

		datum = toml_get(elem, "mountpoint");
		if (datum.type != TOML_UNKNOWN) {
			err = copy_str("mountpoint", i, datum, dev->mountpoint,
				       sizeof(dev->mountpoint));
			if (err) {
				return err;
			}
		}

		datum = toml_get(elem, "subtree");
		if (datum.type != TOML_UNKNOWN) {
			err = copy_str("subtree", i, datum, dev->subtree, sizeof(dev->subtree));
			if (err) {
				return err;
			}
		}

		/* Left to shm_open() a malformed name surfaces as a bare errno, with nothing tying
		 * it back to this entry. */
		if (strchr(dev->shm_name + 1, '/')) {
			syslog(LOG_ERR,
			       "device %u: 'shm_name' (%s) must not have internal '/'",
			       i, dev->shm_name);
			return -EINVAL;
		}

		if (!dev->shm_name[1]) {
			syslog(LOG_ERR, "device %u: 'shm_name' is just '/'", i);
			return -EINVAL;
		}

		for (unsigned int j = 0; j < i; j++) {
			if (strcmp(conf->devs[j].shm_name, dev->shm_name)) {
				continue;
			}

			/* A duplicate would leave one device unpublished with no obvious symptom.
			 */
			syslog(LOG_ERR, "devices %u and %u share shm_name(%s)", j, i,
			       dev->shm_name);
			return -EINVAL;
		}
	}

	return 0;
}

int
xal_server_conf_from_toml(const char *path, struct xal_server_conf *conf)
{
	/* Config verbosity, least to most; syslog's own numbering is not contiguous. */
	static const int log_levels[] = {LOG_ERR, LOG_WARNING, LOG_NOTICE, LOG_INFO, LOG_DEBUG};
	toml_result_t result;
	toml_datum_t datum;
	int err;

	conf->log_level = LOG_NOTICE;
	conf->watch_mode = XAL_WATCHMODE_NONE;
	conf->devs = NULL;
	conf->ndevs = 0;

	result = toml_parse_file_ex(path);
	if (!result.ok) {
		syslog(LOG_CRIT, "FAILED: could not parse config(%s): %s", path, result.errmsg);
		toml_free(result);
		return -EINVAL;
	}

	datum = toml_seek(result.toptab, "log_level");
	if (datum.type != TOML_INT64) {
		syslog(LOG_WARNING, "missing or invalid 'log_level', defaulting to notice");
	} else if ((datum.u.int64 < 0) ||
		   (datum.u.int64 >= (int64_t)(sizeof(log_levels) / sizeof(*log_levels)))) {
		syslog(LOG_WARNING, "'log_level' out of range, defaulting to notice");
	} else {
		conf->log_level = log_levels[datum.u.int64];
	}

	datum = toml_seek(result.toptab, "xal.watchmode");
	if (datum.type != TOML_INT64) {
		syslog(LOG_WARNING, "missing or invalid 'xal.watchmode', defaulting to none");
	} else if ((datum.u.int64 < XAL_WATCHMODE_NONE) ||
		   (datum.u.int64 > XAL_WATCHMODE_REFLINK_SNAPSHOT)) {
		syslog(LOG_WARNING, "'xal.watchmode' out of range, defaulting to none");
	} else {
		conf->watch_mode = datum.u.int64;
	}

	err = parse_devices(toml_seek(result.toptab, "devices"), conf);
	if (err) {
		free(conf->devs);
		conf->devs = NULL;
		conf->ndevs = 0;
	}

	toml_free(result);

	return err;
}
