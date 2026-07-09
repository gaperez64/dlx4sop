project(
  'dlx4sop',
  'c',
  version: '0.4',
  default_options: ['c_std=c2x', 'warning_level=3'],
)

cc = meson.get_compiler('c')
inc = include_directories('include')
cli_inc = include_directories('src/cli')
m_dep = cc.find_library('m', required: false)
simd_option = get_option('simd')
add_project_arguments(
  '-DDLX4SOP_VERSION="@0@"'.format(meson.project_version()),
  language: 'c',
)

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

dlx4sop_core_sources = files(
    'src/core/amplitude.c',
    'src/core/residue.c',
    'src/core/bitset.c',
    'src/core/simd_scalar.c',
    'src/core/qsop.c',
    'src/core/qsop_parse.c',
    'src/core/min_fill.c',
    'src/core/qsop_simplify.c',
    'src/core/qsop_stats.c',
    'src/core/qsop_wmc.c',
    'src/core/qsop_write.c',
    'src/solve/branch.c',
    'src/solve/component_key.c',
    'src/solve/rankwidth.c',
    'src/solve/residual.c',
    'src/solve/treewidth.c',
)

cpu_family = host_machine.cpu_family()
simd_link_whole = []
is_arm = cpu_family in ['aarch64', 'arm']
is_x86 = cpu_family in ['x86', 'x86_64']

avx2_args = cc.get_supported_arguments(['-mavx2'])
avx512_args = cc.get_supported_arguments([
  '-mavx512f',
  '-mavx512dq',
  '-mavx512vl',
  '-mavx512vpopcntdq',
])

# Which kernels get *compiled in*. Which one actually runs is decided at run time by
# qsop_simd_runtime_kernel() against __builtin_cpu_supports, so `auto` can safely carry every
# kernel the compiler can emit: an AVX-512 binary now falls back instead of trapping on a host
# without AVX-512. Naming a kernel explicitly still pins the build to just that one, which is what
# distro packaging and reproducible builds want.
build_neon = false
build_avx2 = false
build_avx512 = false

if simd_option == 'auto'
  build_neon = is_arm
  build_avx2 = is_x86 and avx2_args.length() == 1
  build_avx512 = is_x86 and avx512_args.length() == 4
elif simd_option == 'neon'
  if not is_arm
    error('-Dsimd=neon requires an ARM/AArch64 target')
  endif
  build_neon = true
elif simd_option == 'avx2'
  if not is_x86
    error('-Dsimd=avx2 requires an x86 target')
  endif
  if avx2_args.length() < 1
    error('compiler does not support -mavx2')
  endif
  build_avx2 = true
elif simd_option == 'avx512'
  if not is_x86
    error('-Dsimd=avx512 requires an x86 target')
  endif
  if avx512_args.length() < 4
    error('compiler does not support the required AVX-512 flags')
  endif
  build_avx512 = true
endif

# These tell simd_scalar.c's dispatcher which __builtin_cpu_supports probes it may compile: the
# feature strings only exist on toolchains new enough to emit the corresponding kernel. Must be
# added before any build target is declared.
if build_neon
  add_project_arguments('-DQSOP_SIMD_HAVE_NEON', language: 'c')
endif
if build_avx2
  add_project_arguments('-DQSOP_SIMD_HAVE_AVX2', language: 'c')
endif
if build_avx512
  add_project_arguments('-DQSOP_SIMD_HAVE_AVX512', language: 'c')
endif

# Each kernel is its own translation unit compiled with its own -m flags, and keeps LTO on. The
# per-function target attributes GCC derives from those flags are what stop it from inlining zmm
# code into a caller that may run on a CPU without AVX-512; verified on the fat build by
# disassembling and checking that every zmm/ymm instruction lives inside a simd_avx512_*/simd_avx2_*
# function. (A weak-symbol dispatch would break this -- see simd_scalar.c.)
if build_neon
  simd_link_whole += static_library(
    'dlx4sop_simd_neon',
    files('src/core/simd_neon.c'),
    include_directories: inc,
  )
