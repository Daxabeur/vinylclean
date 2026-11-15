// vinylclean — LSAR interpolation. Original work.
//
// Notation: e[n] = sum_{k=0..p} c_k x[n-k]. We look for the unknowns u
// (positions start..start+len-1) minimizing sum_n e[n]^2. The equations that
// involve at least one unknown are n = start .. start+len+p-1.
// The normal system is  T u = b  with T[i][j] = Rc(|i-j|) (Toeplitz, band p)
// where Rc(m) = sum_k c_k c_{k+m}, and b_i = - sum_j c_j ek[i+j], ek being the
// error computed with the unknowns replaced by 0.
#include "lsar.h"
#include <cmath>
#include <algorithm>

namespace vc {

void LsarInterpolator::prepare(int maxGap, int maxOrder) {
    maxGap_ = maxGap;
    maxOrder_ = maxOrder;
    rc_.assign(maxOrder + 1, 0.0);
    xt_.assign(maxGap + 2 * maxOrder, 0.0);
    ek_.assign(maxGap + maxOrder, 0.0);
    b_.assign(maxGap, 0.0);
    chol_.assign(static_cast<size_t>(maxGap) * (maxOrder + 1), 0.0);
}

bool LsarInterpolator::solve(float* x, int start, int len, const double* c, int p) {
    return solve(x, start, len, c, p, nullptr, 0.0);
}

bool LsarInterpolator::solve(float* x, int start, int len, const double* c, int p, const float* prior, double lambda) {
    if (len <= 0 || len > maxGap_ || p < 0 || p > maxOrder_) return false;
    const int w = p + 1;

    // 1) Autocorrelation of the error filter.
    for (int m = 0; m <= p; ++m) {
        double s = 0.0;
        for (int k = 0; k + m <= p; ++k) s += c[k] * c[k + m];
        rc_[m] = s;
    }

    // 2) Context: p samples before, the gap (set to 0), p after.
    for (int j = 0; j < len + 2 * p; ++j) {
        const int idx = start - p + j;
        const bool inGap = (idx >= start && idx < start + len);
        xt_[j] = inGap ? 0.0 : static_cast<double>(x[idx]);
    }

    // 3) Prediction error due to the known samples only.
    for (int i = 0; i < len + p; ++i) {
        double s = 0.0;
        for (int k = 0; k <= p; ++k) s += c[k] * xt_[p + i - k];
        ek_[i] = s;
    }

    // 4) Right-hand side.
    for (int i = 0; i < len; ++i) {
        double s = 0.0;
        for (int j = 0; j <= p; ++j) s += c[j] * ek_[i + j];
        b_[i] = -s + (prior ? lambda * prior[i] : 0.0);
    }

    // 5) Banded Cholesky: T = L L^T. chol_[i*w + d] = L[i][i-d].
    const double diagLoad = rc_[0] * 1e-9 + 1e-30;
    for (int i = 0; i < len; ++i) {
        const int j0 = std::max(0, i - p);
        for (int j = j0; j <= i; ++j) {
            double s = rc_[i - j];
            if (i == j) s += diagLoad + (prior ? lambda : 0.0);
            const int k0 = std::max(j0, j - p);
            for (int k = k0; k < j; ++k)
                s -= chol_[i * w + (i - k)] * chol_[j * w + (j - k)];
            if (i == j) {
                if (!(s > 0.0)) return false;  // should never happen
                chol_[i * w] = std::sqrt(s);
            } else {
                chol_[i * w + (i - j)] = s / chol_[j * w];
            }
        }
    }

    // 6) Forward substitution: L z = b (z stored in b_).
    for (int i = 0; i < len; ++i) {
        double s = b_[i];
        for (int k = std::max(0, i - p); k < i; ++k) s -= chol_[i * w + (i - k)] * b_[k];
        b_[i] = s / chol_[i * w];
    }
    // 7) Back substitution: L^T u = z.
    for (int i = len - 1; i >= 0; --i) {
        double s = b_[i];
        const int kEnd = std::min(len - 1, i + p);
        for (int k = i + 1; k <= kEnd; ++k) s -= chol_[k * w + (k - i)] * b_[k];
        b_[i] = s / chol_[i * w];
    }

    for (int i = 0; i < len; ++i) {
        const double v = b_[i];
        if (!std::isfinite(v)) return false;
    }
    for (int i = 0; i < len; ++i) x[start + i] = static_cast<float>(b_[i]);
    return true;
}

} // namespace vc
