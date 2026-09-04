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

subdir('xal-server')
