#include "Seis2D/CurvilinearSolver2D.hpp"
#include "Seis2D/ModelIo2D.hpp"
#include "SeisCommon/AcquisitionAnalysis.hpp"
#include <algorithm>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <stdexcept>
using namespace Seis2D::Curvilinear;
int main(int argc,char** argv){try{
    const auto folder=argc>1?std::filesystem::path(argv[1]):std::filesystem::path(SEIS3D_OUTPUT_ROOT)/"research/qp-qs-2d";
    std::filesystem::create_directories(folder);const std::string requested=argc>2?argv[2]:"auto";
    if(requested!="cpu"&&requested!="cuda"&&requested!="auto")throw std::runtime_error("Backend must be cpu, cuda or auto.");
    if(requested=="cuda"&&!Seis3D::Curvilinear::queryBackend3D(BackendKind3D::Cuda).available)throw std::runtime_error("CUDA was requested but is unavailable; explicitly choose cpu.");
    const auto backend=requested=="cpu"?BackendKind3D::Cpu:requested=="cuda"?BackendKind3D::Cuda:Seis3D::Curvilinear::defaultBackend3D();
    std::ofstream summary(folder/"cases.csv");summary<<std::setprecision(17)<<"case,relief_m,attenuation,top_qp,top_qs,nx,nz,h_m,dt_s,steps,time_s,compute_s,max_q_fit_error,source_f0_hz,reference_hz,fit_min_hz,fit_max_hz,mechanisms,backend\n";
    struct Case{const char* name;double relief,qp,qs;bool attenuation;};
    const Case cases[]{{"flat_elastic",0,40,25,false},{"flat_q40",0,40,25,true},{"curved_elastic",8,40,25,false},{"curved_q40",8,40,25,true},{"curved_q20",8,20,12.5,true},{"curved_q80",8,80,50,true}};
    for(const auto& c:cases){const auto root=folder/c.name;std::filesystem::create_directories(root);Parameters p;p.nx=481;p.nz=241;p.spacing=1;p.relief=c.relief;p.wavelength=280;p.sourceX=240;p.sourceZ=15;p.frequency=15;p.duration=.6;p.maxSteps=0;p.sampleIntervalMs=.5;
        p.receiverStart=80;p.receiverCount=81;p.receiverStride=4;p.receiverDepth=1;p.spongeX=p.spongeZ=40;p.layered=true;p.layers={{40,900,400,1800,c.qp,c.qs},{0,2000,1000,2300,200,120}};
        p.attenuation.enabled=c.attenuation;p.attenuation.minFrequency=3;p.attenuation.maxFrequency=60;p.attenuation.referenceFrequency=15;p.attenuation.qp=c.qp;p.attenuation.qs=c.qs;
        Simulation sim;std::string error;const auto require=[&](bool okay){if(!okay)throw std::runtime_error(c.name+std::string(": ")+error);};
        require(sim.initialize(p,error,backend,{1}));require(writeModelVts(sim,root/"model.vts",{1},error));
        const int middle=(sim.totalSteps()+1)/2;require(sim.advance(middle,error));SnapshotExportOptions snapshot;snapshot.components.fill(false);snapshot.components[1]=true;require(writeSnapshot(sim,root/"wavefield.vts",error,snapshot));require(sim.advance(sim.totalSteps()-middle,error));require(writeReceivers(sim,root/"records.csv",error));
        SeisWave::RecordDataView records;records.dt=sim.recordDt();records.components={"Vz","Vx"};
        for(const auto& r:sim.receivers()){SeisWave::TraceView t;t.position={r.position[0],0,r.position[1]};t.channels[0]=r.vz;t.channels[1]=r.vx;records.traces.push_back(t);}
        // These are descriptive early/late windows, not automatically classified phases or field SNR.
        require(SeisWave::writeAcquisitionCsv(records,0,{.1,.3},{.3,.6},.1,root/"window-metrics.csv",error));
        const auto& d=sim.diagnostics();summary<<c.name<<','<<p.relief<<','<<p.attenuation.enabled<<','<<c.qp<<','<<c.qs<<','<<p.nx<<','<<p.nz<<','<<p.spacing<<','<<sim.dt()<<','<<sim.step()<<','<<sim.step()*sim.dt()<<','<<d.totalStepMs/1000<<','<<d.maxRelativeQError<<','<<p.frequency<<','<<p.attenuation.referenceFrequency<<','<<p.attenuation.minFrequency<<','<<p.attenuation.maxFrequency<<','<<p.attenuation.mechanisms<<','<<(backend==BackendKind3D::Cuda?"CUDA":"CPU")<<'\n';summary.flush();
        std::cout<<c.name<<": "<<sim.step()<<" steps, "<<d.totalStepMs/1000<<" s, Q fit error "<<d.maxRelativeQError*100<<"%, PPW "<<d.ppw<<'\n';
    }
    std::cout<<"Qp/Qs controlled experiments completed: "<<folder.string()<<'\n';return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
