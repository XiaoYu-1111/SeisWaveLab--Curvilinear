#include "Seis2D/AcousticSolver2D.hpp"
#include "Seis2D/CurvilinearSolver2D.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <stdexcept>
using namespace Seis2D;
void check(bool value, const std::string &e) {
    if (!value)
        throw std::runtime_error(e);
}
double difference(const std::vector<double> &a, const std::vector<double> &b) {
    check(a.size() == b.size(), "Shape mismatch");
    double n = 0, d = 0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        n += (a[i] - b[i]) * (a[i] - b[i]);
        d += a[i] * a[i];
    }
    return std::sqrt(n / std::max(d, 1e-100));
}
int main(int argc, char **argv) {
    try {
        const bool autoCapacity=argc>1&&std::string(argv[1])=="--capacity-auto";
        auto folder = std::filesystem::path(SEIS3D_OUTPUT_ROOT) / (autoCapacity?"validation/acoustic-capacity-auto-2026-10-31":"validation/acoustic-2d");
        std::filesystem::create_directories(folder);
        std::ofstream log(folder / (argc > 1 && std::string(argv[1]) == "--capacity"
                                        ? "capacity-validation.txt"
                                        : "validation.txt"));
        log << std::setprecision(12);
        auto note = [&](const std::string &s) {
            std::cout << s << '\n';
            log << s << '\n';
        };
        std::string e;
        if (autoCapacity||(argc > 1 && std::string(argv[1]) == "--capacity")) {
            auto o = Acoustic::defaultOptions();
            o.backend = Curvilinear::BackendKind3D::Cuda;
            o.memoryMiB = autoCapacity?0:4096;
            auto &p = o.model;
            p.nx = autoCapacity?5000:5001;
            p.nz = autoCapacity?2000:2001;
            p.spacing = 2;
            p.frequency = 15;
            p.duration = .03;
            p.maxSteps = 8;
            p.sourceX = 2500;
            p.sourceZ = 8;
            p.receiverStart = 1;
            p.receiverCount = 128;
            p.receiverStride = 30;
            p.receiverDepth = 2;
            p.precision = Curvilinear::Precision::Float32;
            p.layered = true;
            p.layers = {{50, 500, 0, 1600}, {0, 2000, 0, 2000}};
            const auto memory=Seis3D::Curvilinear::queryBackendMemory3D(o.backend,o.compute.cudaDevice);
            const auto predicted=Acoustic::estimateCapacity(o,memory);
            note("Free RAM MiB="+std::to_string(predicted.freeRamMiB)+"; RAM budget="+std::to_string(predicted.ramBudgetMiB)+"; free VRAM="+std::to_string(predicted.freeDeviceMiB));
            check(predicted.withinBudget,"Large test does not fit currently available RAM/VRAM.");
            Acoustic::Simulation s;
            check(s.initialize(o, e) && s.advance(8, e), e);
            auto field = s.pressureSnapshot();
            double peak = 0;
            for (double x : field) {
                check(std::isfinite(x), "Large acoustic field is not finite");
                peak = std::max(peak, std::abs(x));
            }
            check(peak > 0, "Large acoustic source field empty");
            std::ofstream capacity(folder / "capacity.txt");
            capacity
                << std::setprecision(17) << "logical_nx=" << p.nx << "\nlogical_nz=" << p.nz
                << "\ncompute_nx=" << s.grid().computeNx << "\ncompute_nz=" << s.grid().computeNz
                << "\nprecision=FP32\nboundary=CFS-CPML\nVp_min_m_s=500\nfrequency_Hz=15\nsteps=" << s.step()
                << "\ndt_s=" << s.dt() << "\ninitialization_ms=" << s.diagnostics().initializationMs
                << "\ncompute_ms=" << s.diagnostics().totalStepMs
                << "\nresident_host_arrays_bytes=" << s.diagnostics().residentHostArrayBytes
                << "\nresident_device_arrays_bytes=" << s.diagnostics().residentDeviceBytes
                << "\nRAM_budget_MiB=" << s.diagnostics().estimatedMiB << "\npeak_pressure_Pa=" << peak
                << "\nram_policy=" << (autoCapacity?"automatic":"manual")
                << "\nresolved_ram_budget_MiB=" << s.diagnostics().ramBudgetMiB
                << "\nfree_ram_before_build_MiB=" << predicted.freeRamMiB
                << "\nfree_vram_before_build_MiB=" << predicted.freeDeviceMiB
                << "\nShort capacity check only; not a complete production simulation or convergence test.\n";
            note("PASS: "+std::to_string(p.nx)+"x"+std::to_string(p.nz)+" acoustic GPU FP32 / dry-loess Vp 500 m/s / eight finite steps");
            return 0;
        }
        auto o = Acoustic::defaultOptions();
        auto &p = o.model;
        p.nx = 81;
        p.nz = 81;
        p.spacing = 5;
        p.frequency = 20;
        p.duration = .22;
        p.layered = false;
        p.vp = 1800;
        p.vs = 0;
        p.sourceX = 20;
        p.sourceZ = 20;
        p.receiverStart = 40;
        p.receiverDepth = 20;
        p.receiverCount = 1;
        p.receiverStride = 1;
        p.precision = Curvilinear::Precision::Float64;
        Acoustic::Simulation s;
        check(s.initialize(o, e), e);
        check(s.advance(s.totalSteps(), e), e);
        auto reference = s.pressureSnapshot();
        const auto &record = s.receivers()[0].pressure;
        double expected = 1.5 / p.frequency + 100 / p.vp, peak = 0, peakTime = 0;
        for (std::size_t n = 0; n < record.size(); ++n) {
            double t = n * s.recordDt();
            if (std::abs(t - expected) < .025 && std::abs(record[n]) > peak) {
                peak = std::abs(record[n]);
                peakTime = t;
            }
        }
        note("Analytic direct P arrival (Ricker centre) " + std::to_string(expected) + " s; observed peak " +
             std::to_string(peakTime) + " s");
        check(peak > 0 && std::abs(peakTime - expected) < .015,
              "Acoustic direct arrival outside 15 ms tolerance");
        for (int x = 0; x < p.nx; ++x)
            check(reference[x] == 0, "Free-surface pressure is not zero");
        check(record.size() ==
                      static_cast<std::size_t>(std::floor(s.totalSteps() * s.dt() / s.recordDt() + 1e-7)) +
                          1 &&
                  record[0] == 0,
              "Uniform acoustic sampling / t=0 failed");
        check(s.reset(e) && s.advance(s.totalSteps(), e), e);
        check(s.pressureSnapshot() == reference, "Acoustic reset was not deterministic");
        auto velocity = o;
        velocity.model.vs = 900;
        Acoustic::Simulation unchanged;
        check(unchanged.initialize(velocity, e) && unchanged.advance(unchanged.totalSteps(), e), e);
        check(unchanged.pressureSnapshot() == reference, "Acoustic wavefield depends on Vs");
        auto doubled = o;
        doubled.sourceStrength *= 2;
        Acoustic::Simulation linear;
        check(linear.initialize(doubled, e) && linear.advance(linear.totalSteps(), e), e);
        auto twice = linear.pressureSnapshot();
        for (auto &v : twice)
            v *= .5;
        check(difference(reference, twice) < 1e-12, "Acoustic source is nonlinear");
        auto bad = o;
        bad.model.nx = 400;
        bad.model.nz = 300;
        bad.memoryMiB = 16;
        check(!s.initialize(bad, e) && s.pressureSnapshot() == reference,
              "Failed memory rebuild lost acoustic session");
        bad = o;
        bad.model.attenuation.enabled = true;
        check(!s.initialize(bad, e), "Q silently enabled in acoustic solver");
        auto fp32 = o;
        fp32.model.precision = Curvilinear::Precision::Float32;
        Acoustic::Simulation single;
        check(single.initialize(fp32, e) && single.advance(single.totalSteps(), e), e);
        double rel = difference(reference, single.pressureSnapshot());
        log << "CPU FP32 / FP64 global pressure L2=" << rel << '\n';
        check(rel < .001, "Acoustic FP32 pressure mismatch");
        auto available = Seis3D::Curvilinear::queryBackend3D(Curvilinear::BackendKind3D::Cuda);
        if (available.available) {
            for (auto precision : {Curvilinear::Precision::Float64, Curvilinear::Precision::Float32}) {
                auto gpu = o;
                gpu.backend = Curvilinear::BackendKind3D::Cuda;
                gpu.model.precision = precision;
                Acoustic::Simulation actual;
                check(actual.initialize(gpu, e) && actual.advance(actual.totalSteps(), e), e);
                double error = difference(
                    precision == Curvilinear::Precision::Float64 ? reference : single.pressureSnapshot(),
                    actual.pressureSnapshot());
                log << "CPU / CUDA " << Curvilinear::precisionName(precision) << " pressure L2=" << error
                    << '\n';
                check(error < (precision == Curvilinear::Precision::Float64 ? 1e-10 : .001),
                      "Acoustic GPU mismatch");
            }
        }
        auto destination =
            folder /
            ("experiment-" + std::to_string(std::chrono::system_clock::now().time_since_epoch().count()));
        check(Acoustic::exportResult(s, destination, e), e);
        Acoustic::Options loaded;
        check(Acoustic::readModelVts(destination / "model_acoustic.vts", loaded, e), e);
        Acoustic::Simulation roundtrip;
        check(roundtrip.initialize(loaded, e) && roundtrip.advance(roundtrip.totalSteps(), e), e);
        check(roundtrip.pressureSnapshot() == reference, "Acoustic VTS restart-at-zero differs");
        check(!Acoustic::readModelVts(destination / "pressure.vts", loaded, e),
              "Snapshot accepted as a model");
        {
            std::ifstream f(destination / "model_acoustic.vts");
            std::string text((std::istreambuf_iterator<char>(f)), {});
            auto at = text.find("1800");
            check(at != std::string::npos, "Expected Vp token missing");
            text.replace(at, 4, "1801");
            std::ofstream corrupt(destination / "corrupt.vts");
            corrupt << text;
            corrupt.close();
            auto before = loaded;
            check(!Acoustic::readModelVts(destination / "corrupt.vts", loaded, e) && loaded == before,
                  "Corrupt acoustic model was accepted or mutated destination");
        }
        auto terrain = o;
        terrain.model.importedSurface = true;
        terrain.model.surfaceDatum = 1000;
        terrain.model.surfaceSamples = {{0, 1000}, {100, 1010}, {200, 995}, {300, 1005}, {400, 1000}};
        Acoustic::Simulation curved;
        check(curved.initialize(terrain, e) && curved.advance(30, e), e);
        check(curved.grid().z[20] < 0 && curved.grid().z[40] > 0, "Acoustic DEM orientation mismatch");
        std::ofstream timings(folder / "mode_cost.csv");
        timings
            << "nx,nz,backend,mode,precision,steps,dt_s,compute_ms,host_arrays_bytes,device_arrays_bytes\n"
            << std::setprecision(12);
        for (int size : {161, 401})
            for (auto backend : {Curvilinear::BackendKind3D::Cpu, Curvilinear::BackendKind3D::Cuda}) {
                if (backend == Curvilinear::BackendKind3D::Cuda && !available.available)
                    continue;
                auto test = Acoustic::defaultOptions();
                auto &q = test.model;
                q.nx = size;
                q.nz = (size + 1) / 2;
                q.layered = false;
                q.vp = 1800;
                q.vs = 900;
                q.density = 1800;
                q.spacing = 5;
                q.frequency = 15;
                q.duration = .15;
                q.sourceX = size / 2;
                q.sourceZ = 8;
                q.receiverDepth = 2;
                q.receiverStart = 1;
                q.receiverCount = std::min(80, (size - 2) / 2);
                q.receiverStride = 2;
                q.maxSteps = 0;
                q.precision = Curvilinear::Precision::Float32;
                q.absorbing = true;
                q.externalAbsorption = true;
                q.spongeX = q.spongeZ = 24;
                q.absorber = Curvilinear::Absorber::Sponge;
#ifdef SEIS3D_HAS_OPENMP
                test.compute.cpuThreads = 4;
#else
                test.compute.cpuThreads = 1;
#endif
                test.backend = backend;
                for (int repeat = 0; repeat < 3; ++repeat) {
                    Acoustic::Simulation acoustic;
                    check(acoustic.initialize(test, e) && acoustic.advance(acoustic.totalSteps(), e), e);
                    auto a = acoustic.diagnostics();
                    timings << q.nx << ',' << q.nz << ','
                            << (backend == Curvilinear::BackendKind3D::Cpu
                                    ? "CPU" + std::to_string(test.compute.cpuThreads)
                                    : "GPU")
                            << ",acoustic,FP32," << acoustic.totalSteps() << ',' << acoustic.dt() << ','
                            << a.totalStepMs << ',' << a.residentHostArrayBytes << ','
                            << a.residentDeviceBytes << '\n';
                    Curvilinear::Simulation elastic;
                    check(elastic.initialize(q, e, backend, test.compute) &&
                              elastic.advance(elastic.totalSteps(), e),
                          e);
                    auto d = elastic.diagnostics();
                    timings << q.nx << ',' << q.nz << ','
                            << (backend == Curvilinear::BackendKind3D::Cpu
                                    ? "CPU" + std::to_string(test.compute.cpuThreads)
                                    : "GPU")
                            << ",elastic,FP32," << elastic.totalSteps() << ',' << elastic.dt() << ','
                            << d.totalStepMs << ',' << d.residentHostArrayBytes << ','
                            << d.residentDeviceBytes << '\n';
                }
            }
        note("PASS: direct P arrival / zero-pressure top / Vs independence / source linearity / reset / "
             "FP32-FP64 / CPU-CUDA / DEM / budget / VTS roundtrip / CSV export");
        return 0;
    } catch (const std::exception &ex) {
        std::cerr << ex.what() << '\n';
        return 1;
    }
}
