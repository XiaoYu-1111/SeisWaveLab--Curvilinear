#include "Seis2D/CurvilinearSolver2D.hpp"
#include "Seis2D/ModelIo2D.hpp"
#include "CfsPml2D.hpp"
#include "MemoryBudget2D.hpp"
#include <algorithm>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <stdexcept>
using namespace Seis2D::Curvilinear;
namespace {
void check(bool v,const std::string& e){if(!v)throw std::runtime_error(e);}
void compare(const std::vector<double>& a,const std::vector<double>& b){
    check(a.size()==b.size(),"Array size mismatch");double peak=0,error=0;
    for(std::size_t q=0;q<a.size();++q){check(std::isfinite(a[q])&&std::isfinite(b[q]),"Nonfinite comparison");peak=std::max(peak,std::abs(a[q]));error=std::max(error,std::abs(a[q]-b[q]));}
    check(error<1e-8*std::max(peak,1e-25),"CPU/CUDA/reset mismatch: "+std::to_string(error/std::max(peak,1e-25)));
}
void compare(const Snapshot& a,const Snapshot& b){for(int f=0;f<5;++f)compare(a.fields[f],b.fields[f]);compare(a.syy,b.syy);check(a.memory.size()==b.memory.size(),"GSLS counts differ");for(std::size_t l=0;l<a.memory.size();++l)compare(a.memory[l],b.memory[l]);compare(a.boundaryMemory,b.boundaryMemory);}
Parameters fixture(){Parameters p;p.nx=80;p.nz=72;p.spacing=2;p.vp=550;p.vs=250;p.density=1550;p.relief=1;p.wavelength=80;p.frequency=20;p.duration=.35;p.maxSteps=0;p.cfl=.5;p.sourceX=40;p.sourceZ=8;p.receiverStart=16;p.receiverCount=8;p.receiverStride=6;p.receiverDepth=1;p.spongeX=p.spongeZ=16;p.absorber=Absorber::CfsPml;p.sampleIntervalMs=.5;p.forcePerLength=100;return p;}
void regression(const std::filesystem::path& folder){
    std::string e;ComputeSettings3D settings;settings.cpuThreads=1;
    const auto availability=Seis3D::Curvilinear::selectCudaDevice3D(Seis3D::Curvilinear::queryCudaDevices3D());
    for(bool q:{false,true}){
        auto p=fixture();p.attenuation.enabled=q;p.attenuation.referenceFrequency=20;p.attenuation.maxFrequency=80;
        Simulation cpu;check(cpu.initialize(p,e,BackendKind3D::Cpu,settings),e);check(cpu.diagnostics().cpmlScalars==Detail::cpmlScalars(p),"Compact memory count mismatch");
        check(cpu.advance(cpu.totalSteps(),e),e);check(cpu.diagnostics().tractionRelative<1e-9,"CFS-PML traction closure failed");const auto expected=cpu.snapshot(true);
        check(std::any_of(expected.boundaryMemory.begin(),expected.boundaryMemory.end(),[](double a){return a!=0;}),"PML memories never driven");
        auto invalid=p;invalid.cpml.targetReflection=0;check(!cpu.initialize(invalid,e),"Invalid PML accepted");compare(expected,cpu.snapshot(true));
        check(cpu.reset(e)&&cpu.advance(cpu.totalSteps(),e),e);compare(expected,cpu.snapshot(true));
        check(writeModelVts(cpu,folder/(q?"q_pml.vts":"elastic_pml.vts"),settings,e),e);ModelDocument document;
        check(readModelVts(folder/(q?"q_pml.vts":"elastic_pml.vts"),document,e),e);check(document.parameters==p,"PML/GSLS model roundtrip failed");
        if(availability.available){Simulation gpu;check(gpu.initialize(p,e,BackendKind3D::Cuda),e);check(gpu.advance(gpu.totalSteps(),e),e);compare(expected,gpu.snapshot(true));
            check(gpu.reset(e)&&gpu.advance(gpu.totalSteps(),e),e);compare(expected,gpu.snapshot(true));}
        std::cout<<"PASS CPU / "<<(availability.available?"CUDA":"CUDA unavailable")<<" / reset / VTS / curved free surface / GSLS="<<q<<std::endl;
    }
    {
        auto p=fixture();p.nx=160;p.nz=128;p.sourceX=80;
        p.source.kind=SeisWave::SourceKind::IsotropicMoment;
        Simulation serial;check(serial.initialize(p,e,BackendKind3D::Cpu,settings)&&serial.advance(serial.totalSteps(),e),e);
        const auto expected=serial.snapshot(true);auto workers=settings;workers.cpuThreads=4;
        Simulation parallel;check(parallel.initialize(p,e,BackendKind3D::Cpu,workers)&&parallel.advance(parallel.totalSteps(),e),e);
        compare(expected,parallel.snapshot(true));
        if(availability.available){Simulation gpu;check(gpu.initialize(p,e,BackendKind3D::Cuda)&&gpu.advance(gpu.totalSteps(),e),e);compare(expected,gpu.snapshot(true));}
        std::cout<<"PASS PML moment source / serial vs 4 workers / CUDA"<<std::endl;
    }
    for(bool q:{false,true})for(auto backend:{BackendKind3D::Cpu,BackendKind3D::Cuda}){
        if(backend==BackendKind3D::Cuda&&!availability.available)continue;
        auto p=fixture();p.nx=48;p.nz=40;p.spongeX=p.spongeZ=10;p.sourceX=24;p.receiverCount=1;p.receiverStart=15;p.duration=5;p.attenuation.enabled=q;p.attenuation.referenceFrequency=20;p.attenuation.maxFrequency=80;
        Simulation s;check(s.initialize(p,e,backend,settings)&&s.advance(s.totalSteps(),e),e);
        check(s.diagnostics().finite&&s.diagnostics().energy<s.diagnostics().peakEnergy*1e-3,"PML long-tail growth / retained energy");
        std::cout<<"PASS 5s long tail backend="<<static_cast<int>(backend)<<" GSLS="<<q<<" energy_ratio="<<s.diagnostics().energy/s.diagnostics().peakEnergy<<std::endl;
    }
    auto p=fixture();p.absorbing=false;Simulation off;check(off.initialize(p,e),e);check(off.diagnostics().cpmlScalars==0,"Disabled PML allocated memories");check(off.advance(20,e),e);const auto expected=off.snapshot(true);p.absorber=Absorber::Sponge;Simulation legacy;check(legacy.initialize(p,e)&&legacy.advance(20,e),e);compare(expected,legacy.snapshot(true));
    auto profile=Detail::cpmlProfile(80,16,2,550,1e-4,3,20,true);check(profile[3*40]==0&&profile[3*40+1]==1&&profile[3*40+2]==0,"PML interior not identity");
    auto cp=fixture();auto sp=cp;sp.absorber=Absorber::Sponge;check(Detail::memoryBudget(cp,BackendKind3D::Cpu).hostBytes>Detail::memoryBudget(sp,BackendKind3D::Cpu).hostBytes,"PML memory missing in budget");
}
struct Trace {std::vector<double> vz;double dt{},time{},memory{},traction{},energy{};};
void externalRegression(const std::filesystem::path& folder){
    std::string e;ComputeSettings3D settings;settings.cpuThreads=1;
    const auto availability=Seis3D::Curvilinear::selectCudaDevice3D(Seis3D::Curvilinear::queryCudaDevices3D());
    std::ofstream receipt(folder/"external_absorber_verification.csv");
    receipt<<"absorber,GSLS,model_nx,model_nz,compute_nx,compute_nz,offset_x,receiver_first_x_m,receiver_last_x_m,cuda_checked\n";
    for(auto absorber:{Absorber::Sponge,Absorber::CfsPml})for(bool q:{false,true}){
        auto p=fixture();p.relief=0;p.externalAbsorption=true;p.absorber=absorber;p.duration=.2;
        p.receiverStart=0;p.receiverCount=p.nx;p.receiverStride=1;p.receiverDepth=0;
        p.attenuation.enabled=q;p.attenuation.referenceFrequency=20;p.attenuation.maxFrequency=80;
        Simulation s;check(s.initialize(p,e,BackendKind3D::Cpu,settings),e);
        const auto d=Detail::domainLayout(p);const auto& g=s.grid();
        check(g.nx==d.nx+2*ghost&&g.nz==d.nz+2*ghost&&g.modelOffsetX==16,"External computational extent mismatch");
        check(s.parameters()==p,"External padding changed user parameters");
        check(g.coordinates[g.modelIndex(0,0)][0]==0&&g.coordinates[g.modelIndex(p.nx-1,0)][0]==158,"Logical X span changed");
        check(g.coordinates[g.modelIndex(0,p.nz-1)][1]==142,"Logical bottom changed");
        check(s.receivers().front().x==0&&s.receivers().back().x==79&&s.receivers().front().position[0]==0&&s.receivers().back().position[0]==158,"Receiver positions shifted or cropped");
        check(s.diagnostics().cpmlScalars==Detail::cpmlScalars(p),"External PML memory count mismatch");
        if(absorber==Absorber::CfsPml){const auto x=Detail::cpmlProfile(d.nx,d.widthX,p.spacing,550,1e-4,3,20,true);
            const auto z=Detail::cpmlProfile(d.nz,d.widthZ,p.spacing,550,1e-4,3,20,false);
            for(int i=0;i<p.nx;++i)check(x[3*(i+d.offsetX)]==0&&x[3*(i+d.offsetX)+1]==1,"PML modified target X");
            for(int k=0;k<p.nz;++k)check(z[3*k]==0&&z[3*k+1]==1,"PML modified target Z");}
        check(s.advance(s.totalSteps(),e),e);const auto expected=s.snapshot(true);
        // Independent manual expanded flat-domain setup must give exactly the same fields/records.
        auto manual=p;manual.externalAbsorption=false;manual.nx=d.nx;manual.nz=d.nz;
        manual.sourceX+=d.offsetX;manual.receiverStart+=d.offsetX;manual.flattenDepth=g.flattenDepth;
        Simulation expanded;check(expanded.initialize(manual,e,BackendKind3D::Cpu,settings)&&expanded.advance(expanded.totalSteps(),e),e);
        compare(expected,expanded.snapshot(true));
        for(std::size_t i=0;i<s.receivers().size();++i){compare(s.receivers()[i].vz,expanded.receivers()[i].vz);compare(s.receivers()[i].vx,expanded.receivers()[i].vx);}
        std::vector<float> frame;check(s.readFrame(0,frame,e)&&frame.size()==static_cast<std::size_t>(p.nx)*p.nz,"Logical frame crop dimensions failed");
        for(int k=0;k<p.nz;++k)for(int i=0;i<p.nx;++i)check(frame[i+p.nx*k]==static_cast<float>(expected.fields[1][g.modelIndex(i,k)]),"Logical frame crop selected wrong fields");
        const auto suffix=std::to_string(static_cast<int>(absorber))+"_"+std::to_string(q);
        const auto path=folder/("external_model_"+suffix+".vts");
        check(writeModelVts(s,path,settings,e),e);ModelDocument document;check(readModelVts(path,document,e)&&document.parameters==p,e);
        Simulation restored;check(restored.initialize(document.parameters,e,BackendKind3D::Cpu,settings)&&restored.advance(restored.totalSteps(),e),e);compare(expected,restored.snapshot(true));
        check(writeSnapshot(s,folder/("external_wavefield_"+suffix+".vts"),e)&&writeReceivers(s,folder/("external_receivers_"+suffix+".csv"),e),e);
        if(availability.available){Simulation gpu;check(gpu.initialize(p,e,BackendKind3D::Cuda)&&gpu.advance(gpu.totalSteps(),e),e);compare(expected,gpu.snapshot(true));
            std::vector<float> values;check(gpu.readFrame(0,values,e)&&values.size()==frame.size(),e);double error=0,peak=1e-25;
            for(std::size_t i=0;i<frame.size();++i){error=std::max(error,std::abs(double(values[i])-frame[i]));peak=std::max(peak,std::abs(double(frame[i])));}check(error<1e-6*peak,"External CUDA frame crop differs");
            for(std::size_t i=0;i<s.receivers().size();++i){compare(s.receivers()[i].vz,gpu.receivers()[i].vz);compare(s.receivers()[i].vx,gpu.receivers()[i].vx);}}
        check(s.reset(e)&&s.advance(s.totalSteps(),e),e);compare(expected,s.snapshot(true));
        auto invalid=p;invalid.nx=maxAxisNodes;check(!s.initialize(invalid,e),"Oversized external grid accepted");compare(expected,s.snapshot(true));
        receipt<<static_cast<int>(absorber)<<','<<q<<','<<p.nx<<','<<p.nz<<','<<d.nx<<','<<d.nz<<','<<d.offsetX<<",0,158,"<<availability.available<<'\n';
        std::cout<<"PASS external absorber="<<static_cast<int>(absorber)<<" GSLS="<<q<<" / manual expanded reference / full-width receivers / CPU-CUDA / schema4 / crop / reset"<<std::endl;
    }
    auto p=fixture();p.externalAbsorption=true;p.importedSurface=true;p.surfaceDatum=1000;
    p.attenuation.enabled=true;p.attenuation.referenceFrequency=20;p.attenuation.maxFrequency=80;
    p.layered=true;p.layers={{150,550,250,1550,100,60},{0,900,450,2000,120,70}};
    p.surfaceSamples={{0,1000},{40,1001},{90,999},{158,1000}};
    auto baseline=p;baseline.absorbing=false;
    Simulation terrain,plain;check(terrain.initialize(p,e,BackendKind3D::Cpu,settings)&&plain.initialize(baseline,e,BackendKind3D::Cpu,settings),e);
    check(terrain.grid().flattenDepth==plain.grid().flattenDepth,"Padding changed automatic flatten depth");
    for(int k=p.nz;k<terrain.grid().nz-2*ghost;++k)for(int i=0;i<p.nx;++i)
        check(terrain.grid().vp[terrain.grid().modelIndex(i,k)]==550&&terrain.grid().qp[terrain.grid().modelIndex(i,k)]==100,"Bottom strip introduced a deeper unmodelled layer/Q");
    for(int k=0;k<p.nz;++k)for(int i=0;i<p.nx;++i){const auto a=terrain.grid().modelIndex(i,k),b=plain.grid().modelIndex(i,k);
        check(terrain.grid().coordinates[a]==plain.grid().coordinates[b]&&terrain.grid().metricA[a]==plain.grid().metricA[b]&&terrain.grid().metricB[a]==plain.grid().metricB[b],"DEM target geometry changed with external padding");}
    check(terrain.advance(terrain.totalSteps(),e)&&terrain.diagnostics().finite&&terrain.diagnostics().tractionRelative<1e-9,e);
    if(availability.available){Simulation gpu;check(gpu.initialize(p,e,BackendKind3D::Cuda)&&gpu.advance(gpu.totalSteps(),e),e);compare(terrain.snapshot(true),gpu.snapshot(true));}
    check(Detail::memoryBudget(p,BackendKind3D::Cpu).hostBytes>Detail::memoryBudget(baseline,BackendKind3D::Cpu).hostBytes,"External grid missing in RAM budget");
    check(!Detail::acquisitionInAbsorber(p),"External receiver falsely flagged");
    p.externalAbsorption=false;p.receiverStart=0;check(Detail::acquisitionInAbsorber(p),"Legacy acquisition warning missing");
    auto steep=fixture();steep.externalAbsorption=true;steep.relief=5;steep.wavelength=8;steep.flattenDepth=14;
    Simulation edge;check(edge.initialize(steep,e,BackendKind3D::Cpu,settings)&&edge.grid().minJacobian>0,"Steep edge continuation invalidated the grid: "+e);
    std::cout<<"PASS DEM-only logical coverage / unchanged logical geometry / outer slope continuation / placement warning / memory budget\n";
}
Trace run(Parameters p,BackendKind3D backend){Simulation s;std::string e;ComputeSettings3D c;c.cpuThreads=1;check(s.initialize(p,e,backend,c),e);check(s.advance(s.totalSteps(),e),e);check(s.diagnostics().finite&&s.completed(),"Incomplete/nonfinite run");Trace r;r.vz=s.receivers().front().vz;r.dt=s.recordDt();r.time=s.diagnostics().totalStepMs/1000;r.memory=s.diagnostics().cpmlMemoryMiB;r.traction=s.diagnostics().tractionRelative;r.energy=s.diagnostics().energy;return r;}
void audit(const std::filesystem::path& folder){
    const auto availability=Seis3D::Curvilinear::selectCudaDevice3D(Seis3D::Curvilinear::queryCudaDevices3D());const auto backend=availability.available?BackendKind3D::Cuda:BackendKind3D::Cpu;
    std::ofstream csv(folder/"boundary_comparison.csv");csv<<"scenario,boundary,width_cells,peak_residual_ratio,l2_residual_ratio,seconds,pml_MiB,traction_relative,final_energy\n"<<std::setprecision(17);
    for(int scenario=0;scenario<7;++scenario){
        Parameters p;p.nx=241;p.nz=161;p.spacing=.5;p.vp=550;p.vs=250;p.density=1550;p.relief=(scenario==2||scenario==5)?2:0;p.wavelength=80;p.flattenDepth=28;p.frequency=(scenario==0||scenario==4)?15:30;p.duration=.65;p.maxSteps=0;p.cfl=.6;p.sourceX=120;p.sourceZ=scenario==3?30:2;p.forcePerLength=100;p.receiverStart=150;p.receiverCount=1;p.receiverStride=1;p.receiverDepth=scenario==3?30:0;p.sampleIntervalMs=.5;p.spongeX=p.spongeZ=32;
        if(scenario==3)p.source.direction={1,0,0};
        if(scenario==4||scenario==5){p.layered=true;p.layers={{10,550,250,1550,80,50},{0,900,450,2000,120,70}};}
        if(scenario==5){p.attenuation.enabled=true;p.attenuation.referenceFrequency=30;p.attenuation.maxFrequency=80;}
        if(scenario==6){p.sourceX=190;p.sourceZ=100;p.receiverStart=170;p.receiverDepth=90;}
        // Expanded on both sides and bottom: no high-speed body return in the observation window.
        const int margin=480; // Exactly 3 terrain wavelengths; freeze flattenDepth in both domains.
        auto reference=p;reference.nx+=2*margin;reference.nz+=margin;reference.sourceX+=margin;reference.receiverStart+=margin;reference.absorber=Absorber::CfsPml;reference.spongeX=reference.spongeZ=64;
        const auto ref=run(reference,backend);double incident=0,incidentEnergy=0;
        for(std::size_t i=0;i<ref.vz.size();++i)if(i*ref.dt>=.04&&i*ref.dt<=.22){incident=std::max(incident,std::abs(ref.vz[i]));incidentEnergy+=ref.vz[i]*ref.vz[i];}
        check(incident>1e-20,"Missing incident amplitude");
        double spongePeak=0;
        for(int kind=0;kind<3;++kind){auto test=p;test.absorbing=kind!=0;test.absorber=kind==2?Absorber::CfsPml:Absorber::Sponge;
            const auto result=run(test,backend);check(result.vz.size()==ref.vz.size()&&result.dt==ref.dt,"Record axes mismatch");double peak=0,sum=0;
            std::ofstream traces(folder/("trace_"+std::to_string(scenario)+"_"+std::to_string(kind)+".csv"));traces<<"time_s,small_vz_m_s,reference_vz_m_s,residual_m_s\n"<<std::setprecision(17);
            for(std::size_t i=0;i<ref.vz.size();++i){const double residual=result.vz[i]-ref.vz[i];traces<<i*ref.dt<<','<<result.vz[i]<<','<<ref.vz[i]<<','<<residual<<'\n';if(i*ref.dt>=(scenario==6?.08:.18)){peak=std::max(peak,std::abs(residual));sum+=residual*residual;}}
            if(kind==1)spongePeak=peak/incident;
            if(kind==2){check(peak/incident<1e-3&&std::sqrt(sum/incidentEnergy)<1e-3,"CFS-PML benchmark exceeds 0.1% residual gate");check(peak/incident<spongePeak/4,"CFS-PML does not improve over same-width sponge by 4x");}
            csv<<scenario<<','<<(kind==0?"off":kind==1?"sponge":"cfs_pml")<<','<<p.spongeX<<','<<peak/incident<<','<<std::sqrt(sum/incidentEnergy)<<','<<result.time<<','<<result.memory<<','<<result.traction<<','<<result.energy<<'\n';csv.flush();
            std::cout<<"AUDIT "<<scenario<<" boundary="<<kind<<" peak="<<peak/incident<<" L2="<<std::sqrt(sum/incidentEnergy)<<std::endl;
        }
    }
}
}
int main(int argc,char** argv){try{const auto folder=std::filesystem::path(SEIS3D_OUTPUT_ROOT)/"validation/cfs-pml-2d";std::filesystem::create_directories(folder);if(argc>1&&std::string(argv[1])=="--audit")audit(folder);else if(argc>1&&std::string(argv[1])=="--external")externalRegression(folder);else regression(folder);std::cout<<"CFS-PML validation PASS\n";return 0;}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
