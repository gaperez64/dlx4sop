project(
  'dlx4sop',
  'c',
  version: '0.1.0',
  default_options: ['c_std=c2x', 'warning_level=3'],
)

cc = meson.get_compiler('c')
inc = include_directories('include')

if get_option('developer_warnings')
  warning_flags = [
    '-Wcast-align',
    '-Wconversion',
    '-Wdouble-promotion',
    '-Wextra',
    '-Wformat=2',
    '-Wmissing-prototypes',
    '-Wshadow',
    '-Wstrict-prototypes',
    '-Wundef',
    '-Wunused',
  ]
  add_project_arguments(cc.get_supported_arguments(warning_flags), language: 'c')
endif

dlx4sop_core = static_library(
  'dlx4sop_core',
  files(
    'src/core/residue.c',
    'src/core/qsop.c',
    'src/core/qsop_parse.c',
    'src/core/qsop_stats.c',
    'src/core/qsop_write.c',
    'src/solve/branch.c',
    'src/solve/bruteforce.c',
    'src/solve/components.c',
    'src/solve/residual.c',
  ),
  include_directories: inc,
)

dlx4sop_core_dep = declare_dependency(
  include_directories: inc,
  link_with: dlx4sop_core,
)

sop_check = executable(
  'sop-check',
  'src/cli/sop_check.c',
  dependencies: dlx4sop_core_dep,
  install: true,
)

sop_solve = executable(
  'sop-solve',
  'src/cli/sop_solve.c',
  dependencies: dlx4sop_core_dep,
  install: true,
)

sop_stats = executable(
  'sop-stats',
  'src/cli/sop_stats.c',
  dependencies: dlx4sop_core_dep,
  install: true,
)

qasm2sop = executable(
  'qasm2sop',
  'src/cli/qasm2sop.c',
  dependencies: dlx4sop_core_dep,
  install: true,
)

test_residue = executable(
  'test-residue',
  'tests/unit/test_residue.c',
  dependencies: dlx4sop_core_dep,
)

test('residue unit', test_residue)

test_residual = executable(
  'test-residual',
  'tests/unit/test_residual.c',
  dependencies: dlx4sop_core_dep,
)

test('residual unit', test_residual)

python3 = find_program('python3', required: false)
if python3.found()
  test(
    'sop-check golden',
    python3,
    args: [
      files('tests/test_sop_check.py'),
      sop_check.full_path(),
      meson.current_source_dir(),
    ],
    depends: sop_check,
  )
  test(
    'sop-solve golden',
    python3,
    args: [
      files('tests/test_sop_solve.py'),
      sop_solve.full_path(),
      meson.current_source_dir(),
    ],
    depends: sop_solve,
  )
  test(
    'sop-stats golden',
    python3,
    args: [
      files('tests/test_sop_stats.py'),
      sop_stats.full_path(),
      meson.current_source_dir(),
    ],
    depends: sop_stats,
  )
  test(
    'sop invariants',
    python3,
    args: [
      files('tests/test_sop_invariants.py'),
      sop_check.full_path(),
      sop_solve.full_path(),
      meson.current_source_dir(),
    ],
    depends: [sop_check, sop_solve],
  )
  test(
    'qasm2sop golden',
    python3,
    args: [
      files('tests/test_qasm2sop.py'),
      qasm2sop.full_path(),
      meson.current_source_dir(),
    ],
    depends: qasm2sop,
  )
endif

if get_option('build_fuzzers')
  fuzz_qsop_parse = executable(
    'fuzz-qsop-parse',
    'tests/fuzz/fuzz_qsop_parse.c',
    dependencies: dlx4sop_core_dep,
    install: false,
  )
  test(
    'fuzz parser smoke',
    fuzz_qsop_parse,
    args: files('tests/golden/labelled_raw.qsop'),
  )
endif
