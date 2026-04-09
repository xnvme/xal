struct xal_be_fiemap {
	struct xal_backend_base base;
	char *mountpoint;      ///< Path to mountpoint of dev
	struct xal_inotify *inotify;
	struct xal_bpf *bpf;
	void *path_inode_map;  ///< Map of paths to inodes

	uint8_t _rsvd[8];
};
XAL_STATIC_ASSERT(sizeof(struct xal_be_fiemap) == XAL_BACKEND_SIZE, "Incorrect size");

void
xal_be_fiemap_close(struct xal *xal);

int
xal_be_fiemap_open(struct xal **xal, char *mountpoint, struct xal_opts *opts);

int
xal_be_fiemap_get_inode(struct xal *xal, char *path, struct xal_inode **inode);
