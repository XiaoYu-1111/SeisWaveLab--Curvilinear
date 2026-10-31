#pragma once
#include <cstddef>
#include <vector>

namespace Seis2D::Acoustic::Detail {
// CFS stretching S_i(s)=kappa_i+d_i/(s+alpha_i).
// The pressure weak form has stiffness ratios S_z/S_x, S_x/S_z and mass S_x*S_z.
// Cascaded rational filters avoid division by coincident poles in PML corners.
template <class Real> struct CpmlFilter {
    Real b0{1}, b1{-1}, a1{1};
};
template <class Real> struct CpmlTriangle {
    int node[3]{};
    Real dx[3]{}, dz[3]{}, areaInverseRho{};
    CpmlFilter<Real> inverseX, stretchZ, inverseZ, stretchX;
};
template <class Real> struct CpmlNode {
    int node{};
    CpmlFilter<Real> inverseX, inverseZ;
};
#ifdef __CUDACC__
#define SEIS_ACOUSTIC_HD __host__ __device__
#else
#define SEIS_ACOUSTIC_HD
#endif
template <class Real>
SEIS_ACOUSTIC_HD inline Real cpmlFilter(const CpmlFilter<Real> &f, Real input, Real &history) {
    const Real output = f.b0 * input + history;
    history = f.b1 * input + f.a1 * output;
    return output;
}
template <class Real>
SEIS_ACOUSTIC_HD inline void cpmlFlux(const CpmlTriangle<Real> &t, const Real *current, const Real *rootMass,
                                      Real *history, Real *flux) {
    Real gx = 0, gz = 0;
    for (int j = 0; j < 3; ++j) {
        const Real pressure = current[t.node[j]] / rootMass[t.node[j]];
        gx += t.dx[j] * pressure;
        gz += t.dz[j] * pressure;
    }
    const Real fx = cpmlFilter(t.stretchZ, cpmlFilter(t.inverseX, gx, history[0]), history[1]);
    const Real fz = cpmlFilter(t.stretchX, cpmlFilter(t.inverseZ, gz, history[2]), history[3]);
    flux[0] = t.areaInverseRho * (fx - gx);
    flux[1] = t.areaInverseRho * (fz - gz);
}
#undef SEIS_ACOUSTIC_HD
} // namespace Seis2D::Acoustic::Detail
