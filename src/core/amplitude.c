#include "dlx4sop/qsop_solve.h"

#include <math.h>
#include <stdbool.h>
#include <stdint.h>

/* Half of the mantissa headroom of a long double (64 bits on x86, 53 on aarch64). Exponents beyond
 * this cannot overflow the reconstruction below because the mantissa is already in [1,2), but they
 * would be nonsense for an amplitude, so the clamp doubles as a sanity bound. */
#define QSOP_AMPLITUDE_MAX_EXP2 (1 << 20)

void qsop_amplitude_renormalize(qsop_amplitude_t *amp) {
  if (amp == NULL) {
    return;
  }
  const long double re = fabsl(amp->re);
  const long double im = fabsl(amp->im);
  const long double peak = re > im ? re : im;
  if (peak == 0.0L || !isfinite(peak)) {
    return;
  }

  const int exponent = ilogbl(peak);
  if (exponent == 0) {
    return;
  }
  const long double scale = ldexpl(1.0L, -exponent);
  amp->re *= scale;
  amp->im *= scale;
  amp->scale_exp2 += exponent;
}

void qsop_amplitude_scale_pow2(qsop_amplitude_t *amp, int32_t exp) {
  if (amp != NULL) {
    amp->scale_exp2 += exp;
  }
}

bool qsop_amplitude_normalized(const qsop_amplitude_t *amp, uint64_t norm_h, long double *out_re,
                               long double *out_im) {
  if (amp == NULL || out_re == NULL || out_im == NULL) {
    return false;
  }
  if (amp->re == 0.0L && amp->im == 0.0L) {
    *out_re = 0.0L;
    *out_im = 0.0L;
    return true;
  }

  /* norm_h/2 as an integer shift plus a leftover factor of 2^(-1/2) when norm_h is odd. Doing the
   * halving in the exponent rather than on the value is what keeps this exact and in range. */
  const uint64_t half = norm_h / 2U;
  if (half > (uint64_t)QSOP_AMPLITUDE_MAX_EXP2) {
    return false;
  }
  const int64_t exponent = (int64_t)amp->scale_exp2 - (int64_t)half;
  if (exponent > QSOP_AMPLITUDE_MAX_EXP2 || exponent < -QSOP_AMPLITUDE_MAX_EXP2) {
    return false;
  }

  long double re = ldexpl(amp->re, (int)exponent);
  long double im = ldexpl(amp->im, (int)exponent);
  if ((norm_h % 2U) != 0U) {
    static const long double inv_sqrt2 =
        0.70710678118654752440084436210484903928483593768847403658834L;
    re *= inv_sqrt2;
    im *= inv_sqrt2;
  }
  if (!isfinite(re) || !isfinite(im)) {
    return false;
  }
  *out_re = re;
  *out_im = im;
  return true;
}
