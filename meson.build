project(
  'dlx4sop',
  'c',
  version: '0.1.0',
  default_options: ['c_std=c23', 'warning_level=3'],
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
    'src/core/qsop.c',
    'src/core/qsop_parse.c',
    'src/core/qsop_write.c',
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
  )
endif
