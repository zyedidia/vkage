project(
  'vkage',
  'c',
  version : '0.1.0',
  license : 'GPL-2.0-or-later',
  meson_version : '>= 1.0.0',
  default_options : [
    'c_std=gnu17',
    'warning_level=2',
    'buildtype=debugoptimized',
    # libvduse is built as a plain static_library. Switching this to shared
    # needs `pic : true` on that target in deps/libvduse/meson.build.
    'default_library=static',
  ],
)

subdir('deps/libvduse')

vkage_inc = include_directories('include')

vkage_lib = library(
  'vkage',
  files(
    'src/backend_file.c',
    'src/blk.c',
    'src/bridge.c',
    'src/bus.c',
    'src/error.c',
    'src/loop.c',
  ),
  include_directories : [vkage_inc, include_directories('src')],
  dependencies : libvduse_dep,
  install : true,
)

vkage_dep = declare_dependency(
  link_with : vkage_lib,
  include_directories : vkage_inc,
)

install_headers(
  'include/vkage/blk.h',
  'include/vkage/bridge.h',
  'include/vkage/error.h',
  'include/vkage/loop.h',
  subdir : 'vkage',
)

executable(
  'vkage-file-blk',
  'examples/vkage-file-blk.c',
  dependencies : vkage_dep,
)

executable(
  'vkage-uml',
  'examples/vkage-uml.c',
  dependencies : vkage_dep,
)
