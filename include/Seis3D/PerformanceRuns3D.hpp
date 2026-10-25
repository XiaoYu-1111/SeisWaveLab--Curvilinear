#pragma once
#include "CurvilinearSolver.hpp"
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
namespace Seis3D::Curvilinear {
struct PerformanceRun3D {
    std::uint64_t session{};
    SimulationParameters parameters;
    BackendInfo3D backend;
    ComputeTiming3D timing;
    double dt{};
    int steps{},receivers{};
};
inline bool comparableRuns3D(const PerformanceRun3D& a,const PerformanceRun3D& b) {
    return a.steps>0&&a.steps==b.steps&&a.dt==b.dt&&a.receivers==b.receivers&&a.parameters==b.parameters&&
        a.timing.measuredSteps==static_cast<std::uint64_t>(a.steps)&&b.timing.measuredSteps==static_cast<std::uint64_t>(b.steps)&&a.timing.totalStepMs>0&&b.timing.totalStepMs>0;
}
inline std::string configurationText3D(const SimulationParameters& p) {
    std::ostringstream s;s<<std::setprecision(17)<<p.nx<<' '<<p.ny<<' '<<p.nz<<' '<<p.spacing<<' '<<p.vp<<' '<<p.vs<<' '<<p.density<<' '<<p.layeredMaterial<<' '<<static_cast<int>(p.layerDepthMode)<<' ';
    for(const auto& l:p.layers)s<<std::quoted(l.name)<<' '<<l.thickness<<' '<<l.vp<<' '<<l.vs<<' '<<l.density<<' ';
    const auto& r=p.receiverArray;s<<p.dominantFrequency<<' '<<p.sourceX<<' '<<p.sourceY<<' '<<p.sourceZ<<' '<<p.duration<<' '<<p.maxSteps<<' '<<p.cflSafety<<' '<<p.topographyAmplitude<<' '<<p.topographyWavelength<<' '<<p.terrainTransitionDepth<<' '<<static_cast<int>(r.kind)<<' '<<r.startX<<' '<<r.startY<<' '<<r.z<<' '<<r.countX<<' '<<r.countY<<' '<<r.strideX<<' '<<r.strideY;
    const auto& b=p.absorbingBoundary;s<<" sponge "<<b.enabled<<' '<<b.cells[0]<<' '<<b.cells[1]<<' '<<b.cells[2]<<' '<<b.strength;
    if(!p.surfaceGrid.empty()) {
        const auto& d=p.surfaceGrid;s<<" dem "<<d.nx<<' '<<d.ny<<' '<<d.spacing<<' '<<d.originX<<' '<<d.originY<<' '<<d.datum;
        for(double h:d.elevations)s<<' '<<h;
    }
    return s.str();
}
inline std::string configurationId3D(const SimulationParameters& p) {
    std::uint64_t hash=1469598103934665603ULL;for(unsigned char c:configurationText3D(p)){hash^=c;hash*=1099511628211ULL;}
    std::ostringstream s;s<<std::hex<<hash;return s.str();
}
inline bool writePerformanceCsv3D(const std::vector<PerformanceRun3D>& runs,const std::filesystem::path& path,std::string& error) {
    std::error_code ec;if(!path.parent_path().empty())std::filesystem::create_directories(path.parent_path(),ec);if(ec){error=ec.message();return false;}
    std::ofstream out(path);if(!out){error="Could not write performance CSV.";return false;}
    auto quote=[&](const std::string& text){out<<'"';for(char c:text){if(c=='"')out<<'"';out<<c;}out<<'"';};
    out<<std::setprecision(17)<<"session,configuration_id,backend,device,cpu_workers,nx,ny,nz,receivers,accepted_steps,dt_s,simulation_s,compute_s,initialization_s,mean_ms_per_step,last_ms_per_step,min_ms_per_step,max_ms_per_step,steps_per_wall_s,simulation_s_per_wall_s,configuration\n";
    for(const auto& r:runs){const double seconds=r.timing.totalStepMs/1000;out<<r.session<<','<<configurationId3D(r.parameters)<<','<<(r.backend.kind==BackendKind3D::Cpu?"CPU":"CUDA")<<',';quote(r.backend.deviceName);out<<','<<r.backend.workerThreads<<','<<r.parameters.nx<<','<<r.parameters.ny<<','<<r.parameters.nz<<','<<r.receivers<<','<<r.steps<<','<<r.dt<<','<<r.steps*r.dt<<','<<seconds<<','<<r.timing.initializationMs/1000<<','<<r.timing.averageStepMs()<<','<<r.timing.lastStepMs<<','<<r.timing.minStepMs<<','<<r.timing.maxStepMs<<','<<(seconds>0?r.steps/seconds:0)<<','<<(seconds>0?r.steps*r.dt/seconds:0)<<',';quote(configurationText3D(r.parameters));out<<'\n';}
    if(!out.good()){error="Failed while writing performance CSV.";return false;}return true;
}
}
