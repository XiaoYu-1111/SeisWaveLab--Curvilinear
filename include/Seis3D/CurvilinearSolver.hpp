#pragma once
#include "Seis3D/KernelTypes3D.hpp"
#include "Seis3D/SurfaceGrid3D.hpp"
#include "SeisCommon/ComputeDevices.hpp"
#include "SeisCommon/SourceMechanism.hpp"

#include <array>
#include <cstddef>
#include <string>
#include <vector>
#include <memory>
#include <cstdint>

namespace Seis3D::Curvilinear {

// Axis safety bound is independent of the total allocation budget. Thin/wide
// grids may exceed 300 on X/Y while retaining the previous total node budget.
inline constexpr int maxPhysicalNodesPerAxis3D=4096;
inline constexpr std::size_t maxPaddedNodes3D=306ull*306*306;
inline constexpr int maxTimeSteps3D=20000,maxCpuThreads3D=64;
inline constexpr double minCflCoefficient3D=.02;
inline constexpr double maxCflCoefficient3D=.8;
inline constexpr double defaultCflCoefficient3D=.8;


struct Grid3D {
    int nx{};
    int ny{};
    int nz{};
    int ghost{ghostWidth3D};
    double dx{10.0};
    double dy{10.0};
    double dz{10.0};
    double topographyAmplitude{35.0};
    double topographyWavelength{240.0};
    double terrainTransitionDepth{};
    SurfaceGrid3D surfaceGrid;
    bool hasTerrain() const {return !surfaceGrid.empty()||topographyAmplitude!=0;}
    std::vector<std::array<double, 3>> coordinates;
    std::vector<std::array<double, 9>> inverseMetrics;
    std::vector<double> jacobian;
    std::vector<std::array<double, 3>> surfaceNormal;

