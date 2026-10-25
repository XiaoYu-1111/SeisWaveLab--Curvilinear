#include "Seis3D/ModelPresets3D.hpp"
#include <fstream>
#include <iostream>
#include <stdexcept>
using namespace Seis3D::Curvilinear;
namespace {
int checks=0;
void check(bool ok,const std::string& why){if(!ok)throw std::runtime_error(why);++checks;}
// Observe the real allocator at the instant the old session is destroyed.
// A replacement CUDA volume must not exist at this point.
class Observed final:public ISimulationBackend3D {
    std::unique_ptr<ISimulationBackend3D> s;BackendMemory3D& observed;
public:
    Observed(std::unique_ptr<ISimulationBackend3D> a,BackendMemory3D& m):s(std::move(a)),observed(m){}
    ~Observed(){observed=queryBackendMemory3D(s->backendInfo().kind,s->backendInfo().cudaDevice);}
    bool initialize(const SimulationParameters& p,std::string& e,const ComputeSettings3D& c) override{return s->initialize(p,e,c);}
    bool advance(int n,std::string& e)override{return s->advance(n,e);}
    bool resetFields(std::string& e)override{return s->resetFields(e);}
    bool initialized()const noexcept override{return s->initialized();}
    bool completed()const noexcept override{return s->completed();}
    int currentStep()const noexcept override{return s->currentStep();}
    int totalSteps()const noexcept override{return s->totalSteps();}
    double dt()const noexcept override{return s->dt();}
    std::uint64_t revision()const noexcept override{return s->revision();}
    BackendInfo3D backendInfo()const noexcept override{return s->backendInfo();}
    BackendMemory3D residentMemory()const noexcept override{return s->residentMemory();}
    const Grid3D& grid()const override{return s->grid();}
    const SimulationParameters& parameters()const override{return s->parameters();}
    const Diagnostics3D& diagnostics()const override{return s->diagnostics();}
    const std::vector<ReceiverTrace3D>& receivers()const override{return s->receivers();}
    bool readDisplayFrame(const DisplayRequest3D& r,DisplayFrame3D& f,std::string& e)const override{return s->readDisplayFrame(r,f,e);}
    SimulationResult snapshot()const override{return s->snapshot();}
};
}
int main(){try {
    const auto dir=std::filesystem::path(SEIS3D_OUTPUT_ROOT)/"validation/rebuild-memory";
    std::filesystem::create_directories(dir);std::ofstream log(dir/"resident.csv");
    log<<"backend,iteration,nx,ny,nz,resident_host_bytes,resident_device_bytes,free_device_before,free_device_at_release\n";
    std::unique_ptr<ISimulationBackend3D> active;std::string error;
    for(auto kind:{BackendKind3D::Cpu,BackendKind3D::Cuda}) {
        if(!queryBackend3D(kind).available)continue;
        SimulationParameters p;p.nx=88;p.ny=72;p.nz=52;p.spacing=5;p.topographyAmplitude=0;p.dominantFrequency=3;p.maxSteps=2;
        auto& d=p.surfaceGrid;d.nx=100;d.ny=88;d.spacing=5;d.datum=1500;
        for(int j=0;j<d.ny;++j)for(int i=0;i<d.nx;++i)d.elevations.push_back(1500+3*std::cos(i*.1)+2*std::sin(j*.13));
        auto result=rebuildSimulation3D(active,p,kind,{1},error);check(result.success,error);
        std::uint64_t sameHost=0,sameDevice=0;
        for(int iteration=0;iteration<6;++iteration) {
            if(iteration==2){p.nx=96;p.ny=80;}
            if(iteration==4){p.nx=88;p.ny=72;}
            BackendMemory3D atRelease;active=std::make_unique<Observed>(std::move(active),atRelease);
            const auto old=active->residentMemory();const auto free=queryBackendMemory3D(kind,active->backendInfo().cudaDevice);
            const auto budget=memoryForRebuild3D(active.get(),kind);
            if(kind==BackendKind3D::Cuda)check(budget.deviceAvailableBytes>=free.deviceAvailableBytes+old.deviceAvailableBytes-8*1024*1024,"Rebuild budget failed to recover active VRAM");
            result=rebuildSimulation3D(active,p,kind,{1},error);
            check(result.success&&result.released&&!result.restored,error);
            check(active->currentStep()==0&&active->parameters()==p,"Repeated rebuild retained fields or stale parameters");
            const auto owned=active->residentMemory();
            check(owned.hostAvailableBytes>active->grid().coordinates.size()*128,"Resident RAM omitted volume arrays");
            if(kind==BackendKind3D::Cuda) {
                check(owned.deviceAvailableBytes>active->grid().coordinates.size()*600,"Resident VRAM omitted RK buffers");
                check(atRelease.deviceAvailableBytes+64*1024*1024>=free.deviceAvailableBytes,"New CUDA volume was allocated before releasing old volume");
            }
            if(iteration==0){sameHost=owned.hostAvailableBytes;sameDevice=owned.deviceAvailableBytes;}
            if(iteration==1||iteration==5)check(owned.hostAvailableBytes==sameHost&&owned.deviceAvailableBytes==sameDevice,"Repeated equal-size rebuild leaked model allocations");
            log<<static_cast<int>(kind)<<','<<iteration<<','<<p.nx<<','<<p.ny<<','<<p.nz<<','<<owned.hostAvailableBytes<<','<<owned.deviceAvailableBytes<<','<<free.deviceAvailableBytes<<','<<atRelease.deviceAvailableBytes<<'\n';
        }
        check(active->advance(1,error),error);const auto revision=active->revision();const auto* identity=active.get();
        auto invalid=p;invalid.terrainTransitionDepth=1;
        result=rebuildSimulation3D(active,invalid,kind,{1},error);
        check(!result.success&&!result.released&&active.get()==identity&&active->revision()==revision,"Invalid geometry discarded the active session");
        invalid=p;invalid.nx=maxPhysicalNodesPerAxis3D+1;
        result=rebuildSimulation3D(active,invalid,kind,{1},error);
        check(!result.success&&!result.released&&active->revision()==revision,"Invalid grid discarded active fields");
        invalid=p;invalid.maxSteps=0;invalid.duration=10;invalid.dominantFrequency=1e6;
        result=rebuildSimulation3D(active,invalid,kind,{1},error);
        check(!result.success&&result.released&&result.restored&&!result.fallback,"Post-release failure did not restore the previous model");
        check(active->parameters()==p&&active->currentStep()==0&&error.find("t=0")!=std::string::npos,"Recovery lost parameters or failed to disclose cleared fields");
        check(active->receivers().front().samples==std::vector<double>{0},"Recovery retained stale receiver samples");
        check(active->advance(1,error)&&active->diagnostics().allFinite,"Recovered model cannot advance");
        std::cout<<"PASS "<<(kind==BackendKind3D::Cpu?"CPU":"CUDA")<<" release-before-allocation, 6 repeated/grow/shrink DEM rebuilds, no resident growth, rejection and recovery\n";
    }
    std::cout<<"Rebuild memory validation PASS: "<<checks<<" checks\n";
}catch(const std::exception& ex){std::cerr<<"FAIL: "<<ex.what()<<'\n';return 1;}}
