project('blake3', 'c',
    version : '1.8.5',
    default_options : ['c_std=c99']
)

host_cpu = host_machine.cpu_family()

blake3_sources = [
    'c/blake3.c',
    'c/blake3_dispatch.c',
    'c/blake3_portable.c',
]

if host_cpu == 'x86_64'
    blake3_sources += [
        'c/blake3_avx2_x86-64_unix.S',
        'c/blake3_avx512_x86-64_unix.S',
        'c/blake3_sse2_x86-64_unix.S',
        'c/blake3_sse41_x86-64_unix.S',
    ]
else
    blake3_sources += []
    add_project_arguments('-DBLAKE3_NO_SSE2', language : 'c')
    add_project_arguments('-DBLAKE3_NO_SSE41', language : 'c')
    add_project_arguments('-DBLAKE3_NO_AVX2', language : 'c')
    add_project_arguments('-DBLAKE3_NO_AVX512', language : 'c')
endif

blake3_inc = include_directories('c')

blake3_lib = static_library('blake3',
    blake3_sources,
    include_directories : blake3_inc,
    install : false
)

blake3_dep = declare_dependency(
    link_with : blake3_lib,
    include_directories : blake3_inc
)
