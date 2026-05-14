project('covered', 'cpp',
    version : '0.1.0',
    default_options : ['cpp_std=c++17', 'warning_level=3']
)

sqlite3_dep = dependency('sqlite3')

sources = files(
    'src/main.cpp',
    'src/db.cpp',
    'src/scanner.cpp'
)

executable('covered_scan', sources,
    dependencies : [sqlite3_dep],
    install : true
)
