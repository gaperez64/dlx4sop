#include "dlx4sop/qsop.h"
#include "dlx4sop/qsop_wmc.h"

#include <errno.h>
#include <stdbool.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void print_usage(FILE *file) {
  fputs("usage: sop2wmc [--encoding auto|amp-and|amp-soft|amp-block|residue-fourier|residue-accumulator] "
        "[--wmc-fourier-inner amp-and|amp-soft] "
        "[--wmc-preprocess none|peel1|peel2-safe] [--residue K|all] "
        "[--wmc-fourier-mode all|T] [--wmc-peel2-fill-budget N] "
        "[--wmc-block-min-side N] [--wmc-block-min-savings N] "
        "[--stats-only] [-o PATH] [--no-metadata] [PATH|-]\n",
        file);
}

static void print_error(const qsop_error_t *error, const char *fallback_path) {
  const char *path = error->path != NULL ? error->path : fallback_path;
  if (path == NULL) {
    path = "<input>";
  }
  if (error->line > 0) {
    fprintf(stderr, "error: %s:%zu:%zu: %s\n", path, error->line, error->column, error->message);
  } else {
    fprintf(stderr, "error: %s: %s\n", path, error->message);
  }
}

static const char *encoding_name(qsop_wmc_encoding_t encoding) {
  switch (encoding) {
  case QSOP_WMC_ENCODING_RESIDUE:
    return "residue";
  case QSOP_WMC_ENCODING_AMPLITUDE:
    return "amp-and";
  case QSOP_WMC_ENCODING_AMP_SOFT:
    return "amp-soft";
  case QSOP_WMC_ENCODING_RESIDUE_FOURIER:
    return "residue-fourier";
  case QSOP_WMC_ENCODING_AMP_BLOCK:
    return "amp-block";
  }
  return "unknown";
}

static bool write_wmc_stats(FILE *file, const qsop_wmc_stats_t *stats,
                            const char *selected_encoding, qsop_error_t *error) {
  if (file == NULL || stats == NULL || selected_encoding == NULL) {
    if (error != NULL) {
      error->path = NULL;
      error->line = 0;
      error->column = 0;
      snprintf(error->message, sizeof(error->message),
               "internal error: null WMC stats argument");
    }
    return false;
  }
  fprintf(file, "encoding: %s\n", selected_encoding);
  fprintf(file, "r: %" PRIu64 "\n", stats->r);
  fprintf(file, "norm_h: %" PRIu64 "\n", stats->norm_h);
  fprintf(file, "original_nvars: %" PRIu32 "\n", stats->original_nvars);
  fprintf(file, "original_edges: %" PRIu32 "\n", stats->original_edges);
  fprintf(file, "is_zero: %s\n", stats->is_zero ? "true" : "false");
  fprintf(file, "active_vars: %" PRIu32 "\n", stats->active_vars);
  fprintf(file, "eliminated_vars: %" PRIu32 "\n", stats->eliminated_vars);
  fprintf(file, "residual_edges: %" PRIu32 "\n", stats->residual_edges);
  fprintf(file, "aux_vars: %" PRIu32 "\n", stats->aux_vars);
  fprintf(file, "clauses_unit: %" PRIu64 "\n", stats->clauses_unit);
  fprintf(file, "clauses_binary: %" PRIu64 "\n", stats->clauses_binary);
  fprintf(file, "clauses_ternary: %" PRIu64 "\n", stats->clauses_ternary);
  fprintf(file, "total_clauses: %" PRIu64 "\n", stats->total_clauses);
  fprintf(file, "estimated_cnf_bytes: %" PRIu64 "\n", stats->estimated_bytes);
  fprintf(file, "encoded_edges: %" PRIu32 "\n", stats->encoded_edges);
  fprintf(file, "skipped_edges: %" PRIu32 "\n", stats->skipped_edges);
  fprintf(file, "block_count: %" PRIu32 "\n", stats->block_count);
  fprintf(file, "block_edges: %" PRIu32 "\n", stats->block_edges);
  fprintf(file, "max_block_a_size: %" PRIu32 "\n", stats->max_block_a_size);
  fprintf(file, "max_block_b_size: %" PRIu32 "\n", stats->max_block_b_size);
  if (ferror(file)) {
    if (error != NULL) {
      error->path = NULL;
      error->line = 0;
      error->column = 0;
      snprintf(error->message, sizeof(error->message), "write failed: %s", strerror(errno));
    }
    return false;
  }
  return true;
}

