#pragma once
#include "CurvilinearSolver2D.hpp"
namespace Seis2D::Acoustic {
using namespace Curvilinear;
enum class AbsorbingBoundary { Sponge = 0, Cpml = 1 };
enum class Propagator { P1 = 0, CurvilinearDrp = 1 };
struct Options {
    Parameters model;
    ComputeSettings3D compute;
    BackendKind3D backend{BackendKind3D::Cpu};
    Propagator propagator{Propagator::P1}; // Experimental DRP is opt-in; RTM remains independent.
    double drpDissipation{.2};             // Sixth-difference pressure dissipation; DRP only.
    int absorberCells{24};
    bool damping{true};
    AbsorbingBoundary absorber{AbsorbingBoundary::Cpml};
    CfsPmlSettings cpml{1e-5, 1, 0};
    double memoryMiB{}; // 0: automatic RAM budget from free memory; positive: explicit manual cap.
    // p_tt ... = strength * delta_2D(x-xs) * Ricker(t), units Pa*m^2/s^2.
    double sourceStrength{1e8};
    bool operator==(const Options &) const = default;
};
struct Grid {
    int nx{}, nz{}, computeNx{}, computeNz{};
    ScalarArray x, z, vp, density; // logical target nodes; X fastest, Z positive down
};
struct Receiver {
    std::array<double, 2> position{};
    std::vector<double> pressure;
};
struct Diagnostics {
    double totalStepMs{}, initializationMs{}, peakPressure{}, ppw{}, estimatedMiB{}, ramBudgetMiB{};
    std::size_t residentHostArrayBytes{}, residentDeviceBytes{};
};
class Simulation {
  public:
    struct Impl;

  private:
    std::unique_ptr<Impl> impl_;

  public:
    Simulation();
    ~Simulation();
    Simulation(const Simulation &) = delete;
    bool initialize(const Options &, std::string &);
    bool advance(int, std::string &);
    bool reset(std::string &);
    bool initialized() const;
    bool completed() const;
    int step() const;
    int totalSteps() const;
    double dt() const;
    double recordDt() const;
    const Options &options() const;
    const Grid &grid() const;
    const std::vector<Receiver> &receivers() const;
    const Diagnostics &diagnostics() const;
    BackendInfo3D backend() const;
    // Default: logical target domain. With absorber: computeNx x computeNz, X fastest;
    // the target starts at X=absorberCells, Z=0. Values remain physical pressure in Pa.
    bool readPressure(std::vector<float> &, std::string &, bool includeAbsorber = false) const;
    std::vector<double> pressureSnapshot(bool includeAbsorber = false) const;
};
Options defaultOptions();
struct Capacity {
    enum class Limit { None, Invalid, Axis, ManualRam, SystemRam, DeviceMemory, DeviceUnavailable };
    int maxNxGivenNz{}, maxNzGivenNx{}, aspectNx{}, aspectNz{};
    std::size_t aspectNodes{}, computeNodes{};
    double estimatedRamMiB{}, estimatedDeviceMiB{}, freeRamMiB{}, freeDeviceMiB{}, ramBudgetMiB{};
    bool withinBudget{};
    Limit limit{Limit::Invalid};
};
double effectiveRamBudgetMiB(const Options &, const Seis3D::Curvilinear::BackendMemory3D &);
// Conservative allocation-free estimate. Physical validity and free memory are rechecked on rebuild.
Capacity estimateCapacity(const Options &, const Seis3D::Curvilinear::BackendMemory3D &);
bool writeModelVts(const Simulation &, const std::filesystem::path &, std::string &);
bool readModelVts(const std::filesystem::path &, Options &, std::string &);
bool writePressureSegy(const Simulation &, const std::filesystem::path &, std::string &, bool ieee64 = false);
bool exportResult(const Simulation &, const std::filesystem::path &, std::string &, bool segyIeee64 = false);
} // namespace Seis2D::Acoustic
