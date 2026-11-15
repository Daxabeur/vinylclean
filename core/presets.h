// vinylclean — sensitivities 0 to 5 (declick and decrackle) and default settings.
//
// Each sensitivity maps to an internal intensity (0 to 10). The steps were
// tuned by listening and by measuring how many events each step repairs on
// real records, from very light (1) to very strong (5).
// The intensities can still be fine-tuned ("advanced settings").
#pragma once

#include "decrackle.h"
#include <cmath>
#include <cstring>

namespace vc {

constexpr int kMaxSens = 5;

// Internal intensity for each sensitivity 0..5 (0 = off).
// Detection thresholds obtained for each step (declick / decrackle):
//   sens 1: k 21.4 / 21.4    sens 2: 13.7 / 13.7    sens 3: 9.8 / 8.8
//   sens 4: 7.8 / 7.8        sens 5: 6.3 / 6.3
// (A lower declick threshold at step 5 doubled the damage on heavily
// distorted metal music: 6.3 is kept.)
inline float declickIntensityForSens(int s) {
    static const float t[kMaxSens + 1] = {0.f, 2.5f, 4.5f, 6.f, 7.f, 8.f};
    return t[s < 0 ? 0 : s > kMaxSens ? kMaxSens : s];
}
inline float decrackleIntensityForSens(int s) {
    static const float t[kMaxSens + 1] = {0.f, 2.5f, 4.5f, 6.5f, 7.f, 8.f};
    return t[s < 0 ? 0 : s > kMaxSens ? kMaxSens : s];
}
// Sensitivity matching an intensity exactly, otherwise −1 ("custom").
inline int declickSensForIntensity(float v) {
    for (int s = 0; s <= kMaxSens; ++s) if (declickIntensityForSens(s) == v) return s;
    return -1;
}
inline int decrackleSensForIntensity(float v) {
    for (int s = 0; s <= kMaxSens; ++s) if (decrackleIntensityForSens(s) == v) return s;
    return -1;
}

constexpr int kDefaultDeclickSens = 3;
constexpr int kDefaultDecrackleSens = 3;   // single slider: same sensitivity for both
constexpr int kDefaultProtect = 1;        // 0 none, 1 normal, 2 maximum
constexpr float kDefaultMaxClickMs = 2.5f;
constexpr bool kDefaultMultiband = true;
constexpr bool kDefaultStereo = true;

inline void applySensitivity(ChainParams& p, int declickSens, int decrackleSens) {
    p.declick.intensity = declickIntensityForSens(declickSens);
    p.decrackle.intensity = decrackleIntensityForSens(decrackleSens);
}

inline ChainParams defaultChainParams() {
    ChainParams p;
    applySensitivity(p, kDefaultDeclickSens, kDefaultDecrackleSens);
    p.declick.maxClickMs = kDefaultMaxClickMs;
    p.declick.protect = p.decrackle.protect = kDefaultProtect;
    return p;
}

inline Mode modeFromName(const char* m) {
    if (!std::strcmp(m, "bypass")) return Mode::Bypass;
    if (!std::strcmp(m, "removed")) return Mode::Removed;
    return Mode::Repair;
}
inline const char* modeName(Mode m) {
    return m == Mode::Bypass ? "bypass" : m == Mode::Removed ? "removed" : "repair";
}

} // namespace vc
