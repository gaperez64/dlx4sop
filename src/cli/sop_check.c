#include "dlx4sop/qsop.h"
#include "cli_common.h"

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void print_usage(FILE *file) {
  static const char *const core[] = {
      "--quiet",
      "--output PATH",
      "--version",
      "--help",
      "PATH|-",
  };
  static const dlx4sop_cli_usage_section_t sections[] = {
      {.title = "Options", .items = core, .nitems = sizeof(core) / sizeof(core[0])},
  };
  dlx4sop_cli_print_usage(file, "usage: sop-check [--quiet] [--output PATH] [PATH|-]",
                          sections, sizeof(sections) / sizeof(sections[0]));
}

static void print_error(const qsop_error_t *error) {
  const char *path = error->path == NULL ? "<input>" : error->path;
  if (error->line > 0) {
    fprintf(stderr, "error: %s:%zu:%zu: %s\n", path, error->line, error->column,
            error->message);
  } else {
    fprintf(stderr, "error: %s: %s\n", path, error->message);
  }
}

int main(int argc, char **argv) {
  const char *input_path = NULL;
  const char *output_path = NULL;
  bool quiet = false;

  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--help") == 0) {
      print_usage(stdout);
      return 0;
    }
    if (dlx4sop_cli_is_version_arg(argv[i])) {
      dlx4sop_cli_print_version(stdout, "sop-check");
      return 0;
    }
    if (strcmp(argv[i], "--quiet") == 0) {
      quiet = true;
      continue;
    }
    if (strcmp(argv[i], "--output") == 0) {
      if (i + 1 >= argc) {
        fputs("error: --output requires a path\n", stderr);
        return 2;
      }
      output_path = argv[++i];
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
    print_error(&error);
    return 1;
  }

  if (!quiet) {
    FILE *output = stdout;
    if (output_path != NULL) {
      output = fopen(output_path, "w");
      if (output == NULL) {
        fprintf(stderr, "error: %s: %s\n", output_path, strerror(errno));
        qsop_free(qsop);
        return 1;
      }
    }

    ok = qsop_write_file(output, qsop, &error);
    if (output != stdout && fclose(output) != 0) {
      fprintf(stderr, "error: %s: %s\n", output_path, strerror(errno));
      qsop_free(qsop);
      return 1;
    }
    if (!ok) {
      print_error(&error);
      qsop_free(qsop);
      return 1;
    }
  }

  qsop_free(qsop);
  return 0;
}
