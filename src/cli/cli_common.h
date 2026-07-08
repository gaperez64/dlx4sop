#ifndef DLX4SOP_CLI_COMMON_H
#define DLX4SOP_CLI_COMMON_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

typedef struct dlx4sop_cli_usage_section {
  const char *title;
  const char *const *items;
  size_t nitems;
} dlx4sop_cli_usage_section_t;

bool dlx4sop_cli_is_version_arg(const char *arg);
void dlx4sop_cli_print_version(FILE *file, const char *program);
void dlx4sop_cli_print_usage(FILE *file, const char *usage,
                             const dlx4sop_cli_usage_section_t *sections,
                             size_t nsections);

bool dlx4sop_cli_parse_u32(const char *flag, const char *text, uint32_t *out);
bool dlx4sop_cli_parse_u64(const char *flag, const char *text, uint64_t *out);
bool dlx4sop_cli_parse_double(const char *flag, const char *text, double *out);

#endif
