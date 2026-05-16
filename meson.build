project('covered', 'cpp',
    version : '0.1.0',
    default_options : ['cpp_std=c++17', 'warning_level=3']
)

sqlite3_dep = dependency('sqlite3')
blake3_dep = dependency('blake3', required: false)
if not blake3_dep.found()
    blake3_proj = subproject('blake3')
    blake3_dep = blake3_proj.get_variable('blake3_dep')
endif

scan_sources = files(
    'src/main.cpp',
    'src/db.cpp',
    'src/scanner.cpp'
)

covered_scan_size_exe = executable('covered_scan_size', scan_sources,
    dependencies : [sqlite3_dep],
    install : true
)

match_sources = files(
    'src/match_main.cpp',
    'src/matcher.cpp',
    'src/db.cpp',
)

covered_match_exe = executable('covered_match', match_sources,
    dependencies : [sqlite3_dep, blake3_dep],
    install : true
)

test_script = find_program('test_match.sh')

test('match_end_to_end', test_script,
    depends : [covered_scan_size_exe, covered_match_exe],
    workdir : meson.current_source_dir(),
    timeout : 60
)
