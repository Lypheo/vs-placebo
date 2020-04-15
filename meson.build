project('vs-placebo', ['c', 'cpp'],
    default_options: ['buildtype=release', 'b_ndebug=if-release', 'c_std=c99', 'cpp_std=c++11'],
    meson_version: '>=0.46.0',
    version: '1.0'
)

cc = meson.get_compiler('c')

placebo = cc.find_library('placebo', required: true)

sources = ['vs-placebo.c', 'deband.c', 'tonemap.c', 'resample.c']

p2p = static_library('p2p', ['libp2p/p2p_api.cpp', 'libp2p/v210.cpp'], pic: true)

shared_module('vs_placebo', sources, dependencies: [placebo], link_with: [p2p], name_prefix: 'lib')