endif

if build_avx2
  simd_link_whole += static_library(
    'dlx4sop_simd_avx2',
    files('src/core/simd_avx2.c'),
    include_directories: inc,
    c_args: avx2_args,
  )
endif

if build_avx512
  simd_link_whole += static_library(
    'dlx4sop_simd_avx512',
    files('src/core/simd_avx512.c'),
    include_directories: inc,
    c_args: avx512_args,
  )
endif

simd_compiled = []
if build_avx512
  simd_compiled += 'avx512'
endif
if build_avx2
  simd_compiled += 'avx2'
endif
if build_neon
  simd_compiled += 'neon'
endif
simd_compiled += 'scalar'
message('SIMD kernels compiled in (best supported is chosen at run time): ' + ', '.join(simd_compiled))

dlx4sop_core = static_library(
  'dlx4sop_core',
  dlx4sop_core_sources,
  include_directories: inc,
)

dlx4sop_core_dep = declare_dependency(
  include_directories: inc,
  link_with: dlx4sop_core,
  link_whole: simd_link_whole,
  dependencies: m_dep,
)

dlx4sop_cli = static_library(
  'dlx4sop_cli',
  files('src/cli/cli_common.c'),
  include_directories: cli_inc,
)

dlx4sop_cli_dep = declare_dependency(
  include_directories: cli_inc,
  link_with: dlx4sop_cli,
)

sop_check = executable(
  'sop-check',
  'src/cli/sop_check.c',
  dependencies: [dlx4sop_core_dep, dlx4sop_cli_dep],
  install: true,
)

sop_solve = executable(
  'sop-solve',
  'src/cli/sop_solve.c',
  dependencies: [dlx4sop_core_dep, dlx4sop_cli_dep, m_dep],
  install: true,
)

sop_stats = executable(
  'sop-stats',
  'src/cli/sop_stats.c',
  dependencies: [dlx4sop_core_dep, dlx4sop_cli_dep],
  install: true,
)

qasm2sop = executable(
  'qasm2sop',
  'src/cli/qasm2sop.c',
  dependencies: [dlx4sop_core_dep, dlx4sop_cli_dep],
  install: true,
)

sop2wmc = executable(
  'sop2wmc',
  'src/cli/sop2wmc.c',
  dependencies: [dlx4sop_core_dep, dlx4sop_cli_dep],
  install: true,
)

bench_kernels = executable(
  'bench-kernels',
  'tests/unit/bench_kernels.c',
  dependencies: dlx4sop_core_dep,
  install: false,
)

# Memcheck over the C unit tests only. They drive every allocator in the library -- the treewidth
# factor pool, the residual trail, the branch caches -- and are the only tests meson can wrap
# directly; the python ones exec the CLI, so an exe_wrapper would valgrind their interpreter.
# Run with: meson test -C build --setup=valgrind --suite=unit
valgrind = find_program('valgrind', required: false)
if valgrind.found()
  add_test_setup(
    'valgrind',
    exe_wrapper: [
      valgrind,
      '--leak-check=full',
      '--errors-for-leak-kinds=definite,indirect',
      '--error-exitcode=99',
      '--quiet',
    ],
    timeout_multiplier: 40,
  )
endif

test_residue = executable(
  'test-residue',
  'tests/unit/test_residue.c',
  dependencies: dlx4sop_core_dep,
)

test('residue unit', test_residue, suite: 'unit')

test_residual = executable(
  'test-residual',
  'tests/unit/test_residual.c',
  dependencies: dlx4sop_core_dep,
)

test('residual unit', test_residual, suite: 'unit')

test_bitset = executable(
  'test-bitset',
  'tests/unit/test_bitset.c',
  dependencies: dlx4sop_core_dep,
)

