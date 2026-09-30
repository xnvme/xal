/**
 * Command-line interface goes here
 */
#define _GNU_SOURCE
#include <assert.h>
#include <endian.h>
#include <errno.h>
#include <fcntl.h>
#include <libxal.h>
#include <libxnvme.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define BUF_NBYTES 4096

struct xal_cli_args {
	bool bmap;
	bool find;
	bool meta;
	bool stats;
	bool file_lookup_map;
	char *backend;
	char *watch_mode;
	char *dev_uri;
	char *filename;
};

struct xal_nodeinspector_args {
	uint64_t ndirs;
	uint64_t nfiles;
	struct xal_cli_args *cli_args;
};

int
parse_args(int argc, char *argv[], struct xal_cli_args *args)
{
	if (argc < 2) {
		fprintf(stderr, "Usage: %s [-b | --verbose] <dev_uri>\n", argv[0]);
		return -EINVAL;
	}

	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--bmap") == 0) {
			args->bmap = 1;
		} else if (strcmp(argv[i], "--find") == 0) {
			args->find = 1;
		} else if (strcmp(argv[i], "--meta") == 0) {
			args->meta = 1;
		} else if (strcmp(argv[i], "--stats") == 0) {
			args->stats = 1;
		} else if (strcmp(argv[i], "--file_lookup_map") == 0) {
			args->file_lookup_map = 1;
		} else if (strcmp(argv[i], "--backend") == 0) {
			if (i+1 >= argc) {
				fprintf(stderr, "Error: Backend argument must define a valid backend (choices: xfs, fiemap)\n");
				return -EINVAL;
			}
			args->backend = argv[++i];
		} else if (strcmp(argv[i], "--watch-mode") == 0) {
			if (i+1 >= argc) {
				fprintf(stderr, "Error: Watch-mode argument must define a valid mode (choices: none, dirty, extent)\n");
				return -EINVAL;
			}
			args->watch_mode = argv[++i];
		} else if (strcmp(argv[i], "--filename") == 0) {
			if (i+1 >= argc) {
				fprintf(stderr, "Error: Filename argument must define a valid path: --filename <filename>\n");
				return -EINVAL;
			}
			args->filename = argv[++i];
		} else if (args->dev_uri == NULL) {
			args->dev_uri = argv[i];
		} else {
			fprintf(stderr, "Unexpected argument: %s\n", argv[i]);
			return -EINVAL;
		}
	}

	if (args->dev_uri == NULL) {
		fprintf(stderr, "Error: Device uri is required\n");
		return -EINVAL;
	}

	return 0;
}

/**
 * Produces output on stdout similar to the output produced by running "find /mount/point"
 */
int
node_inspector_find(struct xal *xal, struct xal_inode *inode, void *cb_args,
		    int XAL_UNUSED(level))
{
	struct xal_nodeinspector_args *args = cb_args;

	if (xal_inode_is_dir(inode)) {
		args->ndirs += 1;
	} else if (xal_inode_is_file(inode)) {
		args->nfiles += 1;
	} else {
		printf("# UNKNOWN(%.*s)", inode->namelen, inode->name);
		return 0;
	}

	printf("%s", args->cli_args->dev_uri);
	if ((inode->parent_idx != XAL_POOL_IDX_NONE) &&
	    (args->cli_args->dev_uri[strlen(args->cli_args->dev_uri) - 1] == '/')) {
		printf("/");
	}
	xal_inode_path_pp(xal, inode);
	printf("\n");
	return 0;
}

static int
pp_inode_extents(struct xal *xal, struct xal_inode *inode)
{
	uint32_t blocksize = xal_get_sb_blocksize(xal);

	for (uint32_t i = 0; i < inode->content.extents.count; ++i) {
		struct xal_extent *extent = xal_extent_at(xal, inode->content.extents.extent_idx + i);
		size_t fofz_begin, fofz_end, bofz_begin, bofz_end;

		fofz_begin = (extent->start_offset * blocksize) / 512;
		fofz_end = fofz_begin + (extent->nblocks * blocksize) / 512 - 1;
		bofz_begin = xal_fsbno_offset(xal, extent->start_block) / 512;
		bofz_end = bofz_begin + (extent->nblocks * blocksize) / 512 - 1;

		printf("- [%" PRIu64 ", %" PRIu64 ", %" PRIu64 ", %" PRIu64 "]\n", fofz_begin,
		       fofz_end, bofz_begin, bofz_end);
	}

	return 0;
}

