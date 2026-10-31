#pragma once
#include "SeisCommon/ComputeDevices.hpp"
#include "SurfaceProfile.hpp"
#include "Storage2D.hpp"
#include "SeisCommon/SourceMechanism.hpp"
#include "SeisCommon/Gsls.hpp"
#include <array>
#include <filesystem>
#include <memory>
#include <vector>
namespace Seis2D::Curvilinear {
using Seis3D::Curvilinear::BackendInfo3D;
using Seis3D::Curvilinear::BackendKind3D;
using Seis3D::Curvilinear::ComputeSettings3D;
inline constexpr int ghost = 3;
inline constexpr int maxAxisNodes = 16384;
inline constexpr int maxTimeSteps = 200000;
inline constexpr int maxSpongeCells = 512;
enum class Absorber { Sponge=0, CfsPml=1 };
struct CfsPmlSettings {
    double targetReflection{1e-4},kappaMax{3},frequency{}; // 0 follows the source frequency.
    bool operator==(const CfsPmlSettings&) const = default;
};
struct Layer {
    double thickness{}, vp{}, vs{}, density{};
    double qp{100},qs{60};
    bool operator==(const Layer &) const = default;
};
struct Parameters {
    Precision precision{Precision::Float64};
    int nx{200}, nz{200};
    double spacing{2.5}, vp{3000}, vs{1700}, density{2200};
    double relief{10}, wavelength{400}, flattenDepth{}, frequency{30}, duration{1}, cfl{.8},
        forcePerLength{2e5};
    int sourceX{-1}, sourceZ{40}, maxSteps{5000};
    SeisWave::SourceMechanism source;
    double momentPerLength{2e6}; // Isotropic moment per unit Y length, N (= N*m/m).
    // Zero/zero preserves the historical grid-defined kernel exactly.
    double sourceSigmaM{}, sourceRadiusM{};
    bool absorbing{true};
    int spongeX{16}, spongeZ{16};
    double spongeStrength{3};
    Absorber absorber{Absorber::Sponge}; // Preserve all existing experiments by default.
    CfsPmlSettings cpml;
    bool externalAbsorption{}; // Logical model stays unchanged; add absorbing strips outside it. False preserves legacy files/API experiments.
    int receiverStart{2}, receiverCount{96}, receiverStride{2}, receiverDepth{1};
    bool autoSurfaceReceivers{}; // Off for historical/API experiments; interactive defaults enable it.
    int receiverTargetCount{}; // Persistent desired count; actual count is quantized to grid nodes.
    double sampleIntervalMs{}; // 0 = each accepted step; otherwise uniform output times, linearly interpolated.
    bool layered{};
    SeisWave::Rheology::Settings attenuation;
    bool importedSurface{};
    double surfaceDatum{}; // z = datum - elevation; depth is positive downward.
    SurfaceSamples surfaceSamples;
    std::vector<Layer> layers{{20, 1500, 700, 2050}, {50, 2500, 1300, 2250}, {0, 4000, 2300, 2650}};
    bool operator==(const Parameters &) const = default;
};
struct Grid {
    int nx{}, nz{};
    int modelOffsetX{}; // Storage columns before the logical model, excluding ghost columns.
    double h{}, flattenDepth{}, maxMetric{}, minJacobian{}, maxEdge{};
    VectorArray coordinates, normals;
    ScalarArray metricA, metricB, jacobian, vp, vs, density;
    ScalarArray qp,qs;
    std::size_t index(int i, int k) const {
        return i + static_cast<std::size_t>(nx) * k;
    }
    std::size_t modelIndex(int i,int k) const { return index(i+ghost+modelOffsetX,k+ghost); }
};
struct Receiver {
    int x{}, z{};
    std::array<double, 2> position{};
    std::vector<double> vx, vz;
};
struct Diagnostics {
    double energy{}, peakEnergy{}, maxVelocity{}, tractionRelative{}, ppw{}, waveDt{}, dampingDt{},
        sourceDt{}, initializationMs{}, totalStepMs{}, lastStepMs{};
    bool finite{true};
    double sourceWeightSum{},sourceRmsRadiusM{}; // FP64 construction quadrature for physical source kernels.
    double relaxationDt{},maxRelativeQError{};
    int stateFields{5};
    double cpmlDt{},cpmlMemoryMiB{};
    std::size_t cpmlScalars{}; // Compact auxiliary values, per RK state.
    std::size_t residentHostArrayBytes{},residentDeviceBytes{},modelArrayBytes{},stateArrayBytes{},boundaryArrayBytes{};
    int rhsRegisters{};std::size_t rhsLocalBytes{};
};
struct Snapshot {
    std::array<std::vector<double>, 5> fields;
    std::vector<double> syy;
    std::vector<std::vector<double>> memory;
    std::vector<double> boundaryMemory; // Only returned when includeMemories=true; not a restart file.
}; // Vx,Vz,Sxx,Szz,Sxz; padded storage.
class Simulation {
  public:
    Simulation();
    ~Simulation();
    Simulation(const Simulation &) = delete;
    bool initialize(const Parameters &, std::string &, BackendKind3D = BackendKind3D::Cpu,
                    const ComputeSettings3D & = {});
    bool advance(int, std::string &);
    bool reset(std::string &);
    bool initialized() const;
    bool completed() const;
    int step() const;
    int totalSteps() const;
    double dt() const;
    double recordDt() const;
    const Parameters &parameters() const;
    const Grid &grid() const;
    const Diagnostics &diagnostics() const;
    const std::vector<Receiver> &receivers() const;
    BackendInfo3D backend() const;
    // includeAbsorber: full physical compute domain, excluding ghost cells, X fastest.
    // Target starts at X=modelOffsetX, Z=0. Default retains target-only export/API behavior.
    bool readFrame(int component, std::vector<float> &, std::string &, bool includeAbsorber=false) const;
    Snapshot snapshot(bool includeMemories=false) const;
    // Logical node indices; compact Vx,Vz,Sxx,Szz,Sxz readback without modifying state.
    bool readProbes(const std::vector<std::array<int,2>> &, std::vector<std::array<double,5>> &, std::string &) const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
struct SnapshotExportOptions {
    // Vx, Vz, velocity magnitude, pressure, Sxx, Szz, Sxz. Defaults preserve the legacy API output.
    std::array<bool, 7> components{true, true, false, false, true, true, true};
    bool includeMaterial{true};
};
inline SnapshotExportOptions displayedSnapshotOptions(int component) {
    SnapshotExportOptions options;
    options.components.fill(false);
    options.includeMaterial = false;
    constexpr int indices[]{1, 0, 2, 3}; // Display order: Vz, Vx, |V|, pressure.
    if (component >= 0 && component < 4)
        options.components[indices[component]] = true;
    return options;
}
bool writeSnapshot(const Simulation &, const std::filesystem::path &, std::string &,
                   const SnapshotExportOptions & = {});
bool writeReceivers(const Simulation &, const std::filesystem::path &, std::string &);
} // namespace Seis2D::Curvilinear
