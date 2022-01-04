project('vs-placebo', ['c', 'cpp'],
    default_options: ['buildtype=release', 'b_ndebug=if-release', 'c_std=c99', 'cpp_std=c++11'],
    meson_version: '>=0.46.0',
    version: '1.2'
)

placebo = dependency('libplacebo', required: true)
vapoursynth_dep = dependency('vapoursynth').partial_dependency(includes: true, compile_args: true)
dovi = dependency('dovi', required: false)

config_vsplacebo = configuration_data()
config_vsplacebo.set('HAVE_DOVI', dovi.found())

configure_file(
  output: 'config_vsplacebo.h',
  configuration: config_vsplacebo,
)

sources = ['vs-placebo.c', 'deband.c', 'tonemap.c', 'resample.c', 'shader.c']

p2p = static_library('p2p', ['libp2p/p2p_api.cpp', 'libp2p/v210.cpp'], pic: true)

shared_module('vs_placebo', sources,
    dependencies: [placebo, vapoursynth_dep, dovi],
    link_with: [p2p],
    name_prefix: 'lib',
    install_dir : join_paths(vapoursynth_dep.get_pkgconfig_variable('libdir'), 'vapoursynth'),
    install: true
)
