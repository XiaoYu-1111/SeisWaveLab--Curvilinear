#include "Seis2D/AcousticSolver2D.hpp"
#include "AcousticModel2D.hpp"
#include "AcousticSession2D.hpp"
#include "Seis2D/SurfaceSurvey2D.hpp"
#include <chrono>
// Forward-only adapter reuses exactly the acoustic geometry, operator and CUDA kernels used by RTM.
// It owns ONE propagator: no true/background pair, adjoint, checkpoints or replay history.
namespace Seis2D::Acoustic {
using Detail::require;
namespace {
template <class Real> struct Forward final : Simulation::Impl {
    Detail::Operator<Real> op;
    std::unique_ptr<Detail::Engine<Real>> engine;
    std::vector<int> crop;
    int source{};
    Real sourceScale{};
    explicit Forward(const Options &o) {
        const auto &p = o.model;
        require(p.maxSteps >= 0 && p.maxSteps <= 200000, "Acoustic step cap must be 0..200000.");
        require(std::isfinite(o.sourceStrength) && o.sourceStrength > 0 && o.sourceStrength <= 1e30,
                "Invalid acoustic pressure source strength.");
        require(p.receiverStart >= 0 && p.receiverCount > 0 && p.receiverCount <= 4096 &&
                    p.receiverStride > 0 &&
                    static_cast<long long>(p.receiverStart) +
                            static_cast<long long>(p.receiverCount - 1) * p.receiverStride <
                        p.nx,
                "Acoustic receiver line is outside the target model.");
        require(std::isfinite(p.sampleIntervalMs) &&
                    (p.sampleIntervalMs == 0 || (p.sampleIntervalMs >= .01 && p.sampleIntervalMs <= 1000)),
                "Invalid acoustic receiver sample interval.");
        int sx = p.sourceX < 0 ? p.nx / 2 : p.sourceX;
        require(sx >= 1 && sx < p.nx - 1,
                "Acoustic source X must be at least one target node inside each side.");
        require(p.sourceZ >= 1 && p.sourceZ < p.nz - 1 && p.receiverDepth >= 1 && p.receiverDepth < p.nz - 1,
                "Acoustic sources and receivers must be below the zero-pressure surface.");
        Detail::ModelSettings config;
        config.model = p;
        config.compute = o.compute;
        config.backend = o.backend;
        config.absorberCells = o.absorberCells;
        require(o.absorber == AbsorbingBoundary::Sponge || o.absorber == AbsorbingBoundary::Cpml,
                "Unknown acoustic absorbing boundary.");
        config.cpml = o.damping && o.absorber == AbsorbingBoundary::Cpml;
        config.cpmlSettings = o.cpml;
        config.memoryMiB = o.memoryMiB;
        config.smoothIterations = 0;
        const auto available = Seis3D::Curvilinear::queryBackend3D(o.backend, o.compute.cudaDevice);
        require(available.available, available.description.c_str());
        require(std::isfinite(o.memoryMiB) &&
                    (o.memoryMiB == 0 || (o.memoryMiB >= 16 && o.memoryMiB <= 32768)),
                "RAM budget must be automatic (0) or 16..32768 MiB.");
        // The pre-build snapshot excludes the active session, but not any candidate
        // allocations. Do not charge this candidate's geometry twice after mesh().
        const auto buildMemory = Seis3D::Curvilinear::queryBackendMemory3D(o.backend, available.cudaDevice);
        config.memoryMiB = effectiveRamBudgetMiB(o, buildMemory);
        require(config.memoryMiB >= 16, "Insufficient free RAM for automatic acoustic construction.");
        auto g = Detail::mesh(config);
        double requested = std::ceil(p.duration / g.dt);
        if (p.maxSteps)
            requested = std::min(requested, double(p.maxSteps));
        require(requested >= 1 && requested <= 200000, "Acoustic full-duration propagation exceeds 200000 "
                                                       "steps; adjust resolution/duration or step cap.");
        options = o;
        options.model.sourceX = sx;
        total = static_cast<int>(requested);
        dt = g.dt;
        recordDt = p.sampleIntervalMs > 0 ? p.sampleIntervalMs * .001 : dt;
        const auto samples = static_cast<std::size_t>(std::floor(total * dt / recordDt + 1e-7)) + 1;
        const double extraCpml =
            g.cpml ? g.pmlTriangles.size() * (140. + 25 * sizeof(Real)) +
                         (double(g.mass.size()) - double(p.nx - 2) * (p.nz - 1)) * (48. + 12 * sizeof(Real))
                   : 0;
        const double bytes = extraCpml + double(g.mass.size()) * (sizeof(Real) == 4 ? 256 : 384) +
                             double(samples) * p.receiverCount * sizeof(double);
        const auto memory = Seis3D::Curvilinear::queryBackendMemory3D(o.backend, available.cudaDevice);
        require(bytes < config.memoryMiB * 1048576.,
                "Acoustic construction and full receiver records exceed the RAM budget.");
        require(!buildMemory.hostAvailableBytes || bytes < .75 * buildMemory.hostAvailableBytes,
                "Insufficient available RAM for acoustic rebuild.");
        if (o.backend == BackendKind3D::Cuda)
            require(memory.deviceAvailableBytes >
                        (g.mass.size() * (16. * sizeof(Real) + 8) + extraCpml) * 1.25,
                    "Insufficient VRAM for acoustic propagation.");
        g.receivers.clear();
        for (int r = 0; r < p.receiverCount; ++r)
            g.receivers.push_back(p.receiverDepth * g.nx + g.pad + p.receiverStart + r * p.receiverStride);
        op = Detail::makeOperator<Real>(g, false, Detail::workers(config));
        if (!o.damping) {
            std::fill(op.a.begin(), op.a.end(), Real(1));
            std::fill(op.b.begin(), op.b.end(), Real(-1));
        }
        source = p.sourceZ * g.nx + g.pad + sx;
        const double area = g.mass[source] * g.rho[source] * g.vp[source] * g.vp[source];
        sourceScale = static_cast<Real>(dt * dt * double(op.a[source]) * double(op.rootMass[source]) *
                                        o.sourceStrength / area);
        require(std::isfinite(sourceScale), "Source overflows selected acoustic precision.");
        grid.nx = p.nx;
        grid.nz = p.nz;
        grid.computeNx = g.nx;
        grid.computeNz = g.nz;
        const auto logical = std::size_t(p.nx) * p.nz;
        crop.resize(logical);
        for (auto *array : {&grid.x, &grid.z, &grid.vp, &grid.density}) {
            array->convert(p.precision);
            array->resize(logical);
        }
        for (int k = 0; k < p.nz; ++k)
            for (int i = 0; i < p.nx; ++i) {
                const auto j = std::size_t(k) * p.nx + i;
                const int q = k * g.nx + g.pad + i;
                crop[j] = q;
                grid.x.set(j, g.x[q]);
                grid.z.set(j, g.z[q]);
                grid.vp.set(j, g.vp[q]);
                grid.density.set(j, g.rho[q]);
            }
        receivers.resize(p.receiverCount);
        previousSamples.assign(p.receiverCount, 0);
        for (int r = 0; r < p.receiverCount; ++r) {
            int q = g.receivers[r];
            receivers[r].position = {g.x[q], g.z[q]};
            receivers[r].pressure.reserve(samples);
            receivers[r].pressure.push_back(0);
        }
        engine = Detail::engine(op, o.backend == BackendKind3D::Cuda, available.cudaDevice);
        backend.kind = o.backend;
        backend.cudaDevice = available.cudaDevice;
        backend.workerThreads = o.backend == BackendKind3D::Cpu ? Detail::workers(config) : 1;
#ifdef SEIS3D_HAS_OPENMP
        backend.openMpAvailable = true;
#endif
        backend.deviceName = o.backend == BackendKind3D::Cuda ? available.description : "CPU / OpenMP";
        diagnostics.ppw = g.ppw;
        diagnostics.estimatedMiB = bytes / 1048576.;
        diagnostics.ramBudgetMiB = config.memoryMiB;
        const auto count = op.size(), nr = op.receivers.size();
        diagnostics.residentHostArrayBytes = count * (12 * sizeof(Real) + 1) +
                                             logical * (4 * sizeof(Real) + sizeof(int)) + nr * sizeof(double);
        for (const auto &r : receivers)
            diagnostics.residentHostArrayBytes += r.pressure.capacity() * sizeof(double);
        const auto cpmlStateBytes = (op.pmlMemorySize() + 2 * op.pmlTriangles.size()) * sizeof(Real);
        diagnostics.residentHostArrayBytes += op.pmlArrayBytes();
        if (o.backend == BackendKind3D::Cpu)
            diagnostics.residentHostArrayBytes += cpmlStateBytes;
        if (o.backend == BackendKind3D::Cpu)
            diagnostics.residentHostArrayBytes += 3 * count * sizeof(Real);
        else
            diagnostics.residentDeviceBytes = count * (15 * sizeof(Real) + 1) +
                                              nr * (2 * sizeof(Real) + sizeof(int)) + op.pmlArrayBytes() +
                                              cpmlStateBytes;
    }
    void advance(int n) override {
        require(n >= 0, "Acoustic step count cannot be negative.");
        auto start = std::chrono::steady_clock::now();
        for (int count = 0; count < n && step < total; ++count) {
            engine->step(false, source,
                         sourceScale *
                             static_cast<Real>(Detail::wavelet(step * dt, options.model.frequency)));
            auto data = engine->sample();
            const double end = (step + 1) * dt;
            for (Real value : data)
                require(std::isfinite(value), "Nonfinite acoustic pressure record.");
            for (std::size_t sample = receivers.front().pressure.size(); sample * recordDt <= end + 1e-8 * dt;
                 ++sample) {
                const double fraction = std::clamp((sample * recordDt - step * dt) / dt, 0., 1.);
                for (std::size_t r = 0; r < receivers.size(); ++r) {
                    double value = std::lerp(previousSamples[r], double(data[r]), fraction);
                    receivers[r].pressure.push_back(value);
                    diagnostics.peakPressure = std::max(diagnostics.peakPressure, std::abs(value));
                }
            }
            for (std::size_t r = 0; r < data.size(); ++r)
                previousSamples[r] = data[r];
            ++step;
        }
        diagnostics.totalStepMs +=
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    }
    void reset() override {
        engine->clear();
        step = 0;
        diagnostics.totalStepMs = 0;
        diagnostics.peakPressure = 0;
        for (auto &r : receivers) {
            r.pressure.clear();
            r.pressure.push_back(0);
        }
        std::fill(previousSamples.begin(), previousSamples.end(), 0);
    }
    std::vector<double> snapshot(bool includeAbsorber) const override {
        auto w = engine->field();
        std::vector<double> p(includeAbsorber ? w.size() : crop.size());
        for (std::size_t j = 0; j < p.size(); ++j) {
            const auto q = includeAbsorber ? j : static_cast<std::size_t>(crop[j]);
            p[j] = double(w[q]) / op.rootMass[q];
            require(std::isfinite(p[j]), "Nonfinite acoustic wavefield.");
        }
        return p;
    }
};
} // namespace
Simulation::Simulation() = default;
Simulation::~Simulation() = default;
bool Simulation::initialize(const Options &o, std::string &error) {
    try {
        auto begin = std::chrono::steady_clock::now();
        auto normalized = o;
        require(normalized.model.receiverTargetCount >= 0 && normalized.model.receiverTargetCount <= 4096,
                "Desired acoustic surface receiver count must be 0..4096.");
        require(synchronizeSurfaceReceivers(normalized.model, true, 4096),
                "Invalid automatic acoustic surface receiver layout.");
        if (normalized.model.sourceX < 0)
            normalized.model.sourceX = normalized.model.nx / 2;
        if (impl_ && impl_->options == normalized) {
            const auto available = Seis3D::Curvilinear::queryBackend3D(o.backend, o.compute.cudaDevice);
            if (available.available && available.cudaDevice == impl_->backend.cudaDevice) {
                impl_->reset();
                error.clear();
                return true;
            }
        }
        std::unique_ptr<Impl> next;
        require(o.propagator == Propagator::P1 || o.propagator == Propagator::CurvilinearDrp,
                "Unknown acoustic propagator.");
        if (o.propagator == Propagator::CurvilinearDrp)
            next = makeDrpForward(normalized);
        else if (o.model.precision == Precision::Float32)
            next = std::make_unique<Forward<float>>(normalized);
        else
            next = std::make_unique<Forward<double>>(normalized);
        next->diagnostics.initializationMs =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();
        impl_ = std::move(next);
        error.clear();
        return true;
    } catch (const std::exception &e) {
        error = e.what();
        return false;
    }
}
bool Simulation::advance(int n, std::string &error) {
    try {
        require(initialized(), "Initialize acoustic model first.");
        impl_->advance(n);
        error.clear();
        return true;
    } catch (const std::exception &e) {
        error = e.what();
        return false;
    }
}
bool Simulation::reset(std::string &error) {
    try {
        require(initialized(), "Initialize acoustic model first.");
        impl_->reset();
        error.clear();
        return true;
    } catch (const std::exception &e) {
        error = e.what();
        return false;
    }
}
bool Simulation::initialized() const { return bool(impl_); }
bool Simulation::completed() const { return impl_ && impl_->step >= impl_->total; }
int Simulation::step() const { return impl_ ? impl_->step : 0; }
int Simulation::totalSteps() const { return impl_ ? impl_->total : 0; }
double Simulation::dt() const { return impl_ ? impl_->dt : 0; }
double Simulation::recordDt() const { return impl_ ? impl_->recordDt : 0; }
const Options &Simulation::options() const { return impl_->options; }
const Grid &Simulation::grid() const { return impl_->grid; }
const std::vector<Receiver> &Simulation::receivers() const { return impl_->receivers; }
const Diagnostics &Simulation::diagnostics() const { return impl_->diagnostics; }
BackendInfo3D Simulation::backend() const { return impl_ ? impl_->backend : BackendInfo3D{}; }
std::vector<double> Simulation::pressureSnapshot(bool includeAbsorber) const {
    require(initialized(), "Initialize acoustic model first.");
    return impl_->snapshot(includeAbsorber);
}
bool Simulation::readPressure(std::vector<float> &out, std::string &error, bool includeAbsorber) const {
    try {
        auto p = pressureSnapshot(includeAbsorber);
        std::vector<float> next;
        next.reserve(p.size());
        for (double v : p) {
            float value = static_cast<float>(v);
            require(std::isfinite(value),
                    "Pressure exceeds float display range; raw FP64 data remain available.");
            next.push_back(value);
        }
        out = std::move(next);
        error.clear();
        return true;
    } catch (const std::exception &e) {
        error = e.what();
        return false;
    }
}
Options defaultOptions() {
    Options o;
    o.model.vp = 1800;
    o.model.vs = 900;
    o.model.density = 1800;
    o.model.relief = 0;
    o.model.frequency = 15;
    o.model.precision = Precision::Float32;
    o.model.layered = true;
    o.model.layers = {{200, 1800, 900, 1800}, {0, 2300, 1200, 1800}};
    o.model.nx = 241;
    o.model.nz = 121;
    o.model.spacing = 5;
    o.model.duration = .8;
    o.model.maxSteps = 0;
    o.model.sourceX = 120;
    o.model.sourceZ = 3;
    o.model.receiverStart = 1;
    o.model.receiverDepth = 2;
    o.model.receiverStride = 2;
    o.model.receiverCount = 120;
    o.model.sampleIntervalMs = .5;
#ifdef SEIS3D_HAS_OPENMP
    o.compute.cpuThreads = 4;
#else
    o.compute.cpuThreads = 1;
#endif
    return o;
}
} // namespace Seis2D::Acoustic
