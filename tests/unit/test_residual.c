#include "dlx4sop/residual.h"

#include <stdio.h>
#include <stdlib.h>

static int expect_u32(const char *name, uint32_t actual, uint32_t expected) {
  if (actual != expected) {
    fprintf(stderr, "%s: expected %u got %u\n", name, expected, actual);
    return 1;
  }
  return 0;
}

static int expect_bool(const char *name, bool actual, bool expected) {
  if (actual != expected) {
    fprintf(stderr, "%s: expected %s got %s\n", name, expected ? "true" : "false",
            actual ? "true" : "false");
    return 1;
  }
  return 0;
}

static qsop_instance_t fixture_instance(void) {
  static uint32_t unary[] = {1, 2, 5};
  static uint32_t edge_u[] = {0, 1};
  static uint32_t edge_v[] = {1, 2};
  static uint32_t edge_q[] = {4, 3};

  return (qsop_instance_t){
      .r = 8,
      .nvars = 3,
      .norm_h = 4,
      .constant = 7,
      .mode = QSOP_MODE_LABELLED,
      .unary = unary,
      .nedges = 2,
      .edge_u = edge_u,
      .edge_v = edge_v,
      .edge_q = edge_q,
  };
}

static int expect_initial_state(qsop_residual_t *residual) {
  if (expect_u32("modulus", qsop_residual_modulus(residual), 8) != 0 ||
      expect_u32("nvars", qsop_residual_nvars(residual), 3) != 0 ||
      expect_u32("nedges", qsop_residual_nedges(residual), 2) != 0 ||
      expect_u32("active_vars", qsop_residual_active_vars(residual), 3) != 0 ||
      expect_u32("active_edges", qsop_residual_active_edges(residual), 2) != 0 ||
      expect_u32("constant", qsop_residual_constant(residual), 7) != 0 ||
      expect_u32("unary0", qsop_residual_unary(residual, 0), 1) != 0 ||
      expect_u32("unary1", qsop_residual_unary(residual, 1), 2) != 0 ||
      expect_u32("unary2", qsop_residual_unary(residual, 2), 5) != 0 ||
      expect_u32("degree0", qsop_residual_active_degree(residual, 0), 1) != 0 ||
      expect_u32("degree1", qsop_residual_active_degree(residual, 1), 2) != 0 ||
      expect_u32("degree2", qsop_residual_active_degree(residual, 2), 1) != 0 ||
      expect_u32("degree3", qsop_residual_active_degree(residual, 3), 0) != 0 ||
      expect_bool("var0", qsop_residual_var_active(residual, 0), true) != 0 ||
      expect_bool("var1", qsop_residual_var_active(residual, 1), true) != 0 ||
      expect_bool("var2", qsop_residual_var_active(residual, 2), true) != 0 ||
      expect_bool("edge0", qsop_residual_edge_active(residual, 0), true) != 0 ||
      expect_bool("edge1", qsop_residual_edge_active(residual, 1), true) != 0) {
    return 1;
  }
  return 0;
}

static int test_branch_zero_undo(void) {
  qsop_error_t error = {0};
  qsop_instance_t qsop = fixture_instance();
  qsop_residual_t *residual = NULL;
  if (!qsop_residual_create(&qsop, &residual, &error)) {
    fprintf(stderr, "create failed: %s\n", error.message);
    return 1;
  }

  const size_t checkpoint = qsop_residual_checkpoint(residual);
  if (!qsop_residual_branch(residual, 1, 0, &error)) {
    fprintf(stderr, "branch zero failed: %s\n", error.message);
    qsop_residual_free(residual);
    return 1;
  }
  if (expect_u32("zero active_vars", qsop_residual_active_vars(residual), 2) != 0 ||
      expect_u32("zero active_edges", qsop_residual_active_edges(residual), 0) != 0 ||
      expect_u32("zero constant", qsop_residual_constant(residual), 7) != 0 ||
      expect_u32("zero unary0", qsop_residual_unary(residual, 0), 1) != 0 ||
      expect_u32("zero unary2", qsop_residual_unary(residual, 2), 5) != 0 ||
      expect_u32("zero degree0", qsop_residual_active_degree(residual, 0), 0) != 0 ||
      expect_u32("zero degree1", qsop_residual_active_degree(residual, 1), 0) != 0 ||
      expect_u32("zero degree2", qsop_residual_active_degree(residual, 2), 0) != 0 ||
      expect_bool("zero var1", qsop_residual_var_active(residual, 1), false) != 0 ||
      expect_bool("zero edge0", qsop_residual_edge_active(residual, 0), false) != 0 ||
      expect_bool("zero edge1", qsop_residual_edge_active(residual, 1), false) != 0) {
    qsop_residual_free(residual);
    return 1;
  }

  if (!qsop_residual_undo(residual, checkpoint, &error)) {
    fprintf(stderr, "undo zero failed: %s\n", error.message);
    qsop_residual_free(residual);
    return 1;
  }
  const int result = expect_initial_state(residual);
  qsop_residual_free(residual);
  return result;
}