    [[nodiscard]] std::size_t index(int i, int j, int k) const noexcept;
    [[nodiscard]] bool buildGeometry(std::string& error);
    [[nodiscard]] bool validateGeometry(std::string& error); // No volume allocation; resolves flatten depth.
    [[nodiscard]] std::size_t physicalPointCount() const noexcept;
    [[nodiscard]] GridLayout3D layout() const noexcept;
};

enum class LayerDepthMode3D { GridIndex, BelowSurface, Datum };
struct MaterialLayer3D {
    std::string name;
    double thickness{},vp{},vs{},density{}; // Last thickness=0: basement fills remaining depth.
    bool operator==(const MaterialLayer3D&) const = default;
};
enum class ReceiverArrayKind3D { AutoXLine, XLine, YLine, GridXY };
struct ReceiverArraySettings3D {
    ReceiverArrayKind3D kind{ReceiverArrayKind3D::AutoXLine};
    int startX{2},startY{-1},z{1}; // -1 centers X/Y; all indices physical, zero-based.
    int countX{24},countY{1},strideX{1},strideY{1};
    bool operator==(const ReceiverArraySettings3D&) const = default;
};
struct AbsorbingBoundarySettings3D {
    bool enabled{true};
    std::array<int,3> cells{8,6,8}; // X/Y sides, Z bottom; each capped at one third of its own axis.
    double strength{3.0}; // sigma_max = strength * Vp_max / physical thickness.
    bool operator==(const AbsorbingBoundarySettings3D&) const = default;
};
struct SimulationParameters {
    int nx{28};
    int ny{24};
    int nz{28};
    double spacing{10.0};
    double vp{3000.0};
    double vs{1700.0};
    double density{2200.0};
    bool layeredMaterial{false};
    LayerDepthMode3D layerDepthMode{LayerDepthMode3D::GridIndex}; // Same k => same material; optional physical-depth rasterization.
    std::vector<MaterialLayer3D> layers;
    double dominantFrequency{12.0};
    int sourceX{-1},sourceY{-1},sourceZ{3}; // Physical node indices, zero-based; -1 centers X/Y.
    SeisWave::SourceMechanism source;
    double sourceForce{2e8},sourceMoment{2e9}; // N (force), N*m (each diagonal isotropic moment component).
    double duration{1.0};
    int maxSteps{5000}; // Stop at duration OR cap, whichever first; 0 = duration only. Never changes CFL dt.
    double cflSafety{defaultCflCoefficient3D}; // DRP coefficient in dt=C*min(h/M,Lmin)/Vp; validated range 0.02..0.8.
    double topographyAmplitude{20.0};
    double topographyWavelength{300.0};
    double terrainTransitionDepth{}; // 0 selects an automatic shallow transition to horizontal layers.
    SurfaceGrid3D surfaceGrid; // Embedded in reproducible models; empty selects analytic terrain.
    ReceiverArraySettings3D receiverArray;
    AbsorbingBoundarySettings3D absorbingBoundary;
    bool operator==(const SimulationParameters&) const = default;
};

inline std::array<int,3> sourceIndices3D(const SimulationParameters& p) {
    return {p.sourceX<0?p.nx/2:p.sourceX,p.sourceY<0?p.ny/2:p.sourceY,p.sourceZ};
}


struct ComputeTiming3D {
    double initializationMs{},totalStepMs{},lastStepMs{},minStepMs{},maxStepMs{};
    std::uint64_t measuredSteps{};
    double averageStepMs() const {return measuredSteps?totalStepMs/measuredSteps:0;}
};
struct Diagnostics3D {
    ComputeTiming3D timing;
    double minJacobian{};
    double maxMetricNorm{};
    double maxAbsVelocity{};
    double maxTractionRateRelative{};
    double estimatedMemoryMiB{}; // Active core estimate; CUDA includes both RAM and VRAM.
    double pointsPerShortestWavelength{};
    bool allFinite{true};
    std::array<double,5> displayPeaks{}; // Vz, Vx, Vy, |V|, pressure; physical nodes only.
    double minCellLength{};
    double waveDtLimit{},dampingDtLimit{},maxDampingDt{};
    double elasticEnergy{},peakEnergy{};
    double sourceDtLimit{},stepsPerDominantPeriod{},maxCellEdge{};
    double materialMaxVp{},materialMinVs{},materialMaxVs{},materialMaxDensity{},minLayerCells{};
    int unresolvedLayers{};
};

struct DisplayRequest3D { int x{},y{},z{},component{}; bool operator==(const DisplayRequest3D&) const = default; };
struct SliceField3D { int width{},height{}; std::vector<float> values; };
struct DisplayFrame3D { DisplayRequest3D request; std::uint64_t revision{}; std::array<SliceField3D,3> planes; std::size_t bytes{}; };

struct ReceiverTrace3D {
    std::array<double, 3> position{};
    std::size_t gridIndex{};
    std::vector<double> samples;
    std::vector<double> samplesVx,samplesVy; // Same t=0-inclusive time axis; samples remains raw Vz for compatibility.
};

struct SimulationResult {
    Grid3D grid;
    std::vector<double> vx;
    std::vector<double> vy;
    std::vector<double> vz;
    std::vector<double> sxx;
    std::vector<double> syy;
    std::vector<double> szz;
    std::vector<double> sxy;
    std::vector<double> sxz;
    std::vector<double> syz;
    int steps{};
    double dt{};
    double modelVp{};
    double modelVs{};
    double modelDensity{};
    std::vector<double> materialVp,materialVs,materialDensity;
    std::vector<int> layerIds;
    std::array<double,3> sourcePosition{};
    double maxAbsVz{};
    double maxFreeSurfaceTraction{};
    bool allFinite{};
    Diagnostics3D diagnostics;
    BackendInfo3D computeBackend;
    SeisWave::SourceMechanism sourceMechanism;
    double sourceForce{},sourceMoment{},sourceFrequency{};
};

// Backend boundary: rendering requests three compact host slices; a full-volume snapshot is an explicit export operation.
class ISimulationBackend3D {
public:
    virtual ~ISimulationBackend3D()=default;
    virtual bool initialize(const SimulationParameters&,std::string&,const ComputeSettings3D& = {})=0;
    virtual bool advance(int,std::string&)=0;
    virtual bool resetFields(std::string&)=0;
    virtual bool initialized() const noexcept=0;
    virtual bool completed() const noexcept=0;
    virtual int currentStep() const noexcept=0;
    virtual int totalSteps() const noexcept=0;
    virtual double dt() const noexcept=0;
    virtual std::uint64_t revision() const noexcept=0;
    virtual BackendInfo3D backendInfo() const noexcept=0;
    virtual BackendMemory3D residentMemory() const noexcept=0;
    virtual const Grid3D& grid() const=0;
    virtual const SimulationParameters& parameters() const=0;
    virtual const Diagnostics3D& diagnostics() const=0;
    virtual const std::vector<ReceiverTrace3D>& receivers() const=0;
    virtual bool readDisplayFrame(const DisplayRequest3D&,DisplayFrame3D&,std::string&) const=0;
    virtual SimulationResult snapshot() const=0;
};

// Shared session orchestration; explicit CPU reference by default. Applications
// use defaultBackend3D() to prefer CUDA. GPU fields/RK buffers remain device-resident.
class CpuSimulation3D final : public ISimulationBackend3D {
public:
    explicit CpuSimulation3D(BackendKind3D kind=BackendKind3D::Cpu);
    ~CpuSimulation3D();
    CpuSimulation3D(const CpuSimulation3D&) = delete;
    CpuSimulation3D& operator=(const CpuSimulation3D&) = delete;
    [[nodiscard]] bool initialize(const SimulationParameters& parameters, std::string& error,const ComputeSettings3D& settings = {}) override;
    [[nodiscard]] bool advance(int count, std::string& error);
    [[nodiscard]] bool resetFields(std::string& error) override;
    [[nodiscard]] bool initialized() const noexcept;
    [[nodiscard]] bool completed() const noexcept;
    [[nodiscard]] int currentStep() const noexcept;
    [[nodiscard]] int totalSteps() const noexcept;
    [[nodiscard]] double dt() const noexcept;
    [[nodiscard]] const Grid3D& grid() const;
    [[nodiscard]] const SimulationParameters& parameters() const;
    [[nodiscard]] const Diagnostics3D& diagnostics() const;
    [[nodiscard]] const std::vector<double>& field(Field3D component) const;
    [[nodiscard]] const std::vector<ReceiverTrace3D>& receivers() const;
    [[nodiscard]] SimulationResult snapshot() const;
    [[nodiscard]] std::uint64_t revision() const noexcept override;
    [[nodiscard]] BackendInfo3D backendInfo() const noexcept override;
    [[nodiscard]] BackendMemory3D residentMemory() const noexcept override;
    [[nodiscard]] bool readDisplayFrame(const DisplayRequest3D&,DisplayFrame3D&,std::string&) const override;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::uint64_t generation_{};
    BackendKind3D requestedBackend_{BackendKind3D::Cpu};
};

// Unavailable CUDA selection fails explicitly; it never silently runs a different engine.
[[nodiscard]] std::unique_ptr<ISimulationBackend3D> createSimulationBackend3D(BackendKind3D,std::string& error);

// UI rebuilds release the old volume before allocating its replacement. Direct
// initialize remains transactional for callers that need to retain live fields.
struct RebuildOutcome3D {bool success{},released{},restored{},fallback{};BackendMemory3D reclaimed;};
bool validateSimulationConfiguration3D(const SimulationParameters&,BackendKind3D,std::string&,const ComputeSettings3D& = {});
BackendMemory3D memoryForRebuild3D(const ISimulationBackend3D*,BackendKind3D,int cudaDevice=-1);
RebuildOutcome3D rebuildSimulation3D(std::unique_ptr<ISimulationBackend3D>&,const SimulationParameters&,BackendKind3D,const ComputeSettings3D&,std::string&);

// Builds a body-fitted elastic model and advances alternating DRP/RK stages
// on the application-preferred backend.
[[nodiscard]] bool runDemo(const SimulationParameters& parameters, SimulationResult& result,
                           std::string& error);

} // namespace Seis3D::Curvilinear