test('bitset unit', test_bitset, suite: 'unit')

test_simd_kernels = executable(
  'test-simd-kernels',
  'tests/unit/test_simd_kernels.c',
  dependencies: dlx4sop_core_dep,
)

test('simd kernels unit', test_simd_kernels, suite: 'unit')

test_wmc = executable(
  'test-wmc',
  'tests/unit/test_wmc.c',
  dependencies: dlx4sop_core_dep,
)

test('wmc unit', test_wmc, suite: 'unit')

test_qsop_simplify = executable(
  'test-qsop-simplify',
  'tests/unit/test_qsop_simplify.c',
  dependencies: dlx4sop_core_dep,
)

test('qsop simplify unit', test_qsop_simplify, suite: 'unit')

test_min_fill = executable(
  'test-min-fill',
  'tests/unit/test_min_fill.c',
  dependencies: dlx4sop_core_dep,
)

test('min-fill unit', test_min_fill, suite: 'unit')

test_branch_api = executable(
  'test-branch-api',
  'tests/unit/test_branch_api.c',
  dependencies: dlx4sop_core_dep,
)

test('branch api unit', test_branch_api, suite: 'unit')

test_branch_propagate = executable(
  'test-branch-propagate',
  'tests/unit/test_branch_propagate.c',
  dependencies: dlx4sop_core_dep,
)

test('branch propagate unit', test_branch_propagate, suite: 'unit', timeout: 120)

test_branch_single_cache = executable(
  'test-branch-single-cache',
  'tests/unit/test_branch_single_cache.c',
  dependencies: dlx4sop_core_dep,
)

test('branch single cache unit', test_branch_single_cache, suite: 'unit', timeout: 120)

test_treewidth_join = executable(
  'test-treewidth-join',
  'tests/unit/test_treewidth_join.c',
  dependencies: dlx4sop_core_dep,
)

test('treewidth join unit', test_treewidth_join, suite: 'unit', timeout: 120)

