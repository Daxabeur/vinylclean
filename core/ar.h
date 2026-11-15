// vinylclean — autoregressive (AR) model.
// Autocorrelation method + Levinson-Durbin recursion (signal processing
// textbooks; used by Godsill & Rayner, "Digital Audio Restoration", 1998).
// Original work.
#pragma once

namespace vc {

// r[k] = sum_i x[i] * x[i-k], for k = 0..p (x of length n).
void autocorrelation(const float* x, int n, int p, double* r);

// Levinson-Durbin in double precision.
// Input: r[0..p]. Output: a[0..p-1] = coefficients a_1..a_p such that
//   x[n] ≈ a_1 x[n-1] + ... + a_p x[n-p].
// tmp: work array of p doubles. Returns the power of the prediction
// error (≥ 0). If the signal is zero, all coefficients are 0.
double levinson(const double* r, int p, double* a, double* tmp);

} // namespace vc
