project(
  'dlx4sop',
  'c',
  version: '0.3',
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
    'src/core/residue.c',
    'src/core/bitset.c',
    'src/core/simd_scalar.c',
    'src/core/qsop.c',
    'src/core/qsop_parse.c',
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
simd_arch = simd_option
avx512_args = cc.get_supported_arguments([
  '-mavx512f',
  '-mavx512dq',
  '-mavx512vl',
  '-mavx512vpopcntdq',
])

if simd_option == 'auto'
  if cpu_family in ['aarch64', 'arm']
    simd_arch = 'neon'
  elif cpu_family in ['x86', 'x86_64'] and meson.can_run_host_binaries()
    avx512_probe = '''
#if defined(__x86_64__) || defined(__i386__)
int main(void) {
#if defined(__GNUC__) || defined(__clang__)
  __builtin_cpu_init();
  return (__builtin_cpu_supports("avx512f") &&
          __builtin_cpu_supports("avx512dq") &&
          __builtin_cpu_supports("avx512vl") &&
          __builtin_cpu_supports("avx512vpopcntdq")) ? 0 : 1;
#else
  return 1;
#endif
}
#else
int main(void) { return 1; }
#endif
'''
    avx512_host = cc.run(avx512_probe, name: 'AVX-512 host probe')
    if avx512_args.length() == 4 and avx512_host.compiled() and avx512_host.returncode() == 0
      simd_arch = 'avx512'
    else
      simd_arch = 'scalar'
    endif
  else
    simd_arch = 'scalar'
  endif
endif

if simd_arch == 'neon'
  if cpu_family not in ['aarch64', 'arm']
    error('-Dsimd=neon requires an ARM/AArch64 target')
  endif
  add_project_arguments('-DQSOP_SIMD_ARCH_NEON', language: 'c')
  simd_neon_lib = static_library(
    'dlx4sop_simd_neon',
    files('src/core/simd_neon.c'),
    include_directories: inc,
  )
  simd_link_whole += simd_neon_lib
endif

if simd_arch == 'avx512'
  if cpu_family not in ['x86', 'x86_64']
    error('-Dsimd=avx512 requires an x86 target')
  endif
  if avx512_args.length() < 4
    error('compiler does not support the required AVX-512 flags')
  endif
  add_project_arguments('-DQSOP_SIMD_ARCH_AVX512', language: 'c')
  simd_avx512_lib = static_library(
    'dlx4sop_simd_avx512',
    files('src/core/simd_avx512.c'),
    include_directories: inc,
    c_args: avx512_args,
  )
  simd_link_whole += simd_avx512_lib
endif

if simd_arch == 'scalar'
  add_project_arguments('-DQSOP_SIMD_ARCH_SCALAR', language: 'c')
endif

message('SIMD architecture: ' + simd_arch)

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

test_bitset = executable(
  'test-bitset',
  'tests/unit/test_bitset.c',
  dependencies: dlx4sop_core_dep,
)

test('bitset unit', test_bitset)

test_wmc = executable(
  'test-wmc',
  'tests/unit/test_wmc.c',
  dependencies: dlx4sop_core_dep,
)

test('wmc unit', test_wmc)

test_branch_api = executable(
  'test-branch-api',
  'tests/unit/test_branch_api.c',
  dependencies: dlx4sop_core_dep,
)

test('branch api unit', test_branch_api)

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
  test(
    'branch treewidth benchmark smoke',
    python3,
    args: [
      files('tests/test_bench_branch_treewidth.py'),
      files('scripts/bench_branch_treewidth.py'),
      sop_solve.full_path(),
      meson.current_source_dir(),
    ],
    depends: sop_solve,
    timeout: 40,
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
