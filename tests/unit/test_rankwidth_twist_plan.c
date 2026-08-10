/* Unit tests for the twist-diagonalized join machinery in rankwidth_join_twist.c.
 *
 * The heart of the twist kernel is algebra, and each algebraic claim is checked here against
 * a naive oracle: (1) the crossing-form rank factorization read off child signatures alone
 * must reproduce the pairwise crossing parity a^T A[X_L,X_R] b for random graphs and
 * assignments, with the plan's crossing rank agreeing with rw_cut_rank_bitsets; (2) the
 * WHT butterflies must satisfy the involution WHT(WHT(x)) = 2^n x and the full
 * bin/transform/contract pipeline must reproduce a naively-summed twisted join; (3) the
 * modular count-WHT must reproduce naive XOR-convolution support counts; (4) the plan build
 * must refuse (TOO_LARGE) once p + c exceeds the requested cap. */
#include "dlx4sop/bitset.h"
#include "dlx4sop/qsop.h"

#include "../../src/solve/rankwidth_internal.h"

#include <inttypes.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TP_MAX_VARS 24U
#define TP_TRIALS 200U
#define TP_ASSIGNMENTS 24U

static uint64_t xorshift(uint64_t *state) {
  uint64_t x = *state;
  x ^= x << 13;
  x ^= x >> 7;
  x ^= x << 17;
  *state = x;
  return x;
}

/* Build a random symmetric adjacency over nvars <= 64 (single word), no self loops. */
static void random_adjacency(uint64_t *state, uint64_t *adj, uint32_t nvars, size_t words,
                             uint32_t density_percent) {
  memset(adj, 0, (size_t)nvars * words * sizeof(*adj));
  for (uint32_t v = 0; v < nvars; v++) {
    for (uint32_t u = v + 1U; u < nvars; u++) {
      if (xorshift(state) % 100U < density_percent) {
        qsop_bitset_set(qsop_bitset_row(adj, words, v), u);
        qsop_bitset_set(qsop_bitset_row(adj, words, u), v);
      }
    }
  }
}

/* XOR of the adjacency rows selected by assignment, masked to keep, as a one-word signature. */
static uint64_t signature_bits_of(const uint64_t *adj, size_t words, uint32_t nvars,
                                  uint64_t assignment, uint64_t keep_mask) {
  uint64_t bits = 0;
  for (uint32_t v = 0; v < nvars; v++) {
    if ((assignment >> v) & 1U) {
      bits ^= qsop_bitset_const_row(adj, words, v)[0];
    }
  }
  return bits & keep_mask;
}

static uint32_t naive_crossing_parity(const uint64_t *adj, size_t words, uint32_t nvars,
                                      uint64_t left_assignment, uint64_t right_assignment) {
  uint32_t parity = 0;
  for (uint32_t v = 0; v < nvars; v++) {
    if ((left_assignment >> v) & 1U) {
      parity ^= qsop_popcount_u64(qsop_bitset_const_row(adj, words, v)[0] & right_assignment) & 1U;
    }
  }
  return parity;
}

/* Factorization oracle: the signature-only twist coordinates must reproduce the pairwise
 * crossing parity, and the plan's crossing rank must equal the cut rank of A[X_L, X_R]. */
