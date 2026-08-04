project(
  'vduse-blk',
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

vduse_blk_inc = include_directories('include')

vduse_blk_lib = library(
  'vduse-blk',
  files(
    'src/backend_file.c',
    'src/blk.c',
    'src/bus.c',
    'src/error.c',
    'src/loop.c',
  ),
  include_directories : [vduse_blk_inc, include_directories('src')],
  dependencies : libvduse_dep,
  install : true,
)

vduse_blk_dep = declare_dependency(
  link_with : vduse_blk_lib,
  include_directories : vduse_blk_inc,
)

install_headers(
  'include/vduse/blk.h',
  'include/vduse/error.h',
  'include/vduse/loop.h',
  subdir : 'vduse',
)

executable(
  'vduse-file-blk',
  'examples/vduse-file-blk.c',
  dependencies : vduse_blk_dep,
)
