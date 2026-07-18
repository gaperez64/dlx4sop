/* A left-deep rank decomposition can be as deep as the number of variables. Validation must use
 * an explicit work stack: recursive validation overflowed the 8 MiB process stack on real imported
 * circuits with more than 100,000 variables. Run the generator on a deliberately small thread
 * stack so the regression stays cheap enough for the ordinary and Valgrind unit suites. */
#include "dlx4sop/qsop.h"
#include "dlx4sop/qsop_solve.h"

#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#define STRESS_VARS 4096U
#define STRESS_STACK_BYTES (256U * 1024U)

typedef struct validation_stress_result {
  bool ok;
  char error[256];
} validation_stress_result_t;

static void *generate_left_deep(void *opaque) {
  validation_stress_result_t *result = opaque;
  const qsop_instance_t qsop = {
      .r = 8U,
      .nvars = STRESS_VARS,
  };
  qsop_rankwidth_decomposition_t *decomposition = NULL;
  qsop_error_t error = {0};
  result->ok = qsop_rankwidth_decomposition_generate(
      &qsop, QSOP_RANKWIDTH_GENERATOR_LEFT_DEEP, &decomposition, &error);
  if (!result->ok) {
    snprintf(result->error, sizeof(result->error), "%s", error.message);
  }
  qsop_rankwidth_decomposition_free(decomposition);
  return NULL;
}

int main(void) {
  pthread_attr_t attr;
  int rc = pthread_attr_init(&attr);
  if (rc != 0) {
    fprintf(stderr, "pthread_attr_init failed: %s\n", strerror(rc));
    return 1;
  }
  rc = pthread_attr_setstacksize(&attr, STRESS_STACK_BYTES);
  if (rc != 0) {
    fprintf(stderr, "pthread_attr_setstacksize failed: %s\n", strerror(rc));
    pthread_attr_destroy(&attr);
    return 1;
  }

  validation_stress_result_t result = {0};
  pthread_t thread;
  rc = pthread_create(&thread, &attr, generate_left_deep, &result);
  pthread_attr_destroy(&attr);
  if (rc != 0) {
    fprintf(stderr, "pthread_create failed: %s\n", strerror(rc));
    return 1;
  }
  rc = pthread_join(thread, NULL);
  if (rc != 0) {
    fprintf(stderr, "pthread_join failed: %s\n", strerror(rc));
    return 1;
  }
  if (!result.ok) {
    fprintf(stderr, "left-deep decomposition generation failed: %s\n", result.error);
    return 1;
  }

  fprintf(stderr, "rankwidth decomposition validation stress passed\n");
  return 0;
}
