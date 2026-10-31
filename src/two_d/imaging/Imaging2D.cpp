#include "Seis2D/Imaging2D.hpp"
#include "../acoustic/AcousticModel2D.hpp"
#include "Seis2D/SurfaceSurvey2D.hpp"
#include <chrono>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <limits>
#include <numeric>
#include <random>
#include <stdexcept>
#ifdef SEIS3D_HAS_OPENMP
#include <omp.h>
#endif
namespace Seis2D::Imaging {
namespace {
using namespace Seis2D::Acoustic::Detail;
inline void cancelled(std::atomic_bool *flag) {
    if (flag && flag->load())
        throw std::runtime_error("Imaging cancelled; previous result retained.");
}
ModelSettings modelSettings(const Options &o) {
    ModelSettings c;
    c.model = o.model;
    c.compute = o.compute;
    c.backend = o.backend;
    c.absorberCells = o.absorberCells;
    c.smoothIterations = o.smoothIterations;
    c.backgroundVp = o.backgroundVp;
    c.memoryMiB = o.memoryMiB;
    c.background = true;
    c.axisLimit = 4096;
    return c;
}
Mesh imagingMesh(const Options &input) {
    auto o = input;
    if (o.autoSurfaceReceivers)
        require(alignSurfaceReceivers(o), "Invalid model or receiver count for surface layout.");
    const auto &p = o.model;
    require(o.shots >= 1 && o.shots <= 128 && o.firstShot >= 1 && o.lastShot >= o.firstShot &&
                o.lastShot < p.nx - 1,
            "Shot positions must be inside target model.");
    require(o.sourceDepth >= 1 && o.sourceDepth < p.nz - 1 && o.receiverDepth >= 1 &&
                o.receiverDepth < p.nz - 1 && o.receiverStride >= 1 && o.receiverStride < p.nx,
            "Pressure sources/receivers must be below the free surface.");
    require(o.checkpointInterval >= 0 && o.checkpointInterval <= 20000 &&
                std::isfinite(o.illuminationFloor) && o.illuminationFloor > 0 && o.illuminationFloor <= .1,
            "Invalid RTM checkpoint or illumination settings.");
    require(o.receiverStart >= 0 && o.receiverStart < p.nx, "Receiver start is outside the target model.");
    const int nr =
        o.receiverCount > 0
            ? o.receiverCount
            : (o.receiverStart <= p.nx - 2 ? 1 + (p.nx - 2 - o.receiverStart) / o.receiverStride : 0);
    require(o.receiverCount >= 0 && o.receiverStart >= 0 && nr >= 1 && nr <= 4096 &&
                static_cast<long long>(o.receiverStart) + static_cast<long long>(nr - 1) * o.receiverStride <
                    p.nx,
            "Receiver start/count/stride exceeds the target model.");
    auto g = mesh(modelSettings(o));
    for (int r = 0; r < nr; ++r)
        g.receivers.push_back(o.receiverDepth * g.nx + g.pad + o.receiverStart + r * o.receiverStride);
    require(g.receivers.size() <= 4096, "Too many RTM receivers.");
    return g;
}
struct Workload {
    int nt{}, nr{}, block{};
    double estimated{};
};
template <class Real> Workload workload(const Options &o, const Mesh &g, bool preview) {
    const auto availability = Seis3D::Curvilinear::queryBackend3D(o.backend, o.compute.cudaDevice);
    require(availability.available, availability.description.c_str());
    const auto count = g.mass.size();
    const double stepCount = std::ceil(o.model.duration / g.dt);
    require(stepCount >= 1 && stepCount <= 20000,
            "RTM supports at most 20000 complete-duration steps; reduce duration or model resolution.");
    const int nt = static_cast<int>(stepCount), nr = static_cast<int>(g.receivers.size());
    const int block = o.checkpointInterval ? std::min(nt, o.checkpointInterval)
                                           : std::max(1, static_cast<int>(std::ceil(std::sqrt(2. * nt))));
    const double estimated =
        double(count) * (512 + sizeof(Real) * (40 + 2. * ((nt + block - 1) / block) + block)) +
        double(nt) * nr * (3 * sizeof(Real) + sizeof(double)) +
        (preview ? 3. * std::min(o.model.nx, 256) * std::min(o.model.nz, 192) * 8 * sizeof(double) : 0);
    require(estimated < o.memoryMiB * 1048576., "Checkpoint/replay RAM estimate exceeds imaging budget. "
                                                "Reduce target grid/duration or increase the budget.");
    const auto memory = Seis3D::Curvilinear::queryBackendMemory3D(o.backend, availability.cudaDevice);
    require(!memory.hostAvailableBytes || estimated < .7 * memory.hostAvailableBytes,
            "Insufficient free RAM for RTM checkpoints with reserve.");
    if (o.backend == BackendKind3D::Cuda)
        require(memory.deviceAvailableBytes > double(count) * (sizeof(Real) * 48 + 16) * 1.25,
                "Insufficient VRAM for acoustic RTM operators.");
    return {nt, nr, block, estimated};
}
template <class Real>
Result calculate(const Options &o, std::atomic_bool *cancel, Progress progress, PreviewSink preview) {
    const auto start = std::chrono::steady_clock::now();
    cancelled(cancel);
    const auto availability = Seis3D::Curvilinear::queryBackend3D(o.backend, o.compute.cudaDevice);
    require(availability.available, availability.description.c_str());
    auto g = imagingMesh(o);
    const auto w = workload<Real>(o, g, bool(preview));
    const auto count = g.mass.size();
    const int nt = w.nt, nr = w.nr, block = w.block;
    const double estimated = w.estimated;
    auto truth = makeOperator<Real>(g, false, workers(modelSettings(o))),
         background = makeOperator<Real>(g, true, workers(modelSettings(o)));
    // Geometry assembly temporaries are not retained through the time loop.
    std::vector<double>().swap(g.k);
    auto forward = engine(truth, o.backend == BackendKind3D::Cuda, availability.cudaDevice);
    auto source = engine(background, o.backend == BackendKind3D::Cuda, availability.cudaDevice);
    auto receiver = engine(background, o.backend == BackendKind3D::Cuda, availability.cudaDevice);
    Result r;
    r.options = o;
    r.nx = o.model.nx;
    r.nz = o.model.nz;
    r.dt = g.dt;
    r.steps = nt;
    r.shots = o.shots;
    r.receivers = nr;
    r.checkpointInterval = block;
    r.estimatedMiB = estimated / 1048576.;
    r.minPpw = g.ppw;
    r.method = "Acoustic variable-density pressure RTM; P1 FEM/lumped mass; discrete adjoint; external "
               "damped strips; zero-pressure top; zero-lag correlation";
    r.device = (o.backend == BackendKind3D::Cpu
                    ? "CPU / OpenMP (" + std::to_string(workers(modelSettings(o))) + " workers)"
                    : availability.description) +
               std::string(o.model.precision == Precision::Float32 ? " / FP32" : " / FP64");
    const auto logical = std::size_t(r.nx) * r.nz;
    for (auto *a : {&r.x, &r.z, &r.vp, &r.backgroundVp, &r.image, &r.illumination, &r.normalizedImage})
        a->resize(logical);
    std::vector<int> crop(logical);
    for (int z = 0; z < r.nz; ++z)
        for (int x = 0; x < r.nx; ++x) {
            int q = z * g.nx + x + g.pad;
            auto j = std::size_t(z) * r.nx + x;
            crop[j] = q;
            r.x[j] = g.x[q];
            r.z[j] = g.z[q];
            r.vp[j] = g.vp[q];
            r.backgroundVp[j] = g.bg[q];
        }
    for (int q : g.receivers) {
        r.receiverX.push_back(g.x[q]);
        r.receiverZ.push_back(g.z[q]);
    }
    auto report = [&](int shot, double fraction, const char *phase) {
        if (progress)
            progress((shot + fraction) / o.shots, std::string(phase) + " / shot " + std::to_string(shot + 1) +
                                                      " / " + std::to_string(o.shots));
    };
    std::vector<Real> residual(std::size_t(nt) * nr), injection(nr);
    // Bounded, throttled snapshots only. No solver state or full Result is shared with the UI.
    auto lastPreview = std::chrono::steady_clock::now() - std::chrono::seconds(1);
    const int strideX = std::max(1, (r.nx - 1 + 254) / 255), strideZ = std::max(1, (r.nz - 1 + 190) / 191);
    std::vector<int> previewX, previewZ;
    for (int x = 0;; x = std::min(r.nx - 1, x + strideX)) {
        previewX.push_back(x);
        if (x == r.nx - 1)
            break;
    }
    for (int z = 0;; z = std::min(r.nz - 1, z + strideZ)) {
        previewZ.push_back(z);
        if (z == r.nz - 1)
            break;
    }
    const auto wantsPreview = [&](int n) {
        return preview && (n == 0 || n + 1 == nt ||
                           std::chrono::steady_clock::now() - lastPreview >= std::chrono::milliseconds(100));
    };
    const auto publishPreview = [&](int shot, int n, int phase, const std::vector<Real> &field,
                                    const Operator<Real> &op) {
        Preview view;
        view.nx = int(previewX.size());
        view.nz = int(previewZ.size());
        view.shot = shot + 1;
        view.shots = o.shots;
        view.step = n + 1;
        view.steps = nt;
        view.time = (n + 1) * g.dt;
        view.phase = phase;
        for (int z : previewZ)
            for (int x : previewX) {
                const auto logicalIndex = std::size_t(z) * r.nx + x;
                const auto q = crop[logicalIndex];
                view.x.push_back(r.x[logicalIndex]);
                view.z.push_back(r.z[logicalIndex]);
                view.vp.push_back(r.vp[logicalIndex]);
                view.backgroundVp.push_back(r.backgroundVp[logicalIndex]);
                view.pressure.push_back(double(field[q]) / op.rootMass[q]);
                view.image.push_back(r.image[logicalIndex]);
                view.illumination.push_back(r.illumination[logicalIndex]);
            }
        preview(std::move(view));
        lastPreview = std::chrono::steady_clock::now();
    };
    for (int shot = 0; shot < o.shots; ++shot) {
        cancelled(cancel);
        const int sx = o.shots == 1
                           ? (o.firstShot + o.lastShot) / 2
                           : o.firstShot + static_cast<int>(std::lround(
                                               double(shot) * (o.lastShot - o.firstShot) / (o.shots - 1)));
        const int sq = o.sourceDepth * g.nx + g.pad + sx;
        forward->clear();
        for (int n = 0; n < nt; ++n) {
            cancelled(cancel);
            forward->step(false, sq, truth.rootMass[sq] * Real(wavelet(n * g.dt, o.model.frequency)));
            auto d = forward->sample();
            std::copy(d.begin(), d.end(), residual.begin() + std::size_t(n) * nr);
            if (wantsPreview(n))
                publishPreview(shot, n, 0, forward->field(), truth);
            if (n % 64 == 0)
                report(shot, .25 * n / nt, "True-pressure modelling");
        }
        source->clear();
        std::vector<State<Real>> checkpoints;
        checkpoints.reserve((nt + block - 1) / block);
        for (int n = 0; n < nt; ++n) {
            cancelled(cancel);
            if (n % block == 0)
                checkpoints.push_back(source->state());
            source->step(false, sq, background.rootMass[sq] * Real(wavelet(n * g.dt, o.model.frequency)));
            auto d = source->sample();
            for (int k = 0; k < nr; ++k)
                residual[std::size_t(n) * nr + k] -= d[k];
            auto u = source->field();
            for (std::size_t j = 0; j < logical; ++j) {
                const int q = crop[j];
                double p = double(u[q]) / background.rootMass[q];
                r.illumination[j] += p * p * g.dt;
            }
            if (wantsPreview(n))
                publishPreview(shot, n, 1, u, background);
            if (n % 64 == 0)
                report(shot, .25 + .25 * n / nt, "Background / checkpoints");
        }
        // lambda_nt = R^T d_(nt-1); every sample was recorded AFTER its forward step.
        State<Real> initial;
        initial.previous.assign(count, 0);
        initial.current.assign(count, 0);
        for (int k = 0; k < nr; ++k)
            initial.current[g.receivers[k]] +=
                residual[std::size_t(nt - 1) * nr + k] / background.rootMass[g.receivers[k]];
        receiver->restore(initial);
        for (int segment = (nt - 1) / block; segment >= 0; --segment) {
            int first = segment * block, last = std::min(nt, first + block);
            source->restore(checkpoints[segment]);
            std::vector<std::vector<Real>> replay;
            replay.reserve(last - first);
            for (int n = first; n < last; ++n) {
                cancelled(cancel);
                source->step(false, sq, background.rootMass[sq] * Real(wavelet(n * g.dt, o.model.frequency)));
                replay.push_back(source->field());
            }
            for (int n = last - 1; n >= first; --n) {
                cancelled(cancel);
                auto v = receiver->field();
                const auto &u = replay[n - first];
                for (std::size_t j = 0; j < logical; ++j) {
                    int q = crop[j];
                    r.image[j] += double(u[q]) * double(v[q]) / g.bgMass[q] * g.dt;
                }
                if (wantsPreview(n))
                    publishPreview(shot, n, 2, u, background);
                if (n > 0) {
                    for (int k = 0; k < nr; ++k)
                        injection[k] = residual[std::size_t(n - 1) * nr + k];
                    receiver->step(true, -1, 0, injection);
                }
                if (n % 64 == 0)
                    report(shot, .5 + .5 * (nt - n) / nt, "Adjoint / imaging");
            }
        }
        if (shot + 1 == o.shots) {
            r.records.assign(residual.begin(), residual.end());
        }
    }
    const double maximum = *std::max_element(r.illumination.begin(), r.illumination.end());
    require(maximum > 0 && std::isfinite(maximum), "Empty/nonfinite illumination.");
    for (std::size_t j = 0; j < logical; ++j) {
        require(std::isfinite(r.image[j]) && std::isfinite(r.illumination[j]), "Nonfinite RTM result.");
        r.normalizedImage[j] = r.image[j] / (r.illumination[j] + o.illuminationFloor * maximum);
    }
    r.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    if (progress)
        progress(1, "RTM completed");
    return r;
}
template <class Real> double dotTest(const Options &o) {
    auto g = imagingMesh(o);
    auto op = makeOperator<Real>(g, true, workers(modelSettings(o)));
    const auto available = Seis3D::Curvilinear::queryBackend3D(o.backend, o.compute.cudaDevice);
    require(available.available, available.description.c_str());
    auto e = engine(op, o.backend == BackendKind3D::Cuda, available.cudaDevice);
    const int nt = 250, nr = static_cast<int>(g.receivers.size()),
              sq = o.sourceDepth * g.nx + g.pad + o.firstShot;
    std::mt19937 random(747);
    std::uniform_real_distribution<double> uniform(-1, 1);
    std::vector<Real> q(nt), data(std::size_t(nt) * nr), injection(nr);
    for (auto &x : q)
        x = Real(uniform(random));
    for (auto &x : data)
        x = Real(uniform(random));
    double lhs = 0, rhs = 0;
    for (int n = 0; n < nt; ++n) {
        e->step(false, sq, op.rootMass[sq] * q[n]);
        auto d = e->sample();
        for (int k = 0; k < nr; ++k)
            lhs += double(d[k]) * data[std::size_t(n) * nr + k];
    }
    State<Real> s;
    s.previous.assign(op.size(), 0);
    s.current.assign(op.size(), 0);
    for (int k = 0; k < nr; ++k)
        s.current[g.receivers[k]] += data[std::size_t(nt - 1) * nr + k] / op.rootMass[g.receivers[k]];
    e->restore(s);
    for (int n = nt - 1; n >= 0; --n) {
        auto u = e->field();
        rhs += double(q[n]) * op.rootMass[sq] * u[sq];
        if (n > 0) {
            for (int k = 0; k < nr; ++k)
                injection[k] = data[std::size_t(n - 1) * nr + k];
            e->step(true, -1, 0, injection);
        }
    }
    return std::abs(lhs - rhs) / std::max({std::abs(lhs), std::abs(rhs), 1e-30});
}
} // namespace
Options benchmarkOptions() {
    Options o;
    auto &p = o.model;
    p.nx = 121;
    p.nz = 81;
    p.spacing = 5;
    p.relief = 0;
    p.frequency = 15;
    p.duration = .6;
    p.precision = Precision::Float32;
    p.layered = true;
    p.layers = {{200, 1800, 900, 1800}, {0, 2300, 1200, 1800}};
    p.vp = 1800;
    p.vs = 900;
    p.density = 1800;
    p.attenuation.enabled = false;
    o.backgroundVp = 1800;
    o.compute.cpuThreads = 4;
    return o;
}
bool alignSurfaceReceivers(Options &o) {
    const auto &p = o.model;
    if (p.nx < 16 || p.nx > 4096 || p.nz < 16 || p.nz > 4096 || o.receiverStride < 1 || o.receiverCount < 0 ||
        o.receiverCount > 4096 || o.receiverTargetCount < 0 || o.receiverTargetCount > 4096)
        return false;
    auto line = p;
    const int desired = o.receiverTargetCount > 0 ? o.receiverTargetCount
                        : o.receiverCount > 0     ? o.receiverCount
                                                  : 1 + (p.nx - 3) / o.receiverStride;
    line.receiverCount = desired;
    if (!Curvilinear::setUniformSurfaceReceivers(line, true, 4096, 1))
        return false;
    o.receiverStart = line.receiverStart;
    o.receiverCount = line.receiverCount;
    o.receiverStride = line.receiverStride;
    o.receiverDepth = 1;
    o.receiverTargetCount = desired;
    o.model.receiverStart = o.receiverStart;
    o.model.receiverCount = o.receiverCount;
    o.model.receiverStride = o.receiverStride;
    o.model.receiverDepth = 1;
    o.model.autoSurfaceReceivers = o.autoSurfaceReceivers;
    o.model.receiverTargetCount = desired;
    return true;
}
bool prepareModel(const Options &input, Result &out, std::string &error, std::atomic_bool *cancel) {
    try {
        cancelled(cancel);
        auto o = input;
        if (o.autoSurfaceReceivers)
            require(alignSurfaceReceivers(o), "Invalid surface receiver layout.");
        auto g = imagingMesh(o);
        cancelled(cancel);
        const auto w = o.model.precision == Precision::Float32 ? workload<float>(o, g, true)
                                                               : workload<double>(o, g, true);
        Result r;
        r.options = o;
        r.nx = o.model.nx;
        r.nz = o.model.nz;
        r.steps = w.nt;
        r.receivers = w.nr;
        r.shots = o.shots;
        r.dt = g.dt;
        r.checkpointInterval = w.block;
        r.estimatedMiB = w.estimated / 1048576.;
        r.minPpw = g.ppw;
        r.method = "Prepared RTM model / survey only; no propagated or imaged result";
        const auto n = std::size_t(r.nx) * r.nz;
        for (auto *a : {&r.x, &r.z, &r.vp, &r.backgroundVp})
            a->resize(n);
        for (int j = 0; j < r.nz; ++j)
            for (int i = 0; i < r.nx; ++i) {
                const auto k = std::size_t(j) * r.nx + i, q = std::size_t(j) * g.nx + g.pad + i;
                r.x[k] = g.x[q];
                r.z[k] = g.z[q];
                r.vp[k] = g.vp[q];
                r.backgroundVp[k] = g.bg[q];
            }
        for (int q : g.receivers) {
            r.receiverX.push_back(g.x[q]);
            r.receiverZ.push_back(g.z[q]);
        }
        cancelled(cancel);
        out = std::move(r);
        error.clear();
        return true;
    } catch (const std::exception &e) {
        error = e.what();
        return false;
    }
}
bool run(const Options &o, Result &out, std::string &error, std::atomic_bool *cancel, Progress progress,
         PreviewSink preview) {
    try {
        auto applied = o;
        if (applied.autoSurfaceReceivers)
            require(alignSurfaceReceivers(applied), "Invalid surface receiver layout.");
        auto next = applied.model.precision == Precision::Float32
                        ? calculate<float>(applied, cancel, progress, preview)
                        : calculate<double>(applied, cancel, progress, preview);
        out = std::move(next);
        error.clear();
        return true;
    } catch (const std::exception &ex) {
        error = ex.what();
        return false;
    }
}
double adjointDotTest(const Options &o) {
    return o.model.precision == Precision::Float32 ? dotTest<float>(o) : dotTest<double>(o);
}
bool exportResult(const Result &r, const std::filesystem::path &folder, std::string &error) {
    try {
        require(r.nx > 0 && r.nz > 0 && r.steps > 0 && r.image.size() == std::size_t(r.nx) * r.nz,
                "No completed RTM result to export.");
        // A fresh folder avoids overwriting a previous reproducible experiment.
        require(!std::filesystem::exists(folder),
                "Export folder already exists; choose a new experiment folder.");
        std::filesystem::create_directories(folder);
        auto file = [&](const char *name) {
            std::ofstream f(folder / name);
            f.exceptions(std::ios::badbit | std::ios::failbit);
            f.imbue(std::locale::classic());
            f << std::setprecision(17);
            return f;
        };
        {
            auto f = file("rtm.vts");
            f << "<?xml version=\"1.0\"?>\n<VTKFile type=\"StructuredGrid\" version=\"1.0\" "
                 "byte_order=\"LittleEndian\"><StructuredGrid WholeExtent=\"0 "
              << r.nx - 1 << " 0 0 0 " << r.nz - 1 << "\"><Piece Extent=\"0 " << r.nx - 1 << " 0 0 0 "
              << r.nz - 1 << "\"><PointData>\n";
            for (auto [name, values] : {std::pair{"Vp_true", &r.vp},
                                        {"Vp_background", &r.backgroundVp},
                                        {"RTM_raw", &r.image},
                                        {"RTM_illumination_normalized", &r.normalizedImage},
                                        {"Source_illumination", &r.illumination}}) {
                f << "<DataArray type=\"Float64\" Name=\"" << name << "\" format=\"ascii\">\n";
                for (double v : *values)
                    f << v << ' ';
                f << "\n</DataArray>\n";
            }
            f << "</PointData><Points><DataArray type=\"Float64\" NumberOfComponents=\"3\" "
                 "format=\"ascii\">\n";
            for (std::size_t q = 0; q < r.x.size(); ++q)
                f << r.x[q] << " 0 " << r.z[q] << ' ';
            f << "\n</DataArray></Points></Piece></StructuredGrid></VTKFile>\n";
        }
        {
            auto f = file("last_shot_pressure_residual.csv");
            f << "# component=pressure_Pa; data=true-background; time-major; coordinates in receivers.csv\n";
            f << "time_s";
            for (int k = 0; k < r.receivers; ++k)
                f << ",receiver_" << k;
            f << '\n';
            for (int n = 0; n < r.steps; ++n) {
                f << (n + 1) * r.dt;
                for (int k = 0; k < r.receivers; ++k)
                    f << ',' << r.records[std::size_t(n) * r.receivers + k];
                f << '\n';
            }
        }
        {
            auto f = file("receivers.csv");
            f << "receiver,x_m,z_positive_down_m\n";
            for (int k = 0; k < r.receivers; ++k)
                f << k << ',' << r.receiverX[k] << ',' << r.receiverZ[k] << '\n';
        }
        {
            auto f = file("model.csv");
            f << "i,k,x_m,z_positive_down_m,vp_true_m_s,vp_background_m_s,density_kg_m3\n";
            const auto &p = r.options.model;
            for (int k = 0; k < r.nz; ++k)
                for (int i = 0; i < r.nx; ++i) {
                    auto q = std::size_t(k) * r.nx + i;
                    double rho = p.density, depth = 0;
                    if (p.layered)
                        for (std::size_t l = 0; l < p.layers.size(); ++l) {
                            depth += p.layers[l].thickness;
                            if (l + 1 == p.layers.size() || k * p.spacing < depth) {
                                rho = p.layers[l].density;
                                break;
                            }
                        }
                    f << i << ',' << k << ',' << r.x[q] << ',' << r.z[q] << ',' << r.vp[q] << ','
                      << r.backgroundVp[q] << ',' << rho << '\n';
                }
        }
        {
            auto f = file("experiment.txt");
            const auto &o = r.options;
            const auto &p = o.model;
            f << r.method << "\nDevice: " << r.device
              << "\nprecision=" << (p.precision == Precision::Float32 ? "FP32" : "FP64") << "\nnx=" << r.nx
              << "\nnz=" << r.nz << "\nh_m=" << p.spacing << "\nfrequency_Hz=" << p.frequency
              << "\nrequested_duration_s=" << p.duration << "\nsteps=" << r.steps << "\ndt_s=" << r.dt
              << "\nshots=" << r.shots << "\nfirst_shot_index=" << o.firstShot
              << "\nlast_shot_index=" << o.lastShot << "\nshot_depth_index=" << o.sourceDepth
              << "\nreceiver_depth_index=" << o.receiverDepth << "\nreceiver_stride=" << o.receiverStride
              << "\nreceiver_start=" << o.receiverStart << "\nreceiver_count=" << r.receivers
              << "\nauto_surface_receivers=" << o.autoSurfaceReceivers
              << "\nreceiver_target_count=" << o.receiverTargetCount
              << "\nexternal_absorber_cells=" << o.absorberCells
              << "\nsmooth_iterations=" << o.smoothIterations << "\nbackground_constant_vp=" << o.backgroundVp
              << "\nillumination_floor=" << o.illuminationFloor
              << "\ncheckpoint_interval=" << r.checkpointInterval
              << "\nestimated_peak_RAM_MiB=" << r.estimatedMiB << "\nwall_seconds=" << r.seconds
              << "\nmin_peak_P_ppw=" << r.minPpw
              << "\nsource=unit pressure increment Ricker at t=n*dt, delay=1.5/f; samples after step\nsource "
                 "model coordinates and density in model.csv; z positive down\nVs and Q ignored/not "
                 "inverted; GSLS rejected. Acoustic pressure is not Vz/Vx.\nSponge strips are NOT the "
                 "elastic CFS-PML. Source illumination is not a Hessian diagonal.\nImages have arbitrary "
                 "scale; illumination-normalized output is not calibrated reflectivity.\nOnly the final shot "
                 "pressure residual is exported; all shot images are summed.\n";
        }
        error.clear();
        return true;
    } catch (const std::exception &ex) {
        error = ex.what();
        return false;
    }
}
} // namespace Seis2D::Imaging
