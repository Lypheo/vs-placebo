project('vs-placebo', ['c', 'cpp'],
  default_options: ['buildtype=release', 'b_ndebug=if-release', 'c_std=c11', 'cpp_std=c++11'],
  meson_version: '>=0.51.0',
  version: '1.4.4'
)

win32 = host_machine.system() == 'windows' or host_machine.system() == 'cygwin'

placebo = dependency('libplacebo', required: true, version: '>=6.269.0', static: win32)
vapoursynth_dep = dependency('vapoursynth', static: win32).partial_dependency(includes: true, compile_args: true)
dovi = dependency('dovi', required: false)

use_dovi = dovi.found()

config_vsplacebo = configuration_data()
config_vsplacebo.set('HAVE_DOVI', use_dovi)

configure_file(
  output: 'config_vsplacebo.h',
  configuration: config_vsplacebo,
)

p2p = static_library('p2p', ['libp2p/p2p_api.cpp', 'libp2p/v210.cpp'], pic: true)

sources = []

subdir('src')

shared_module('vs_placebo', sources,
  dependencies: [placebo, vapoursynth_dep, dovi],
  link_with: [p2p],
  name_prefix: 'lib',
  install_dir : join_paths(vapoursynth_dep.get_variable(pkgconfig: 'libdir'), 'vapoursynth'),
  install: true
)