static int test_branch_one_undo(void) {
  qsop_error_t error = {0};
  qsop_instance_t qsop = fixture_instance();
  qsop_residual_t *residual = NULL;
  if (!qsop_residual_create(&qsop, &residual, &error)) {
    fprintf(stderr, "create failed: %s\n", error.message);
    return 1;
  }

  const size_t checkpoint = qsop_residual_checkpoint(residual);
  if (!qsop_residual_branch(residual, 1, 1, &error)) {
    fprintf(stderr, "branch one failed: %s\n", error.message);
    qsop_residual_free(residual);
    return 1;
  }
  if (expect_u32("one active_vars", qsop_residual_active_vars(residual), 2) != 0 ||
      expect_u32("one active_edges", qsop_residual_active_edges(residual), 0) != 0 ||
      expect_u32("one constant", qsop_residual_constant(residual), 1) != 0 ||
      expect_u32("one unary0", qsop_residual_unary(residual, 0), 5) != 0 ||
      expect_u32("one unary2", qsop_residual_unary(residual, 2), 0) != 0 ||
      expect_u32("one degree0", qsop_residual_active_degree(residual, 0), 0) != 0 ||
      expect_u32("one degree1", qsop_residual_active_degree(residual, 1), 0) != 0 ||
      expect_u32("one degree2", qsop_residual_active_degree(residual, 2), 0) != 0 ||
      expect_bool("one var1", qsop_residual_var_active(residual, 1), false) != 0 ||
      expect_bool("one edge0", qsop_residual_edge_active(residual, 0), false) != 0 ||
      expect_bool("one edge1", qsop_residual_edge_active(residual, 1), false) != 0) {
    qsop_residual_free(residual);
    return 1;
  }

  if (!qsop_residual_undo(residual, checkpoint, &error)) {
    fprintf(stderr, "undo one failed: %s\n", error.message);
    qsop_residual_free(residual);
    return 1;
  }
  const int result = expect_initial_state(residual);
  qsop_residual_free(residual);
  return result;
}

static int test_nested_undo(void) {
  qsop_error_t error = {0};
  qsop_instance_t qsop = fixture_instance();
  qsop_residual_t *residual = NULL;
  if (!qsop_residual_create(&qsop, &residual, &error)) {
    fprintf(stderr, "create failed: %s\n", error.message);
    return 1;
  }

  const size_t root = qsop_residual_checkpoint(residual);
  if (!qsop_residual_branch(residual, 0, 1, &error)) {
    fprintf(stderr, "branch first failed: %s\n", error.message);
    qsop_residual_free(residual);
    return 1;
  }
  const size_t after_first = qsop_residual_checkpoint(residual);
  if (!qsop_residual_branch(residual, 2, 1, &error)) {
    fprintf(stderr, "branch second failed: %s\n", error.message);
    qsop_residual_free(residual);
    return 1;
  }

  if (!qsop_residual_undo(residual, after_first, &error)) {
    fprintf(stderr, "nested partial undo failed: %s\n", error.message);
    qsop_residual_free(residual);
    return 1;
  }
  if (expect_u32("nested partial active_vars", qsop_residual_active_vars(residual), 2) != 0 ||
      expect_bool("nested var0", qsop_residual_var_active(residual, 0), false) != 0 ||
      expect_bool("nested var2", qsop_residual_var_active(residual, 2), true) != 0) {
    qsop_residual_free(residual);
    return 1;
  }

  if (!qsop_residual_undo(residual, root, &error)) {
    fprintf(stderr, "nested root undo failed: %s\n", error.message);
    qsop_residual_free(residual);
    return 1;
  }
  const int result = expect_initial_state(residual);
  qsop_residual_free(residual);
  return result;
}

static int test_invalid_branch(void) {
  qsop_error_t error = {0};
  qsop_instance_t qsop = fixture_instance();
  qsop_residual_t *residual = NULL;
  if (!qsop_residual_create(&qsop, &residual, &error)) {
    fprintf(stderr, "create failed: %s\n", error.message);
    return 1;
  }

  if (qsop_residual_branch(residual, 3, 0, &error)) {
    fprintf(stderr, "out-of-range branch unexpectedly succeeded\n");
    qsop_residual_free(residual);
    return 1;
  }
  if (qsop_residual_branch(residual, 0, 2, &error)) {
    fprintf(stderr, "bad-value branch unexpectedly succeeded\n");
    qsop_residual_free(residual);
    return 1;
  }
  if (!qsop_residual_branch(residual, 0, 0, &error)) {
    fprintf(stderr, "valid branch failed: %s\n", error.message);
    qsop_residual_free(residual);
    return 1;
  }
  if (qsop_residual_branch(residual, 0, 0, &error)) {
    fprintf(stderr, "inactive branch unexpectedly succeeded\n");
    qsop_residual_free(residual);
    return 1;
  }

  qsop_residual_free(residual);
  return 0;
}

int main(void) {
  if (test_branch_zero_undo() != 0) {
    return 1;
  }
  if (test_branch_one_undo() != 0) {
    return 1;
  }
  if (test_nested_undo() != 0) {
    return 1;
  }
  if (test_invalid_branch() != 0) {
    return 1;
  }
  return 0;
}
