#pragma once
#include "AcousticKernel2D.hpp"
#include "Seis2D/CurvilinearSolver2D.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#ifdef SEIS3D_HAS_OPENMP
#include <omp.h>
#endif
// Shared acoustic assembly for forward propagation and RTM; no mutable session state.
namespace Seis2D::Acoustic::Detail {
using namespace Seis2D::Curvilinear;
struct ModelSettings {
    Parameters model;
    ComputeSettings3D compute;
    BackendKind3D backend{BackendKind3D::Cpu};
    int absorberCells{24}, smoothIterations{}, axisLimit{16384};
    double backgroundVp{}, memoryMiB{1536};
    bool background{}, cpml{}, geometryOnly{};
    CfsPmlSettings cpmlSettings{1e-5, 1, 0};
};
constexpr double pi = 3.14159265358979323846;
inline void require(bool b, const char *s) {
    if (!b)
        throw std::runtime_error(s);
}
inline double wavelet(double t, double f) {
    double a = pi * f * (t - 1.5 / f);
    return (1 - 2 * a * a) * std::exp(-a * a);
}
struct PmlTriangleGeometry {
    int node[3]{};
    double dx[3]{}, dz[3]{}, areaInverseRho{}, x{}, z{};
};
struct Mesh {
    int nx{}, nz{}, pad{};
    bool cpml{};
    CfsPmlSettings cpmlSettings;
    double pmlFrequency{};
    std::vector<PmlTriangleGeometry> pmlTriangles;
    double dt{}, ppw{}, maximumVp{};
    std::vector<double> x, z, vp, bg, rho, k, mass, bgMass;
    std::vector<unsigned char> active;
    std::vector<int> receivers;
};
inline Mesh mesh(const ModelSettings &o) {
    const auto &p = o.model;
    const bool forwardOnly = !o.background;
    const int axisLimit = o.axisLimit;
    require(p.nx >= 16 && p.nz >= 16 && p.nx <= axisLimit && p.nz <= axisLimit,
            "Acoustic model dimensions exceed supported axes or memory budget.");
    require(std::isfinite(p.spacing) && p.spacing > 0 && std::isfinite(p.frequency) && p.frequency > 0 &&
                std::isfinite(p.duration) && p.duration > 0 && p.duration <= 10,
            "Invalid acoustic spacing, frequency or duration.");
    require(p.precision == Precision::Float32 || p.precision == Precision::Float64,
            "Invalid acoustic precision.");
    require(!p.attenuation.enabled,
            "Acoustic propagation does not support GSLS/Q. Disable attenuation explicitly.");
    require(o.absorberCells >= 8 && o.absorberCells <= 128 && o.smoothIterations >= 0 &&
                o.smoothIterations <= 1000,
            "Invalid acoustic absorber/smoothing settings.");
    require(std::isfinite(o.backgroundVp) && o.backgroundVp >= 0 && std::isfinite(o.memoryMiB) &&
                o.memoryMiB >= 16 && o.memoryMiB <= 32768,
            "Invalid acoustic background/memory budget.");
    require(o.compute.cpuThreads >= 0 && o.compute.cpuThreads <= 64, "CPU threads must be 0..64.");
    require(std::isfinite(p.relief) && std::isfinite(p.wavelength) && p.wavelength > 0 &&
                std::isfinite(p.surfaceDatum) && std::isfinite(p.flattenDepth) && p.flattenDepth >= 0,
            "Invalid terrain parameters.");
    require(!p.layered || (!p.layers.empty() && p.layers.size() <= 16),
            "Acoustic model requires 1..16 layers.");
    auto validMaterial = [](double v, double d) {
        return std::isfinite(v) && v > 0 && std::isfinite(d) && d > 0;
    };
    require(validMaterial(p.vp, p.density), "Invalid acoustic Vp/density.");
    if (p.layered)
        for (std::size_t j = 0; j < p.layers.size(); ++j) {
            const auto &l = p.layers[j];
            require(validMaterial(l.vp, l.density) && std::isfinite(l.thickness) && l.thickness >= 0 &&
                        (j + 1 == p.layers.size() || l.thickness > 0),
                    "Invalid acoustic layer.");
        }
    if (o.cpml)
        require(std::isfinite(o.cpmlSettings.targetReflection) && o.cpmlSettings.targetReflection >= 1e-10 &&
                    o.cpmlSettings.targetReflection <= .1 && std::isfinite(o.cpmlSettings.kappaMax) &&
                    o.cpmlSettings.kappaMax >= 1 && o.cpmlSettings.kappaMax <= 8 &&
                    std::isfinite(o.cpmlSettings.frequency) && o.cpmlSettings.frequency >= 0 &&
                    o.cpmlSettings.frequency <= 10000,
                "Invalid acoustic CFS-CPML parameters.");
    Mesh g;
    g.cpml = o.cpml;
    g.cpmlSettings = o.cpmlSettings;
    g.pmlFrequency = o.cpmlSettings.frequency > 0 ? o.cpmlSettings.frequency : p.frequency;
    g.pad = o.absorberCells;
    g.nx = p.nx + 2 * g.pad;
    g.nz = p.nz + g.pad;
    const auto count = std::size_t(g.nx) * g.nz;
    const auto available = Seis3D::Curvilinear::queryBackendMemory3D(o.backend, o.compute.cudaDevice);
    const double realBytes = p.precision == Precision::Float32 ? 4 : 8;
    const double pmlTriangles = 2. * ((g.nx - 1.) * (g.nz - 1.) - (p.nx - 1.) * (p.nz - 1.));
    const double pmlNodes = double(count) - double(p.nx - 2) * (p.nz - 1);
    const double pmlBuildBytes =
        o.cpml ? pmlTriangles * (140 + 25 * realBytes) + pmlNodes * (48 + 12 * realBytes) : 0;
    const double buildBytes =
        double(count) * (forwardOnly ? (p.precision == Precision::Float32 ? 256 : 384) : 512) + pmlBuildBytes;
    require(buildBytes < o.memoryMiB * 1048576.,
            "Geometry/operator construction exceeds the imaging RAM budget; reduce model dimensions.");
    require(!available.hostAvailableBytes || buildBytes < .7 * available.hostAvailableBytes,
            "Insufficient RAM for imaging geometry construction.");
    if (o.cpml)
        g.pmlTriangles.reserve(static_cast<std::size_t>(pmlTriangles));
    g.x.resize(count);
    g.z.resize(count);
    g.vp.resize(count);
    g.rho.resize(count);
    g.active.resize(count);
    std::unique_ptr<SurfaceProfile> profile;
    if (p.importedSurface) {
        profile = std::make_unique<SurfaceProfile>(p.surfaceSamples);
        require(p.surfaceSamples.front()[0] <= 0 && p.surfaceSamples.back()[0] >= (p.nx - 1) * p.spacing,
                "Terrain profile must cover the full target span.");
    }
    std::vector<double> surface(g.nx);
    double relief = 0;
    for (int i = 0; i < g.nx; ++i) {
        double x = std::clamp(i - g.pad, 0, p.nx - 1) * p.spacing;
        surface[i] =
            profile ? p.surfaceDatum - profile->sample(x)[0] : p.relief * std::cos(2 * pi * x / p.wavelength);
        relief = std::max(relief, std::abs(surface[i]));
    }
    const double bottom = (p.nz - 1) * p.spacing;
    const double flatten =
        p.flattenDepth > 0
            ? p.flattenDepth
            : std::min(bottom, std::max({.35 * bottom, 6 * p.spacing, 4 * relief + 2 * p.spacing}));
    require(flatten <= bottom && (relief == 0 || flatten > 2.6 * relief),
            "Terrain flatten depth must be within model and >2.6 times relief.");
    for (int j = 0; j < g.nz; ++j)
        for (int i = 0; i < g.nx; ++i) {
            const auto q = std::size_t(j) * g.nx + i;
            double eta = j * p.spacing, t = std::clamp(eta / flatten, 0., 1.);
            double fade = 1 - std::pow(t, 5) * (126 + t * (-420 + t * (540 + t * (-315 + 70 * t))));
            g.x[q] = (i - g.pad) * p.spacing;
            g.z[q] = eta + surface[i] * fade;
            double vp = p.vp, rho = p.density, depth = 0;
            if (p.layered)
                for (std::size_t l = 0; l < p.layers.size(); ++l) {
                    depth += p.layers[l].thickness;
                    if (l + 1 == p.layers.size() || eta < depth) {
                        vp = p.layers[l].vp;
                        rho = p.layers[l].density;
                        break;
                    }
                }
            g.vp[q] = vp;
            g.rho[q] = rho;
            g.active[q] = (i > 0 && i < g.nx - 1 && j > 0 && j < g.nz - 1) ? 1 : 0;
        }
    if (!forwardOnly) {
        g.bg = g.vp;
        if (o.backgroundVp > 0)
            std::fill(g.bg.begin(), g.bg.end(), o.backgroundVp);
        else {
            auto tmp = g.bg;
            for (int n = 0; n < o.smoothIterations; ++n) {
                for (int j = 0; j < g.nz; ++j)
                    for (int i = 0; i < g.nx; ++i) {
                        int q = j * g.nx + i;
                        tmp[q] = .5 * g.bg[q] + .125 * (g.bg[j * g.nx + std::max(0, i - 1)] +
                                                        g.bg[j * g.nx + std::min(g.nx - 1, i + 1)] +
                                                        g.bg[std::max(0, j - 1) * g.nx + i] +
                                                        g.bg[std::min(g.nz - 1, j + 1) * g.nx + i]);
                    }
                g.bg.swap(tmp);
            }
        }
    }
    g.maximumVp = *std::max_element(g.vp.begin(), g.vp.end());
    if (o.geometryOnly)
        return g; // DRP owns its metrics/operators; never assembles a P1 stiffness matrix.
    g.k.assign(count * 9, 0);
    g.mass.assign(count, 0);
    if (!forwardOnly)
        g.bgMass.assign(count, 0);
    const int offsets[9]{-g.nx - 1, -g.nx, -g.nx + 1, -1, 0, 1, g.nx - 1, g.nx, g.nx + 1};
    auto triangle = [&](int a, int b, int c) {
        const int ids[3]{a, b, c};
        const double area2 = (g.x[b] - g.x[a]) * (g.z[c] - g.z[a]) - (g.x[c] - g.x[a]) * (g.z[b] - g.z[a]);
        require(area2 > 1e-12 && std::isfinite(area2), "Folded or degenerate acoustic terrain cell.");
        const double gx[3]{g.z[b] - g.z[c], g.z[c] - g.z[a], g.z[a] - g.z[b]};
        const double gz[3]{g.x[c] - g.x[b], g.x[a] - g.x[c], g.x[b] - g.x[a]};
        const double inverseRho = (1 / g.rho[a] + 1 / g.rho[b] + 1 / g.rho[c]) / 3;
        const double ix = double(a % g.nx + b % g.nx + c % g.nx) / 3,
                     iz = double(a / g.nx + b / g.nx + c / g.nx) / 3;
        if (o.cpml && (ix < g.pad || ix > g.nx - 1 - g.pad || iz > g.nz - 1 - g.pad)) {
            PmlTriangleGeometry triangle;
            triangle.x = ix;
            triangle.z = iz;
            triangle.areaInverseRho = .5 * area2 * inverseRho;
            for (int j = 0; j < 3; ++j) {
                triangle.node[j] = ids[j];
                triangle.dx[j] = gx[j] / area2;
                triangle.dz[j] = gz[j] / area2;
            }
            g.pmlTriangles.push_back(triangle);
        }
        for (int u = 0; u < 3; ++u) {
            int q = ids[u];
            g.mass[q] += area2 / 6 / g.rho[q] / (g.vp[q] * g.vp[q]);
            if (!forwardOnly)
                g.bgMass[q] += area2 / 6 / g.rho[q] / (g.bg[q] * g.bg[q]);
            for (int v = 0; v < 3; ++v) {
                int d = ids[v] - q;
                int slot = 0;
                while (slot < 9 && offsets[slot] != d)
                    ++slot;
                require(slot < 9, "Invalid FEM adjacency.");
                g.k[std::size_t(q) * 9 + slot] += inverseRho * (gx[u] * gx[v] + gz[u] * gz[v]) / (2 * area2);
            }
        }
    };
    double maxEdge = 0;
    for (int j = 0; j < g.nz - 1; ++j)
        for (int i = 0; i < g.nx - 1; ++i) {
            int a = j * g.nx + i, b = a + 1, c = a + g.nx, d = c + 1;
            if ((i + j) % 2) {
                triangle(a, b, c);
                triangle(b, d, c);
            } else {
                triangle(a, b, d);
                triangle(a, d, c);
            }
            maxEdge = std::max({maxEdge, std::hypot(g.x[b] - g.x[a], g.z[b] - g.z[a]),
                                std::hypot(g.x[c] - g.x[a], g.z[c] - g.z[a])});
        }
    double bound = 0;
    for (int q = 0; q < static_cast<int>(count); ++q)
        if (g.active[q]) {
            double row = 0, bgRow = 0;
            for (int k = 0; k < 9; ++k) {
                int t = q + offsets[k];
                double v = g.k[std::size_t(q) * 9 + k];
                if (v && t >= 0 && t < static_cast<int>(count) && g.active[t]) {
                    row += std::abs(v) / std::sqrt(g.mass[q] * g.mass[t]);
                    if (!forwardOnly)
                        bgRow += std::abs(v) / std::sqrt(g.bgMass[q] * g.bgMass[t]);
                }
            }
            bound = std::max({bound, row, bgRow});
        }
    require(bound > 0 && std::isfinite(bound), "Invalid acoustic spectral bound.");
    g.dt = .8 * 2 / std::sqrt(bound);
    double minimumVp = *std::min_element(g.vp.begin(), g.vp.end());
    if (!g.bg.empty())
        minimumVp = std::min(minimumVp, *std::min_element(g.bg.begin(), g.bg.end()));
    g.ppw = minimumVp / (p.frequency * maxEdge);
    require(
        g.ppw >= 12,
        "Acoustic FEM baseline requires at least 12 nodes per peak P wavelength. Reduce spacing/frequency.");
    return g;
}
struct StretchProfile {
    double kappa{1}, d{}, alpha{};
};
inline StretchProfile pmlProfile(const Mesh &g, double index, bool vertical) {
    const double depth = vertical
                             ? std::max(0., (index - (g.nz - 1 - g.pad)) / g.pad)
                             : std::max({0., (g.pad - index) / g.pad, (index - (g.nx - 1 - g.pad)) / g.pad});
    const double u = std::clamp(depth, 0., 1.);
    if (u == 0)
        return {};
    const double vmax = g.maximumVp;
    const double width = g.pad * (g.x[1] - g.x[0]);
    const double power = u * u * u;
    return {1 + (g.cpmlSettings.kappaMax - 1) * power,
            -2 * vmax * std::log(g.cpmlSettings.targetReflection) / width * power,
            pi * g.pmlFrequency * (1 - u)};
}
template <class Real> inline CpmlFilter<Real> pmlFilter(StretchProfile s, double dt, bool inverse) {
    if (s.d == 0)
        return {}; // Identity at and inside the physical/PML interface.
    const double beta = inverse ? s.alpha + s.d / s.kappa : s.alpha;
    const double direct = inverse ? 1 / s.kappa : s.kappa;
    const double residue = inverse ? -s.d / (s.kappa * s.kappa) : s.d;
    const double denominator = 1 + .5 * dt * beta, a = (1 - .5 * dt * beta) / denominator;
    return {Real(direct + .5 * dt * residue / denominator),
            Real(-direct * a + .5 * dt * residue / denominator), Real(a)};
}
template <class Real> inline void buildCpml(const Mesh &g, Operator<Real> &op) {
    if (!g.cpml)
        return;
    const auto count = g.mass.size();
    std::vector<int> nodeMap(count, -1), counts(count, 0);
    op.pmlTriangles.reserve(g.pmlTriangles.size());
    for (const auto &source : g.pmlTriangles) {
        CpmlTriangle<Real> t;
        for (int j = 0; j < 3; ++j) {
            t.node[j] = source.node[j];
            t.dx[j] = Real(source.dx[j]);
            t.dz[j] = Real(source.dz[j]);
            ++counts[t.node[j]];
        }
        t.areaInverseRho = Real(source.areaInverseRho);
        const auto x = pmlProfile(g, source.x, false), z = pmlProfile(g, source.z, true);
        t.inverseX = pmlFilter<Real>(x, g.dt, true);
        t.stretchX = pmlFilter<Real>(x, g.dt, false);
        t.inverseZ = pmlFilter<Real>(z, g.dt, true);
        t.stretchZ = pmlFilter<Real>(z, g.dt, false);
        op.pmlTriangles.push_back(t);
    }
    const auto nodeCount = std::count_if(counts.begin(), counts.end(), [](int c) { return c > 0; });
    op.pmlNodes.reserve(nodeCount);
    op.pmlRows.reserve(nodeCount + 1);
    op.pmlRows.push_back(0);
    for (int q = 0; q < static_cast<int>(count); ++q)
        if (counts[q]) {
            nodeMap[q] = static_cast<int>(op.pmlNodes.size());
            CpmlNode<Real> n;
            n.node = q;
            n.inverseX = pmlFilter<Real>(pmlProfile(g, q % g.nx, false), g.dt, true);
            n.inverseZ = pmlFilter<Real>(pmlProfile(g, q / g.nx, true), g.dt, true);
            op.pmlNodes.push_back(n);
            op.pmlRows.push_back(op.pmlRows.back() + counts[q]);
        }
    op.pmlReferences.resize(op.pmlRows.back());
    auto cursor = op.pmlRows;
    for (int t = 0; t < static_cast<int>(op.pmlTriangles.size()); ++t)
        for (int j = 0; j < 3; ++j)
            op.pmlReferences[cursor[nodeMap[op.pmlTriangles[t].node[j]]]++] = 3 * t + j;
}
template <class Real> Operator<Real> makeOperator(const Mesh &g, bool background, int threads) {
    Operator<Real> op;
    op.nx = g.nx;
    op.nz = g.nz;
    op.threads = threads;
    op.dt2 = Real(g.dt * g.dt);
    op.active = g.active;
    op.receivers = g.receivers;
    const auto count = g.mass.size();
    op.h.assign(count * 9, 0);
    op.a.resize(count);
    op.b.resize(count);
    op.rootMass.resize(count);
    const auto &mass = background ? g.bgMass : g.mass;
    const int offsets[9]{-g.nx - 1, -g.nx, -g.nx + 1, -1, 0, 1, g.nx - 1, g.nx, g.nx + 1};
    double vmax = *std::max_element(g.vp.begin(), g.vp.end());
    if (!g.bg.empty())
        vmax = std::max(vmax, *std::max_element(g.bg.begin(), g.bg.end()));
    const double h = g.x[1] - g.x[0], sigmaMax = 3 * vmax * std::log(1e4) / (2 * g.pad * h);
    for (int q = 0; q < static_cast<int>(count); ++q) {
        op.rootMass[q] = Real(std::sqrt(mass[q]));
        int x = q % g.nx, z = q / g.nx;
        double distance = std::max({0., double(g.pad - x) / g.pad, double(x - (g.nx - 1 - g.pad)) / g.pad,
                                    double(z - (g.nz - 1 - g.pad)) / g.pad});
        double damp = .5 * g.dt * sigmaMax * distance * distance;
        op.a[q] = g.cpml ? Real(1) : Real(1 / (1 + damp));
        op.b[q] = g.cpml ? Real(-1) : Real(-(1 - damp) / (1 + damp));
        if (op.active[q])
            for (int k = 0; k < 9; ++k) {
                int t = q + offsets[k];
                if (t >= 0 && t < static_cast<int>(count) && op.active[t])
                    op.h[std::size_t(q) * 9 + k] =
                        Real(g.k[std::size_t(q) * 9 + k] / std::sqrt(mass[q] * mass[t]));
            }
    }
    for (Real value : op.h)
        require(std::isfinite(value), "Acoustic stiffness overflows selected precision.");
    for (Real value : op.rootMass)
        require(value > 0 && std::isfinite(value), "Acoustic mass underflows/overflows selected precision.");
    require(op.dt2 > 0 && std::isfinite(op.dt2),
            "Acoustic time step underflows/overflows selected precision.");
    buildCpml(g, op);
    return op;
}
inline int workers(const ModelSettings &o) {
#ifdef SEIS3D_HAS_OPENMP
    return o.compute.cpuThreads ? o.compute.cpuThreads : std::min(8, omp_get_num_procs());
#else
    require(o.compute.cpuThreads <= 1, "This build has no OpenMP support.");
    return 1;
#endif
}
} // namespace Seis2D::Acoustic::Detail
