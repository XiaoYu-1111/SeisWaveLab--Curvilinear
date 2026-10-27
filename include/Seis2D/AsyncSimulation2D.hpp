#pragma once
#include "CurvilinearSolver2D.hpp"
namespace Seis2D::Curvilinear {
struct BackgroundTiming {double computeMs{},transferMs{};std::uint64_t batches{},steps{};};
class AsyncSimulation {
    struct Impl;std::unique_ptr<Impl> impl_;
public:
    AsyncSimulation();~AsyncSimulation();
    bool queue(int steps,double budgetMs);bool poll(std::string&);void pause();void synchronize();bool busy()const;
    BackgroundTiming backgroundTiming()const;
    bool initialize(const Parameters&,std::string&,BackendKind3D=BackendKind3D::Cpu,const ComputeSettings3D& = {});
    bool advance(int,std::string&);bool reset(std::string&);
    bool initialized()const;bool completed()const;int step()const;int totalSteps()const;double dt()const;double recordDt()const;
    const Parameters& parameters()const;const Grid& grid()const;const Diagnostics& diagnostics()const;
    const std::vector<Receiver>& receivers()const;BackendInfo3D backend()const;
    bool readFrame(int,std::vector<float>&,std::string&)const;Snapshot snapshot(bool includeMemories=false)const;
    // Explicit model/snapshot exporters synchronize before accessing core state.
    operator const Simulation&()const;
};
}
