#define _GNU_SOURCE

#include "dlx4sop/qsop.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

static FILE *open_bytes(const uint8_t *data, size_t size) {
  if (size == 0) {
    return NULL;
  }
  return fmemopen((void *)data, size, "rb");
}

static bool parse_bytes(const uint8_t *data, size_t size, qsop_instance_t **out) {
  qsop_error_t error = {0};
  FILE *file = open_bytes(data, size);
  if (file == NULL) {
    return false;
  }
  const bool ok = qsop_parse_file(file, "<fuzz>", out, &error);
  fclose(file);
  return ok;
}

static char *write_canonical(const qsop_instance_t *qsop, size_t *out_len) {
  char *text = NULL;
  size_t len = 0;
  qsop_error_t error = {0};
  FILE *file = open_memstream(&text, &len);
  if (file == NULL) {
    abort();
  }
  if (!qsop_write_file(file, qsop, &error)) {
    fclose(file);
    free(text);
    abort();
  }
  if (fclose(file) != 0) {
    free(text);
    abort();
  }
  *out_len = len;
  return text;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  qsop_instance_t *first = NULL;
  if (!parse_bytes(data, size, &first)) {
    return 0;
  }

  size_t canonical_len = 0;
  char *canonical = write_canonical(first, &canonical_len);
  qsop_free(first);

  qsop_instance_t *second = NULL;
  if (!parse_bytes((const uint8_t *)canonical, canonical_len, &second)) {
    free(canonical);
    abort();
  }

  size_t canonical_again_len = 0;
  char *canonical_again = write_canonical(second, &canonical_again_len);
  qsop_free(second);

  if (canonical_len != canonical_again_len ||
      memcmp(canonical, canonical_again, canonical_len) != 0) {
    free(canonical);
    free(canonical_again);
    abort();
  }

  free(canonical);
  free(canonical_again);
  return 0;
}

#ifndef DLX4SOP_LIBFUZZER
static uint8_t *read_all(FILE *file, size_t *out_len) {
  size_t cap = 4096;
  size_t len = 0;
  uint8_t *data = malloc(cap);
  if (data == NULL) {
    return NULL;
  }

  for (;;) {
    if (len == cap) {
      if (cap > SIZE_MAX / 2U) {
        free(data);
        return NULL;
      }
      cap *= 2U;
      uint8_t *next = realloc(data, cap);
      if (next == NULL) {
        free(data);
        return NULL;
      }
      data = next;
    }

    const size_t nread = fread(data + len, 1, cap - len, file);
    len += nread;
    if (nread == 0) {
      if (ferror(file)) {
        free(data);
        return NULL;
      }
      break;
    }
  }

  *out_len = len;
  return data;
}

static int run_path(const char *path) {
  FILE *file = strcmp(path, "-") == 0 ? stdin : fopen(path, "rb");
  if (file == NULL) {
    perror(path);
    return 1;
  }

  size_t len = 0;
  uint8_t *data = read_all(file, &len);
  if (file != stdin) {
    fclose(file);
  }
  if (data == NULL) {
    fprintf(stderr, "%s: failed to read input\n", path);
    return 1;
  }

  LLVMFuzzerTestOneInput(data, len);
  free(data);
  return 0;
}

int main(int argc, char **argv) {
  if (argc == 1) {
    return run_path("-");
  }

  for (int i = 1; i < argc; i++) {
    if (run_path(argv[i]) != 0) {
      return 1;
    }
  }
  return 0;
}
#endif
