#include "dlx4sop/qsop.h"
#include "dlx4sop/qsop_wmc.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void print_usage(FILE *file) {
  fputs("usage: sop2wmc [--encoding amp-and|amp-soft|amp-block|residue-fourier|residue-accumulator] "
        "[--wmc-fourier-inner amp-and|amp-soft] "
        "[--wmc-preprocess none|peel1|peel2-safe] [--residue K|all] "
        "[--wmc-block-min-side N] [--wmc-block-min-savings N] "
        "[-o PATH] [--no-metadata] [PATH|-]\n",
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

int main(int argc, char **argv) {
  const char *input_path = NULL;
  const char *output_path = NULL;
  qsop_wmc_options_t options = qsop_wmc_options_default();

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
      if (strcmp(enc, "residue-accumulator") == 0 || strcmp(enc, "residue") == 0) {
        options.encoding = QSOP_WMC_ENCODING_RESIDUE;
      } else if (strcmp(enc, "amp-and") == 0 || strcmp(enc, "amplitude") == 0) {
        options.encoding = QSOP_WMC_ENCODING_AMPLITUDE;
      } else if (strcmp(enc, "amp-soft") == 0) {
        options.encoding = QSOP_WMC_ENCODING_AMP_SOFT;
      } else if (strcmp(enc, "residue-fourier") == 0) {
        options.encoding = QSOP_WMC_ENCODING_RESIDUE_FOURIER;
      } else if (strcmp(enc, "amp-block") == 0) {
        options.encoding = QSOP_WMC_ENCODING_AMP_BLOCK;
      } else {
        fputs("error: --encoding must be 'amp-and', 'amp-soft', 'amp-block', 'residue-fourier', "
              "or 'residue-accumulator'\n",
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

  FILE *output = stdout;
  if (output_path != NULL) {
    output = fopen(output_path, "w");
    if (output == NULL) {
      fprintf(stderr, "error: %s: %s\n", output_path, strerror(errno));
      qsop_free(qsop);
      return 1;
    }
  }

  ok = qsop_wmc_write(output, qsop, &options, &error);
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
