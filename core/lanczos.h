#pragma once

#include <QVector>

#include <algorithm>
#include <cmath>

// Lanczos-windowed sinc kernel from a precomputed table: one linear
// interpolation per (pixel, sample) term instead of two trig calls.
// a = 10 lobes; beyond that the window is exactly zero.
struct LanczosKernel {
    static constexpr int kLobes = 10;
    static constexpr int kRes = 512; // table steps per unit distance

    QVector<double> tab;
    LanczosKernel() : tab(kLobes * kRes + 2, 0.0)
    {
        const double pi = std::acos(-1.0);
        tab[0] = 1.0;
        for (int i = 1; i < tab.size(); ++i) {
            const double x = double(i) / kRes;
            const double px = pi * x;
            tab[i] = (std::sin(px) / px) * (std::sin(px / kLobes) / (px / kLobes));
        }
    }
    double at(double d) const
    {
        if (d >= kLobes)
            return 0.0;
        const double u = d * kRes;
        const int i = int(u);
        return tab[i] + (tab[i + 1] - tab[i]) * (u - i);
    }
    static const LanczosKernel &instance()
    {
        static const LanczosKernel k;
        return k;
    }
};

// The drawn sin(x)/x curve at fractional sample index u (0 <= u <= v.size() - 1).
// Normalized by the weight sum: near the ends of the data the kernel is cut
// short, and the raw sum would ring and droop. Passes through every sample.
inline double lanczosCurve(const QVector<float> &v, double u)
{
    const LanczosKernel &kernel = LanczosKernel::instance();
    const int n = v.size();
    const int k0 = std::max(0, int(std::floor(u)) - LanczosKernel::kLobes);
    const int k1 = std::min(n - 1, int(std::ceil(u)) + LanczosKernel::kLobes);
    double y = 0.0, wsum = 0.0;
    for (int k = k0; k <= k1; ++k) {
        const double w = kernel.at(std::abs(u - k));
        y += double(v[k]) * w;
        wsum += w;
    }
    return wsum > 1e-6 ? y / wsum : 0.0;
}
