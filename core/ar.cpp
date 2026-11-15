// vinylclean — autoregressive model. Original work.
#include "ar.h"
#include "vc_util.h"

namespace vc {

void autocorrelation(const float* x, int n, int p, double* r) {
    for (int k = 0; k <= p; ++k)
        r[k] = (k < n) ? static_cast<double>(dot(x + k, x, n - k)) : 0.0;
}

double levinson(const double* r, int p, double* a, double* tmp) {
    for (int j = 0; j < p; ++j) a[j] = 0.0;
    // White-noise correction (−50 dB): stabilizes the computation on very
    // pure sounds without changing detection.
    double err = r[0] * (1.0 + 1e-5);
    if (!(err > 1e-30)) return 0.0;  // silence (or NaN): null model
    for (int i = 1; i <= p; ++i) {
        double acc = r[i];
        for (int j = 1; j < i; ++j) acc -= a[j - 1] * r[i - j];
        const double k = acc / err;
        if (!(k > -1.0 && k < 1.0)) break;  // keeps the model stable
        for (int j = 1; j < i; ++j) tmp[j - 1] = a[j - 1] - k * a[i - j - 1];
        for (int j = 1; j < i; ++j) a[j - 1] = tmp[j - 1];
        a[i - 1] = k;
        err *= (1.0 - k * k);
        if (!(err > 0.0)) { err = 0.0; break; }
    }
    return err;
}

} // namespace vc
