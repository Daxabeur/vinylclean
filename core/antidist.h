// vinylclean — playback anti-distortion (end of side).
//
// Original work, based on the published geometry of record playback
// (Baerwald 1941, Löfgren 1938, Stevenson 1966; Corrington, tracing
// distortion, 1951; see README):
//
// 1) Tracking error (pivoted arm). Arm of effective length L, null points
//    r1 and r2 (chosen by the alignment): offset angle
//    sin β = (r1 + r2) / 2L, pivot-to-spindle distance d = √(L² − r1·r2), and
//    the error at radius r is φ(r) = asin((L² + r² − d²) / 2Lr) − β (EXACT). The
//    stylus then reads the groove slightly early or late depending on its lateral
//    displacement D: the position read is z(t − D·tan φ / v), v = groove speed,
//    and the velocity read is y = u − (tan φ / v)·d(D·u)/dt to first order.
// 2) Tracing distortion: a stylus of radius ρ (along the groove) cannot follow
//    very tight undulations exactly; to first order, the velocity read is
//    y = u + (ρ / v²)·u·du/dt.
// Both grow towards the center (v drops: 52 cm/s at the edge, 21 cm/s at the
// end of an LP side).
//
// Correction: the groove velocity u is recovered (inverse RIAA curve), the
// distortion term predicted by the model is computed and removed (first-order
// inverse). Products are computed at twice the sample rate (no aliasing),
// then brought back to the audio domain (RIAA curve).
//
// Limitation: the distortion depends on the REAL groove velocity (cm/s), which
// the software only sees up to a gain factor. The level is estimated (loud
// passages of a record are cut at about 10 cm/s); an intensity setting lets
// the user adjust. The arm geometry itself is exact.
#pragma once

#include "riaa.h"
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

