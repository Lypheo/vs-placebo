project('vs-placebo', ['c', 'cpp'],
  default_options: ['buildtype=release', 'b_ndebug=if-release', 'c_std=c17', 'cpp_std=c++20'],
  meson_version: '>=1.4.0',
  version: '2.0.0'
)

cc = meson.get_compiler('c')
win32 = host_machine.system() == 'windows' or host_machine.system() == 'cygwin'
static_deps = win32 or (get_option('default_library') == 'static' and get_option('prefer_static'))

if static_deps
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

libplacebo_default_options = []
if static_deps
  libplacebo_default_options += 'default_library=static'
endif

libplacebo = dependency('libplacebo', version: '>=5.229.0', static: static_deps,
  fallback : ['libplacebo', 'libplacebo'],
  default_options: libplacebo_default_options)

libdovi = dependency('dovi', required: false, static: static_deps)

deps = [dependency('threads'), libplacebo, libdovi]

vs_compat = get_option('r73-compat')
if vs_compat
  vapoursynth_dep = dependency('vapoursynth', static: static_deps).partial_dependency(includes: true, compile_args: true)

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

libp2p = subproject('libp2p')
p2p_static = libp2p.get_variable('libp2p')
p2p_inc_dirs = libp2p.get_variable('p2p_inc_dirs')

link_with_list = [p2p_static]

if libp2p.get_variable('simd_enabled')
  link_with_list += libp2p.get_variable('libp2p_simd')
endif

sources = []
subdir('src')

shared_module('vs_placebo', sources,
  dependencies: deps,
  link_with: link_with_list,
  name_prefix: 'lib',
  include_directories: [incdir, p2p_inc_dirs],
  install: true,
  install_dir: install_dir,
)