int
node_inspector_bmap(struct xal *xal, struct xal_inode *inode, void *cb_args, int XAL_UNUSED(level))
{
	struct xal_nodeinspector_args *args = cb_args;

	if (xal_inode_is_dir(inode)) {
		args->ndirs += 1;
		return 0;
	} else if (xal_inode_is_file(inode)) {
		args->nfiles += 1;
	} else {
		printf("# UNKNOWN(%.*s)", inode->namelen, inode->name);
		return 0;
	}

	printf("'%s", args->cli_args->dev_uri);
	if ((inode->parent_idx != XAL_POOL_IDX_NONE) &&
	    (args->cli_args->dev_uri[strlen(args->cli_args->dev_uri) - 1] == '/')) {
		printf("/");
	}

	xal_inode_path_pp(xal, inode);
	printf("':");

	if (!inode->content.extents.count) {
		printf(" ~\n");
		return 0;
	}

	printf("\n");

	return pp_inode_extents(xal, inode);
}

int
main(int argc, char *argv[])
{
	struct xal_cli_args args = {0};
	struct xnvme_opts xnvme_opts = {0};
	struct xal_opts opts = {0};
	struct xal_nodeinspector_args cb_args;
	struct xnvme_dev *dev = NULL;
	struct xal *xal = NULL;
	int err;

	err = parse_args(argc, argv, &args);
	if (err) {
		return err;
	}

	xnvme_opts_set_defaults(&xnvme_opts);

	dev = xnvme_dev_open(args.dev_uri, &xnvme_opts);
	if (!dev) {
		err = -errno;
		fprintf(stderr, "xnvme_dev_open(...); err(%d)\n", errno);
		return err;
	}

	if (args.backend) {
		if (strcmp(args.backend, "xfs") == 0) {
			opts.be = XAL_BACKEND_XFS;
		} else if (strcmp(args.backend, "fiemap") == 0) {
			opts.be = XAL_BACKEND_FIEMAP;
		} else {
			fprintf(stderr, "Invalid backend: %s; Valid choices: xfs, fiemap\n",
				args.backend);
			err = -EINVAL;
			goto exit;
		}
	} else if (args.filename) {
		opts.be = XAL_BACKEND_FIEMAP;
	}

	if (args.watch_mode) {
		if (strcmp(args.watch_mode, "none") == 0) {
			opts.watch_mode = XAL_WATCHMODE_NONE;
		} else if (strcmp(args.watch_mode, "dirty") == 0) {
			opts.watch_mode = XAL_WATCHMODE_DIRTY_DETECTION;
		} else if (strcmp(args.watch_mode, "extent") == 0) {
			opts.watch_mode = XAL_WATCHMODE_EXTENT_UPDATE;
		} else {
			fprintf(stderr,
				"Invalid watch-mode: %s; Valid choices: none, dirty, extent\n",
				args.watch_mode);
			err = -EINVAL;
			goto exit;
		}
	}

	if (args.file_lookup_map) {
		opts.file_lookupmode = XAL_FILE_LOOKUPMODE_HASHMAP;
	}

	err = xal_open(dev, &xal, &opts);
	if (err < 0) {
		fprintf(stderr, "xal_open(...); err(%d)\n", err);
		goto exit;
	}

	if (args.meta) {
		xal_pp(xal);
	}

	err = xal_dinodes_retrieve(xal);
	if (err) {
		fprintf(stderr, "xal_dinodes_retrieve(...); err(%d)\n", err);
		goto exit;
	}

	err = xal_index(xal);
	if (err) {
		fprintf(stderr, "xal_index(...); err(%d)\n", err);
		goto exit;
	}

	if (args.bmap) {
		struct xal_inode *root = xal_get_root(xal);

		memset(&cb_args, 0, sizeof(cb_args));
		cb_args.cli_args = &args;

		err = xal_walk(xal, root, node_inspector_bmap, &cb_args);
		if (err) {
			fprintf(stderr, "xal_walk(.. node_visistor_bmap ..); err(%d)\n", err);
			goto exit;
		}
	}

	if (args.find) {
		struct xal_inode *root = xal_get_root(xal);

		memset(&cb_args, 0, sizeof(cb_args));
		cb_args.cli_args = &args;

		err = xal_walk(xal, root, node_inspector_find, &cb_args);
		if (err) {
			fprintf(stderr, "xal_walk(.. node_visistor_find ..); err(%d)\n", err);
			goto exit;
		}
	}

	if (args.filename != NULL) {
		struct xal_inode *inode;

		err = xal_get_inode(xal, args.filename, &inode);
		if (err) {
			fprintf(stderr, "FAILED: xal_get_inode(); err(%d)\n", err);
			goto exit;
		}

		printf("'%s':\n", args.filename);
		pp_inode_extents(xal, inode);
	}

	if (args.stats) {
		printf("ndirs(%" PRIu64 "); nfiles(%" PRIu64 ")\n", cb_args.ndirs, cb_args.nfiles);
	}

exit:
	xal_close(xal);
	xnvme_dev_close(dev);

	return err;
}