namespace vc {

enum class Alignment { Baerwald = 0, LofgrenB = 1, Stevenson = 2, Custom = 3 };
enum class DiscFormat { LP33 = 0, Single45 = 1, Maxi45 = 2 };

struct ArmGeometry {
    double effLenMm = 222.0;          // effective length (pivot → stylus)
    Alignment align = Alignment::Stevenson;
    double null1 = 60.325, null2 = 117.42;   // null points (custom)
    // Computed:
    double r1 = 0, r2 = 0, offsetDeg = 0, overhangMm = 0, mountMm = 0;
    void compute();                          // r1, r2, offset angle, overhang, pivot-to-spindle distance
    double trackingErrorDeg(double rMm) const;
    static void nullPoints(Alignment a, double& n1, double& n2);
};

struct DiscGeometry {
    DiscFormat fmt = DiscFormat::LP33;
    double rpm() const { return fmt == DiscFormat::LP33 ? 100.0 / 3.0 : 45.0; }
    double rStart() const { return fmt == DiscFormat::Single45 ? 82.0 : 146.05; }   // mm, start of the music
    double rEnd() const { return fmt == DiscFormat::Single45 ? 54.0 : 60.325; }     // mm, end (IEC standard)
};

// Stylus radius along the groove (µm) — usual manufacturer values.
enum class StylusType { Conical = 0, Elliptical37 = 1, Elliptical27 = 2, Shibata = 3, MicroLine = 4, Custom = 5 };
double stylusRadiusUm(StylusType t, double customUm);

struct AntiDistParams {
    bool   enabled = false;
    ArmGeometry arm;
    DiscGeometry disc;
    StylusType stylus = StylusType::Elliptical27;
    double stylusUm = 7.6;            // custom
    bool   autoPos = true;            // position estimated from the time since the stylus drop
    double posMm = 100.0;             // manual position (radius)
    double sideMin = 20.0;            // length of a side (for the estimate)
    double intensity = 1.0;           // 0..1.5
};

struct AntiDistState {                // for display
    double radiusMm = 0, trackDeg = 0, grooveCms = 0, levelCms = 0, sideSec = -1;
    bool levelReady = false;
};

// Command-line names. Returns false if the name is unknown.
inline bool alignFromName(const std::string& n, Alignment& a) {
    if (n == "baerwald") a = Alignment::Baerwald; else if (n == "lofgren" || n == "lofgren-b") a = Alignment::LofgrenB;
    else if (n == "stevenson") a = Alignment::Stevenson; else return false;
    return true;
}
inline bool stylusFromName(const std::string& n, StylusType& t, double& um) {
    if (n == "conical" || n == "spherical") t = StylusType::Conical;
    else if (n == "elliptical") t = StylusType::Elliptical37;
    else if (n == "fine-elliptical") t = StylusType::Elliptical27;
    else if (n == "shibata") t = StylusType::Shibata;
    else if (n == "microline") t = StylusType::MicroLine;
    else { char* e = nullptr; const double v = std::strtod(n.c_str(), &e); if (!e || *e || v < 1 || v > 25) return false; t = StylusType::Custom; um = v; }
    return true;
}
inline bool discFromName(const std::string& n, DiscFormat& d) {
    if (n == "33" || n == "lp") d = DiscFormat::LP33; else if (n == "45") d = DiscFormat::Single45;
    else if (n == "maxi" || n == "maxi45") d = DiscFormat::Maxi45; else return false;
    return true;
}

class AntiDistortion {
public:
    bool prepare(double fs, int channels);
    void reset();
    int  latency() const { return ld_; }
    void setParams(const AntiDistParams& p);
    void process(const float* const* in, float* const* out, int n);   // in == out allowed
    AntiDistState state() const { return st_; }
    // For the tests: forces the scale (m/s per digital velocity unit) and the position.
    void forceScale(double s) { forcedScale_ = s; }
    void forceSideSec(double t) { newSide(); sideT_ = t; }   // t = 0: "stylus just dropped"
    void forceRho(double m) { forcedRho_ = m; updateGeometry(); }

private:
    struct Chan {
        RiaaFilter de;                       // de-emphasis (correction → audio)
        double pz[3] = {0, 0, 0}, pp[3] = {0, 0, 0};   // pre-emphasis (inverse): states
        std::vector<float> upHist, dnHist, dry;
        int upPos = 0, dnPos = 0, dryPos = 0;
        double prevU2 = 0, disp = 0;
    };
    double preEmph(Chan& c, double x);
    void updateGeometry();

    double fs_ = 48000;
    int nch_ = 0, taps_ = 47, ld_ = 23;
    std::vector<double> h_, g_;              // half-band (double rate) and its derivative
    std::vector<double> hEven_, gEven_, gOdd_;   // useful coefficients (polyphase)
    double hCenter_ = 0.5;
    std::vector<Chan> ch_;
    RiaaFilter ref_;                         // curve coefficients
    AntiDistParams p_;
    double rho_ = 7.6e-6, tanPhi_ = 0, v_ = 0.5, leak_ = 0.9997;
    double gainNow_ = 0;                      // smoothed intensity (fade)
    // Level: maxima of |u| per block of 1024 over 60 s.
    std::vector<float> blkMax_;
    int blkN_ = 0, blkFill_ = 0, blkPos_ = 0, blkCount_ = 0;
    float curMax_ = 0, blkPeak_ = 0;
    static constexpr double kRefVel = 0.10;   // loud passages of a record ≈ 10 cm/s
    static constexpr double kLimit = 0.8;     // correction soft-limited at 8 cm/s: beyond that the stylus mistracks anyway
    double p90max_ = 0;                       // level of the loud passages of the side (long memory)
    void newSide();
    double scale_ = 0, forcedScale_ = 0, forcedRho_ = -1;
    // Position (stylus drop: silence ≥ 5 s then music).
    double silentSec_ = 0, sideT_ = 0;
    bool sideKnown_ = false;
    int blkSamples_ = 0;
    AntiDistState st_;
};

} // namespace vc
