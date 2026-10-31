#include "AcousticDrpModel2D.hpp"
#include "AcousticSession2D.hpp"
#include <chrono>
namespace Seis2D::Acoustic {
namespace {
template <class T> class DrpForward final : public Simulation::Impl {
    Drp::Operator<T> op;
    std::unique_ptr<Drp::Engine<T>> engine;
    int source{};
    double sourceScale{};

  public:
    explicit DrpForward(const Options &o) {
        using Detail::require;
        const auto &p = o.model;
        require(p.maxSteps >= 0 && p.maxSteps <= 200000, "Invalid DRP step cap.");
        require(p.precision == Precision::Float32 || p.precision == Precision::Float64,
                "Invalid DRP precision.");
        require(std::isfinite(o.sourceStrength) && o.sourceStrength > 0 && o.sourceStrength <= 1e30,
                "Invalid pressure source strength.");
        require(p.receiverStart >= 0 && p.receiverCount >= 1 && p.receiverCount <= 4096 &&
                    p.receiverStride >= 1 &&
                    static_cast<long long>(p.receiverStart) +
                            static_cast<long long>(p.receiverCount - 1) * p.receiverStride <
                        p.nx,
                "DRP receiver line is outside the target.");
        require(p.sourceZ >= 1 && p.sourceZ < p.nz - 1 && p.receiverDepth >= 1 && p.receiverDepth < p.nz - 1,
                "Pressure sources/receivers must be below the zero-pressure surface.");
        const int sx = p.sourceX < 0 ? p.nx / 2 : p.sourceX;
        require(sx >= 1 && sx < p.nx - 1, "DRP source X is outside the target.");
        require(std::isfinite(p.sampleIntervalMs) &&
                    (p.sampleIntervalMs == 0 || (p.sampleIntervalMs >= .01 && p.sampleIntervalMs <= 1000)),
                "Invalid DRP record interval.");
        require(o.absorber == AbsorbingBoundary::Cpml || o.absorber == AbsorbingBoundary::Sponge,
                "Invalid DRP boundary.");
        const auto info = Seis3D::Curvilinear::queryBackend3D(o.backend, o.compute.cudaDevice);
        require(info.available, info.description.c_str());
        const auto memory = Seis3D::Curvilinear::queryBackendMemory3D(o.backend, info.cudaDevice);
        const auto capacity = estimateCapacity(o, memory);
        require(capacity.withinBudget, "DRP model exceeds current RAM/VRAM budget.");
        Detail::ModelSettings config;
        config.model = p;
        config.backend = o.backend;
        config.compute = o.compute;
        config.absorberCells = o.absorberCells;
        config.memoryMiB = capacity.ramBudgetMiB;
        config.geometryOnly = true;
        // Geometry-only path deliberately avoids all P1/CPML element assembly.
        auto g = Detail::mesh(config);
        op = Drp::makeOperator<T>(o, g);
        options = o;
        options.model.sourceX = sx;
        dt = double(op.dt);
        recordDt = p.sampleIntervalMs > 0 ? p.sampleIntervalMs * .001 : dt;
        double steps = std::ceil(p.duration / dt);
        if (p.maxSteps)
            steps = std::min(steps, double(p.maxSteps));
        require(steps >= 1 && steps <= 200000,
                "DRP duration exceeds 200000 steps; adjust resolution/duration.");
        total = int(steps);
        const auto samples = std::size_t(std::floor(total * dt / recordDt + 1e-7)) + 1;
        const double recordBytes = double(samples) * p.receiverCount * sizeof(double);
        const auto n = op.size(), logical = std::size_t(p.nx) * p.nz;
        const double stateBytes = 30. * n * sizeof(T), coefficients = double(op.coefficientBytes());
        const double peak =
            stateBytes + coefficients + recordBytes + double(logical) * 4 * sizeof(T) + double(n) * 33;
        require(peak < capacity.ramBudgetMiB * 1048576. &&
                    (!memory.hostAvailableBytes || peak < .75 * memory.hostAvailableBytes),
                "Insufficient RAM for DRP stages and full records.");
        const auto deviceBytes = std::size_t(stateBytes + coefficients + op.receivers.size() * sizeof(T));
        if (o.backend == BackendKind3D::Cuda) {
            const auto current = Seis3D::Curvilinear::queryBackendMemory3D(o.backend, info.cudaDevice);
            require(deviceBytes * 1.25 < current.deviceAvailableBytes,
                    "Insufficient VRAM for DRP RK/PML stages.");
        }
        grid.nx = p.nx;
        grid.nz = p.nz;
        grid.computeNx = g.nx;
        grid.computeNz = g.nz;
        for (auto *a : {&grid.x, &grid.z, &grid.vp, &grid.density}) {
            a->convert(p.precision);
            a->resize(logical);
        }
        for (int j = 0; j < p.nz; ++j)
            for (int i = 0; i < p.nx; ++i) {
                const auto q = std::size_t(j) * g.nx + g.pad + i, k = std::size_t(j) * p.nx + i;
                grid.x.set(k, g.x[q]);
                grid.z.set(k, g.z[q]);
                grid.vp.set(k, g.vp[q]);
                grid.density.set(k, g.rho[q]);
            }
        source = p.sourceZ * g.nx + g.pad + sx;
        sourceScale = o.sourceStrength * double(op.geometry[source].inverseJ) / (p.spacing * p.spacing);
        require(std::isfinite(T(sourceScale / p.frequency)), "DRP source overflows selected precision.");
        previousSamples.assign(p.receiverCount, 0);
        receivers.resize(p.receiverCount);
        for (int r = 0; r < p.receiverCount; ++r) {
            const int q = op.receivers[r];
            receivers[r].position = {g.x[q], g.z[q]};
            receivers[r].pressure.reserve(samples);
            receivers[r].pressure.push_back(0);
        }
        engine = Drp::engine(op, o.backend == BackendKind3D::Cuda, info.cudaDevice);
        backend.kind = o.backend;
        backend.cudaDevice = info.cudaDevice;
        backend.workerThreads = o.backend == BackendKind3D::Cpu ? op.threads : 1;
        backend.deviceName = o.backend == BackendKind3D::Cuda ? info.description : "CPU / OpenMP / DRP7-RK4";
#ifdef SEIS3D_HAS_OPENMP
        backend.openMpAvailable = true;
#endif
        diagnostics.ppw = *std::min_element(g.vp.begin(), g.vp.end()) / (p.frequency * p.spacing);
        diagnostics.estimatedMiB = peak / 1048576.;
        diagnostics.ramBudgetMiB = capacity.ramBudgetMiB;
        diagnostics.residentHostArrayBytes =
            op.coefficientBytes() + logical * 4 * sizeof(T) + p.receiverCount * sizeof(double);
        for (const auto &r : receivers)
            diagnostics.residentHostArrayBytes += r.pressure.capacity() * sizeof(double);
        if (o.backend == BackendKind3D::Cpu)
            diagnostics.residentHostArrayBytes += std::size_t(stateBytes);
        else
            diagnostics.residentDeviceBytes = deviceBytes;
    }
    void advance(int count) override {
        Detail::require(count >= 0, "Negative DRP step count.");
        const auto begin = std::chrono::steady_clock::now();
        const double f = options.model.frequency, t0 = 1.5 / f, a = Detail::pi * f;
        const auto primitive = [&](double t) {
            const double s = t - t0;
            return s * std::exp(-a * a * s * s);
        };
        for (int k = 0; k < count && step < total; ++k) {
            std::array<T, 4> rates{};
            for (int j = 0; j < 4; ++j)
                rates[j] = T(sourceScale * (primitive((step + (j == 0   ? 0
                                                               : j == 3 ? 1
                                                                        : .5)) *
                                                      dt) -
                                            primitive(0)));
            engine->step(source, rates);
            const auto data = engine->sample();
            const double end = (step + 1) * dt;
            for (T v : data)
                Detail::require(std::isfinite(v), "Nonfinite DRP pressure record.");
            for (std::size_t s = receivers.front().pressure.size(); s * recordDt <= end + 1e-8 * dt; ++s) {
                const double fraction = std::clamp((s * recordDt - step * dt) / dt, 0., 1.);
                for (std::size_t r = 0; r < data.size(); ++r) {
                    const double v = std::lerp(previousSamples[r], double(data[r]), fraction);
                    receivers[r].pressure.push_back(v);
                    diagnostics.peakPressure = std::max(diagnostics.peakPressure, std::abs(v));
                }
            }
            for (std::size_t r = 0; r < data.size(); ++r)
                previousSamples[r] = data[r];
            ++step;
        }
        diagnostics.totalStepMs +=
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();
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
    std::vector<double> snapshot(bool full) const override {
        const auto values = engine->field();
        std::vector<double> result(full ? values.size() : grid.x.size());
        for (std::size_t j = 0; j < result.size(); ++j) {
            const auto q = full ? j : (j / grid.nx) * grid.computeNx + options.absorberCells + j % grid.nx;
            result[j] = double(values[q]);
            Detail::require(std::isfinite(result[j]), "Nonfinite DRP pressure field.");
        }
        return result;
    }
};
} // namespace
std::unique_ptr<Simulation::Impl> makeDrpForward(const Options &o) {
    if (o.model.precision == Precision::Float32)
        return std::make_unique<DrpForward<float>>(o);
    return std::make_unique<DrpForward<double>>(o);
}
} // namespace Seis2D::Acoustic
