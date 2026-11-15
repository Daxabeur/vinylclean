// vinylclean — offline processing of an interleaved signal, with latency
// compensation (the output is aligned with the input). Used by the WAV tool,
// the tests and the benchmark: it is EXACTLY the same DSP code as in real
// time, called block by block.
#pragma once

#include <vector>
#include <algorithm>

namespace vc {

template <class Proc>
std::vector<float> processOffline(Proc& proc, const std::vector<float>& in, int nch, int block) {
    const size_t frames = in.size() / nch;
    const size_t D = static_cast<size_t>(proc.latency());
    std::vector<float> out(frames * nch, 0.f);
    std::vector<float> pin(static_cast<size_t>(block) * nch), pout(static_cast<size_t>(block) * nch);
    std::vector<const float*> ip(nch);
    std::vector<float*> op(nch);
    for (int c = 0; c < nch; ++c) { ip[c] = &pin[c * block]; op[c] = &pout[c * block]; }
    const size_t total = frames + D;
    for (size_t t = 0; t < total; t += block) {
        const int m = static_cast<int>(std::min<size_t>(block, total - t));
        for (int i = 0; i < m; ++i)
            for (int c = 0; c < nch; ++c)
                pin[c * block + i] = (t + i < frames) ? in[(t + i) * nch + c] : 0.f;
        proc.process(ip.data(), op.data(), m);
        for (int i = 0; i < m; ++i) {
            const size_t o = t + i;
            if (o < D) continue;
            for (int c = 0; c < nch; ++c) out[(o - D) * nch + c] = pout[c * block + i];
        }
    }
    return out;
}

} // namespace vc
