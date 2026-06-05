project('xal', 'c',
  version: '0.2.0',
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

conf_data = configuration_data()
conf_data.set('XAL_DEBUG_ENABLED', get_option('buildtype') == 'debug' and get_option('debug-logging'))
conf = configure_file(
  configuration : conf_data,
  output : 'xal_config.h',
)

xallib_libdir = get_option('libdir')
xallib_rpath = get_option('prefix') + '/' + xallib_libdir

add_project_arguments(['-include', 'xal_config.h'], language: 'c')

rt_dep = cc.find_library('rt', required: true)
if not cc.has_function('clock_gettime', prefix: '#include <time.h>', dependencies: rt_dep)
  error('clock_gettime not found')
endif

xnvme_dep = dependency(
  'xnvme',
  version: '>=0.7.0',
  required: true
)

# BPF: build dependencies
libbpf_dep = dependency('libbpf', required: true)
libelf_dep = dependency('libelf', required: true)
zlib_dep = dependency('zlib', required: true)

bpftool = find_program('bpftool', required: true)
clang = find_program('clang', required: true)

# BPF: BTF source file
vmlinux_btf = '/sys/kernel/btf/vmlinux'

# BPF: generate vmlinux.h
vmlinux_h = custom_target(
  'xal-vmlinux-h',
  output : 'vmlinux.h',
  command : [bpftool, 'btf', 'dump', 'file', vmlinux_btf, 'format', 'c'],
  capture : true
)

# BPF: map host cpu_family() to the __TARGET_ARCH_* macro libbpf expects
bpf_arch_map = {
  'x86_64'  : 'x86',
  'x86'     : 'x86',
  'aarch64' : 'arm64',
  'arm'     : 'arm',
  'ppc64'   : 'powerpc',
  'riscv64' : 'riscv',
  's390x'   : 's390',
  'mips64'  : 'mips',
  'mips'    : 'mips',
}
bpf_cpu = host_machine.cpu_family()
if not bpf_arch_map.has_key(bpf_cpu)
  error('Unsupported BPF target arch: ' + bpf_cpu)
endif

# BPF: compile flags
bpf_cflags = [
  '-O2', '-g',
  '-target', 'bpf',
  '-D__TARGET_ARCH_' + bpf_arch_map[bpf_cpu],
  '-I' + meson.current_build_dir(),
  '-I' + meson.current_source_dir() / 'src',
  '-I' + meson.current_source_dir() / 'include',
]

# BPF: build .bpf.o
bpf_obj = custom_target(
  'xal-bpf-events-obj',
  input : ['src/xal_bpf_events.bpf.c', vmlinux_h],
  output : 'xal_bpf_events.bpf.o',
  command : [
    clang,
	bpf_cflags,
	'-c', '@INPUT0@',
	'-o', '@OUTPUT@'
  ]
)

# BPF: generate skeleton
# 'name' overrides the struct/prefix bpftool would otherwise derive from the
# object basename ("xal_bpf_events_bpf"). Keep this in sync with the struct
# name used in src/xal_bpf.c and include/xal_bpf.h.
skel = custom_target(
  'xal-bpf-events-skel',
  input : bpf_obj,
  output : 'xal_bpf_events.skel.h',
  command : [bpftool, 'gen', 'skeleton', '@INPUT@', 'name', 'xal_bpf_events'],
  capture : true
)

xallib_deps = [
  rt_dep,
  xnvme_dep,
  libbpf_dep,
  libelf_dep,
  zlib_dep,
]

public_headers = files(
  'include/libxal.h',
  'include/libxal_util.h',
)

sources = files(
  'src/xal.c',
  'src/xal_be_fiemap.c',
  'src/xal_be_fiemap_inotify.c',
  'src/xal_bpf.c',
  'src/xal_be_xfs.c',
  'src/xal_pool.c',
  'src/pp.c',
  'src/utils.c'
)

include_dirs = include_directories('include', 'tp')

xal_library = library('xal',
  sources: [skel, sources],
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