static bool collect_wmc_stats(const qsop_instance_t *qsop, qsop_wmc_options_t options,
                              qsop_wmc_encoding_t encoding, qsop_wmc_preprocess_t preprocess,
                              qsop_wmc_stats_t *stats, qsop_error_t *error) {
  FILE *sink = fopen("/dev/null", "w");
  if (sink == NULL) {
    if (error != NULL) {
      error->path = NULL;
      error->line = 0;
      error->column = 0;
      snprintf(error->message, sizeof(error->message), "/dev/null: %s", strerror(errno));
    }
    return false;
  }
  options.encoding = encoding;
  options.preprocess = preprocess;
  options.emit_metadata = false;
  options.stats_out = stats;
  const bool ok = qsop_wmc_write(sink, qsop, &options, error);
  if (fclose(sink) != 0 && ok) {
    if (error != NULL) {
      error->path = NULL;
      error->line = 0;
      error->column = 0;
      snprintf(error->message, sizeof(error->message), "/dev/null: %s", strerror(errno));
    }
    return false;
  }
  return ok;
}

static bool choose_auto_encoding(const qsop_instance_t *qsop, const qsop_wmc_options_t *base,
                                 bool preprocess_was_set, qsop_wmc_encoding_t *encoding_out,
                                 qsop_wmc_preprocess_t *preprocess_out,
                                 qsop_wmc_stats_t *stats_out, qsop_error_t *error) {
  const qsop_wmc_preprocess_t pp =
      preprocess_was_set ? base->preprocess : QSOP_WMC_PREPROCESS_PEEL1;
  qsop_wmc_stats_t block = {0};
  qsop_wmc_stats_t soft = {0};
  qsop_error_t block_error = {0};
  const bool block_ok =
      collect_wmc_stats(qsop, *base, QSOP_WMC_ENCODING_AMP_BLOCK, pp, &block, &block_error);
  const bool soft_ok =
      collect_wmc_stats(qsop, *base, QSOP_WMC_ENCODING_AMP_SOFT, pp, &soft, error);
  if (!soft_ok && !block_ok) {
    if (block_error.message[0] != '\0') {
      *error = block_error;
    }
    return false;
  }
  if (!soft_ok) {
    *encoding_out = QSOP_WMC_ENCODING_AMP_BLOCK;
    *preprocess_out = pp;
    if (stats_out != NULL) {
      *stats_out = block;
    }
    return true;
  }
  bool choose_block = false;
  if (block_ok && block.block_count > 0) {
    const uint64_t soft_bytes = soft.estimated_bytes == 0 ? UINT64_MAX : soft.estimated_bytes;
    const uint64_t block_bytes = block.estimated_bytes == 0 ? UINT64_MAX : block.estimated_bytes;
    choose_block = block_bytes <= soft_bytes ||
                   (block.block_edges >= soft.encoded_edges / 2U &&
                    block_bytes <= soft_bytes + soft_bytes / 2U);
  }
  *encoding_out = choose_block ? QSOP_WMC_ENCODING_AMP_BLOCK : QSOP_WMC_ENCODING_AMP_SOFT;
  *preprocess_out = pp;
  if (stats_out != NULL) {
    *stats_out = choose_block ? block : soft;
  }
  return true;
}

