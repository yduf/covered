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

fuse3_dep = dependency('fuse3', required: false)

scan_sources = files(
    'src/main.cpp',
    'src/db.cpp',
    'src/scanner.cpp'
)

covered_scan_size_exe = executable('covered_scan_size', scan_sources,
    dependencies : [sqlite3_dep, blake3_dep],
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

report_sources = files(
    'src/report_main.cpp',
    'src/db.cpp',
)

cover_report_exe = executable('cover_report', report_sources,
    dependencies : [sqlite3_dep],
    install : true
)

if fuse3_dep.found()
    fuse_sources = files(
        'src/fuse_main.cpp',
        'src/db.cpp',
    )

    cover_fuse_exe = executable('cover_fuse', fuse_sources,
        dependencies : [sqlite3_dep, fuse3_dep],
        install : true
    )

    # FUSE xattr test
    test_fuse_xattr_script = find_program('tests/test_fuse_xattr.sh')
    test('fuse_xattr', test_fuse_xattr_script,
        depends : [covered_scan_size_exe, covered_match_exe, cover_fuse_exe],
        workdir : meson.current_source_dir(),
        timeout : 30
    )
else
    warning('libfuse3-dev not found – cover_fuse will not be built. Install with: sudo apt install libfuse3-dev')
endif

# Nemo extension – installed to the system-wide Nemo Python extensions directory
install_data(
    'nemo-extension/nemo-covered.py',
    install_dir : '/usr/share/nemo-python/extensions',
    rename : 'nemo-covered.py',
)

test_match_script = find_program('tests/test_match.sh')
test_match_spaces_script = find_program('tests/test_match_spaces.sh')

test('match_end_to_end', test_match_script,
    depends : [covered_scan_size_exe, covered_match_exe],
    workdir : meson.current_source_dir(),
    timeout : 60
)

test('match_end_to_end_spaces', test_match_spaces_script,
    depends : [covered_scan_size_exe, covered_match_exe],
    workdir : meson.current_source_dir(),
    timeout : 60
)