#include "Seis2D/CurvilinearSolver2D.hpp"
#include "PointKernels2D.hpp"
#include "MemoryBudget2D.hpp"
#ifdef SEIS3D_HAS_CUDA
#include "CudaEngine2D.hpp"
#endif
#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <limits>
#include <locale>
#include <sstream>
#include <stdexcept>
#ifdef SEIS3D_HAS_OPENMP
#include <omp.h>
#endif
namespace Seis2D::Curvilinear {
namespace {
constexpr double pi = 3.14159265358979323846;
using State = std::array<std::vector<double>, Point::maxFields>;
Point::Fields view(State &state) {
    Point::Fields w;
    for (int f = 0; f < Point::maxFields; ++f)
        w.f[f] = state[f].data();
    return w;
}
double pulse(double t, double f) {
    const double a = pi * f * (t - 1.5 / f);
    return (1 - 2 * a * a) * std::exp(-a * a);
}
bool material(double vp, double vs, double rho) {
    return std::isfinite(vp) && std::isfinite(vs) && std::isfinite(rho) && vp > 0 && vs > 0 && rho > 0 &&
           vp * vp > 4 * vs * vs / 3 && std::isfinite(rho * vp * vp);
}
} // namespace
struct Simulation::Impl {
    Parameters p;
    ComputeSettings3D settings;
    Grid grid;
    State fields[7];
    std::vector<double> lambda, mu, source, damping;
    std::array<std::vector<double>, 2> sourceGradient;
    std::array<std::vector<double>,3> deltaP,deltaS;
    Point::Mesh mesh;
    std::vector<Receiver> receivers;
    std::vector<Receiver> probes;
    std::vector<std::array<double, 2>> previousSamples;
    BackendInfo3D backend;
    Diagnostics diagnostics;
    int step{}, total{}, threads{1};
    double dt{};
    std::string fault;
#ifdef SEIS3D_HAS_CUDA
    std::unique_ptr<CudaEngine> cuda;
#endif
    void project(State &state) {
        for (int i = ghost; i < grid.nx - ghost; ++i)
            Point::project(view(state), mesh, i);
    }
    void rhs(State &state, State &result, double time, int stage) {
        for (auto &f : result)
            std::fill(f.begin(), f.end(), 0);
        const auto in = view(state), out = view(result);
        const auto m = mesh;
        const double forcing =
            (p.source.kind == SeisWave::SourceKind::IsotropicMoment ? p.momentPerLength : p.forcePerLength) /
            (p.spacing * p.spacing) * pulse(time, p.frequency);
#ifdef SEIS3D_HAS_OPENMP
#pragma omp parallel for schedule(static) num_threads(threads) if (threads > 1 && p.nx * p.nz >= 16384)
#endif
        for (int k = ghost; k < grid.nz - ghost; ++k)
            for (int i = ghost; i < grid.nx - ghost; ++i)
                Point::rhs(in, out, m, grid.index(i, k), forcing, step, stage);
    }
    void combine(State &out, const State &rate, double weight, bool final = false) {
#ifdef SEIS3D_HAS_OPENMP
#pragma omp parallel for schedule(static) num_threads(threads) if (threads > 1 && p.nx * p.nz >= 16384)
#endif
        for (int f = 0; f < mesh.fieldCount; ++f)
            for (std::size_t q = 0; q < mesh.count; ++q)
                out[f][q] = fields[1][f][q] + (final ? dt *
                                                           (fields[2][f][q] + 2 * fields[3][f][q] +
                                                            2 * fields[4][f][q] + fields[5][f][q]) /
                                                           6
                                                     : weight * rate[f][q]);
        project(out);
    }
    void inspect() {
#ifdef SEIS3D_HAS_CUDA
        if (cuda) {
            cuda->diagnostics(diagnostics);
            return;
        }
#endif
        diagnostics.finite = true;
        diagnostics.energy = diagnostics.maxVelocity = diagnostics.tractionRelative = 0;
        const auto w = view(fields[0]);
        for (int k = ghost; k < grid.nz - ghost; ++k)
            for (int i = ghost; i < grid.nx - ghost; ++i) {
                const auto q = grid.index(i, k);
                for (int f = 0; f < mesh.fieldCount; ++f)
                    diagnostics.finite &= std::isfinite(w.f[f][q]);
                diagnostics.energy += Point::energy(w, mesh, q);
                diagnostics.maxVelocity = std::max(diagnostics.maxVelocity, Point::value(w, mesh, q, 2));
                if (k == ghost) {
                    const auto n = grid.normals[q];
                    const double tx = w.f[2][q] * n[0] + w.f[4][q] * n[1],
                                 tz = w.f[4][q] * n[0] + w.f[3][q] * n[1],
                                 norm = std::sqrt(w.f[2][q] * w.f[2][q] + w.f[3][q] * w.f[3][q] +
                                                  2 * w.f[4][q] * w.f[4][q]);
                    diagnostics.tractionRelative =
                        std::max(diagnostics.tractionRelative, std::hypot(tx, tz) / std::max(norm, 1e-25));
                }
            }
        diagnostics.finite &= std::isfinite(diagnostics.energy) && std::isfinite(diagnostics.maxVelocity);
    }
    void sample() {
        for (auto &r : probes) {
            r.vx.clear();
            r.vz.clear();
        }
#ifdef SEIS3D_HAS_CUDA
        if (cuda) {
            cuda->sample(probes);
        } else
#endif
        for (auto &r : probes) {
            const auto q = grid.index(r.x + ghost, r.z + ghost);
            r.vx.push_back(fields[0][0][q]);
            r.vz.push_back(fields[0][1][q]);
        }
        const double interval = p.sampleIntervalMs > 0 ? p.sampleIntervalMs * .001 : dt;
        const double end = (step + 1) * dt;
        // Integer sample number avoids accumulated rounding error and preserves SEG-Y's uniform axis.
        for (std::size_t n = receivers.front().vx.size(); n * interval <= end + 1e-9 * dt; ++n) {
            const double a = std::clamp((n * interval - step * dt) / dt, 0.0, 1.0);
            for (std::size_t r = 0; r < receivers.size(); ++r) {
                receivers[r].vx.push_back(std::lerp(previousSamples[r][0], probes[r].vx.back(), a));
                receivers[r].vz.push_back(std::lerp(previousSamples[r][1], probes[r].vz.back(), a));
            }
        }
        for (std::size_t r = 0; r < receivers.size(); ++r)
            previousSamples[r] = {probes[r].vx.back(), probes[r].vz.back()};
    }
};
Simulation::Simulation() = default;
Simulation::~Simulation() = default;
bool Simulation::initialize(const Parameters &p, std::string &error, BackendKind3D kind,
                            const ComputeSettings3D &settings) {
    const auto begin = std::chrono::steady_clock::now();
    error.clear();
    try {
        const auto require = [](bool ok, const char *message) {
            if (!ok)
                throw std::runtime_error(message);
        };
        require(p.nx >= 8 && p.nz >= 8 && p.nx <= maxAxisNodes && p.nz <= maxAxisNodes,
                "2D dimensions must be in 8..16384 (subject to RAM/VRAM).");
        for (double v : {p.spacing, p.relief, p.wavelength, p.flattenDepth, p.frequency, p.duration, p.cfl,
                         p.forcePerLength, p.momentPerLength, p.spongeStrength, p.surfaceDatum, p.sampleIntervalMs})
            require(std::isfinite(v), "2D parameters must be finite.");
        require(p.spacing > 0 && p.frequency > 0 && p.frequency <= 1e6 && p.duration > 0 &&
                    p.duration <= 10 && p.cfl >= .02 && p.cfl <= .8 && p.wavelength > 0 &&
                    p.flattenDepth >= 0 &&
                    (p.source.kind == SeisWave::SourceKind::IsotropicMoment ? p.momentPerLength
                                                                            : p.forcePerLength) > 0,
                "Invalid 2D spacing, frequency, duration, CFL or source amplitude.");
        require(material(p.vp, p.vs, p.density),
                "Require positive material values and positive elastic bulk modulus.");
        require(p.sampleIntervalMs == 0 || (p.sampleIntervalMs >= .01 && p.sampleIntervalMs <= 1000),
                "Record interval must be 0 (each step) or 0.01..1000 ms.");
        require(p.maxSteps >= 0 && p.maxSteps <= maxTimeSteps && settings.cpuThreads >= 0 &&
                    settings.cpuThreads <= 64,
                "Step cap must be 0..200000 (0 = full duration); CPU workers must be 0..64.");
        if (!SeisWave::validateSource(p.source, true, error))
            throw std::runtime_error(error);
        const bool explosive = p.source.kind == SeisWave::SourceKind::IsotropicMoment;
        const int sx = p.sourceX < 0 ? p.nx / 2 : p.sourceX;
        if (explosive)
            require(
                p.nx >= 11 && p.nz >= 11 && sx >= 5 && sx <= p.nx - 6 && p.sourceZ >= 5 &&
                    p.sourceZ <= p.nz - 6,
                "Isotropic moment needs 11+ nodes per axis and source center five nodes inside each face.");
        require(p.sourceX >= -1 && sx >= 2 && sx <= p.nx - 3 && p.sourceZ >= 2 && p.sourceZ <= p.nz - 3,
                "2D source must stay at least two cells inside the domain.");
        require(p.receiverStart >= 0 && p.receiverCount > 0 && p.receiverCount <= 1024 &&
                    p.receiverStride > 0 && p.receiverDepth >= 0 && p.receiverDepth < p.nz &&
                    static_cast<long long>(p.receiverStart) +
                            (static_cast<long long>(p.receiverCount) - 1) * p.receiverStride <
                        p.nx,
                "Receiver line is outside the 2D model or exceeds 1024 positions.");
        require(p.spongeX >= 1 && p.spongeX <= maxSpongeCells && p.spongeZ >= 1 && p.spongeZ <= maxSpongeCells &&
                    p.spongeStrength >= .1 && p.spongeStrength <= 10,
                "Sponge widths must be 1..512 cells (effective width <= axis nodes / 3) and strength 0.1..10.");
        if (p.layered) {
            require(!p.layers.empty() && p.layers.size() <= 16, "Require 1..16 material layers.");
            for (std::size_t l = 0; l < p.layers.size(); ++l) {
                const auto &layer = p.layers[l];
                require(material(layer.vp, layer.vs, layer.density) && std::isfinite(layer.thickness) &&
                            layer.thickness >= 0 && (l + 1 == p.layers.size() || layer.thickness > 0),
                        "Invalid layer thickness or material.");
            }
        }
        if(!SeisWave::Rheology::validateSettings(p.attenuation,error))throw std::runtime_error(error);
        std::vector<SeisWave::Rheology::Material> rheology;
        if(p.attenuation.enabled){
            const auto fit=[&](double vp,double vs,double rho,double qp,double qs){SeisWave::Rheology::Material m;if(!SeisWave::Rheology::fitMaterial(vp,vs,rho,qp,qs,p.attenuation,m,error))throw std::runtime_error(error);rheology.push_back(m);};
            if(p.layered)for(const auto& l:p.layers)fit(l.vp,l.vs,l.density,l.qp,l.qs);
            else fit(p.vp,p.vs,p.density,p.attenuation.qp,p.attenuation.qs);
        }
        const auto available = Seis3D::Curvilinear::queryBackend3D(kind, settings.cudaDevice);
        require(available.available, available.description.c_str());
        // Applying an unchanged configuration only clears the wavefield and
        // records. Reuse storage before checking space for a second full model.
        if (impl_ && impl_->p == p && impl_->settings == settings && impl_->backend.kind == kind &&
            impl_->backend.cudaDevice == available.cudaDevice) {
            if (!reset(error))
                return false;
            impl_->diagnostics.initializationMs =
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();
            return true;
        }
        const std::size_t count = static_cast<std::size_t>(p.nx + 6) * (p.nz + 6);
        const auto memory = Seis3D::Curvilinear::queryBackendMemory3D(kind, available.cudaDevice);
        const auto budget = Detail::memoryBudget(p, kind);
        const auto requireMemory = [&](double bytes, std::uint64_t free, double fraction, const char *name) {
            if (bytes < free * fraction)
                return;
            constexpr double gib = 1024. * 1024 * 1024;
            std::ostringstream message;
            message.imbue(std::locale::classic());
            message << "Insufficient " << name << " for a transactional 2D rebuild. "
                    << std::fixed << std::setprecision(2) << "Additional budget: " << bytes / gib
                    << " GiB; available: " << free / gib << " GiB; required free with reserve: "
                    << bytes / fraction / gib << " GiB.";
            if (impl_)
                message << " The active model is retained until the replacement succeeds.";
            throw std::runtime_error(message.str());
        };
        if (memory.hostAvailableBytes)
            requireMemory(budget.hostBytes, memory.hostAvailableBytes, .8, "RAM");
        if (kind == BackendKind3D::Cuda)
            requireMemory(budget.deviceBytes, memory.deviceAvailableBytes, .85, "VRAM");
        auto next = std::make_unique<Impl>();
        auto &s = *next;
        s.p = p;
        s.settings = settings;
        s.backend.kind = kind;
        s.backend.deviceName = available.description;
        s.backend.cudaDevice = available.cudaDevice;
#ifdef SEIS3D_HAS_OPENMP
        s.backend.openMpAvailable = true;
        s.threads = settings.cpuThreads ? settings.cpuThreads : std::min(8, omp_get_num_procs());
#else
        require(settings.cpuThreads <= 1, "This build supports one CPU worker.");
#endif
        s.backend.workerThreads = kind == BackendKind3D::Cpu ? s.threads : 1;
        auto &g = s.grid;
        g.nx = p.nx + 6;
        g.nz = p.nz + 6;
        g.h = p.spacing;
        const double bottom = (p.nz - 1) * p.spacing;
        std::vector<std::array<double, 2>> surface(g.nx);
        double relief = std::abs(p.relief);
        if (p.importedSurface) {
            SurfaceProfile profile(p.surfaceSamples);
            require(p.surfaceSamples.front()[0] <= 0 && p.surfaceSamples.back()[0] >= (p.nx - 1) * p.spacing,
                    "Imported terrain must cover x=0 through the full physical model span.");
            relief = 0;
            for (int i = 0; i < g.nx; ++i) {
                const auto value = profile.sample((i - ghost) * p.spacing);
                surface[i] = {p.surfaceDatum - value[0], -value[1]};
                relief = std::max(relief, std::abs(surface[i][0]));
            }
        } else {
            for (int i = 0; i < g.nx; ++i) {
                const double phase = 2 * pi * (i - ghost) * p.spacing / p.wavelength;
                surface[i] = {p.relief * std::cos(phase),
                              -p.relief * 2 * pi / p.wavelength * std::sin(phase)};
            }
        }
        g.flattenDepth =
            p.flattenDepth > 0
                ? p.flattenDepth
                : std::min(bottom, std::max({.35 * bottom, 6 * p.spacing, 4 * relief + 2 * p.spacing}));
        require(g.flattenDepth <= bottom &&
                    (relief == 0 || ((p.importedSurface || p.wavelength >= 4 * p.spacing) &&
                                     g.flattenDepth >= 4 * p.spacing && g.flattenDepth > 2.6 * relief)),
                "Terrain must be resolved and flatten depth must exceed 2.6 times relief within the model.");
        for (auto *v : {&g.metricA, &g.metricB, &g.jacobian, &g.vp, &g.vs, &g.density, &s.lambda, &s.mu,
                        &s.source, &s.damping})
            v->assign(count, 0);
        g.coordinates.resize(count);
        g.normals.resize(count);
        if(p.attenuation.enabled){g.qp.resize(count);g.qs.resize(count);for(int l=0;l<p.attenuation.mechanisms;++l){s.deltaP[l].resize(count);s.deltaS[l].resize(count);}}
        g.minJacobian = std::numeric_limits<double>::max();
        double maxVp = 0, minVs = std::numeric_limits<double>::max(), maxSigma = 0;
        for (int k = 0; k < g.nz; ++k)
            for (int i = 0; i < g.nx; ++i) {
                const auto q = g.index(i, k);
                const double x = (i - ghost) * p.spacing, eta = (k - ghost) * p.spacing,
                             t = std::clamp(eta / g.flattenDepth, 0.0, 1.0);
                const double fade = 1 - std::pow(t, 5) * (126 + t * (-420 + t * (540 + t * (-315 + 70 * t))));
                const double fadeDerivative =
                    eta > 0 && eta < g.flattenDepth
                        ? -630 * std::pow(t, 4) * std::pow(1 - t, 4) / g.flattenDepth
                        : 0;
                const double zx = surface[i][1] * fade, j = 1 + surface[i][0] * fadeDerivative;
                require(j > 1e-9 && std::isfinite(j), "Nonpositive curvilinear Jacobian.");
                g.coordinates[q] = {x, eta + surface[i][0] * fade};
                g.metricA[q] = -zx / j;
                g.metricB[q] = 1 / j;
                g.jacobian[q] = j;
                const double n = std::hypot(zx, 1.0);
                g.normals[q] = {-zx / n, 1 / n};
                double vp = p.vp, vs = p.vs, rho = p.density;
                std::size_t materialIndex=0;
                if (p.layered) {
                    double boundary = 0;
                    for (std::size_t l = 0; l < p.layers.size(); ++l) {
                        const auto &layer = p.layers[l];
                        boundary += layer.thickness;
                        if (l + 1 == p.layers.size() || std::max(0.0, eta) < boundary) {
                            vp = layer.vp;
                            vs = layer.vs;
                            rho = layer.density;
                            materialIndex=l;
                            break;
                        }
                    }
                }
                g.vp[q] = vp;
                g.vs[q] = vs;
                g.density[q] = rho;
                s.mu[q] = rho * vs * vs;
                s.lambda[q] = rho * vp * vp - 2 * s.mu[q];
                double speedMax=vp,speedMin=vs;
                if(p.attenuation.enabled){const auto& m=rheology[materialIndex];s.mu[q]=m.sUnrelaxed;s.lambda[q]=m.pUnrelaxed-2*m.sUnrelaxed;
                    g.qp[q]=p.layered?p.layers[materialIndex].qp:p.attenuation.qp;g.qs[q]=p.layered?p.layers[materialIndex].qs:p.attenuation.qs;
                    for(int l=0;l<m.mechanisms;++l){s.deltaP[l][q]=m.deltaP[l];s.deltaS[l][q]=m.deltaS[l];}
                    speedMax=std::sqrt(m.pUnrelaxed/rho);speedMin=std::sqrt(m.sRelaxed/rho);
                }
                if (i >= ghost && i < g.nx - ghost && k >= ghost && k < g.nz - ghost) {
                    g.minJacobian = std::min(g.minJacobian, j);
                    g.maxMetric = std::max(g.maxMetric, std::sqrt(1 + g.metricA[q] * g.metricA[q] +
                                                                  g.metricB[q] * g.metricB[q]));
                    maxVp = std::max(maxVp, speedMax);
                    minVs = std::min(minVs, speedMin);
                }
            }
        for (int k = 0; k < p.nz; ++k)
            for (int i = 0; i < p.nx; ++i) {
                const auto q = g.index(i + ghost, k + ghost);
                if (i + 1 < p.nx) {
                    const auto b = g.coordinates[q + 1];
                    g.maxEdge = std::max(g.maxEdge,
                                         std::hypot(b[0] - g.coordinates[q][0], b[1] - g.coordinates[q][1]));
                }
                if (k + 1 < p.nz)
                    g.maxEdge = std::max(g.maxEdge, g.coordinates[q + g.nx][1] - g.coordinates[q][1]);
                if (p.absorbing) {
                    const int wx = std::min(p.spongeX, p.nx / 3), wz = std::min(p.spongeZ, p.nz / 3);
                    const double x = std::max(0.0, 1.0 - std::min(i, p.nx - 1 - i) / static_cast<double>(wx)),
                                 z = std::max(0.0, 1.0 - (p.nz - 1 - k) / static_cast<double>(wz));
                    s.damping[q] = p.spongeStrength * maxVp / p.spacing * (x * x / wx + z * z / wz);
                    maxSigma = std::max(maxSigma, s.damping[q]);
                }
            }
        double weight = 0;
        for (int dk = -2; dk <= 2; ++dk)
            for (int di = -2; di <= 2; ++di) {
                const auto q = g.index(sx + ghost + di, p.sourceZ + ghost + dk);
                s.source[q] = std::exp(-.5 * (di * di + dk * dk));
                weight += s.source[q] * g.jacobian[q];
            }
        for (double &v : s.source)
            v /= weight;
        if (explosive) {
            for (auto &v : s.sourceGradient)
                v.assign(count, 0);
            const auto flux = [&](std::ptrdiff_t q, int axis, int component) {
                const double metric =
                    axis == 0 ? (component == 0 ? 1. : 0.) : (component == 0 ? g.metricA[q] : g.metricB[q]);
                return g.jacobian[q] * metric * s.source[q];
            };
            for (int k = p.sourceZ - 4; k <= p.sourceZ + 4; ++k)
                for (int i = sx - 4; i <= sx + 4; ++i) {
                    const auto q = g.index(i + ghost, k + ghost);
                    const auto force =
                        SeisWave::isotropicMomentPattern(q, 2, {1, g.nx, 0}, p.spacing, g.jacobian[q], flux);
                    for (int a = 0; a < 2; ++a)
                        s.sourceGradient[a][q] = force[a];
                }
        }
        auto &d = s.diagnostics;
        d.ppw = minVs / (2.5 * p.frequency * g.maxEdge);
        d.waveDt = p.cfl * p.spacing / (maxVp * g.maxMetric);
        d.dampingDt = maxSigma > 0 ? .5 / maxSigma : std::numeric_limits<double>::infinity();
        d.sourceDt = 1 / (50 * p.frequency);
        d.relaxationDt=p.attenuation.enabled?.25/rheology.front().rates[p.attenuation.mechanisms-1]:std::numeric_limits<double>::infinity();
        d.stateFields=p.attenuation.enabled?6+4*p.attenuation.mechanisms:5;
        for(const auto& m:rheology)d.maxRelativeQError=std::max(d.maxRelativeQError,m.maxRelativeQError);
        const double safe = std::min({d.waveDt, d.dampingDt, d.sourceDt,d.relaxationDt});
        const double full = std::ceil(p.duration / safe);
        require(std::isfinite(full) && full <= 10000000,
                "Requested duration needs too many steps; increase spacing or lower frequency.");
        s.dt = p.duration / full;
        s.total = p.maxSteps ? std::min(static_cast<int>(full), p.maxSteps) : static_cast<int>(full);
        require(s.total <= maxTimeSteps,
                "2D session exceeds 200000 accepted steps; set an explicit cap or shorter duration. A cap ends before the requested duration.");
        for (int r = 0; r < p.receiverCount; ++r) {
            Receiver receiver;
            receiver.x = p.receiverStart + r * p.receiverStride;
            receiver.z = p.receiverDepth;
            receiver.position = g.coordinates[g.index(receiver.x + ghost, receiver.z + ghost)];
            const auto samples = static_cast<std::size_t>(std::floor(s.total * s.dt /
                (p.sampleIntervalMs > 0 ? p.sampleIntervalMs * .001 : s.dt))) + 1;
            receiver.vx.reserve(samples);
            receiver.vz.reserve(samples);
            receiver.vx.push_back(0);
            receiver.vz.push_back(0);
            s.receivers.push_back(std::move(receiver));
        }
        s.probes = s.receivers;
        s.previousSamples.resize(s.receivers.size(), {0, 0});
        s.mesh = {g.nx,
                  g.nz,
                  count,
                  p.spacing,
                  g.metricA.data(),
                  g.metricB.data(),
                  g.jacobian.data(),
                  reinterpret_cast<const double *>(g.normals.data()),
                  s.lambda.data(),
                  s.mu.data(),
                  g.density.data(),
                  s.source.data(),
                  s.damping.data()};
        const auto direction = SeisWave::unitSourceDirection(p.source);
        s.mesh.fieldCount=d.stateFields;
        if(p.attenuation.enabled){s.mesh.mechanisms=p.attenuation.mechanisms;for(int l=0;l<s.mesh.mechanisms;++l){s.mesh.rates[l]=rheology.front().rates[l];s.mesh.deltaP[l]=s.deltaP[l].data();s.mesh.deltaS[l]=s.deltaS[l].data();}}
        for (int a = 0; a < 3; ++a)
            s.mesh.sourceDirection[a] = direction[a];
        if (explosive)
            for (int a = 0; a < 2; ++a)
                s.mesh.sourceGradient[a] = s.sourceGradient[a].data();
#ifdef SEIS3D_HAS_CUDA
        if (kind == BackendKind3D::Cuda)
            s.cuda = std::make_unique<CudaEngine>(s.mesh, s.receivers, available.cudaDevice);
        else
#endif
            for (auto &state : s.fields)
                for (int f=0;f<s.mesh.fieldCount;++f)
                    state[f].assign(count, 0);
        d.initializationMs =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();
        impl_ = std::move(next);
        return true;
    } catch (const std::exception &ex) {
        error = ex.what();
        return false;
    }
}
bool Simulation::advance(int count, std::string &error) {
    error.clear();
    if (!impl_) {
        error = "Initialize the 2D model first.";
        return false;
    }
    auto &s = *impl_;
    if (!s.fault.empty()) {
        error = s.fault;
        return false;
    }
    try {
        while (count-- > 0 && s.step < s.total) {
            const auto start = std::chrono::steady_clock::now();
            const auto before = s.diagnostics;
#ifdef SEIS3D_HAS_CUDA
            if (s.cuda)
                s.cuda->step(s.step * s.dt, s.dt, s.p.frequency,
                             (s.p.source.kind == SeisWave::SourceKind::IsotropicMoment ? s.p.momentPerLength
                                                                                       : s.p.forcePerLength) /
                                 (s.p.spacing * s.p.spacing),
                             s.step);
            else
#endif
            {
                s.fields[1] = s.fields[0];
                s.rhs(s.fields[0], s.fields[2], s.step * s.dt, 0);
                s.combine(s.fields[6], s.fields[2], s.dt / 2);
                s.rhs(s.fields[6], s.fields[3], (s.step + .5) * s.dt, 1);
                s.combine(s.fields[6], s.fields[3], s.dt / 2);
                s.rhs(s.fields[6], s.fields[4], (s.step + .5) * s.dt, 2);
                s.combine(s.fields[6], s.fields[4], s.dt);
                s.rhs(s.fields[6], s.fields[5], (s.step + 1) * s.dt, 3);
                s.combine(s.fields[0], s.fields[2], 0, true);
            }
            s.inspect();
            if (!s.diagnostics.finite || ((s.step + 1) * s.dt > 3 / s.p.frequency && before.peakEnergy > 0 &&
                                          s.diagnostics.energy > before.peakEnergy * 10000)) {
#ifdef SEIS3D_HAS_CUDA
                if (s.cuda)
                    s.cuda->rollback();
                else
#endif
                    s.fields[0] = s.fields[1];
                s.diagnostics = before;
                s.fault = "2D step rejected: nonfinite fields or excessive late energy growth. Rebuild the "
                          "session.";
                error = s.fault;
                return false;
            }
            s.sample();
            ++s.step;
            s.diagnostics.peakEnergy = std::max(before.peakEnergy, s.diagnostics.energy);
            s.diagnostics.lastStepMs =
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
            s.diagnostics.totalStepMs += s.diagnostics.lastStepMs;
        }
        return true;
    } catch (const std::exception &ex) {
        s.fault = ex.what();
        error = s.fault;
        return false;
    }
}
bool Simulation::reset(std::string &error) {
    error.clear();
    if (!impl_) {
        error = "Initialize the 2D model first.";
        return false;
    }
    try {
        auto &s = *impl_;
#ifdef SEIS3D_HAS_CUDA
        if (s.cuda)
            s.cuda->reset();
        else
#endif
            for (auto &state : s.fields)
                for (auto &field : state)
                    std::fill(field.begin(), field.end(), 0);
        s.step = 0;
        s.fault.clear();
        s.diagnostics.energy = s.diagnostics.peakEnergy = s.diagnostics.maxVelocity =
            s.diagnostics.tractionRelative = s.diagnostics.totalStepMs = s.diagnostics.lastStepMs = 0;
        s.diagnostics.finite = true;
        for (auto &r : s.receivers) {
            r.vx.assign(1, 0);
            r.vz.assign(1, 0);
        }
        std::fill(s.previousSamples.begin(), s.previousSamples.end(), std::array<double, 2>{0, 0});
        return true;
    } catch (const std::exception &ex) {
        error = ex.what();
        return false;
    }
}
bool Simulation::initialized() const {
    return static_cast<bool>(impl_);
}
bool Simulation::completed() const {
    return impl_ && impl_->step >= impl_->total;
}
int Simulation::step() const {
    return impl_ ? impl_->step : 0;
}
int Simulation::totalSteps() const {
    return impl_ ? impl_->total : 0;
}
double Simulation::dt() const {
    return impl_ ? impl_->dt : 0;
}
double Simulation::recordDt() const {
    return impl_ ? (impl_->p.sampleIntervalMs > 0 ? impl_->p.sampleIntervalMs * .001 : impl_->dt) : 0;
}
const Parameters &Simulation::parameters() const {
    return impl_->p;
}
const Grid &Simulation::grid() const {
    return impl_->grid;
}
const Diagnostics &Simulation::diagnostics() const {
    return impl_->diagnostics;
}
const std::vector<Receiver> &Simulation::receivers() const {
    return impl_->receivers;
}
BackendInfo3D Simulation::backend() const {
    return impl_ ? impl_->backend : BackendInfo3D{};
}
bool Simulation::readFrame(int component, std::vector<float> &out, std::string &error) const {
    error.clear();
    if (!impl_ || component < 0 || component > 3) {
        error = "Invalid 2D display component.";
        return false;
    }
    try {
        auto &s = *impl_;
#ifdef SEIS3D_HAS_CUDA
        if (s.cuda) {
            s.cuda->frame(component, out);
            return true;
        }
#endif
        out.resize(static_cast<std::size_t>(s.p.nx) * s.p.nz);
        const auto w = view(s.fields[0]);
        for (int k = 0; k < s.p.nz; ++k)
            for (int i = 0; i < s.p.nx; ++i)
                out[i + static_cast<std::size_t>(s.p.nx) * k] = static_cast<float>(
                    Point::value(w, s.mesh, s.grid.index(i + ghost, k + ghost), component));
        return true;
    } catch (const std::exception &ex) {
        error = ex.what();
        return false;
    }
}
Snapshot Simulation::snapshot(bool includeMemories) const {
    if (!impl_)
        throw std::runtime_error("No 2D session.");
#ifdef SEIS3D_HAS_CUDA
    if (impl_->cuda)
        return impl_->cuda->snapshot(includeMemories);
#endif
    Snapshot result;for(int f=0;f<5;++f)result.fields[f]=impl_->fields[0][f];
    if(impl_->mesh.mechanisms){result.syy=impl_->fields[0][5];if(includeMemories)for(int f=6;f<impl_->mesh.fieldCount;++f)result.memory.push_back(impl_->fields[0][f]);}
    return result;
}
bool writeSnapshot(const Simulation &s, const std::filesystem::path &path, std::string &error,
                   const SnapshotExportOptions &options) {
    try {
        if (std::none_of(options.components.begin(), options.components.end(), [](bool v) { return v; }))
            throw std::runtime_error("Select at least one wavefield export component.");
        const auto data = s.snapshot();
        const auto &p = s.parameters();
        const auto &g = s.grid();
        std::ofstream out(path);
        if (!out)
            throw std::runtime_error("Cannot create 2D VTS file.");
        out.imbue(std::locale::classic());
        out << std::setprecision(17)
            << "<?xml version=\"1.0\"?>\n<VTKFile type=\"StructuredGrid\" version=\"0.1\" "
               "byte_order=\"LittleEndian\"><StructuredGrid WholeExtent=\"0 "
            << p.nx - 1 << " 0 0 0 " << p.nz - 1 << "\">\n";
        const auto direction = SeisWave::unitSourceDirection(p.source);
        out << "<FieldData>";
        const auto scalar = [&](const char *name, double value) {
            out << "<DataArray type=\"Float64\" Name=\"" << name
                << "\" NumberOfTuples=\"1\" format=\"ascii\">" << value << "</DataArray>";
        };
        scalar("SourceKind", static_cast<int>(p.source.kind));
        scalar("SourceForcePerLengthNPerM", p.forcePerLength);
        scalar("SourceMomentPerLengthN", p.momentPerLength);
        scalar("SourceFrequencyHz", p.frequency);
        scalar("SourceDirectionX", direction[0]);
        scalar("SourceDirectionZ", direction[2]);
        scalar("SimulationDt", s.dt());
        scalar("RecordIntervalSeconds", s.recordDt());
        scalar("SimulationStep", s.step());
        scalar("TIME_VALUE", s.step() * s.dt());
        scalar("AttenuationGSLS",p.attenuation.enabled);scalar("QReferenceFrequencyHz",p.attenuation.referenceFrequency);scalar("QBandMinHz",p.attenuation.minFrequency);scalar("QBandMaxHz",p.attenuation.maxFrequency);scalar("RelaxationMechanisms",p.attenuation.enabled?p.attenuation.mechanisms:0);
        const char *names[]{"Vx", "Vz", "VelocityMagnitude", "Pressure", "Sxx", "Szz", "Sxz", "Vp", "Vs", "Density"};
        const auto first = std::find(options.components.begin(), options.components.end(), true) - options.components.begin();
        out << "</FieldData><Piece Extent=\"0 " << p.nx - 1 << " 0 0 0 " << p.nz - 1
            << "\"><PointData Scalars=\"" << names[first] << "\">\n";
        for (int f = 0; f < 10; ++f) {
            if (f < 7 ? !options.components[f] : !options.includeMaterial)
                continue;
            out << "<DataArray type=\"Float64\" Name=\"" << names[f] << "\" format=\"ascii\">\n";
            for (int k = 0; k < p.nz; ++k)
                for (int i = 0; i < p.nx; ++i) {
                    const auto q = g.index(i + ghost, k + ghost);
                    double value{};
                    if (f < 2)
                        value = data.fields[f][q];
                    else if (f == 2)
                        value = std::sqrt(data.fields[0][q] * data.fields[0][q] + data.fields[1][q] * data.fields[1][q]);
                    else if (f == 3) {
                        // Plane strain: Syy=lambda*(Sxx+Szz)/(2*(lambda+mu)); p=-tr(S)/3.
                        // Same physical pressure as the CPU/CUDA display, without Float32 conversion.
                        const double mu = g.density[q] * g.vs[q] * g.vs[q];
                        const double lambda = g.density[q] * g.vp[q] * g.vp[q] - 2 * mu;
                        value = p.attenuation.enabled?-(data.fields[2][q]+data.fields[3][q]+data.syy[q])/3:-(1 + lambda / (2 * (lambda + mu))) * (data.fields[2][q] + data.fields[3][q]) / 3;
                    } else if (f < 7)
                        value = data.fields[f - 2][q];
                    else
                        value = f == 7 ? g.vp[q] : f == 8 ? g.vs[q] : g.density[q];
                    out << value << ' ';
                }
            out << "\n</DataArray>\n";
        }
        if(p.attenuation.enabled){const auto array=[&](const char* name,const std::vector<double>& values){out<<"<DataArray type=\"Float64\" Name=\""<<name<<"\" format=\"ascii\">\n";for(int k=0;k<p.nz;++k)for(int i=0;i<p.nx;++i)out<<values[g.index(i+ghost,k+ghost)]<<' ';out<<"\n</DataArray>\n";};
            if(options.includeMaterial){array("Qp",g.qp);array("Qs",g.qs);}if(options.components[3]||options.components[4]||options.components[5])array("Syy",data.syy);}
        out << "</PointData><Points><DataArray type=\"Float64\" NumberOfComponents=\"3\" format=\"ascii\">\n";
        for (int k = 0; k < p.nz; ++k)
            for (int i = 0; i < p.nx; ++i) {
                const auto x = g.coordinates[g.index(i + ghost, k + ghost)];
                out << x[0] << " 0 " << x[1] << ' ';
            }
        out << "</DataArray></Points></Piece></StructuredGrid></VTKFile>\n";
        if (!out)
            throw std::runtime_error("2D VTS write failed.");
        error.clear();
        return true;
    } catch (const std::exception &ex) {
        error = ex.what();
        return false;
    }
}
bool writeReceivers(const Simulation &s, const std::filesystem::path &path, std::string &error) {
    try {
        if (!s.initialized())
            throw std::runtime_error("Initialize a simulation before exporting traces.");
        if (path.empty() || path.filename().empty())
            throw std::runtime_error("Choose a receiver CSV file name.");
        if (!path.parent_path().empty())
            std::filesystem::create_directories(path.parent_path());
        std::ofstream out(path);
        if (!out)
            throw std::runtime_error("Cannot create 2D receiver CSV.");
        out.imbue(std::locale::classic());
        out << std::setprecision(17)
            << "# P-SV plane strain; x,z in m; velocity in m/s; t=0 included\n# dt=" << s.recordDt()
            << "\n# solver_dt=" << s.dt()
            << "\n# backend=" << s.backend().deviceName;
        const auto &p = s.parameters();
        const auto direction = SeisWave::unitSourceDirection(p.source);
        out<<"\n# GSLS="<<p.attenuation.enabled<<"; mechanisms="<<p.attenuation.mechanisms<<"; reference_Hz="<<p.attenuation.referenceFrequency<<"; fit_band_Hz="<<p.attenuation.minFrequency<<':'<<p.attenuation.maxFrequency;
        if(p.layered){for(std::size_t i=0;i<p.layers.size();++i)out<<"\n# layer_"<<i<<"_Qp_Qs="<<p.layers[i].qp<<','<<p.layers[i].qs;}else out<<"; Qp="<<p.attenuation.qp<<"; Qs="<<p.attenuation.qs;
        out << "\n# source_kind=" << static_cast<int>(p.source.kind)
            << " (0=directional_force,1=isotropic_moment); force_per_length_N_per_m=" << p.forcePerLength
            << "; diagonal_moment_per_length_N=" << p.momentPerLength << "; direction_XZ=" << direction[0]
            << ',' << direction[2] << "; Ricker_f0_Hz=" << p.frequency
            << "\nreceiver,x_m,z_m,time_s,vx_m_s,vz_m_s\n";
        int id = 0;
        for (const auto &r : s.receivers()) {
            for (std::size_t t = 0; t < r.vx.size(); ++t)
                out << id << ',' << r.position[0] << ',' << r.position[1] << ',' << t * s.recordDt() << ','
                    << r.vx[t] << ',' << r.vz[t] << '\n';
            ++id;
        }
        if (!out)
            throw std::runtime_error("2D receiver write failed.");
        error.clear();
        return true;
    } catch (const std::exception &ex) {
        error = ex.what();
        return false;
    }
}
} // namespace Seis2D::Curvilinear