static int test_factorization(uint64_t *state) {
  int rc = 0;
  for (uint32_t trial = 0; trial < TP_TRIALS && rc == 0; trial++) {
    const uint32_t nvars = 8U + (uint32_t)(xorshift(state) % (TP_MAX_VARS - 8U + 1U));
    const size_t words = qsop_bitset_words(nvars);
    const uint64_t all_mask = nvars == 64U ? UINT64_MAX : (UINT64_C(1) << nvars) - 1U;
    uint64_t adj[TP_MAX_VARS];
    random_adjacency(state, adj, nvars, words, 20U + (uint32_t)(xorshift(state) % 70U));

    /* Random disjoint X_L / X_R; the rest is the parent's outside. */
    uint64_t left_vars = 0;
    uint64_t right_vars = 0;
    for (uint32_t v = 0; v < nvars; v++) {
      const uint64_t coin = xorshift(state) % 3U;
      if (coin == 0U) {
        left_vars |= UINT64_C(1) << v;
      } else if (coin == 1U) {
        right_vars |= UINT64_C(1) << v;
      }
    }
    if (left_vars == 0 || right_vars == 0) {
      continue;
    }
    const uint64_t outside = all_mask & ~(left_vars | right_vars);

    qsop_error_t error = {0};
    rw_signature_pool_t pool = {0};
    if (!rw_signature_pool_init(&pool, words, &error)) {
      fprintf(stderr, "factorization: pool init failed: %s\n", error.message);
      return 1;
    }

    uint64_t left_assignments[TP_ASSIGNMENTS];
    uint64_t right_assignments[TP_ASSIGNMENTS];
    uint32_t left_signatures[TP_ASSIGNMENTS];
    uint32_t right_signatures[TP_ASSIGNMENTS];
    bool intern_failed = false;
    for (uint32_t i = 0; i < TP_ASSIGNMENTS; i++) {
      left_assignments[i] = xorshift(state) & left_vars;
      right_assignments[i] = xorshift(state) & right_vars;
      const uint64_t left_sig =
          signature_bits_of(adj, words, nvars, left_assignments[i], all_mask & ~left_vars);
      const uint64_t right_sig =
          signature_bits_of(adj, words, nvars, right_assignments[i], all_mask & ~right_vars);
      if (!rw_signature_pool_intern(&pool, &left_sig, &left_signatures[i], &error) ||
          !rw_signature_pool_intern(&pool, &right_sig, &right_signatures[i], &error)) {
        fprintf(stderr, "factorization: intern failed: %s\n", error.message);
        intern_failed = true;
        break;
      }
    }
    if (intern_failed) {
      rw_signature_pool_free(&pool);
      return 1;
    }

    rw_twist_plan_t plan = {0};
    const rw_twist_feasibility_t status = rw_twist_plan_build(
        nvars, adj, &pool, left_signatures, TP_ASSIGNMENTS, right_signatures, TP_ASSIGNMENTS,
        &left_vars, &right_vars, &outside, words, true, RW_TWIST_MAX_DIM, &plan, &error);
    if (status != RW_TWIST_FEASIBLE) {
      fprintf(stderr, "factorization: plan build refused (status %d) at nvars=%" PRIu32 "\n",
              (int)status, nvars);
      rw_signature_pool_free(&pool);
      return 1;
    }

    const uint32_t cut_rank =
        rw_cut_rank_bitsets(nvars, adj, &left_vars, &right_vars, words, NULL, &error);
    if (plan.c != cut_rank) {
      fprintf(stderr, "factorization: plan crossing rank %" PRIu32 " != cut rank %" PRIu32 "\n",
              plan.c, cut_rank);
      rc = 1;
    }

    uint64_t scratch = 0;
    for (uint32_t i = 0; i < TP_ASSIGNMENTS && rc == 0; i++) {
      for (uint32_t j = 0; j < TP_ASSIGNMENTS && rc == 0; j++) {
        const uint64_t left_bits = rw_signature_bits(&pool, left_signatures[i])[0];
        const uint64_t right_bits = rw_signature_bits(&pool, right_signatures[j])[0];
        uint64_t masked = left_bits & right_vars;
        uint64_t s = 0;
        if (!rw_dense_basis_coord(&plan.cross_basis, &masked, &scratch, &s, &error)) {
          fprintf(stderr, "factorization: left twist coordinate outside cross basis: %s\n",
                  error.message);
          rc = 1;
          break;
        }
        uint64_t t = 0;
        for (uint32_t jb = 0; jb < plan.c; jb++) {
          if ((right_bits >> plan.gen_vertex[jb]) & 1U) {
            t |= UINT64_C(1) << jb;
          }
        }
        const uint32_t factored = qsop_popcount_u64(s & t) & 1U;
        const uint32_t naive =
            naive_crossing_parity(adj, words, nvars, left_assignments[i], right_assignments[j]);
        if (factored != naive) {
          fprintf(stderr,
                  "factorization: parity mismatch (trial %" PRIu32 ", i=%" PRIu32 ", j=%" PRIu32
                  "): factored %" PRIu32 " naive %" PRIu32 "\n",
                  trial, i, j, factored, naive);
          rc = 1;
        }

        /* Parent-coordinate linearity: coord(a) ^ coord(b) == coord(a ^ b) on outside. */
        uint64_t parent_masked = left_bits & outside;
        uint64_t w1 = 0;
        uint64_t w2 = 0;
        uint64_t w12 = 0;
        uint64_t xor_masked = (left_bits ^ right_bits) & outside;
        uint64_t right_masked = right_bits & outside;
        if (!rw_dense_basis_coord(&plan.parent_basis, &parent_masked, &scratch, &w1, &error) ||
            !rw_dense_basis_coord(&plan.parent_basis, &right_masked, &scratch, &w2, &error) ||
            !rw_dense_basis_coord(&plan.parent_basis, &xor_masked, &scratch, &w12, &error)) {
          fprintf(stderr, "factorization: parent coordinate outside basis: %s\n", error.message);
          rc = 1;
          break;
        }
        if ((w1 ^ w2) != w12) {
          fprintf(stderr, "factorization: parent coordinate not linear\n");
          rc = 1;
        }
      }
    }

    /* Untwisted build: no cross basis, no generators. */
    rw_twist_plan_t even_plan = {0};
    if (rw_twist_plan_build(nvars, adj, &pool, left_signatures, TP_ASSIGNMENTS, right_signatures,
                            TP_ASSIGNMENTS, &left_vars, &right_vars, &outside, words, false,
                            RW_TWIST_MAX_DIM, &even_plan, &error) != RW_TWIST_FEASIBLE) {
      fprintf(stderr, "factorization: even-mode plan build refused\n");
      rc = 1;
    } else if (even_plan.c != 0 || even_plan.gen_vertex != NULL) {
      fprintf(stderr, "factorization: even-mode plan has a twist axis\n");
      rc = 1;
    }
    rw_twist_plan_free(&even_plan);
    rw_twist_plan_free(&plan);
    rw_signature_pool_free(&pool);
  }
  return rc;
}