int main(int argc, char **argv) {
  const char *input_path = NULL;
  const char *output_path = NULL;
  qsop_wmc_options_t options = qsop_wmc_options_default();
  bool encoding_auto = true;
  bool encoding_set = false;
  bool preprocess_set = false;
  bool stats_only = false;

  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--help") == 0) {
      print_usage(stdout);
      return 0;
    }
    if (strcmp(argv[i], "--encoding") == 0) {
      if (i + 1 >= argc) {
        fputs("error: --encoding requires a value\n", stderr);
        return 2;
      }
      const char *enc = argv[++i];
      encoding_set = true;
      if (strcmp(enc, "auto") == 0) {
        encoding_auto = true;
      } else if (strcmp(enc, "residue-accumulator") == 0 || strcmp(enc, "residue") == 0) {
        encoding_auto = false;
        options.encoding = QSOP_WMC_ENCODING_RESIDUE;
      } else if (strcmp(enc, "amp-and") == 0 || strcmp(enc, "amplitude") == 0) {
        encoding_auto = false;
        options.encoding = QSOP_WMC_ENCODING_AMPLITUDE;
      } else if (strcmp(enc, "amp-soft") == 0) {
        encoding_auto = false;
        options.encoding = QSOP_WMC_ENCODING_AMP_SOFT;
      } else if (strcmp(enc, "residue-fourier") == 0) {
        encoding_auto = false;
        options.encoding = QSOP_WMC_ENCODING_RESIDUE_FOURIER;
      } else if (strcmp(enc, "amp-block") == 0) {
        encoding_auto = false;
        options.encoding = QSOP_WMC_ENCODING_AMP_BLOCK;
      } else {
        fputs("error: --encoding must be 'auto', 'amp-and', 'amp-soft', 'amp-block', "
              "'residue-fourier', or 'residue-accumulator'\n",
              stderr);
        return 2;
      }
      continue;
    }
    if (strcmp(argv[i], "--residue") == 0) {
      if (i + 1 >= argc) {
        fputs("error: --residue requires a value\n", stderr);
        return 2;
      }
      const char *value = argv[++i];
      if (!encoding_set) {
        encoding_auto = false;
        options.encoding = QSOP_WMC_ENCODING_RESIDUE;
      }
      if (strcmp(value, "all") == 0) {
        options.all_residues = true;
      } else {
        char *end = NULL;
        errno = 0;
        const unsigned long parsed = strtoul(value, &end, 10);
        if (errno != 0 || end == value || *end != '\0' || parsed > UINT32_MAX) {
          fputs("error: --residue must be 'all' or a non-negative integer\n", stderr);
          return 2;
        }
        options.all_residues = false;
        options.residue = (uint32_t)parsed;
      }
      continue;
    }
    if (strcmp(argv[i], "-o") == 0 || strcmp(argv[i], "--output") == 0) {
      if (i + 1 >= argc) {
        fputs("error: -o requires a path\n", stderr);
        return 2;
      }
      output_path = argv[++i];
      continue;
    }
    if (strcmp(argv[i], "--wmc-fourier-inner") == 0) {
      if (i + 1 >= argc) {
        fputs("error: --wmc-fourier-inner requires a value\n", stderr);
        return 2;
      }
      const char *inner = argv[++i];
      if (strcmp(inner, "amp-and") == 0 || strcmp(inner, "amplitude") == 0) {
        options.fourier_inner = QSOP_WMC_ENCODING_AMPLITUDE;
      } else if (strcmp(inner, "amp-soft") == 0) {
        options.fourier_inner = QSOP_WMC_ENCODING_AMP_SOFT;
      } else {
        fputs("error: --wmc-fourier-inner must be 'amp-and' or 'amp-soft'\n", stderr);
        return 2;
      }
      continue;
    }
    if (strcmp(argv[i], "--wmc-preprocess") == 0) {
      if (i + 1 >= argc) {
        fputs("error: --wmc-preprocess requires a value\n", stderr);
        return 2;
      }
      const char *pp = argv[++i];
      if (strcmp(pp, "none") == 0) {
        options.preprocess = QSOP_WMC_PREPROCESS_NONE;
      } else if (strcmp(pp, "peel1") == 0) {
        options.preprocess = QSOP_WMC_PREPROCESS_PEEL1;
      } else if (strcmp(pp, "peel2-safe") == 0) {
        options.preprocess = QSOP_WMC_PREPROCESS_PEEL2_SAFE;
      } else {
        fputs("error: --wmc-preprocess must be 'none', 'peel1', or 'peel2-safe'\n", stderr);
        return 2;
      }
      preprocess_set = true;
      continue;
    }
    if (strcmp(argv[i], "--wmc-fourier-mode") == 0) {
      if (i + 1 >= argc) {
        fputs("error: --wmc-fourier-mode requires 'all' or a non-negative integer\n", stderr);
        return 2;
      }
      const char *mode = argv[++i];
      if (strcmp(mode, "all") == 0) {
        options.fourier_all_modes = true;
      } else {
        char *end = NULL;
        errno = 0;
        const unsigned long parsed = strtoul(mode, &end, 10);
        if (errno != 0 || end == mode || *end != '\0' || parsed > UINT32_MAX) {
          fputs("error: --wmc-fourier-mode must be 'all' or a non-negative integer\n", stderr);
          return 2;
        }
        options.fourier_all_modes = false;
        options.fourier_mode = (uint32_t)parsed;
      }
      continue;
    }
    if (strcmp(argv[i], "--wmc-peel2-fill-budget") == 0) {
      if (i + 1 >= argc) {
        fputs("error: --wmc-peel2-fill-budget requires a non-negative integer\n", stderr);
        return 2;
      }
      char *end = NULL;
      errno = 0;
      const unsigned long parsed = strtoul(argv[++i], &end, 10);
      if (errno != 0 || end == argv[i] || *end != '\0' || parsed > UINT32_MAX) {
        fputs("error: --wmc-peel2-fill-budget must be a non-negative integer\n", stderr);
        return 2;
      }
      options.peel2_fill_budget = (uint32_t)parsed;
      continue;
    }
    if (strcmp(argv[i], "--wmc-block-min-side") == 0) {
      if (i + 1 >= argc) {
        fputs("error: --wmc-block-min-side requires a value\n", stderr);
        return 2;
      }
      char *end = NULL;
      errno = 0;
      const unsigned long parsed = strtoul(argv[++i], &end, 10);
      if (errno != 0 || end == argv[i] || *end != '\0' || parsed > UINT32_MAX) {
        fputs("error: --wmc-block-min-side must be a non-negative integer\n", stderr);
        return 2;
      }
      options.block_min_side = (uint32_t)parsed;
      continue;
    }
    if (strcmp(argv[i], "--wmc-block-min-savings") == 0) {
      if (i + 1 >= argc) {
        fputs("error: --wmc-block-min-savings requires a value\n", stderr);
        return 2;
      }
      char *end = NULL;
      errno = 0;
      const long long parsed = strtoll(argv[++i], &end, 10);
      if (errno != 0 || end == argv[i] || *end != '\0') {
        fputs("error: --wmc-block-min-savings must be an integer\n", stderr);
        return 2;
      }
      options.block_min_savings = (int64_t)parsed;
      continue;
    }
    if (strcmp(argv[i], "--no-metadata") == 0) {
      options.emit_metadata = false;
      continue;
    }
    if (strcmp(argv[i], "--stats-only") == 0) {
      stats_only = true;
      continue;
    }
    if (argv[i][0] == '-') {
      if (strcmp(argv[i], "-") == 0 && input_path == NULL) {
        input_path = argv[i];
        continue;
      }
      fprintf(stderr, "error: unknown option '%s'\n", argv[i]);
      print_usage(stderr);
      return 2;
    }
    if (input_path != NULL) {
      fputs("error: expected at most one input path\n", stderr);
      print_usage(stderr);
      return 2;
    }
    input_path = argv[i];
  }

  FILE *input = stdin;
  const char *diagnostic_path = "<stdin>";
  if (input_path != NULL && strcmp(input_path, "-") != 0) {
    input = fopen(input_path, "r");
    diagnostic_path = input_path;
    if (input == NULL) {
      fprintf(stderr, "error: %s: %s\n", input_path, strerror(errno));
      return 1;
    }
  }

  qsop_error_t error = {0};
  qsop_instance_t *qsop = NULL;
  bool ok = qsop_parse_file(input, diagnostic_path, &qsop, &error);
  if (input != stdin) {
    fclose(input);
  }
  if (!ok) {
    print_error(&error, diagnostic_path);
    return 1;
  }

  qsop_wmc_stats_t selected_stats = {0};
  qsop_wmc_stats_t *selected_stats_ptr = NULL;
  const char *selected_encoding = encoding_name(options.encoding);
  if (encoding_auto) {
    qsop_wmc_encoding_t chosen = QSOP_WMC_ENCODING_AMP_SOFT;
    qsop_wmc_preprocess_t chosen_preprocess = QSOP_WMC_PREPROCESS_PEEL1;
    if (!choose_auto_encoding(qsop, &options, preprocess_set, &chosen, &chosen_preprocess,
                              &selected_stats, &error)) {
      print_error(&error, diagnostic_path);
      qsop_free(qsop);
      return 1;
    }
    options.encoding = chosen;
    options.preprocess = chosen_preprocess;
    selected_encoding = encoding_name(chosen);
    selected_stats_ptr = &selected_stats;
  }

  FILE *output = stdout;
  if (output_path != NULL) {
    output = fopen(output_path, "w");
    if (output == NULL) {
      fprintf(stderr, "error: %s: %s\n", output_path, strerror(errno));
      qsop_free(qsop);
      return 1;
    }
  }

  if (stats_only) {
    qsop_wmc_stats_t stats = {0};
    if (selected_stats_ptr != NULL) {
      stats = *selected_stats_ptr;
      ok = true;
    } else {
      if (!collect_wmc_stats(qsop, options, options.encoding, options.preprocess, &stats,
                             &error)) {
        ok = false;
      } else {
        ok = true;
      }
    }
    if (ok) {
      ok = write_wmc_stats(output, &stats, selected_encoding, &error);
    }
  } else {
    ok = qsop_wmc_write(output, qsop, &options, &error);
  }
  if (output != stdout && fclose(output) != 0) {
    fprintf(stderr, "error: %s: %s\n", output_path, strerror(errno));
    qsop_free(qsop);
    return 1;
  }
  qsop_free(qsop);
  if (!ok) {
    print_error(&error, output_path != NULL ? output_path : "<stdout>");
    return 1;
  }
  return 0;
}
