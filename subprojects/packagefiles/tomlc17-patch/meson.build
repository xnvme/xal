project('tomlc17', 'c',
  version: 'R260131',
  license: 'MIT',
)

toml_lib = static_library('toml',
  'src/tomlc17.c',
  include_directories: include_directories('src'),
)

toml_dep = declare_dependency(
  include_directories: include_directories('src'),
  link_with: toml_lib,
)

# MIT asks for the notice to travel with what it is linked into, and xal-server
# links this statically.
install_data('LICENSE',
  install_dir: get_option('datadir') / 'licenses' / 'tomlc17',
)
