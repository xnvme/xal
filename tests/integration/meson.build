# Installed so the cijoe suite can run them from PATH, as with the tools/ test aids.
test_sources = [
  'test_inotify.c',
  # A reader of an xal-server index, for the cijoe test of re-indexing in XAL_WATCHMODE_NONE.
  'test_mark_dirty.c',
  'test_shm_version.c',
]

foreach src : test_sources
  name = src.split('.')[0]
  executable('xal_' + name, src,
    dependencies: xallib_deps,
    link_with: xal_library,
    include_directories: [include_dirs, include_directories('..')],
    install_rpath: xallib_rpath,
    install: true,
  )
endforeach