static long double random_unit(uint64_t *state) {
  return (long double)(int64_t)(xorshift(state) % 2001U) / 1000.0L - 1.0L;
}

/* WHT involution: transforming twice along an axis multiplies by the axis length. */
static int test_wht_involution(uint64_t *state) {
  for (uint32_t p = 0; p <= 4U; p++) {
    for (uint32_t c = 0; c <= 3U; c++) {
      const size_t len = (size_t)1 << (p + c);
      long double re[1U << 7U];
      long double im[1U << 7U];
      long double ref_re[1U << 7U];
      long double ref_im[1U << 7U];
      for (size_t i = 0; i < len; i++) {
        re[i] = ref_re[i] = random_unit(state);
        im[i] = ref_im[i] = random_unit(state);
      }
      rw_twist_wht_rows_l(re, im, p, c);
      rw_twist_wht_rows_l(re, im, p, c);
      rw_twist_wht_cols_l(re, im, p, c);
      rw_twist_wht_cols_l(re, im, p, c);
      const long double scale = (long double)((size_t)1 << p) * (long double)((size_t)1 << c);
      for (size_t i = 0; i < len; i++) {
        if (fabsl(re[i] - scale * ref_re[i]) > 1e-12L ||
            fabsl(im[i] - scale * ref_im[i]) > 1e-12L) {
          fprintf(stderr, "wht involution failed at p=%" PRIu32 " c=%" PRIu32 "\n", p, c);
          return 1;
        }
      }

      double re64[1U << 7U];
      double im64[1U << 7U];
      for (size_t i = 0; i < len; i++) {
        re64[i] = (double)ref_re[i];
        im64[i] = (double)ref_im[i];
      }
      rw_twist_wht_rows_f64(re64, im64, p, c);
      rw_twist_wht_rows_f64(re64, im64, p, c);
      rw_twist_wht_cols_f64(re64, im64, p, c);
      rw_twist_wht_cols_f64(re64, im64, p, c);
      for (size_t i = 0; i < len; i++) {
        if (fabs(re64[i] - (double)(scale * ref_re[i])) > 1e-9 ||
            fabs(im64[i] - (double)(scale * ref_im[i])) > 1e-9) {
          fprintf(stderr, "wht f64 involution failed at p=%" PRIu32 " c=%" PRIu32 "\n", p, c);
          return 1;
        }
      }
    }
  }
  return 0;
}

/* The exact transform pipeline of the kernel, against a naively-summed twisted join:
 * H(w) = sum over w1 ^ w2 == w, t, s of Phi[w1][t] * Gamma[w2][s] * (-1)^<t,s>. */
