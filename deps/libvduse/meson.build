# Vendored from QEMU v10.0.0, subprojects/libvduse.
# Upstream: https://gitlab.com/qemu-project/qemu/-/tree/v10.0.0/subprojects/libvduse
# Licensed GPL-2.0-or-later; see VENDOR.md for the list of local changes.

libvduse_inc = include_directories('.')

libvduse_lib = static_library(
  'vduse',
  'libvduse.c',
  include_directories : libvduse_inc,
  # Upstream code; don't fail our build on its warnings.
  c_args : ['-Wno-unused-parameter', '-Wno-sign-compare', '-Wno-calloc-transposed-args'],
  install : false,
)

libvduse_dep = declare_dependency(
  link_with : libvduse_lib,
  include_directories : libvduse_inc,
)
