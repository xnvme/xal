project('xal', 'c',
  version: '0.3.3',
  meson_version: '>=0.55.0',
  default_options: [
    'c_std=gnu11',
    'warning_level=2',
    'buildtype=release',
    'default_library=both',
    'b_staticpic=true'
  ],
)

pkg = import('pkgconfig')
cc = meson.get_compiler('c')

xallib_libdir = get_option('libdir')
xallib_rpath = get_option('prefix') + '/' + xallib_libdir

rt_dep = cc.find_library('rt', required: true)
if not cc.has_function('clock_gettime', prefix: '#include <time.h>', dependencies: rt_dep)
  error('clock_gettime not found')
endif

xnvme_dep = dependency(
  'xnvme',
  version: '>=0.7.0',
  required: true
)

xallib_deps = [
  rt_dep,
  xnvme_dep,
]

public_headers = files(
  'include/libxal.h',
  'include/libxal_util.h',
)

sources = files(
  'src/xal.c',
  'src/xal_be_fiemap.c',
  'src/xal_be_fiemap_inotify.c',
  'src/xal_be_xfs.c',
  'src/xal_pool.c',
  'src/pp.c',
  'src/utils.c'
)

xal_sources = [sources]

include_dirs = include_directories('include', 'tp')

conf_data = configuration_data()
conf_data.set('XAL_DEBUG_ENABLED', get_option('buildtype') == 'debug' and get_option('debug-logging'))

conf = configure_file(
  configuration : conf_data,
  output : 'xal_config.h',
)

add_project_arguments(['-include', 'xal_config.h'], language: 'c')

xal_library = library('xal',
  sources: xal_sources,
  dependencies: xallib_deps,
  include_directories: include_dirs,
  install: true,
)

xal_cli = executable('xal',
  sources: 'src/cli.c',
  dependencies: xallib_deps,
  link_with: xal_library,
  include_directories: include_dirs,
  install_rpath: xallib_rpath,
  install: true
)

subdir('tools')

pkg.generate(xal_library)

install_headers(public_headers)

