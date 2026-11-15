// vinylclean — least-squares autoregressive interpolation (LSAR).
// Published principle: Vaseghi & Rayner (1990); Godsill & Rayner, "Digital Audio
// Restoration" (1998). The missing samples are chosen to minimize the energy
// of the AR model's prediction error around the gap.
// Original work: banded Toeplitz system solved by banded Cholesky.
#pragma once

#include <vector>

namespace vc {

class LsarInterpolator {
public:
    // maxGap: maximum gap length; maxOrder: maximum AR order.
    // Allocates all memory (call outside the audio thread).
    void prepare(int maxGap, int maxOrder);

    // Replaces x[start .. start+len) with the LSAR interpolation.
    // c[0..p] = [1, -a_1, ..., -a_p] (prediction-error filter).
    // Requirements: x[start-p .. start+len+p) accessible; len ≤ maxGap.
    // Returns false if the requirements are not met (nothing is modified).
    bool solve(float* x, int start, int len, const double* c, int p);
    // Variant with a prior estimate "prior" (len values) of weight
    // lambda: minimizes Σ e² + λ Σ (u − prior)².
    bool solve(float* x, int start, int len, const double* c, int p, const float* prior, double lambda);

private:
    int maxGap_ = 0, maxOrder_ = 0;
    std::vector<double> rc_;   // autocorrelation of filter c
    std::vector<double> xt_;   // context with the gap set to zero
    std::vector<double> ek_;   // error due to the known samples
    std::vector<double> b_;    // right-hand side
    std::vector<double> chol_; // banded Cholesky factor (len × (p+1))
};

} // namespace vc
