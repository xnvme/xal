xal_bmap_yaml_exe = executable(
  'xal_bmap_mp_yaml',
  'xal_bmap_mp_yaml.c',
  install: true
)

# Links libxal to drive a live reflink snapshot (used by the reflink cijoe tests).
xal_snapshot_hold_exe = executable(
  'xal_snapshot_hold',
  'xal_snapshot_hold.c',
  dependencies: xallib_deps,
  link_with: xal_library,
  include_directories: include_dirs,
  install_rpath: xallib_rpath,
  install: true,
)

# Links libxal to hold a device with the XFS backend (used by the backend cijoe tests).
xal_xfs_hold_exe = executable(
  'xal_xfs_hold',
  'xal_xfs_hold.c',
  dependencies: xallib_deps,
  link_with: xal_library,
  include_directories: include_dirs,
  install_rpath: xallib_rpath,
  install: true,
)

# Installed so the cijoe suite can run it from PATH, as with the tools/ test aids.
xal_test_shm_version_exe = executable(
  'xal_test_shm_version',
  'test_shm_version.c',
  dependencies: xallib_deps,
  link_with: xal_library,
  include_directories: include_dirs,
  install_rpath: xallib_rpath,
  install: true,
)

# A reader of an xal-server index, for the cijoe test of re-indexing in XAL_WATCHMODE_NONE.
xal_test_mark_dirty_exe = executable(
  'xal_test_mark_dirty',
  'test_mark_dirty.c',
  dependencies: xallib_deps,
  link_with: xal_library,
  include_directories: include_dirs,
  install_rpath: xallib_rpath,
  install: true,
)

subdir('xal-server')
