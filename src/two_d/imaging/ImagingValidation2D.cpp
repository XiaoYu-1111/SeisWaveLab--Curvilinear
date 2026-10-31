#include "Seis2D/Imaging2D.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
using namespace Seis2D::Imaging;
void check(bool ok, const std::string &error) {
    if (!ok)
        throw std::runtime_error(error);
}
double relative(const std::vector<double> &a, const std::vector<double> &b) {
    double n = 0, d = 0;
    check(a.size() == b.size(), "Shape mismatch");
    for (std::size_t i = 0; i < a.size(); ++i) {
        n += (a[i] - b[i]) * (a[i] - b[i]);
        d += a[i] * a[i];
    }
    return std::sqrt(n / std::max(1e-100, d));
}
int main() {
    try {
        const auto folder = std::filesystem::path(SEIS3D_OUTPUT_ROOT) / "validation/rtm-2d";
        std::filesystem::create_directories(folder);
        std::ofstream report(folder / "validation.txt");
        auto log = [&](const std::string &s) {
            std::cout << s << '\n';
            report << s << '\n';
        };
        auto number = [](double d) {
            std::ostringstream s;
            s << std::scientific << std::setprecision(10) << d;
            return s.str();
        };
        auto o = benchmarkOptions();
        o.model.precision = Precision::Float64;
        o.shots = 3;
        o.firstShot = 35;
        o.lastShot = 85;
        auto dot = o;
        dot.model.nx = 41;
        dot.model.nz = 35;
        dot.model.relief = 3;
        dot.model.wavelength = 150;
        dot.firstShot = 4;
        dot.lastShot = 32;
        dot.receiverStride = 2;
        dot.absorberCells = 8;
        dot.sourceDepth = 2;
        dot.model.layers = {{50, 1800, 900, 1700}, {0, 2200, 1000, 2200}};
        double d = adjointDotTest(dot);
        log("CPU FP64 damped curved variable-density full propagation dot error=" + number(d));
        check(d < 1e-10, "Discrete adjoint failed FP64 dot test");
        dot.model.precision = Precision::Float32;
        d = adjointDotTest(dot);
        log("CPU FP32 dot error=" + number(d));
        check(d < 5e-4, "Discrete adjoint failed FP32 dot test");
        Result base;
        std::string error;
        check(run(o, base, error), error);
        // Stack the central aperture to find the strongest coherent reflector below 100 m.
        int peak = 0;
        double value = 0;
        for (int k = 20; k < 65; ++k) {
            double sum = 0;
            for (int i = 40; i < 81; ++i)
                sum += std::abs(base.image[std::size_t(k) * base.nx + i]);
            if (sum > value) {
                value = sum;
                peak = k;
            }
        }
        log("Horizontal interface expected=200m; imaged peak=" + std::to_string(peak * o.model.spacing) +
            "m");
        check(std::abs(peak * o.model.spacing - 200) <= 20, "RTM reflector mislocated by >20m");
        check(*std::max_element(base.illumination.begin(), base.illumination.end()) > 0,
              "Empty illumination");
        o.checkpointInterval = 17;
        Result replay;
        check(run(o, replay, error), error);
        double err = relative(base.image, replay.image);
        log("Checkpoint interval invariance L2=" + number(err));
        check(err < 1e-12, "Checkpoint replay differs");
        auto same = o;
        same.model.layers = {{0, 1800, 900, 1800}};
        Result zero;
        check(run(same, zero, error), error);
        check(std::all_of(zero.image.begin(), zero.image.end(), [](double v) { return v == 0; }),
              "Equal true/background model produced nonzero image");
        auto limited = o;
        limited.memoryMiB = 16;
        Result retained = base;
        check(!run(limited, retained, error) && retained.image == base.image,
              "Memory rejection did not retain old result");
        std::atomic_bool cancel{true};
        check(!run(o, retained, error, &cancel) && retained.image == base.image,
              "Cancellation mutated result");
        auto invalid = o;
        cancel = false;
        check(!run(o, retained, error, &cancel,
                   [&](double fraction, const std::string &) {
                       if (fraction > .3)
                           cancel = true;
                   }) &&
                  retained.image == base.image,
              "In-flight cancellation mutated result");
        invalid.model.attenuation.enabled = true;
        check(!run(invalid, retained, error), "Q silently accepted by acoustic RTM");
        o.model.precision = Precision::Float32;
        Result fp32;
        check(run(o, fp32, error), error);
        err = relative(base.image, fp32.image);
        log("CPU FP32 versus FP64 image L2=" + number(err));
        check(err < .002, "FP32 RTM image mismatch");
        auto gpu = Seis3D::Curvilinear::queryBackend3D(BackendKind3D::Cuda);
        if (gpu.available) {
            dot.backend = BackendKind3D::Cuda;
            d = adjointDotTest(dot);
            log("CUDA FP32 dot error=" + number(d));
            check(d < 5e-4, "CUDA FP32 adjoint failed");
            dot.model.precision = Precision::Float64;
            d = adjointDotTest(dot);
            log("CUDA FP64 dot error=" + number(d));
            check(d < 1e-10, "CUDA FP64 adjoint failed");
            o.backend = BackendKind3D::Cuda;
            Result actual;
            check(run(o, actual, error), error);
            err = relative(fp32.image, actual.image);
            log("CPU/CUDA FP32 image L2=" + number(err));
            check(err < .002, "CPU/CUDA RTM image mismatch");
            o.model.precision = Precision::Float64;
            check(run(o, actual, error), error);
            err = relative(base.image, actual.image);
            log("CPU/CUDA FP64 image L2=" + number(err));
            check(err < 1e-10, "CPU/CUDA FP64 image mismatch");
        } else
            log("CUDA SKIPPED: " + gpu.description);
        // External DEM fixture tests geometry independently of the analytic terrain.
        auto terrain = benchmarkOptions();
        terrain.shots = 1;
        terrain.model.importedSurface = true;
        terrain.model.surfaceDatum = 1000;
        terrain.model.surfaceSamples = {{0, 1000}, {150, 1010}, {300, 995}, {450, 1005}, {600, 1000}};
        terrain.model.duration = .4;
        Result curved;
        check(run(terrain, curved, error), error);
        check(curved.z[30] < 0 && curved.z[60] > 0, "DEM orientation/datum mismatch");
        auto manual = benchmarkOptions();
        manual.shots = 1;
        manual.firstShot = manual.lastShot = 20;
        manual.model.nx = 61;
        manual.model.nz = 41;
        manual.model.duration = .2;
        manual.receiverStart = 3;
        manual.receiverCount = 9;
        manual.receiverStride = 4;
        manual.receiverDepth = 3;
        manual.model.relief = 5;
        manual.model.wavelength = 200;
        manual.absorberCells = 8;
        Result prepared, manualRun;
        check(prepareModel(manual, prepared, error), error);
        check(prepared.image.empty() && prepared.records.empty() && prepared.receivers == 9,
              "Rebuild performed propagation or lost receiver count");
        for (int r = 0; r < prepared.receivers; ++r) {
            const auto q = std::size_t(manual.receiverDepth) * manual.model.nx + manual.receiverStart +
                           r * manual.receiverStride;
            check(prepared.receiverX[r] == prepared.x[q] && prepared.receiverZ[r] == prepared.z[q],
                  "Manual receiver geometry mismatch");
        }
        check(run(manual, manualRun, error), error);
        check(prepared.x == manualRun.x && prepared.z == manualRun.z &&
                  prepared.receiverX == manualRun.receiverX && prepared.receiverZ == manualRun.receiverZ &&
                  prepared.dt == manualRun.dt,
              "Rebuild/run geometry drift");
        check(adjointDotTest(manual) < 5e-4, "Manual receiver adjoint failed");
        auto automatic = manual;
        automatic.autoSurfaceReceivers = true;
        automatic.model.nx = 121;
        check(prepareModel(automatic, prepared, error), error);
        const auto applied = prepared.options;
        const int last = applied.receiverStart + (prepared.receivers - 1) * applied.receiverStride;
        auto stable = applied;
        check(alignSurfaceReceivers(stable) && stable == applied && stable.receiverTargetCount == 9,
              "Repeated automatic alignment changed the line or requested count");
        check(applied.receiverDepth == 1 && applied.receiverStart >= 1 && last <= automatic.model.nx - 2 &&
                  std::abs((applied.receiverStart - 1) - (automatic.model.nx - 2 - last)) <= 1,
              "Automatic receiver line not centered/re-aligned after resize");
        for (int r = 0; r < prepared.receivers; ++r) {
            const auto q =
                std::size_t(automatic.model.nx) + applied.receiverStart + r * applied.receiverStride;
            check(prepared.receiverZ[r] == prepared.z[q], "Auto receivers did not follow terrain row 1");
        }
        check(adjointDotTest(automatic) < 5e-4, "Automatic receiver adjoint failed");
        if (gpu.available) {
            auto manualGpu = manual;
            manualGpu.backend = BackendKind3D::Cuda;
            Result actual;
            check(run(manualGpu, actual, error), error);
            check(relative(actual.image, manualRun.image) < .002, "Manual line CPU/CUDA mismatch");
        }
        const auto preservedX = prepared.x;
        auto bad = automatic;
        bad.memoryMiB = 16;
        bad.model.duration = 1;
        bad.checkpointInterval = 1;
        check(!prepareModel(bad, prepared, error) && prepared.x == preservedX,
              "Failed rebuild discarded the applied model");
        std::atomic_bool cancelledPrepare{true};
        check(!prepareModel(automatic, prepared, error, &cancelledPrepare) && prepared.x == preservedX,
              "Cancelled rebuild changed applied model");
        check(base.receivers == 60 && base.receiverX.front() == 5 && base.receiverX.back() == 595,
              "Legacy RTM line changed");
        log("PASS: rebuild-only preflight / manual start-count-stride-depth / automatic terrain alignment "
            "after resize / survey adjoints / failure and cancellation transaction / legacy line");
        auto destination =
            folder /
            ("experiment-" + std::to_string(std::chrono::system_clock::now().time_since_epoch().count()));
        check(exportResult(base, destination, error), error);
        check(std::filesystem::file_size(destination / "rtm.vts") > 1000, "RTM export empty");
        check(!exportResult(base, destination, error), "Existing experiment overwritten");
        log("PASS: interface / zero residual / curved DEM / checkpoint replay / CPU-CUDA / FP32-FP64 / "
            "cancellation / memory guard / exports");
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
