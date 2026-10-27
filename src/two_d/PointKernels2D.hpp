#pragma once
#include "SeisCommon/FiniteDifference.hpp"
#include <cmath>
#include <cstddef>
#ifdef __CUDACC__
#define SEIS_2D_HD __host__ __device__ inline
#else
#define SEIS_2D_HD inline
#endif
namespace Seis2D::Curvilinear::Point {
inline constexpr int maxFields=18; // 5 elastic, or 6 plane-strain stresses/velocities + 4 per GSLS branch.
struct Mesh {
    int nx{}, nz{};
    std::size_t count{};
    double h{};
    const double *a{}, *b{}, *jac{}, *normals{}, *lambda{}, *mu{}, *rho{}, *source{}, *damping{};
    double sourceDirection[3]{0, 0, 1};
    const double *sourceGradient[2]{};
    int mechanisms{},fieldCount{5};
    double rates[3]{};
    const double *deltaP[3]{},*deltaS[3]{};
};
struct Fields {
    double *f[maxFields]{};
};
SEIS_2D_HD std::size_t index(const Mesh &g, int i, int k) {
    return i + static_cast<std::size_t>(g.nx) * k;
}
SEIS_2D_HD double derivative(const double *v, const Mesh &g, std::size_t q, int axis, int dir,
                             int depth = -1) {
    int offsets[5];
    double c[5];
    const int n = SeisWave::Difference::stencil(dir, depth, offsets, c);
    double result = 0;
    for (int l = 0; l < n; ++l)
        result += c[l] * v[static_cast<std::ptrdiff_t>(q) + offsets[l] * (axis ? g.nx : 1)];
    return result / g.h;
}
SEIS_2D_HD void project(Fields w, const Mesh &g, int i) {
    const auto q = index(g, i, 3);
    const double nx = g.normals[2 * q], nz = g.normals[2 * q + 1];
    const double tx = w.f[2][q] * nx + w.f[4][q] * nz, tz = w.f[4][q] * nx + w.f[3][q] * nz,
                 nn = nx * tx + nz * tz;
    w.f[2][q] += -2 * nx * tx + nn * nx * nx;
    w.f[3][q] += -2 * nz * tz + nn * nz * nz;
    w.f[4][q] += -nx * tz - tx * nz + nn * nx * nz;
}
SEIS_2D_HD void surfaceDerivative(const Fields &w, const Mesh &g, std::size_t q, int dir, double *result) {
    const double c[2]{g.a[q], g.b[q]}, c2 = c[0] * c[0] + c[1] * c[1], length = ::sqrt(c2),
                                       n[2]{c[0] / length, c[1] / length};
    const double dx = derivative(w.f[0], g, q, 0, dir), dz = derivative(w.f[1], g, q, 0, dir),
                 lambda = g.lambda[q], mu = g.mu[q];
    double t[2]{(lambda + 2 * mu) * dx * c[0] + mu * dz * c[1], mu * dz * c[0] + lambda * dx * c[1]};
    for(int l=0;l<g.mechanisms;++l){const int m=6+4*l;t[0]-=w.f[m][q]*c[0]+w.f[m+2][q]*c[1];t[1]-=w.f[m+2][q]*c[0]+w.f[m+1][q]*c[1];}
    const double normal = t[0] * n[0] + t[1] * n[1];
    for (int v = 0; v < 2; ++v)
        result[v] = -(t[v] - n[v] * normal) / (mu * c2) - n[v] * normal / ((lambda + 2 * mu) * c2);
}
SEIS_2D_HD double flux(const Fields &w, const Mesh &g, std::size_t q, int axis, int component) {
    const double x = axis ? g.a[q] : 1, z = axis ? g.b[q] : 0;
    return g.jac[q] * (x * w.f[component ? 4 : 2][q] + z * w.f[component ? 3 : 4][q]);
}
SEIS_2D_HD double momentum(const Fields &w, const Mesh &g, std::size_t q, int component, const int *dirs,
                           int depth) {
    double result = 0;
    for (int axis = 0; axis < 2; ++axis) {
        int offsets[5];
        double c[5];
        const int n = SeisWave::Difference::stencil(dirs[axis], -1, offsets, c);
        for (int l = 0; l < n; ++l) {
            int offset = offsets[l];
            double sign = 1;
            if (axis) {
                const int k = depth + offset;
                if (k == 0)
                    continue;
                if (k < 0) {
                    offset = -k - depth;
                    sign = -1;
                }
            }
            result +=
                sign * c[l] *
                flux(w, g, static_cast<std::ptrdiff_t>(q) + offset * (axis ? g.nx : 1), axis, component);
        }
    }
    return result / (g.h * g.jac[q] * g.rho[q]);
}
SEIS_2D_HD void rhs(const Fields &w, Fields out, const Mesh &g, std::size_t q, double forcing, int step,
                    int stage) {
    const int depth = static_cast<int>(q / g.nx) - 3,
              dirs[2]{SeisWave::Difference::direction(step, stage, 0),
                      SeisWave::Difference::direction(step, stage, 2)};
    double surface[2]{};
    if (depth == 0)
        surfaceDerivative(w, g, q, dirs[0], surface);
    double dx[5]{}, dz[5]{};
    for (int f = 0; f < 5; ++f) {
        const double xi = derivative(w.f[f], g, q, 0, dirs[0]);
        const double eta =
            f < 2 && depth == 0 ? surface[f] : derivative(w.f[f], g, q, 1, dirs[1], f < 2 ? depth : -1);
        dx[f] = xi + g.a[q] * eta;
        dz[f] = g.b[q] * eta;
    }
    out.f[0][q] = (dx[2] + dz[4]) / g.rho[q];
    out.f[1][q] = (dx[4] + dz[3]) / g.rho[q];
    if (depth <= (dirs[1] ? 3 : 1))
        for (int v = 0; v < 2; ++v)
            out.f[v][q] = momentum(w, g, q, v, dirs, depth);
    for (int v = 0; v < 2; ++v)
        out.f[v][q] +=
            forcing *
            (g.sourceGradient[v] ? g.sourceGradient[v][q] : g.source[q] * g.sourceDirection[v == 0 ? 0 : 2]) /
            g.rho[q];
    const double lambda = g.lambda[q], mu = g.mu[q], div = dx[0] + dz[1];
    out.f[2][q] = lambda * div + 2 * mu * dx[0];
    out.f[3][q] = lambda * div + 2 * mu * dz[1];
    out.f[4][q] = mu * (dz[0] + dx[1]);
    if(g.mechanisms){out.f[5][q]=lambda*div;
        for(int l=0;l<g.mechanisms;++l){const int m=6+4*l;const double dp=g.deltaP[l][q],ds=g.deltaS[l][q],dl=dp-2*ds;
            const double drive[4]{dl*div+2*ds*dx[0],dl*div+2*ds*dz[1],ds*(dz[0]+dx[1]),dl*div};
            out.f[2][q]-=w.f[m][q];out.f[3][q]-=w.f[m+1][q];out.f[4][q]-=w.f[m+2][q];out.f[5][q]-=w.f[m+3][q];
            for(int f=0;f<4;++f)out.f[m+f][q]=g.rates[l]*(drive[f]-w.f[m+f][q]);
        }
    }
    for (int f = 0; f < g.fieldCount; ++f)
        out.f[f][q] -= g.damping[q] * w.f[f][q];
}
SEIS_2D_HD double value(const Fields &w, const Mesh &g, std::size_t q, int component) {
    return component == 0   ? w.f[1][q]
           : component == 1 ? w.f[0][q]
           : component == 2
               ? ::sqrt(w.f[0][q] * w.f[0][q] + w.f[1][q] * w.f[1][q])
               : g.mechanisms ? -(w.f[2][q]+w.f[3][q]+w.f[5][q])/3
               : -(1 + g.lambda[q] / (2 * (g.lambda[q] + g.mu[q]))) * (w.f[2][q] + w.f[3][q]) / 3;
}
SEIS_2D_HD double energy(const Fields &w, const Mesh &g, std::size_t q) {
    const double l = g.lambda[q], m = g.mu[q], xx = w.f[2][q], zz = w.f[3][q], xz = w.f[4][q];
    return (.5 * g.rho[q] * (w.f[0][q] * w.f[0][q] + w.f[1][q] * w.f[1][q]) +
            ((l + 2 * m) * (xx * xx + zz * zz) - 2 * l * xx * zz) / (8 * m * (l + m)) + xz * xz / (2 * m)) *
           g.jac[q] * g.h * g.h;
}
} // namespace Seis2D::Curvilinear::Point
#undef SEIS_2D_HD