static int test_twisted_pipeline(uint64_t *state) {
  for (uint32_t p = 0; p <= 4U; p++) {
    for (uint32_t c = 0; c <= 3U; c++) {
      const size_t n_p = (size_t)1 << p;
      const size_t n_c = (size_t)1 << c;
      const size_t len = n_p * n_c;
      long double phi_re[1U << 7U];
      long double phi_im[1U << 7U];
      long double gam_re[1U << 7U];
      long double gam_im[1U << 7U];
      long double naive_re[1U << 4U];
      long double naive_im[1U << 4U];
      for (size_t i = 0; i < len; i++) {
        phi_re[i] = random_unit(state);
        phi_im[i] = random_unit(state);
        gam_re[i] = random_unit(state);
        gam_im[i] = random_unit(state);
      }
      for (size_t w = 0; w < n_p; w++) {
        naive_re[w] = 0.0L;
        naive_im[w] = 0.0L;
        for (size_t w1 = 0; w1 < n_p; w1++) {
          const size_t w2 = w1 ^ w;
          for (size_t t = 0; t < n_c; t++) {
            for (size_t s = 0; s < n_c; s++) {
              const long double sign =
                  (qsop_popcount_u64((uint64_t)(t & s)) & 1U) != 0U ? -1.0L : 1.0L;
              const long double fre = phi_re[(w1 << c) | t];
              const long double fim = phi_im[(w1 << c) | t];
              const long double gre = gam_re[(w2 << c) | s];
              const long double gim = gam_im[(w2 << c) | s];
              naive_re[w] += sign * (fre * gre - fim * gim);
              naive_im[w] += sign * (fre * gim + fim * gre);
            }
          }
        }
      }

      /* Kernel pipeline: WHT the twist axis of Gamma, WHT the parent axis of both, pointwise
       * multiply and contract the twist axis, inverse-WHT the parent axis, scale by 2^-p. */
      rw_twist_wht_cols_l(gam_re, gam_im, p, c);
      rw_twist_wht_rows_l(phi_re, phi_im, p, c);
      rw_twist_wht_rows_l(gam_re, gam_im, p, c);
      long double acc_re[1U << 4U];
      long double acc_im[1U << 4U];
      for (size_t w = 0; w < n_p; w++) {
        acc_re[w] = 0.0L;
        acc_im[w] = 0.0L;
        for (size_t t = 0; t < n_c; t++) {
          const size_t index = (w << c) | t;
          acc_re[w] += phi_re[index] * gam_re[index] - phi_im[index] * gam_im[index];
          acc_im[w] += phi_re[index] * gam_im[index] + phi_im[index] * gam_re[index];
        }
      }
      rw_twist_wht_rows_l(acc_re, acc_im, p, 0U);
      const long double scale = ldexpl(1.0L, -(int)p);
      for (size_t w = 0; w < n_p; w++) {
        acc_re[w] *= scale;
        acc_im[w] *= scale;
        if (fabsl(acc_re[w] - naive_re[w]) > 1e-9L || fabsl(acc_im[w] - naive_im[w]) > 1e-9L) {
          fprintf(stderr, "twisted pipeline mismatch at p=%" PRIu32 " c=%" PRIu32 " w=%zu\n", p, c,
                  w);
          return 1;
        }
      }
    }
  }
  return 0;
}

static int test_reach_counts(uint64_t *state) {
  for (uint32_t p = 0; p <= 8U; p++) {
    const size_t len = (size_t)1 << p;
    uint64_t occ_left[(1U << 8U) / 64U + 1U] = {0};
    uint64_t occ_right[(1U << 8U) / 64U + 1U] = {0};
    for (size_t i = 0; i < len; i++) {
      if (xorshift(state) % 3U == 0U) {
        qsop_bitset_set(occ_left, (uint32_t)i);
      }
      if (xorshift(state) % 3U == 0U) {
        qsop_bitset_set(occ_right, (uint32_t)i);
      }
    }
    uint64_t work[2U * (1U << 8U)];
    uint64_t counts[1U << 8U];
    qsop_error_t error = {0};
    if (!rw_twist_reach_counts(occ_left, occ_right, p, work, counts, &error)) {
      fprintf(stderr, "reach counts failed: %s\n", error.message);
      return 1;
    }
    for (size_t w = 0; w < len; w++) {
      uint64_t naive = 0;
      for (size_t w1 = 0; w1 < len; w1++) {
        if (qsop_bitset_get(occ_left, (uint32_t)w1) &&
            qsop_bitset_get(occ_right, (uint32_t)(w1 ^ w))) {
          naive++;
        }
      }
      if (counts[w] != naive) {
        fprintf(stderr, "reach counts mismatch at p=%" PRIu32 " w=%zu: %" PRIu64 " != %" PRIu64
                        "\n",
                p, w, counts[w], naive);
        return 1;
      }
    }
  }
  return 0;
}

/* A 3-edge perfect matching between the shores has crossing rank 3: a cap of 2 must refuse,
 * and a parent basis wider than max_dim - c must refuse via the joint early exit. */
