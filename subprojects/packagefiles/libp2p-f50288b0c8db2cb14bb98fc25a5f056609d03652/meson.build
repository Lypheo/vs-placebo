project('libp2p', 'cpp',
    default_options: ['buildtype=release', 'default_library=static'],
    version: '1.0.0'
)

p2p_sources = ['p2p_api.cpp', 'v210.cpp']
p2p_inc_dirs = include_directories('.')

simd_enabled = get_option('simd') and host_machine.cpu_family().contains('x86')

if simd_enabled
    add_project_arguments('-DP2P_SIMD', language: 'cpp')

    p2p_simd_sources = p2p_sources
    p2p_simd_sources += ['simd/p2p_simd.cpp', 'simd/p2p_sse41.cpp', 'simd/cpuinfo_x86.cpp']

    p2p_simd_inc_dirs = include_directories('simd')
endif

libp2p = static_library('p2p', p2p_sources, pic: true, include_directories: p2p_inc_dirs)

if simd_enabled
    libp2p_simd = static_library('p2p_simd', p2p_simd_sources, pic: true,
        include_directories: [p2p_inc_dirs, p2p_simd_inc_dirs],
        cpp_args: ['-msse4.1'])
endif
