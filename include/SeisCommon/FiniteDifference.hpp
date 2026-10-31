#pragma once
// Shared DRP coefficients and direction schedule; no dimension-specific storage.
#ifdef __CUDACC__
#define SEIS_SHARED_HD __host__ __device__ inline
#else
#define SEIS_SHARED_HD inline
#endif
namespace SeisWave::Difference {
SEIS_SHARED_HD int direction(int step, int stage, int axis) {
    const int flags[8][3]{{0, 0, 0}, {1, 1, 0}, {1, 1, 1}, {0, 0, 1},
                          {0, 1, 0}, {1, 0, 0}, {1, 0, 1}, {0, 1, 1}};
    return (flags[step % 8][axis] + stage) % 2;
}
template<class Real> SEIS_SHARED_HD int stencil(int dir, int depth, int *offsets, Real *coefficients) {
    const int n = depth == 1 ? 2 : depth == 2 ? 3 : 5;
    const Real forward[5]{Real(-.30874), Real(-.6326), Real(1.233), Real(-.3334), Real(.04168)};
    const Real lower[3]{Real(-7) / Real(6), Real(8) / Real(6), Real(-1) / Real(6)};
    for (int l = 0; l < n; ++l) {
        const int pos = n == 5 ? l - 1 : l;
        const Real c = n == 5 ? forward[l] : n == 3 ? lower[l] : l == 0 ? Real(-1) : Real(1);
        offsets[l] = dir ? -pos : pos;
        coefficients[l] = dir ? -c : c;
    }
    return n;
}
} // namespace SeisWave::Difference
#undef SEIS_SHARED_HD