static int test_too_large(void) {
  const uint32_t nvars = 12U;
  const size_t words = qsop_bitset_words(nvars);
  uint64_t adj[12U] = {0};
  /* Matching l_i -- r_i for i < 3 (l = 0..2, r = 3..5); vertices 6..11 are outside pins:
   * connect l_i to outside vertex 6 + i so left signatures can span a wide parent space. */
  for (uint32_t i = 0; i < 3U; i++) {
    qsop_bitset_set(qsop_bitset_row(adj, words, i), 3U + i);
    qsop_bitset_set(qsop_bitset_row(adj, words, 3U + i), i);
    qsop_bitset_set(qsop_bitset_row(adj, words, i), 6U + i);
    qsop_bitset_set(qsop_bitset_row(adj, words, 6U + i), i);
  }
  const uint64_t left_vars = 0x7U;   /* {0,1,2} */
  const uint64_t right_vars = 0x38U; /* {3,4,5} */
  const uint64_t outside = 0xFC0U;   /* {6..11} */

  qsop_error_t error = {0};
  rw_signature_pool_t pool = {0};
  if (!rw_signature_pool_init(&pool, words, &error)) {
    fprintf(stderr, "too-large: pool init failed\n");
    return 1;
  }
  int rc = 0;
  uint32_t left_signatures[3];
  uint32_t right_signatures[1];
  for (uint32_t i = 0; i < 3U && rc == 0; i++) {
    const uint64_t bits =
        signature_bits_of(adj, words, nvars, UINT64_C(1) << i, ((UINT64_C(1) << nvars) - 1U) &
                                                                   ~left_vars);
    if (!rw_signature_pool_intern(&pool, &bits, &left_signatures[i], &error)) {
      rc = 1;
    }
  }
  const uint64_t right_bits =
      signature_bits_of(adj, words, nvars, UINT64_C(1) << 3U,
                        ((UINT64_C(1) << nvars) - 1U) & ~right_vars);
  if (rc == 0 && !rw_signature_pool_intern(&pool, &right_bits, &right_signatures[0], &error)) {
    rc = 1;
  }

  rw_twist_plan_t plan = {0};
  if (rc == 0) {
    /* Crossing rank is 3 > max_dim = 2: must refuse while building the cross basis. */
    const rw_twist_feasibility_t status =
        rw_twist_plan_build(nvars, adj, &pool, left_signatures, 3U, right_signatures, 1U,
                            &left_vars, &right_vars, &outside, words, true, 2U, &plan, &error);
    if (status != RW_TWIST_TOO_LARGE) {
      fprintf(stderr, "too-large: crossing-rank cap not enforced (status %d)\n", (int)status);
      rc = 1;
    }
    rw_twist_plan_free(&plan);
  }
  if (rc == 0) {
    /* c = 3 and the three left signatures span 3 parent dimensions: p + c = 6 > 5. */
    const rw_twist_feasibility_t status =
        rw_twist_plan_build(nvars, adj, &pool, left_signatures, 3U, right_signatures, 1U,
                            &left_vars, &right_vars, &outside, words, true, 5U, &plan, &error);
    if (status != RW_TWIST_TOO_LARGE) {
      fprintf(stderr, "too-large: joint p+c cap not enforced (status %d)\n", (int)status);
      rc = 1;
    }
    rw_twist_plan_free(&plan);
  }
  if (rc == 0) {
    /* The same geometry fits a cap of 6. */
    const rw_twist_feasibility_t status =
        rw_twist_plan_build(nvars, adj, &pool, left_signatures, 3U, right_signatures, 1U,
                            &left_vars, &right_vars, &outside, words, true, 6U, &plan, &error);
    if (status != RW_TWIST_FEASIBLE || plan.c != 3U || plan.p != 3U) {
      fprintf(stderr, "too-large: feasible geometry refused (status %d, p=%" PRIu32
                      ", c=%" PRIu32 ")\n",
              (int)status, plan.p, plan.c);
      rc = 1;
    }
    rw_twist_plan_free(&plan);
  }
  rw_signature_pool_free(&pool);
  return rc;
}

int main(void) {
  uint64_t state = UINT64_C(0x7715BEEFCAFE1234);
  int rc = 0;
  rc |= test_factorization(&state);
  rc |= test_wht_involution(&state);
  rc |= test_twisted_pipeline(&state);
  rc |= test_reach_counts(&state);
  rc |= test_too_large();
  if (rc == 0) {
    printf("rankwidth twist plan unit tests passed\n");
  }
  return rc;
}
