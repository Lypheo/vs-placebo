project('vs-placebo', ['c', 'cpp'],
  default_options: ['buildtype=release', 'b_ndebug=if-release', 'c_std=c17', 'cpp_std=c++20'],
  meson_version: '>=1.4.0',
  version: '2.0.0'
)

cc = meson.get_compiler('c')
win32 = host_machine.system() == 'windows' or host_machine.system() == 'cygwin'

if win32
  # Use static versions of these libraries
  shaderc_combined = dependency('shaderc_combined')
  meson.override_dependency('shaderc', shaderc_combined)
  spirv_cross_c = dependency('spirv-cross-c')
  meson.override_dependency('spirv-cross-c-shared', spirv_cross_c)
endif

add_project_arguments(
   cc.get_supported_arguments(
    '-Werror-implicit-function-declaration',
    '-Werror=discarded-qualifiers',
    '-Werror=incompatible-pointer-types'
  ),
  language: 'c'
)

libplacebo = dependency('libplacebo', required: true, version: '>=5.229.0', static: win32)
libdovi = dependency('dovi', required: false, static: win32)

deps = [dependency('threads'), libplacebo, libdovi]

vs_compat = get_option('r73-compat')
if vs_compat
  vapoursynth_dep = dependency('vapoursynth', static: win32).partial_dependency(includes: true, compile_args: true)

  deps += vapoursynth_dep

  incdir = []
  install_dir = join_paths(vapoursynth_dep.get_variable(pkgconfig: 'libdir'), 'vapoursynth')
else
  py = import('python').find_installation(pure: false)

  incdir = include_directories(
    run_command(py, '-c', 'import vapoursynth as vs; print(vs.get_include())', check: true).stdout().strip(),
  )
  install_dir = py.get_install_dir() / 'vapoursynth/plugins'
endif

use_libdovi = libdovi.found()

config_vsplacebo = configuration_data()
config_vsplacebo.set('HAVE_DOVI', use_libdovi)

configure_file(
  output: 'config_vsplacebo.h',
  configuration: config_vsplacebo,
)

p2p = static_library('p2p', ['libp2p/p2p_api.cpp', 'libp2p/v210.cpp'], pic: true)

sources = []
subdir('src')

shared_module('vs_placebo', sources,
  dependencies: deps,
  link_with: [p2p],
  name_prefix: 'lib',
  include_directories: incdir,
  install: true,
  install_dir: install_dir,
)
