#include "dlx4sop/qsop.h"
#include "dlx4sop/qsop_solve.h"

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void print_usage(FILE *file) {
  fputs("usage: sop-solve [--format residue-vector] [--max-vars N] [PATH|-]\n", file);
}

static void print_error(const qsop_error_t *error, const char *fallback_path) {
  const char *path = error->path != NULL ? error->path : fallback_path;
  if (path == NULL) {
    path = "<input>";
  }
  if (error->line > 0) {
    fprintf(stderr, "error: %s:%zu:%zu: %s\n", path, error->line, error->column,
            error->message);
  } else {
    fprintf(stderr, "error: %s: %s\n", path, error->message);
  }
}

static bool parse_max_vars(const char *text, uint32_t *out) {
  if (text == NULL || text[0] == '-' || text[0] == '\0') {
    return false;
  }

  errno = 0;
  char *end = NULL;
  unsigned long value = strtoul(text, &end, 10);
  if (errno != 0 || end == text || *end != '\0' || value > UINT32_MAX) {
    return false;
  }

  *out = (uint32_t)value;
  return true;
}

int main(int argc, char **argv) {
  const char *input_path = NULL;
  uint32_t max_vars = 24;

  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--help") == 0) {
      print_usage(stdout);
      return 0;
    }
    if (strcmp(argv[i], "--format") == 0) {
      if (i + 1 >= argc) {
        fputs("error: --format requires a value\n", stderr);
        return 2;
      }
      const char *format = argv[++i];
      if (strcmp(format, "residue-vector") != 0) {
        fprintf(stderr, "error: unsupported format '%s'\n", format);
        return 2;
      }
      continue;
    }
    if (strcmp(argv[i], "--max-vars") == 0) {
      if (i + 1 >= argc || !parse_max_vars(argv[i + 1], &max_vars)) {
        fputs("error: --max-vars requires a non-negative uint32 value\n", stderr);
        return 2;
      }
      i++;
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

  qsop_result_t *result = NULL;
  ok = qsop_solve_bruteforce(qsop, max_vars, &result, &error);
  qsop_free(qsop);
  if (!ok) {
    print_error(&error, diagnostic_path);
    return 1;
  }

  ok = qsop_result_write_residue_vector(stdout, result, &error);
  qsop_result_free(result);
  if (!ok) {
    print_error(&error, "<stdout>");
    return 1;
  }

  return 0;
}
