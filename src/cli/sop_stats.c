#include "dlx4sop/qsop.h"
#include "dlx4sop/qsop_stats.h"

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

typedef enum stats_format {
  STATS_FORMAT_TEXT,
  STATS_FORMAT_JSON,
} stats_format_t;

static void print_usage(FILE *file) {
  fputs("usage: sop-stats [--json|--format text|json] [PATH|-]\n", file);
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

int main(int argc, char **argv) {
  const char *input_path = NULL;
  stats_format_t format = STATS_FORMAT_TEXT;

  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--help") == 0) {
      print_usage(stdout);
      return 0;
    }
    if (strcmp(argv[i], "--json") == 0) {
      format = STATS_FORMAT_JSON;
      continue;
    }
    if (strcmp(argv[i], "--format") == 0) {
      if (i + 1 >= argc) {
        fputs("error: --format requires a value\n", stderr);
        return 2;
      }
      const char *value = argv[++i];
      if (strcmp(value, "text") == 0) {
        format = STATS_FORMAT_TEXT;
      } else if (strcmp(value, "json") == 0) {
        format = STATS_FORMAT_JSON;
      } else {
        fprintf(stderr, "error: unsupported format '%s'\n", value);
        return 2;
      }
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

  qsop_stats_t stats = {0};
  ok = qsop_compute_stats(qsop, &stats, &error);
  qsop_free(qsop);
  if (!ok) {
    print_error(&error, diagnostic_path);
    return 1;
  }

  if (format == STATS_FORMAT_JSON) {
    ok = qsop_stats_write_json(stdout, &stats, &error);
  } else {
    ok = qsop_stats_write_text(stdout, &stats, &error);
  }
  if (!ok) {
    print_error(&error, "<stdout>");
    return 1;
  }

  return 0;
}