test(
  'kernel benchmark smoke',
  bench_kernels,
  args: ['--quick'],
  timeout: 30,
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
    'sop-solve jsonl smoke',
    python3,
    args: [
      files('tests/test_sop_solve_jsonl.py'),
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
    'differential backends',
    python3,
    args: [
      files('tests/test_differential_backends.py'),
      sop_solve.full_path(),
      meson.current_source_dir(),
    ],
    depends: sop_solve,
    timeout: 300,
  )
  test(
    'rankwidth table',
    python3,
    args: [
      files('tests/test_rankwidth_table.py'),
      sop_solve.full_path(),
      meson.current_source_dir(),
    ],
    depends: sop_solve,
    timeout: 60,
  )
  test(
    'rankwidth dump',
    python3,
    args: [
      files('tests/test_rankwidth_dump.py'),
      sop_solve.full_path(),
      meson.current_source_dir(),
    ],
    depends: sop_solve,
    timeout: 60,
  )
  test(
    'rankwidth best',
    python3,
    args: [
      files('tests/test_rankwidth_best.py'),
      sop_solve.full_path(),
      meson.current_source_dir(),
    ],
    depends: sop_solve,
    timeout: 120,
  )
  test(
    'rankwidth search',
    python3,
    args: [
      files('tests/test_rankwidth_search.py'),
      sop_solve.full_path(),
      meson.current_source_dir(),
    ],
    depends: sop_solve,
    timeout: 120,
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
  test(
    'sop2wmc golden',
    python3,
    args: [
      files('tests/test_sop2wmc.py'),
      sop2wmc.full_path(),
      sop_solve.full_path(),
      meson.current_source_dir(),
    ],
    depends: [sop2wmc, sop_solve],
  )
  test(
    'qasm2sop optimize differential',
    python3,
    args: [
      files('tests/test_qasm2sop_optimize.py'),
      qasm2sop.full_path(),
      sop_solve.full_path(),
      meson.current_source_dir(),
    ],
    depends: [qasm2sop, sop_solve],
    timeout: 120,
  )
  test(
    'qasm2sop amplitudes',
    python3,
    args: [
      files('tests/test_qasm_amplitudes.py'),
      qasm2sop.full_path(),
      sop_solve.full_path(),
    ],
    depends: [qasm2sop, sop_solve],
    # Toffoli-family gates (ccx/ccz/cswap) now lower through cx-sandwiches to stay sign-only,
    # adding variables the default (non-Fourier) backend used for this differential check feels
    # more than the O(2^k) single-fourier-mode backend real gauntlet circuits actually use.
    timeout: 90,
  )
  test(
    'qasm solver corpus',
    python3,
    args: [
      files('tests/test_qasm_solver_corpus.py'),
      qasm2sop.full_path(),
      sop_solve.full_path(),
    ],
    depends: [qasm2sop, sop_solve],
  )
  test(
    'qasm width benchmark smoke',
    python3,
    args: [
      files('tests/test_qasm_exact_widths.py'),
      qasm2sop.full_path(),
      sop_stats.full_path(),
    ],
    depends: [qasm2sop, sop_stats],
  )
  test(
    'qasm manifest helpers smoke',
    python3,
    args: [
      files('tests/test_qasm_manifest_helpers.py'),
      files('scripts/build_external_qasm_manifest.py'),
    ],
  )
  test(
    'branch policy smoke',
    python3,
    args: [
      files('tests/test_branch_policy.py'),
      sop_solve.full_path(),
    ],
    depends: sop_solve,
  )
  test(
    'branch regression guard smoke',
    python3,
    args: [
      files('tests/test_branch_regression_guard.py'),
      sop_solve.full_path(),
    ],
    depends: sop_solve,
    timeout: 90,
  )
  test(
    'rankwidth memory budget smoke',
    python3,
    args: [
      files('tests/test_rankwidth_memory_budget.py'),
      sop_solve.full_path(),
    ],
    depends: sop_solve,
    timeout: 60,
  )
  test(
    'rankwidth join strategy smoke',
    python3,
    args: [
      files('tests/test_rankwidth_join_strategy.py'),
      sop_solve.full_path(),
    ],
    depends: sop_solve,
    timeout: 120,
  )
  test(
    'rankwidth family benchmark smoke',
    python3,
    args: [
      files('tests/test_rankwidth_family_backends.py'),
      sop_solve.full_path(),
    ],
    depends: sop_solve,
    timeout: 60,
  )
  test(
    'wmc ganak benchmark metadata smoke',
    python3,
    args: [
      files('tests/test_bench_wmc_ganak.py'),
      files('scripts/bench_wmc_ganak.py'),
    ],
  )
  # Synthetic scaling smoke: a large, low-treewidth synthetic instance built inline, run
  # through sop-solve with a small timeout cap so CI exercises it without long runs.
  test(
    'synthetic scaling smoke',
    python3,
    args: [
      files('tests/test_treewidth_scaling_smoke.py'),
      sop_solve.full_path(),
    ],
    depends: [sop_solve],
    timeout: 120,
  )
  if get_option('qiskit_tests')
    test(
      'qasm2sop qiskit',
      python3,
      args: [
        files('tests/test_qasm_qiskit.py'),
        qasm2sop.full_path(),
        sop_solve.full_path(),
      ],
      depends: [qasm2sop, sop_solve],
    )
  endif
  if get_option('ganak_tests')
    test(
      'sop2wmc ganak',
      python3,
      args: [
        files('tests/test_sop2wmc_ganak.py'),
        sop2wmc.full_path(),
        sop_solve.full_path(),
        meson.current_source_dir(),
      ],
      depends: [sop2wmc, sop_solve],
    )
  endif
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
    args: files('tests/golden/sign_raw.qsop'),
  )
endif
