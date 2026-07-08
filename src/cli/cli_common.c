#include "cli_common.h"

#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

#ifndef DLX4SOP_VERSION
#define DLX4SOP_VERSION "unknown"
#endif

bool dlx4sop_cli_is_version_arg(const char *arg) {
  return arg != NULL && strcmp(arg, "--version") == 0;
}

void dlx4sop_cli_print_version(FILE *file, const char *program) {
  fprintf(file, "%s %s\n", program, DLX4SOP_VERSION);
}

void dlx4sop_cli_print_usage(FILE *file, const char *usage,
                             const dlx4sop_cli_usage_section_t *sections,
                             size_t nsections) {
  fputs(usage, file);
  fputc('\n', file);
  for (size_t i = 0; i < nsections; i++) {
    fprintf(file, "\n%s:\n", sections[i].title);
    for (size_t j = 0; j < sections[i].nitems; j++) {
      fprintf(file, "  %s\n", sections[i].items[j]);
    }
  }
}

bool dlx4sop_cli_parse_u32(const char *flag, const char *text, uint32_t *out) {
  if (text == NULL || text[0] == '-' || text[0] == '\0') {
    fprintf(stderr, "error: %s must be a non-negative integer\n", flag);
    return false;
  }
  errno = 0;
  char *end = NULL;
  const unsigned long long value = strtoull(text, &end, 10);
  if (errno != 0 || end == text || *end != '\0' || value > UINT32_MAX) {
    fprintf(stderr, "error: %s must be a non-negative integer\n", flag);
    return false;
  }
  *out = (uint32_t)value;
  return true;
}

bool dlx4sop_cli_parse_u64(const char *flag, const char *text, uint64_t *out) {
  if (text == NULL || text[0] == '-' || text[0] == '\0') {
    fprintf(stderr, "error: %s must be a non-negative integer\n", flag);
    return false;
  }
  errno = 0;
  char *end = NULL;
  const unsigned long long value = strtoull(text, &end, 10);
  if (errno != 0 || end == text || *end != '\0') {
    fprintf(stderr, "error: %s must be a non-negative integer\n", flag);
    return false;
  }
  *out = (uint64_t)value;
  return true;
}

bool dlx4sop_cli_parse_double(const char *flag, const char *text, double *out) {
  if (text == NULL || text[0] == '\0') {
    fprintf(stderr, "error: %s must be a number\n", flag);
    return false;
  }
  errno = 0;
  char *end = NULL;
  const double value = strtod(text, &end);
  if (errno != 0 || end == text || *end != '\0') {
    fprintf(stderr, "error: %s must be a number\n", flag);
    return false;
  }
  *out = value;
  return true;
}
