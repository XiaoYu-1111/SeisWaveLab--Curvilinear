#pragma once
#include "CurvilinearSolver2D.hpp"
#include <atomic>
#include <functional>
#include <string>
namespace Seis2D::Imaging {
using namespace Curvilinear;
// Phase 1: variable-density acoustic pressure RTM, not the elastic DRP/RK4 solver.
struct Options {
    Parameters model;
    ComputeSettings3D compute;
    BackendKind3D backend{BackendKind3D::Cpu};
    int shots{7}, firstShot{12}, lastShot{108}, sourceDepth{2};
    int receiverDepth{2}, receiverStride{2}, absorberCells{24};
    int receiverStart{1}, receiverCount{}; // 0 preserves the legacy line ending at nx-2.
    int receiverTargetCount{};   // Persistent requested count for automatic quantization; 0 initializes it.
    bool autoSurfaceReceivers{}; // UI defaults on; numerical benchmark defaults remain unchanged.
    int smoothIterations{80}, checkpointInterval{};
    double backgroundVp{}; // >0: homogeneous Vp, density retained; 0: smooth true Vp.
    double memoryMiB{768}, illuminationFloor{.005};
    bool operator==(const Options &) const = default;
};
struct Result {
    int nx{}, nz{}, steps{}, shots{}, receivers{}, checkpointInterval{};
    double dt{}, seconds{}, estimatedMiB{}, minPpw{};
    std::string method, device;
    Options options;
    std::vector<double> x, z, vp, backgroundVp, image, normalizedImage, illumination;
    std::vector<double> receiverX, receiverZ, records; // last shot, time-major, pressure residual, Pa
};
Options benchmarkOptions();
bool alignSurfaceReceivers(Options &); // Quantized uniform X, first subsurface row, source unchanged.
// Transactional preflight/model preview only: no time propagation, images or checkpoints.
bool prepareModel(const Options &, Result &, std::string &, std::atomic_bool *cancel = nullptr);
using Progress = std::function<void(double, const std::string &)>;
struct Preview {
    int nx{}, nz{}, shot{}, shots{}, step{}, steps{},
        phase{}; // 0 true, 1 background, 2 source replay / imaging
    double time{};
    std::vector<double> x, z, vp, backgroundVp, pressure, image, illumination;
};
using PreviewSink = std::function<void(Preview)>;
// Transactional; cancellation and validation errors leave the previous result intact.
bool run(const Options &, Result &, std::string &, std::atomic_bool *cancel = nullptr, Progress = {},
         PreviewSink = {});
bool exportResult(const Result &, const std::filesystem::path &, std::string &);
// Exact discrete full source-to-receiver adjoint check including damping and sampling.
double adjointDotTest(const Options &);
} // namespace Seis2D::Imaging
