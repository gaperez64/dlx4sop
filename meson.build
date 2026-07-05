project(
  'dlx4sop',
  'c',
  version: '0.1.1',
  default_options: ['c_std=c2x', 'warning_level=3'],
)

cc = meson.get_compiler('c')
inc = include_directories('include')
m_dep = cc.find_library('m', required: false)

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
    'src/core/bitset.c',
    'src/core/qsop.c',
    'src/core/qsop_parse.c',
    'src/core/qsop_stats.c',
    'src/core/qsop_wmc.c',
    'src/core/qsop_write.c',
    'src/solve/branch.c',
    'src/solve/bruteforce.c',
    'src/solve/component_key.c',
    'src/solve/components.c',
    'src/solve/rankwidth.c',
    'src/solve/residual.c',
    'src/solve/treewidth.c',
  ),
  include_directories: inc,
)

dlx4sop_core_dep = declare_dependency(
  include_directories: inc,
  link_with: dlx4sop_core,
  dependencies: m_dep,
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
  dependencies: [dlx4sop_core_dep, m_dep],
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

sop2wmc = executable(
  'sop2wmc',
  'src/cli/sop2wmc.c',
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
    'qasm benchmark smoke',
    python3,
    args: [
      files('scripts/bench_qasm_corpus.py'),
      qasm2sop.full_path(),
      sop_solve.full_path(),
      '--manifest',
      files('tests/qasm_solver_corpus.json'),
      '--backend',
      'branch',
      '--limit',
      '1',
      '--trace',
    ],
    depends: [qasm2sop, sop_solve],
  )
  test(
    'qasm benchmark summary smoke',
    python3,
    args: [
      files('scripts/bench_qasm_corpus.py'),
      qasm2sop.full_path(),
      sop_solve.full_path(),
      '--manifest',
      files('tests/qasm_solver_corpus.json'),
      '--backend',
      'branch',
      '--limit',
      '1',
      '--trace',
      '--format',
      'summary',
      '--top',
      '1',
      '--top-metric',
      'search_nodes',
    ],
    depends: [qasm2sop, sop_solve],
  )
  test(
    'qasm benchmark trace metrics',
    python3,
    args: [
      files('tests/test_bench_qasm_corpus.py'),
      files('scripts/bench_qasm_corpus.py'),
    ],
  )
  test(
    'qasm rankwidth benchmark smoke',
    python3,
    args: [
      files('scripts/bench_qasm_corpus.py'),
      qasm2sop.full_path(),
      sop_solve.full_path(),
      '--manifest',
      files('tests/qasm_solver_corpus.json'),
      '--backend',
      'rankwidth',
      '--limit',
      '1',
      '--trace',
      '--rankwidth-sweep',
      '--format',
      'summary',
      '--top',
      '1',
      '--top-metric',
      'max_table_entries',
    ],
    depends: [qasm2sop, sop_solve],
  )
  test(
    'qasm rankwidth diagnostics smoke',
    python3,
    args: [
      files('scripts/bench_qasm_corpus.py'),
      qasm2sop.full_path(),
      sop_solve.full_path(),
      '--manifest',
      files('tests/qasm_solver_corpus.json'),
      '--limit',
      '1',
      '--rankwidth-diagnostics',
      '--format',
      'summary',
      '--top',
      '1',
      '--top-metric',
      'solve_elapsed_ns',
    ],
    depends: [qasm2sop, sop_solve],
  )
  test(
    'qasm rankwidth comparison smoke',
    python3,
    args: [
      files('scripts/bench_qasm_corpus.py'),
      qasm2sop.full_path(),
      sop_solve.full_path(),
      '--manifest',
      files('tests/qasm_solver_corpus.json'),
      '--limit',
      '1',
      '--rankwidth-comparison',
      '--format',
      'summary',
      '--top',
      '1',
      '--top-metric',
      'solve_elapsed_ns',
    ],
    depends: [qasm2sop, sop_solve],
  )
  test(
    'qasm treewidth benchmark smoke',
    python3,
    args: [
      files('scripts/bench_qasm_corpus.py'),
      qasm2sop.full_path(),
      sop_solve.full_path(),
      '--manifest',
      files('tests/qasm_solver_corpus.json'),
      '--backend',
      'treewidth',
      '--treewidth-order',
      'min-fill',
      '--treewidth-order',
      'min-degree',
      '--treewidth-order',
      'min-fill-max-degree',
      '--limit',
      '1',
      '--trace',
      '--format',
      'summary',
      '--top',
      '1',
      '--top-metric',
      'treewidth_max_table_entries',
    ],
    depends: [qasm2sop, sop_solve],
  )
  test(
    'qasm width benchmark smoke',
    python3,
    args: [
      files('scripts/bench_qasm_widths.py'),
      qasm2sop.full_path(),
      sop_stats.full_path(),
      '--manifest',
      files('tests/qasm_solver_corpus.json'),
      '--limit',
      '1',
      '--exact-width-max-vars',
      '12',
      '--format',
      'summary',
    ],
    depends: [qasm2sop, sop_stats],
  )
  test(
    'qgraph2qasm smoke',
    python3,
    args: [
      files('tests/test_qgraph2qasm.py'),
      files('scripts/qgraph2qasm.py'),
    ],
  )
  test(
    'qc2qasm smoke',
    python3,
    args: [
      files('tests/test_qc2qasm.py'),
      files('scripts/qc2qasm.py'),
      qasm2sop.full_path(),
    ],
    depends: qasm2sop,
  )
  test(
    'feynmandd scan smoke',
    python3,
    args: [
      files('scripts/scan_feynmandd_qasm.py'),
      qasm2sop.full_path(),
      meson.current_source_dir() / 'tests' / 'golden',
      '--limit',
      '1',
    ],
    depends: qasm2sop,
  )
  test(
    'mqt bench scan smoke',
    python3,
    args: [
      files('tests/test_scan_mqt_bench.py'),
      files('scripts/scan_mqt_bench.py'),
      qasm2sop.full_path(),
    ],
    depends: qasm2sop,
  )
  test(
    'external manifest smoke',
    python3,
    args: [
      files('tests/test_external_manifest.py'),
      files('scripts/build_external_qasm_manifest.py'),
      qasm2sop.full_path(),
      files('scripts/qc2qasm.py'),
    ],
    depends: qasm2sop,
  )
  test(
    'qasm report summary smoke',
    python3,
    args: [
      files('tests/test_summarize_qasm_report.py'),
      files('scripts/summarize_qasm_report.py'),
    ],
  )
  test(
    'scoreboard render smoke',
    python3,
    args: [
      files('tests/test_render_scoreboard.py'),
      files('scripts/render_scoreboard.py'),
    ],
  )
  test(
    'scoreboard refresh smoke',
    python3,
    args: [
      files('tests/test_refresh_scoreboard.py'),
      files('scripts/refresh_scoreboard.py'),
    ],
  )
  test(
    'native solver comparison smoke',
    python3,
    args: [
      files('tests/test_compare_native_solver_results.py'),
      files('scripts/compare_native_solver_results.py'),
    ],
  )
  test(
    'tune branch thresholds smoke',
    python3,
    args: [
      files('tests/test_tune_branch_thresholds.py'),
      files('scripts/tune_branch_thresholds.py'),
    ],
  )
  test(
    'rankwidth generator comparison smoke',
    python3,
    args: [
      files('tests/test_compare_rankwidth_generators.py'),
      files('scripts/compare_rankwidth_generators.py'),
    ],
  )
  test(
    'rankwidth backend comparison smoke',
    python3,
    args: [
      files('tests/test_compare_rankwidth_backends.py'),
      files('scripts/compare_rankwidth_backends.py'),
    ],
  )
  test(
    'rankwidth family generator smoke',
    python3,
    args: [
      files('tests/test_gen_rankwidth_family.py'),
      files('scripts/gen_rankwidth_family.py'),
    ],
  )
  test(
    'rankwidth family crosscheck smoke',
    python3,
    args: [
      files('tests/test_rankwidth_family_crosscheck.py'),
      files('scripts/gen_rankwidth_family.py'),
      files('scripts/bench_sop_local.py'),
      sop_solve.full_path(),
    ],
    depends: sop_solve,
    timeout: 90,
  )
  test(
    'analyze branch regressions smoke',
    python3,
    args: [
      files('tests/test_analyze_branch_regressions.py'),
    ],
  )
  test(
    'scoreboard json output smoke',
    python3,
    args: [
      files('tests/test_scoreboard_json_output.py'),
    ],
  )
  test(
    'bench tune-mqt smoke',
    python3,
    args: [
      files('tests/test_bench_tune_mqt.py'),
    ],
  )
  test(
    'plot scoreboard smoke',
    python3,
    args: [
      files('tests/test_plot_scoreboard.py'),
    ],
  )
  test(
    'check generated scoreboard smoke',
    python3,
    args: [
      files('tests/test_check_generated_scoreboard.py'),
    ],
  )
  test(
    'record qsop mode smoke',
    python3,
    args: [
      files('tests/test_record_qsop_mode.py'),
    ],
  )
  test(
    'harvest mqt bench smoke',
    python3,
    args: [
      files('tests/test_harvest_mqt_bench.py'),
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
      files('scripts/bench_sop_local.py'),
      '--sop-solve', sop_solve.full_path(),
      '--corpus-dir', meson.current_source_dir() / 'benchmarks' / 'corpus' / 'sop' / 'synthetic' / 'rankwidth',
      '--tier', 'tier-rankwidth',
      '--backend', 'rankwidth:linear',
      '--backend', 'rankwidth:best',
      '--backend', 'rankwidth:best:fourier',
      '--timeout', '5',
      '--max-vars', '128',
      '--quiet',
    ],
    depends: sop_solve,
    timeout: 60,
  )
  test(
    'qasm native compatibility smoke',
    python3,
    args: [
      files('tests/test_qasm_native_compat.py'),
      meson.current_source_dir(),
    ],
  )
  test(
    'native simulator benchmark help',
    python3,
    args: [
      files('scripts/bench_qasm_native_simulator.py'),
      '--help',
    ],
  )
  test(
    'wmc ganak benchmark help',
    python3,
    args: [
      files('scripts/bench_wmc_ganak.py'),
      '--help',
    ],
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
    'wmc runner options smoke',
    python3,
    args: [
      files('tests/test_wmc_runner_options.py'),
      files('scripts/run_corpus_benchmarks.py'),
      files('scripts/bench.py'),
    ],
  )
  test(
    'build corpus help',
    python3,
    args: [
      files('scripts/build_corpus.py'),
      '--help',
    ],
  )
  test(
    'build corpus verify',
    python3,
    args: [
      files('scripts/build_corpus.py'),
      '--verify',
      '--lock', files('benchmarks/corpus.lock.json'),
      '--out', meson.current_source_dir() / 'benchmarks' / 'manifests',
    ],
  )
  test(
    'native simulator benchmark smoke',
    python3,
    args: [
      files('tests/test_native_simulator_benchmark.py'),
      files('scripts/bench_qasm_native_simulator.py'),
    ],
  )
  # Synthetic scaling family (WMC-vs-solver crossover study): run the committed corpus
  # through sop-solve with a small timeout cap so CI exercises it without long runs.
  test(
    'synthetic scaling smoke',
    python3,
    args: [
      files('scripts/bench_sop_local.py'),
      '--sop-solve', sop_solve.full_path(),
      '--corpus-dir', meson.current_source_dir() / 'benchmarks' / 'corpus' / 'sop' / 'synthetic' / 'scaling',
      '--tier', 'tier-scaling',
      '--backend', 'treewidth',
      '--timeout', '3',
      '--max-vars', '4096',
      '--quiet',
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
