# Installed so the cijoe suite can run them from PATH, as with the tools/ test aids.
test_sources = [
  'test_inotify.c',
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
