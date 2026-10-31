#pragma once
#include "AcousticDrp2D.hpp"
#include "AcousticModel2D.hpp"
#include "Seis2D/AcousticSolver2D.hpp"
namespace Seis2D::Acoustic::Drp {
template <class T> Operator<T> makeOperator(const Options &o, const Detail::Mesh &g) {
    const auto &p = o.model;
    Detail::require(std::isfinite(o.drpDissipation) && o.drpDissipation >= 0 && o.drpDissipation <= 1,
                    "Invalid DRP dissipation strength.");
    Detail::require(std::isfinite(o.cpml.targetReflection) && o.cpml.targetReflection >= 1e-10 &&
                        o.cpml.targetReflection <= .1 && std::isfinite(o.cpml.kappaMax) &&
                        o.cpml.kappaMax >= 1 && o.cpml.kappaMax <= 8 && std::isfinite(o.cpml.frequency) &&
                        o.cpml.frequency >= 0 && o.cpml.frequency <= 10000,
                    "Invalid DRP CFS-CPML parameters.");
    Operator<T> op;
    op.nx = g.nx;
    op.nz = g.nz;
    Detail::ModelSettings config;
    config.compute = o.compute;
    op.threads = Detail::workers(config);
    rows(g.nx, p.spacing, op.dx, op.tx);
    rows(g.nz, p.spacing, op.dz, op.tz);
    op.fx = filterRows<T>(g.nx);
    op.fz = filterRows<T>(g.nz);
    op.geometry.resize(g.x.size());
    op.px.resize(g.nx);
    op.pz.resize(g.nz);
    double relief = 0, bound = 0, minimumVp = 1e100, minimumWeight = 1e100;
    for (int i = 0; i < g.nx; ++i)
        relief = std::max(relief, std::abs(g.z[i]));
    const double bottom = (p.nz - 1) * p.spacing;
    const double flatten =
        p.flattenDepth > 0
            ? p.flattenDepth
            : std::min(bottom, std::max({.35 * bottom, 6 * p.spacing, 4 * relief + 2 * p.spacing}));
    std::unique_ptr<Curvilinear::SurfaceProfile> profile;
    if (p.importedSurface)
        profile = std::make_unique<Curvilinear::SurfaceProfile>(p.surfaceSamples);
    for (int j = 0; j < g.nz; ++j)
        for (int i = 0; i < g.nx; ++i) {
            const auto q = std::size_t(j) * g.nx + i;
            const double t = std::clamp(j * p.spacing / flatten, 0., 1.);
            const double fade = 1 - std::pow(t, 5) * (126 + t * (-420 + t * (540 + t * (-315 + 70 * t))));
            const double fadeDerivative = -630 * std::pow(t * (1 - t), 4) / flatten;
            const double jacobian = 1 + g.z[i] * fadeDerivative;
            Detail::require(jacobian > .05 && std::isfinite(jacobian),
                            "DRP terrain Jacobian is too small or folded.");
            const double x = std::clamp(i - g.pad, 0, p.nx - 1) * p.spacing;
            double surfaceDerivative = profile ? -profile->sample(x)[1]
                                               : -p.relief * 2 * Detail::pi / p.wavelength *
                                                     std::sin(2 * Detail::pi * x / p.wavelength);
            if (i < g.pad || i > g.nx - 1 - g.pad)
                surfaceDerivative = 0;
            const double slope = surfaceDerivative * fade;
            op.geometry[q] = {T(1 / jacobian), T(slope), T(1 / g.rho[q]), T(g.rho[q] * g.vp[q] * g.vp[q])};
            const auto material = op.geometry[q];
            Detail::require(std::isfinite(material.bulk) && material.bulk > 0 &&
                                std::isfinite(material.inverseRho) && material.inverseRho > 0 &&
                                std::isfinite(material.inverseJ) && std::isfinite(material.slope),
                            "DRP material/metric exceeds selected floating-point precision.");
            minimumWeight = std::min(minimumWeight, jacobian / (g.rho[q] * g.vp[q] * g.vp[q]));
            bound = std::max(bound, g.vp[q] / p.spacing * (1 + (1 + std::abs(slope)) / jacobian));
            minimumVp = std::min(minimumVp, g.vp[q]);
        }
    const double filterBound = o.drpDissipation * g.maximumVp / p.spacing;
    for (auto &m : op.geometry)
        m.filterRate = T(filterBound * minimumWeight * double(m.bulk) * double(m.inverseJ) / 64);
    double maxDecay = 0;
    const double width = g.pad * p.spacing;
    const double sigmaMax = -2 * g.maximumVp * std::log(o.cpml.targetReflection) / width;
    const double frequency = o.cpml.frequency > 0 ? o.cpml.frequency : p.frequency;
    auto axis = [&](auto &v, int padLeft, int targetLast) {
        for (int i = 0; i < int(v.size()); ++i) {
            const double d =
                std::clamp(std::max(double(padLeft - i), double(i - targetLast)) / g.pad, 0., 1.);
            if (!o.damping || d == 0)
                continue;
            const double sigma = sigmaMax * d * d * d;
            if (o.absorber == AbsorbingBoundary::Cpml) {
                const double kappa = 1 + (o.cpml.kappaMax - 1) * d * d * d;
                v[i] = {T(1 / kappa), T(-sigma / (kappa * kappa)),
                        T(sigma / kappa + Detail::pi * frequency * (1 - d)), T(0)};
                maxDecay = std::max(maxDecay, double(v[i].b));
            } else {
                v[i].sponge = T(sigma);
                maxDecay = std::max(maxDecay, sigma);
            }
        }
    };
    axis(op.px, g.pad, g.nx - 1 - g.pad);
    axis(op.pz, 0, p.nz - 1);
    op.dt =
        T(.9 / (bound + 2 * maxDecay + 2 * filterBound)); // PML and selective dissipation participate in CFL.
    Detail::require(std::isfinite(op.dt) && op.dt > 0,
                    "DRP timestep exceeds selected floating-point precision.");
    Detail::require(minimumVp / (p.frequency * p.spacing) >= 12,
                    "Experimental DRP currently requires >=12 points per dominant P wavelength; "
                    "high-frequency tail needs separate checking.");
    for (int r = 0; r < p.receiverCount; ++r)
        op.receivers.push_back(p.receiverDepth * g.nx + g.pad + p.receiverStart + r * p.receiverStride);
    return op;
}
} // namespace Seis2D::Acoustic::Drp
