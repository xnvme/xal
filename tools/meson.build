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

subdir('xal-server')
