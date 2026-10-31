#include "Seis2D/AcousticSolver2D.hpp"
#include "Seis2D/Imaging2D.hpp"
#include "Seis2D/SurfaceSurvey2D.hpp"
#include "SeisCommon/SegyIo.hpp"
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
namespace {
void check(bool ok, const std::string &error) {
    if (!ok)
        throw std::runtime_error(error);
}
} // namespace
int main() {
    try {
        const auto folder =
            std::filesystem::path(SEIS3D_OUTPUT_ROOT) / "validation/workflow-upgrade-2026-10-31";
        std::filesystem::create_directories(folder);
        std::ofstream log(folder / "native.txt");
        Seis2D::Curvilinear::Parameters survey;
        survey.nx = 101;
        survey.nz = 61;
        survey.receiverCount = 11;
        check(Seis2D::Curvilinear::setUniformSurfaceReceivers(survey, false, 1024) &&
                  survey.receiverStart == 0 && survey.receiverCount == 11 && survey.receiverStride == 10 &&
                  survey.receiverDepth == 0,
              "Elastic surface layout mismatch");
        check(Seis2D::Curvilinear::setUniformSurfaceReceivers(survey, true, 4096) &&
                  survey.receiverDepth == 1,
              "Acoustic receivers were placed on zero-pressure surface");
        check(Seis2D::Curvilinear::setUniformSurfaceReceivers(survey, false, 1024, 16) &&
                  survey.receiverStart >= 16 &&
                  survey.receiverStart + (survey.receiverCount - 1) * survey.receiverStride <= 84,
              "Internal absorber was not excluded");
        survey.nx = 5000;
        survey.receiverCount = 100000;
        check(Seis2D::Curvilinear::setUniformSurfaceReceivers(survey, false, 1024) &&
                  survey.receiverCount <= 1024 &&
                  survey.receiverStart + (survey.receiverCount - 1) * survey.receiverStride < 5000,
              "Receiver limit/span mismatch");
        survey.receiverCount = 1;
        check(Seis2D::Curvilinear::setUniformSurfaceReceivers(survey, true, 4096) &&
                  survey.receiverStart == 2499,
              "Single surface receiver not centered");
        log << "PASS: uniform surface survey; elastic row0/acoustic row1; internal absorber exclusion; count "
               "limit / single receiver\n";
        auto options = Seis2D::Acoustic::defaultOptions();
        options.model.precision = Seis2D::Curvilinear::Precision::Float32;
        Seis3D::Curvilinear::BackendMemory3D memory{8ULL << 30, 8ULL << 30};
        check(options.memoryMiB == 0, "Acoustic default did not use automatic RAM budget");
        auto largeOptions = options;
        largeOptions.backend = Seis3D::Curvilinear::BackendKind3D::Cuda;
        largeOptions.model.nx = 5000;
        largeOptions.model.nz = 2000;
        const auto automaticLarge = Seis2D::Acoustic::estimateCapacity(largeOptions, memory);
        check(automaticLarge.withinBudget && automaticLarge.ramBudgetMiB > 1536,
              "Automatic CUDA budget still blocks 5000x2000 despite sufficient resources");
        largeOptions.memoryMiB = 1536;
        const auto manualLarge = Seis2D::Acoustic::estimateCapacity(largeOptions, memory);
        check(!manualLarge.withinBudget && manualLarge.limit == Seis2D::Acoustic::Capacity::Limit::ManualRam,
              "Manual RAM cap did not report the actual limiting resource");
        largeOptions.memoryMiB = 0;
        const auto lowGpu = Seis2D::Acoustic::estimateCapacity(largeOptions, {8ULL << 30, 64ULL << 20});
        check(!lowGpu.withinBudget && lowGpu.limit == Seis2D::Acoustic::Capacity::Limit::DeviceMemory,
              "CUDA VRAM limit was not distinguished from RAM");
        const auto full = Seis2D::Acoustic::estimateCapacity(options, memory);
        check(full.withinBudget && full.aspectNx >= 16 && full.aspectNz >= 16 && full.aspectNodes > 0,
              "Invalid capacity for normal model");
        auto small = options;
        small.memoryMiB = 256;
        const auto reduced = Seis2D::Acoustic::estimateCapacity(small, memory);
        check(reduced.aspectNodes < full.aspectNodes, "Capacity did not follow RAM budget");
        small = options;
        small.model.precision = Seis2D::Curvilinear::Precision::Float64;
        check(Seis2D::Acoustic::estimateCapacity(small, memory).aspectNodes < full.aspectNodes,
              "Capacity ignored FP64 memory");
        small = options;
        small.backend = Seis3D::Curvilinear::BackendKind3D::Cuda;
        const auto gpu = Seis2D::Acoustic::estimateCapacity(small, {8ULL << 30, 64ULL << 20});
        check(gpu.aspectNodes < full.aspectNodes, "Capacity ignored free VRAM");
        small = options;
        small.model.nx = full.aspectNx;
        small.model.nz = full.aspectNz;
        check(Seis2D::Acoustic::estimateCapacity(small, memory).withinBudget,
              "Suggested grid exceeded estimated budget");
        small.model.nz = std::numeric_limits<int>::max();
        small.model.nx = 16;
        const auto invalid = Seis2D::Acoustic::estimateCapacity(small, memory);
        check(!invalid.withinBudget && invalid.maxNxGivenNz == 0 && invalid.aspectNodes == 0,
              "Invalid extreme dimensions escaped capacity bounds");
        log << "PASS: budget/FP32/FP64/free-VRAM capacity estimates; aspect suggestions within budget\n";
        options.model.precision = Seis2D::Curvilinear::Precision::Float64;
        options.model.nx = 81;
        options.model.nz = 61;
        options.model.sourceX = 40;
        options.model.sourceZ = 2;
        options.model.receiverStart = 10;
        options.model.receiverCount = 8;
        options.model.receiverStride = 8;
        options.model.duration = .2;
        options.compute.cpuThreads = 1;
        Seis2D::Acoustic::Simulation simulation;
        std::string error;
        check(simulation.initialize(options, error) && simulation.advance(100, error), error);
        for (bool wide : {false, true}) {
            const auto file = folder / (wide ? "pressure64.sgy" : "pressure32.sgy");
            check(Seis2D::Acoustic::writePressureSegy(simulation, file, error, wide), error);
            SeisWave::ImportedRecords imported;
            check(SeisWave::readSegy(file, imported, error), error);
            check(imported.units == "Pa" && imported.component == "Pressure" &&
                      imported.traces.size() == simulation.receivers().size() &&
                      std::abs(imported.dt - simulation.recordDt()) < 1e-12,
                  "Pressure SEG-Y metadata changed");
            for (std::size_t r = 0; r < imported.traces.size(); ++r) {
                const auto &original = simulation.receivers()[r];
                const auto &loaded = imported.traces[r];
                check(loaded.samples.size() == original.pressure.size(), "SEG-Y sample count changed");
                check(std::abs(loaded.position[0] - original.position[0]) < .002 &&
                          std::abs(loaded.position[2] - original.position[1]) < .002,
                      "SEG-Y coordinates changed");
                for (std::size_t n = 0; n < loaded.samples.size(); ++n)
                    check(loaded.samples[n] ==
                              (wide ? original.pressure[n] : double(float(original.pressure[n]))),
                          "SEG-Y applied display processing or changed precision");
            }
        }
        log << "PASS: raw-pressure SEG-Y IEEE32/IEEE64 roundtrip, Pa, coordinates, dt and samples\n";
        auto imaging = Seis2D::Imaging::benchmarkOptions();
        imaging.shots = 1;
        imaging.model.duration = .25;
        imaging.compute.cpuThreads = 1;
        imaging.backend = Seis3D::Curvilinear::BackendKind3D::Cpu;
        Seis2D::Imaging::Result plain, live;
        check(Seis2D::Imaging::run(imaging, plain, error), error);
        int frames = 0;
        bool phases[3]{};
        check(Seis2D::Imaging::run(
                  imaging, live, error, nullptr, {},
                  [&](Seis2D::Imaging::Preview frame) {
                      ++frames;
                      check(frame.nx <= 256 && frame.nz <= 192 && frame.phase >= 0 && frame.phase <= 2,
                            "Unbounded/invalid RTM preview");
                      phases[frame.phase] = true;
                      check(frame.backgroundVp.back() == 1800 && frame.vp.back() == 2300,
                            "Live preview confused true and background velocity models");
                      const auto size = std::size_t(frame.nx) * frame.nz;
                      for (const auto *values : {&frame.x, &frame.z, &frame.vp, &frame.backgroundVp,
                                                 &frame.pressure, &frame.image, &frame.illumination}) {
                          check(values->size() == size, "RTM preview shape mismatch");
                          for (double value : *values)
                              check(std::isfinite(value), "Nonfinite RTM preview");
                      }
                  }),
              error);
        check(frames >= 3 && phases[0] && phases[1] && phases[2], "RTM omitted a process phase");
        check(live.image == plain.image && live.illumination == plain.illumination &&
                  live.records == plain.records && live.normalizedImage == plain.normalizedImage,
              "Live preview changed RTM results");
        std::atomic_bool cancel{};
        auto retained = plain;
        check(!Seis2D::Imaging::run(imaging, retained, error, &cancel, {}, [&](auto) { cancel = true; }) &&
                  retained.image == plain.image,
              "Preview cancellation changed previous result");
        log << "PASS: bounded RTM previews across all phases; results bitwise unchanged; cancellation "
               "transactional\n";
        if (Seis3D::Curvilinear::queryBackend3D(Seis3D::Curvilinear::BackendKind3D::Cuda).available) {
            auto gpuOptions = imaging;
            gpuOptions.backend = Seis3D::Curvilinear::BackendKind3D::Cuda;
            Seis2D::Imaging::Result gpuPlain, gpuLive;
            int gpuFrames = 0;
            check(Seis2D::Imaging::run(gpuOptions, gpuPlain, error), error);
            check(Seis2D::Imaging::run(gpuOptions, gpuLive, error, nullptr, {},
                                       [&](auto frame) {
                                           ++gpuFrames;
                                           for (double value : frame.pressure)
                                               check(std::isfinite(value), "Nonfinite CUDA preview");
                                       }),
                  error);
            check(gpuFrames > 0 && gpuPlain.image == gpuLive.image &&
                      gpuPlain.illumination == gpuLive.illumination && gpuPlain.records == gpuLive.records,
                  "CUDA preview changed RTM results");
            log << "PASS: CUDA process previews finite; image/illumination/records bitwise unchanged\n";
        }
        std::cout << "Workflow upgrade validation PASS\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
