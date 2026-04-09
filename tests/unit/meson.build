test_sources = [
  'test_pool.c',
  'test_xal.c',
]

foreach src : test_sources
  name = src.split('.')[0]
  exe = executable(name, src,
    dependencies: xallib_deps,
    link_with: xal_library,
    include_directories: [include_dirs, include_directories('..')],
  )
  test(name, exe, suite: 'unit')
endforeach
