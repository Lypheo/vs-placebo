project('vs-placebo', ['c', 'cpp'],
  default_options: ['buildtype=release', 'b_ndebug=if-release', 'c_std=c11', 'cpp_std=c++11'],
  meson_version: '>=0.51.0',
  version: '1.4.5'
)

win32 = host_machine.system() == 'windows' or host_machine.system() == 'cygwin'

vapoursynth_dep = dependency('vapoursynth', static: win32).partial_dependency(includes: true, compile_args: true)
dovi = dependency('dovi', required: false, static: win32)

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
  dependencies: [vapoursynth_dep, dovi],
  link_with: [p2p],
  name_prefix: 'lib',
  include_directories: [
    'libplacebo/install/include'
  ],
  c_args: [
    # If this isn't defined then libplacebo headers get preprocessed for dynamic linking
    '-DPL_STATIC'
  ],
  link_args: [
    '-static',
    # It's hard to get static links to libplacebo cleanly so just dumping link args here
    meson.current_source_dir() / 'libplacebo/install/lib/libplacebo.a',
    '-lm',
    '-pthread',
    '-lversion',
    '-lvulkan-1.dll',
    '-llcms2',
    '-llcms2_fast_float',
    '-lshlwapi',
    '-lMachineIndependent',
    '-lOSDependent',
    '-lGenericCodeGen',
    '-lOGLCompiler',
    '-lglslang',
    '-lSPIRV',
    '-lSPIRV-Tools-opt',
    '-lSPIRV-Tools',
    '-lSPIRV-Tools-link',
    '-lspirv-cross-glsl',
    '-lshaderc_combined',
    '-lspirv-cross-c'
  ],
  install_dir : join_paths(vapoursynth_dep.get_variable(pkgconfig: 'libdir'), 'vapoursynth'),
  install: true
)
