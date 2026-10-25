#include "Seis3D/ModelPresets3D.hpp"
#include "Seis3D/MaterialModels3D.hpp"
#include "Seis3D/AbsorbingBoundary3D.hpp"
#include "NumberFormat3D.hpp"
#include <iostream>
#include <limits>
#include <stdexcept>
using namespace Seis3D::Curvilinear;
void check(bool value,const std::string& why){if(!value)throw std::runtime_error(why);}
int main(int argc,char** argv){try {
    if(argc>1&&std::string(argv[1])=="--capacity200") {
        if(!queryBackend3D(BackendKind3D::Cuda).available)return 77;
        auto p=applyModelPreset3D(5,{});p.maxSteps=2;p.duration=.1;
        if(!fitsMemory3D(estimateModelMemory3D(p),BackendKind3D::Cuda,queryBackendMemory3D(BackendKind3D::Cuda))){std::cout<<"SKIP 200-cube: insufficient currently free RAM/VRAM\n";return 77;}
        CpuSimulation3D session(BackendKind3D::Cuda);std::string error;
        check(session.initialize(p,error)&&session.advance(2,error),error);DisplayFrame3D frame;
        check(session.readDisplayFrame({100,100,3,0},frame,error)&&frame.bytes==3ull*200*200*sizeof(float)&&session.diagnostics().allFinite,"200-cube failed finite/compact-slice checks");
        check(session.resetFields(error)&&session.currentStep()==0,"200-cube in-place clear failed");
        std::cout<<"PASS actual CUDA 200 x 200 x 200: two steps, dt="<<session.dt()*1000<<" ms, compact slices and in-place clear\n";return 0;
    }
    using Seis3D::Curvilinear::Ui::formatMeasurement3D;
    check(formatMeasurement3D(1990)=="1990"&&formatMeasurement3D(2000)=="2000"&&formatMeasurement3D(1990.125)=="1990.125"&&formatMeasurement3D(-20)=="-20","Coordinate formatter rounded large values or used scientific notation");
    constexpr std::uint64_t gib=1024ull*1024*1024;
    const BackendMemory3D memory{64*gib,8*gib};SimulationParameters seed;
    for(int n=4;n<7;++n){const auto p=applyModelPreset3D(n,seed);const auto m=estimateModelMemory3D(p);check(p.nx==100*(n-3)&&p.nx==p.ny&&p.ny==p.nz&&m.withinBudget,"Cubic preset missing or exceeds logical grid limit");}
    auto largest=applyModelPreset3D(6,seed);const auto largeMemory=estimateModelMemory3D(largest);
    check(largeMemory.points==306ull*306*306&&!fitsMemory3D(largeMemory,BackendKind3D::Cuda,memory),"300-cube capacity estimate ignored ghosts or exceeded 8 GiB VRAM");
    SimulationParameters fitted;check(fitModelToMemory3D(largest,BackendKind3D::Cuda,memory,fitted),"Explicit grid fitting failed");
    check(fitted.nx==fitted.ny&&fitted.ny==fitted.nz&&fitted.nx<300&&fitsMemory3D(estimateModelMemory3D(fitted),BackendKind3D::Cuda,memory),"Fit changed proportions or exceeded memory");
    std::cout<<"300-cube VRAM estimate="<<largeMemory.deviceMiB/1024<<" GiB; explicit fit for 8 GiB="<<fitted.nx<<" cubed\n";
    SimulationParameters p;p.nx=16;p.ny=12;p.nz=20;p.topographyAmplitude=0;p.duration=.213;p.maxSteps=0;p.absorbingBoundary.enabled=false;
    CpuSimulation3D s;std::string error;double last=0;
    for(int test=0;test<8;++test) {
        if(test==1)p.spacing=5;
        if(test==2)p.vp=6000;
        if(test==3)p.dominantFrequency=200;
        if(test==4){p=applyMaterialPreset3D(1,p);p.dominantFrequency=12;p.spacing=10;}
        if(test==5){p.absorbingBoundary.enabled=true;p.ny=8;}
        if(test==6)p.ny=30;
        if(test==7){p.maxSteps=3;p.duration=.183;}
        check(s.initialize(p,error,{1}),error);
        const double vp=p.layeredMaterial?4000:p.vp;
        const auto widths=absorbingWidths3D(p);double damping=0;for(int width:widths)if(width>0)damping+=p.absorbingBoundary.strength*vp/(width*p.spacing);
        const double wave=p.cflSafety*p.spacing/(std::sqrt(3.0)*vp),source=1/(50*p.dominantFrequency),sponge=damping>0?.5/damping:std::numeric_limits<double>::max();
        const double expected=p.duration/std::ceil(p.duration/std::min({wave,source,sponge}));
        check(std::abs(s.dt()-expected)<1e-14,"Reinitialization retained an old dt or failed flat-grid CFL/source/damping formula");
        if(test>0&&test<7)check(s.dt()!=last,"Parameter change failed to update dt in a ceiling-changing case");
        last=s.dt();check(s.advance(std::min(4,s.totalSteps()),error),error);
    }
    p.layeredMaterial=false;p.layers.clear();p.maxSteps=0;p.vp=3000;p.vs=1700;p.absorbingBoundary.enabled=false;p.topographyAmplitude=12;p.topographyWavelength=120;
    for(int depth:{20,40}){p.nz=depth;check(s.initialize(p,error,{1}),error);CpuSimulation3D fresh;check(fresh.initialize(p,error,{1}),error);check(s.dt()==fresh.dt()&&s.grid().terrainTransitionDepth==fresh.grid().terrainTransitionDepth,"Terrain rebuild reused metrics/flatten depth from old grid");}
    for(auto kind:{BackendKind3D::Cpu,BackendKind3D::Cuda}) {
        if(!queryBackend3D(kind).available)continue;
        CpuSimulation3D session(kind);p.topographyAmplitude=0;p.maxSteps=5;
        check(session.initialize(p,error,{1})&&session.advance(4,error),error);
        auto memoryParameters=p;memoryParameters.maxSteps=session.totalSteps();const auto activeMemory=estimateModelMemory3D(memoryParameters);
        const double expectedMemory=kind==BackendKind3D::Cuda?activeMemory.cudaHostMiB+activeMemory.deviceMiB:activeMemory.cpuMiB;
        check(std::abs(session.diagnostics().estimatedMemoryMiB-expectedMemory)<1e-12,"Active memory diagnostics use a different backend or stale step count");
        const double dt=session.dt();const auto revision=session.revision();const auto coordinates=session.grid().coordinates.data();
        check(session.resetFields(error),error);check(session.currentStep()==0&&session.dt()==dt&&session.grid().coordinates.data()==coordinates&&session.revision()!=revision&&session.diagnostics().timing.measuredSteps==0,"Clear rebuilt geometry, changed dt or retained timing");
        for(const auto& r:session.receivers())check(r.samples==std::vector<double>{0}&&r.samplesVx==r.samples&&r.samplesVy==r.samples,"Clear retained receiver samples");
        CpuSimulation3D fresh(kind);check(fresh.initialize(p,error,{1})&&session.advance(2,error)&&fresh.advance(2,error),error);
        DisplayFrame3D a,b;check(session.readDisplayFrame({8,6,3,0},a,error)&&fresh.readDisplayFrame({8,6,3,0},b,error),error);for(int plane=0;plane<3;++plane)check(a.planes[plane].values==b.planes[plane].values,"Restart after clear differs from a fresh session");
        const auto saved=session.revision();auto bad=p;bad.nx=maxPhysicalNodesPerAxis3D+1;check(!session.initialize(bad,error)&&session.revision()==saved&&session.dt()==dt,"Rejected edit changed live dt/session");
        if(kind==BackendKind3D::Cuda&&!fitsMemory3D(largeMemory,kind,queryBackendMemory3D(kind)))check(!session.initialize(largest,error)&&session.revision()==saved,"Oversized CUDA request allocated or discarded the current session");
    }
    std::cout<<"PASS formatting, 100/200/300 presets, memory preflight/fit, eight dt changes, terrain rebuild and CPU/CUDA in-place clear\n";
}catch(const std::exception& ex){std::cerr<<"FAIL: "<<ex.what()<<'\n';return 1;}}
