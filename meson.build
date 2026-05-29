project('covered', 'cpp',
    version : '0.1.0',
    default_options : ['cpp_std=c++17', 'warning_level=3']
)

nlohmann_json_dep = dependency('nlohmann_json', required: false)
if not nlohmann_json_dep.found()
    nlohmann_json_proj = subproject('nlohmann_json')
    nlohmann_json_dep = nlohmann_json_proj.get_variable('nlohmann_json_dep')
endif

sqlite3_dep = dependency('sqlite3')
blake3_dep = dependency('blake3', required: false)
if not blake3_dep.found()
    blake3_proj = subproject('blake3')
    blake3_dep = blake3_proj.get_variable('blake3_dep')
endif

fuse3_dep = dependency('fuse3', required: false)

# ── Source files shared across executables ──────────────────────────
shared_sources = files(
    'src/db.cpp',
)

scan_lib_sources = files(
    'src/main.cpp',
    'src/scanner.cpp',
)

match_lib_sources = files(
    'src/match_main.cpp',
    'src/matcher.cpp',
)

report_lib_sources = files(
    'src/report_main.cpp',
)

fuse_lib_sources = files(
    'src/fuse_main.cpp',
)

update_lib_sources = files(
    'src/update_main.cpp',
)

# ── Old backward-compatible executables ─────────────────────────────

covered_scan_size_exe = executable('covered_scan_size',
    shared_sources, scan_lib_sources, files('src/main_scan_stub.cpp'),
    dependencies : [nlohmann_json_dep, sqlite3_dep, blake3_dep],
    install : true
)

covered_match_exe = executable('covered_match',
    shared_sources, match_lib_sources, files('src/main_match_stub.cpp'),
    dependencies : [nlohmann_json_dep, sqlite3_dep, blake3_dep],
    install : true
)

cover_report_exe = executable('cover_report',
    shared_sources, report_lib_sources, files('src/main_report_stub.cpp'),
    dependencies : [nlohmann_json_dep, sqlite3_dep],
    install : true
)

if fuse3_dep.found()
    cover_fuse_exe = executable('cover_fuse',
        shared_sources, fuse_lib_sources, files('src/main_fuse_stub.cpp'),
        dependencies : [nlohmann_json_dep, sqlite3_dep, fuse3_dep],
        install : true
    )
endif

cover_update_exe = executable('cover_update',
    shared_sources, update_lib_sources, files('src/main_update_stub.cpp'),
    dependencies : [nlohmann_json_dep, sqlite3_dep, blake3_dep],
    install : true
)

# ── Top-hat "cover" dispatcher ──────────────────────────────────────

cover_deps = [nlohmann_json_dep, sqlite3_dep, blake3_dep]
if fuse3_dep.found()
    cover_deps += fuse3_dep
endif

cover_exe = executable('cover',
    shared_sources,
    scan_lib_sources,
    match_lib_sources,
    report_lib_sources,
    fuse_lib_sources,
    update_lib_sources,
    files('src/cover_main.cpp'),
    dependencies : cover_deps,
    install : true
)

# ── Tests ───────────────────────────────────────────────────────────

if fuse3_dep.found()
    # FUSE xattr test
    test_fuse_xattr_script = find_program('tests/test_fuse_xattr.sh')
    test('fuse_xattr', test_fuse_xattr_script,
        depends : [covered_scan_size_exe, covered_match_exe, cover_fuse_exe],
        workdir : meson.current_source_dir(),
        timeout : 30
    )

    # FUSE ffprobe compatibility test
    test_fuse_ffprobe_script = find_program('tests/test_fuse_ffprobe.sh')
    test('fuse_ffprobe', test_fuse_ffprobe_script,
        depends : [covered_scan_size_exe, covered_match_exe, cover_report_exe, cover_fuse_exe],
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