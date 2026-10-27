#pragma once
#include "CurvilinearSolver.hpp"
namespace Seis3D::Curvilinear {
struct BackgroundTiming3D {double computeMs{},transferMs{};std::uint64_t batches{},steps{};};
// A UI-thread facade: mutable getters expose only completed publications. Grid
// and parameters are immutable during stepping and shared without volume copies.
class AsyncSimulation3D final:public ISimulationBackend3D {
    struct Impl;std::unique_ptr<Impl> impl_;
public:
    explicit AsyncSimulation3D(std::unique_ptr<ISimulationBackend3D>);
    ~AsyncSimulation3D();
    bool queue(int maxSteps,double budgetMs);
    bool poll(std::string& error);
    void pause();
    void synchronize();
    bool busy() const;
    BackgroundTiming3D backgroundTiming() const;
    bool initialize(const SimulationParameters&,std::string&,const ComputeSettings3D& = {}) override;
    bool advance(int,std::string&) override;
    bool resetFields(std::string&) override;
    bool initialized()const noexcept override;
    bool completed()const noexcept override;
    int currentStep()const noexcept override;
    int totalSteps()const noexcept override;
    double dt()const noexcept override;
    std::uint64_t revision()const noexcept override;
    BackendInfo3D backendInfo()const noexcept override;
    BackendMemory3D residentMemory()const noexcept override;
    const Grid3D& grid()const override;
    const SimulationParameters& parameters()const override;
    const Diagnostics3D& diagnostics()const override;
    const std::vector<ReceiverTrace3D>& receivers()const override;
    bool readDisplayFrame(const DisplayRequest3D&,DisplayFrame3D&,std::string&)const override;
    SimulationResult snapshot()const override;
};
}
