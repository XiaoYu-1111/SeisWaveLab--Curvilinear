#include "Seis2D/CurvilinearSolver2D.hpp"
#include "Seis2D/ModelIo2D.hpp"
#include "Seis2D/AsyncSimulation2D.hpp"
#include "MemoryBudget2D.hpp"
#include <algorithm>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <stdexcept>
#include <string_view>

using namespace Seis2D::Curvilinear;
namespace {
void check(bool condition,const std::string& message){if(!condition)throw std::runtime_error(message);}
std::string read(const std::filesystem::path& path){std::ifstream in(path);return {std::istreambuf_iterator<char>(in),{}};}
void replace(std::string& text,const std::string& from,const std::string& to){const auto at=text.find(from);check(at!=std::string::npos,"Missing corruption fixture: "+from);text.replace(at,from.size(),to);}
struct Error {double l2{},peak{};};
Error difference(const std::vector<double>& ref,const std::vector<double>& value,std::size_t begin=0,std::size_t end=SIZE_MAX){
    check(ref.size()==value.size(),"Comparison length differs");end=std::min(end,ref.size());
    double norm=0,squared=0,peak=0,error=0;
    for(std::size_t i=begin;i<end;++i){check(std::isfinite(ref[i])&&std::isfinite(value[i]),"Nonfinite comparison");norm+=ref[i]*ref[i];squared+=(ref[i]-value[i])*(ref[i]-value[i]);peak=std::max(peak,std::abs(ref[i]));error=std::max(error,std::abs(ref[i]-value[i]));}
    return {std::sqrt(squared/std::max(norm,1e-100)),error/std::max(peak,1e-50)};
}
Parameters fixture(){
    Parameters p;p.nx=80;p.nz=72;p.spacing=1;p.vp=550;p.vs=250;p.density=1550;p.relief=.5;p.wavelength=80;
    p.frequency=20;p.duration=.35;p.maxSteps=0;p.cfl=.5;p.sourceX=40;p.sourceZ=8;p.forcePerLength=100;
    p.receiverStart=0;p.receiverCount=16;p.receiverStride=5;p.receiverDepth=1;p.spongeX=p.spongeZ=12;p.externalAbsorption=true;
    p.sampleIntervalMs=.25;p.attenuation.referenceFrequency=20;p.attenuation.minFrequency=5;p.attenuation.maxFrequency=60;p.attenuation.qp=40;p.attenuation.qs=25;
    return p;
}
struct Result {Snapshot snapshot;std::vector<double> records;Diagnostics diagnostics;double dt{};std::vector<Receiver> receivers;};
Result run(Parameters p,BackendKind3D backend,bool verifyReset=false){
    ComputeSettings3D settings;settings.cpuThreads=1;std::string error;Simulation s;
    check(s.initialize(p,error,backend,settings),error);check(s.grid().vp.precision()==p.precision&&s.grid().coordinates.precision()==p.precision&&s.grid().normals.precision()==p.precision&&s.grid().metricA.precision()==p.precision&&s.grid().jacobian.precision()==p.precision&&s.grid().density.precision()==p.precision&&s.grid().vs.precision()==p.precision,"Model precision differs from selected field precision");
    check(s.advance(s.totalSteps(),error),error);check(s.diagnostics().finite,"Nonfinite diagnostics");
    check(s.diagnostics().tractionRelative<(p.precision==Precision::Float32?5e-6:1e-9),"Free-surface traction error exceeds precision tolerance");
    Result r;r.snapshot=s.snapshot(true);r.diagnostics=s.diagnostics();r.dt=s.dt();r.receivers=s.receivers();
    for(const auto& rec:r.receivers){r.records.insert(r.records.end(),rec.vz.begin(),rec.vz.end());r.records.insert(r.records.end(),rec.vx.begin(),rec.vx.end());}
    if(verifyReset){check(s.reset(error)&&s.step()==0&&s.advance(s.totalSteps(),error),error);const auto again=s.snapshot(true);check(again.fields==r.snapshot.fields&&again.syy==r.snapshot.syy&&again.memory==r.snapshot.memory&&again.boundaryMemory==r.snapshot.boundaryMemory,"Reset failed bitwise reproducibility within selected precision");
        auto bad=p;bad.precision=static_cast<Precision>(99);check(!s.initialize(bad,error),"Invalid precision accepted");check(s.snapshot(true).fields==again.fields,"Failed rebuild destroyed active state");}
    return r;
}
void compare(const std::string& name,const std::string& pair,const Result& ref,const Result& value,std::ostream& csv){
    check(ref.dt==value.dt,"Precision change modified FP64 time-step plan");
    const auto metric=[&](const std::string& component,const auto& a,const auto& b){const auto e=difference(a,b);csv<<name<<','<<pair<<','<<component<<','<<e.l2<<','<<e.peak<<'\n';check(e.l2<=1e-3&&e.peak<=3e-3,name+" "+pair+" "+component+" FP32 error too large: "+std::to_string(e.l2));};
    constexpr const char* names[]{"Vx","Vz","Sxx","Szz","Sxz"};for(int f=0;f<5;++f)metric(names[f],ref.snapshot.fields[f],value.snapshot.fields[f]);
    metric("Syy",ref.snapshot.syy,value.snapshot.syy);check(ref.snapshot.memory.size()==value.snapshot.memory.size(),"GSLS branch mismatch");
    for(std::size_t l=0;l<ref.snapshot.memory.size();++l)metric("GSLS"+std::to_string(l),ref.snapshot.memory[l],value.snapshot.memory[l]);
    metric("PML",ref.snapshot.boundaryMemory,value.snapshot.boundaryMemory);metric("raw_records",ref.records,value.records);
    for(int window=0;window<2;++window){std::vector<double> a,b;for(std::size_t rec=0;rec<ref.receivers.size();++rec)for(int c=0;c<2;++c){const auto& x=c?ref.receivers[rec].vx:ref.receivers[rec].vz;const auto& y=c?value.receivers[rec].vx:value.receivers[rec].vz;const auto middle=x.size()/2;
            a.insert(a.end(),x.begin()+(window?middle:0),x.begin()+(window?x.size():middle));b.insert(b.end(),y.begin()+(window?middle:0),y.begin()+(window?y.size():middle));}
        metric(window?"late_raw_records":"early_raw_records",a,b);}
}
void modelIo(const std::filesystem::path& folder){
    auto p=fixture();p.precision=Precision::Float32;p.absorber=Absorber::CfsPml;p.attenuation.enabled=true;p.duration=.03;
    std::string e;Simulation a;ComputeSettings3D settings;settings.cpuThreads=1;check(a.initialize(p,e,BackendKind3D::Cpu,settings)&&a.advance(25,e),e);
    const auto path=folder/"fp32_model.vts";check(writeModelVts(a,path,settings,e),e);auto text=read(path);check(text.find("type=\"Float32\" Name=\"Vp\"")!=std::string::npos,"FP32 model exported as Float64");
    ModelDocument doc;check(readModelVts(path,doc,e)&&doc.parameters==p,e);Simulation b;check(b.initialize(doc.parameters,e,BackendKind3D::Cpu,settings)&&b.step()==0&&b.advance(25,e),e);
    check(a.grid().coordinates==b.grid().coordinates&&a.grid().vp==b.grid().vp&&a.snapshot(true).fields==b.snapshot(true).fields,"FP32 VTS roundtrip not exact");
    const auto saved=doc.parameters;replace(text,"Name=\"CalculationPrecision\" NumberOfComponents=\"1\" NumberOfTuples=\"1\" format=\"ascii\">1 ","Name=\"CalculationPrecision\" NumberOfComponents=\"1\" NumberOfTuples=\"1\" format=\"ascii\">9 ");
    const auto bad=folder/"invalid_precision.vts";std::ofstream(bad)<<text;check(!readModelVts(bad,doc,e)&&doc.parameters==saved,"Bad precision accepted or document changed");
    text=read(path);replace(text,"type=\"Float32\" Name=\"Vp\" format=\"ascii\">\n","type=\"Float32\" Name=\"Vp\" format=\"ascii\">\n999 ");std::ofstream(bad)<<text;check(!readModelVts(bad,doc,e),"Changed FP32 model passed checksum");
    check(writeSnapshot(a,folder/"fp32_wavefield.vts",e)&&writeReceivers(a,folder/"fp32_records.csv",e),e);
    check(read(folder/"fp32_wavefield.vts").find("type=\"Float32\" Name=\"Vz\"")!=std::string::npos&&read(folder/"fp32_records.csv").find("calculation_precision=FP32")!=std::string::npos,"Export lost precision provenance");
    p.precision=Precision::Float64;check(a.initialize(p,e,BackendKind3D::Cpu,settings)&&a.step()==0&&a.grid().vp.precision()==Precision::Float64,e);
    std::cout<<"PASS model schema 5, FP32 arrays/checksum/provenance, reset/rebuild precision\n";
}
void longTail(const std::filesystem::path& folder){
    auto p=fixture();p.nx=48;p.nz=40;p.sourceX=24;p.receiverCount=4;p.receiverStride=10;p.duration=5;p.absorber=Absorber::CfsPml;p.attenuation.enabled=true;
    std::string e;Simulation ref,value;check(ref.initialize(p,e,BackendKind3D::Cuda),e);p.precision=Precision::Float32;check(value.initialize(p,e,BackendKind3D::Cuda),e);
    std::ofstream out(folder/"long_tail.csv");out<<std::setprecision(17)<<"time_s,component,relative_l2,relative_peak,reference_peak,error_peak,reference_energy_ratio,fp32_energy_ratio\n";
    std::array<double,5> signalPeak{},errorPeak{};int checkpoint=0;
    while(!ref.completed()){
        const int steps=std::min(ref.totalSteps()-ref.step(),std::max(1,int(.1/ref.dt())));check(ref.advance(steps,e)&&value.advance(steps,e),e);
        const auto a=ref.snapshot(true),b=value.snapshot(true);constexpr const char* labels[]{"Vx","Vz","Sxx","Szz","Sxz"};
        for(int f=0;f<5;++f){double peak=0,error=0;for(std::size_t q=0;q<a.fields[f].size();++q){peak=std::max(peak,std::abs(a.fields[f][q]));error=std::max(error,std::abs(a.fields[f][q]-b.fields[f][q]));}signalPeak[f]=std::max(signalPeak[f],peak);errorPeak[f]=std::max(errorPeak[f],error);
            const auto d=difference(a.fields[f],b.fields[f]);out<<ref.step()*ref.dt()<<','<<labels[f]<<','<<d.l2<<','<<d.peak<<','<<peak<<','<<error<<','<<ref.diagnostics().energy/ref.diagnostics().peakEnergy<<','<<value.diagnostics().energy/value.diagnostics().peakEnergy<<'\n';}
        ++checkpoint;
    }
    for(int f=0;f<5;++f)check(errorPeak[f]<1e-4*signalPeak[f],"FP32 tail absolute error exceeds 0.01% of main signal");
    check(value.diagnostics().finite&&value.diagnostics().energy<value.diagnostics().peakEnergy*1e-3,"FP32 tail energy growth");
    std::cout<<"PASS 5s GSLS/PML tail: "<<checkpoint<<" checkpoints, absolute error <0.01% main signal, final energy ratio="<<value.diagnostics().energy/value.diagnostics().peakEnergy<<std::endl;
}
void validation(const std::filesystem::path& folder){
    const auto gpu=Seis3D::Curvilinear::queryBackend3D(BackendKind3D::Cuda);std::ofstream csv(folder/"accuracy.csv"),storage(folder/"storage.csv"),traces(folder/"dry_loess_traces.csv");
    csv<<std::setprecision(17)<<"case,pair,component,relative_l2,relative_peak\n";storage<<"case,backend,precision,host_arrays_bytes,device_bytes,model_bytes,state_bytes,boundary_bytes,traction,final_to_peak_energy,dt\n";traces<<std::setprecision(17)<<"time_s,cpu64_vz,cpu32_vz,cuda64_vz,cuda32_vz\n";
    for(int test=0;test<6;++test){auto p=fixture();std::string name;
        switch(test){case 0:name="elastic_flat";p.relief=0;break;case 1:name="elastic_curved_moment";p.source.kind=SeisWave::SourceKind::IsotropicMoment;p.momentPerLength=100;break;
        case 2:name="elastic_curved_pml";p.absorber=Absorber::CfsPml;break;
        case 3:name="gsls1_curved_pml";p.absorber=Absorber::CfsPml;p.attenuation.enabled=true;p.attenuation.mechanisms=1;break;
        case 4:name="gsls3_curved_pml";p.absorber=Absorber::CfsPml;p.attenuation.enabled=true;break;
        default:name="dry_loess_gsls3_pml";p.absorber=Absorber::CfsPml;p.attenuation.enabled=true;p.layered=true;p.layers={{3,500,150,1450,40,25},{0,1200,600,1900,80,50}};p.spacing=.25;p.frequency=15;p.relief=.15;p.wavelength=20;p.sourceZ=5;p.duration=.4;break;}
        std::cout<<"START "<<name<<std::endl;auto cpu64=run(p,BackendKind3D::Cpu);
        p.precision=Precision::Float32;auto cpu32=run(p,BackendKind3D::Cpu,test==5);compare(name,"CPU32_CPU64",cpu64,cpu32,csv);
        check(cpu32.diagnostics.modelArrayBytes*2==cpu64.diagnostics.modelArrayBytes&&cpu32.diagnostics.stateArrayBytes*2==cpu64.diagnostics.stateArrayBytes&&cpu32.diagnostics.boundaryArrayBytes*2==cpu64.diagnostics.boundaryArrayBytes,"FP32 arrays do not use exactly half storage");
        const auto log=[&](const Result& r,const char* backend,const char* precision){const auto& d=r.diagnostics;storage<<std::setprecision(17)<<name<<','<<backend<<','<<precision<<','<<d.residentHostArrayBytes<<','<<d.residentDeviceBytes<<','<<d.modelArrayBytes<<','<<d.stateArrayBytes<<','<<d.boundaryArrayBytes<<','<<d.tractionRelative<<','<<d.energy/d.peakEnergy<<','<<r.dt<<'\n';};
        log(cpu64,"CPU","FP64");log(cpu32,"CPU","FP32");
        if(gpu.available){p.precision=Precision::Float64;auto cuda64=run(p,BackendKind3D::Cuda);p.precision=Precision::Float32;auto cuda32=run(p,BackendKind3D::Cuda,test==5);
            compare(name,"CUDA64_CPU64",cpu64,cuda64,csv);compare(name,"CUDA32_CPU64",cpu64,cuda32,csv);compare(name,"CUDA32_CPU32",cpu32,cuda32,csv);log(cuda64,"CUDA","FP64");log(cuda32,"CUDA","FP32");
            if(test==5){const auto r=cpu64.receivers.size()/2;for(std::size_t n=0;n<cpu64.receivers[r].vz.size();++n)traces<<n*.00025<<','<<cpu64.receivers[r].vz[n]<<','<<cpu32.receivers[r].vz[n]<<','<<cuda64.receivers[r].vz[n]<<','<<cuda32.receivers[r].vz[n]<<'\n';}
        }
        std::cout<<"PASS "<<name<<std::endl;csv.flush();storage.flush();
    }
    if(gpu.available)longTail(folder);
    modelIo(folder);
    for(auto backend:{BackendKind3D::Cpu,BackendKind3D::Cuda}){if(backend==BackendKind3D::Cuda&&!gpu.available)continue;
        auto p=fixture();p.precision=Precision::Float32;p.attenuation.enabled=true;p.absorber=Absorber::CfsPml;
        AsyncSimulation live;Simulation reference;ComputeSettings3D settings;settings.cpuThreads=1;std::string e;
        check(live.initialize(p,e,backend,settings)&&reference.initialize(p,e,backend,settings),e);std::vector<float> image;check(live.readFrame(0,image,e),e);
        for(int batch=0;batch<4;++batch){check(live.queue(8,1000),"FP32 worker queue failed");live.synchronize();check(live.poll(e),e);}
        check(reference.advance(live.step(),e),e);check(live.snapshot(true).fields==reference.snapshot(true).fields&&live.snapshot(true).memory==reference.snapshot(true).memory,"FP32 background state differs");
        for(std::size_t r=0;r<live.receivers().size();++r)check(live.receivers()[r].vz==reference.receivers()[r].vz&&live.receivers()[r].vx==reference.receivers()[r].vx,"FP32 background samples differ");
        check(live.readFrame(0,image,e)&&live.grid().vp.precision()==Precision::Float32&&live.reset(e)&&live.step()==0,e);
    }
    std::cout<<"PASS FP32 CPU/CUDA background publication, raw records, display and reset\n";
    std::ofstream(folder/"PASS.txt")<<"Elasticity -> GSLS 1/3 -> PML -> dry loess -> long tail -> model IO -> background publication. Default remains FP64.\nCUDA checked: "<<gpu.available<<'\n';
}
void benchmark(const std::filesystem::path& folder,bool capacity){
    using Clock=std::chrono::steady_clock;const auto available=Seis3D::Curvilinear::queryBackend3D(BackendKind3D::Cuda);check(available.available,available.description);
    std::ofstream csv(folder/(capacity?"capacity.csv":"benchmark.csv"));csv<<std::setprecision(17)<<"case,nx,nz,compute_nodes,backend,precision,repeat,warmup_steps,timed_steps,wall_ms,step_ms,device_payload_bytes,host_array_bytes,free_before_bytes,free_after_bytes,rhs_registers,rhs_local_bytes,initialization_ms,dt,status\n";
    const auto one=[&](int nx,int nz,bool q,Precision precision,int repeat,BackendKind3D backend){
        auto p=fixture();p.nx=nx;p.nz=nz;p.spacing=2;p.relief=5;p.wavelength=500;p.frequency=15;p.vp=1500;p.vs=700;p.density=1800;p.sourceX=nx/2;p.sourceZ=20;p.receiverStart=0;p.receiverCount=32;p.receiverStride=std::max(1,(nx-1)/31);p.duration=10;p.maxSteps=1000;p.absorber=Absorber::CfsPml;p.spongeX=p.spongeZ=16;p.attenuation.enabled=q;p.precision=precision;
        const int warm=capacity?8:20,timed=capacity?20:(backend==BackendKind3D::Cpu?20:60);ComputeSettings3D settings;settings.cpuThreads=8;std::string e;
        auto before=Seis3D::Curvilinear::queryBackendMemory3D(backend).deviceAvailableBytes;Simulation s;
        const std::string name=q?"GSLS3_PML":"elastic_PML";std::cout<<"BENCH "<<nx<<'x'<<nz<<' '<<name<<' '<<precisionName(precision)<<" repeat="<<repeat<<' '<<(backend==BackendKind3D::Cuda?"CUDA":"CPU8")<<std::endl;
        if(!s.initialize(p,e,backend,settings)){check(capacity&&precision==Precision::Float64,"Benchmark init failed: "+e);std::replace(e.begin(),e.end(),',',';');std::replace(e.begin(),e.end(),'\n',' ');csv<<name<<','<<nx<<','<<nz<<",0,CUDA,"<<precisionName(precision)<<','<<repeat<<",0,0,0,0,0,0,"<<before<<",0,0,0,0,0,REJECTED: "<<e<<'\n';csv.flush();std::cout<<"REJECTED "<<e<<std::endl;return;}
        check(s.advance(warm,e),e);const auto after=Seis3D::Curvilinear::queryBackendMemory3D(backend).deviceAvailableBytes;const auto start=Clock::now();check(s.advance(timed,e),e);const auto ms=std::chrono::duration<double,std::milli>(Clock::now()-start).count();const auto& d=s.diagnostics();
        csv<<name<<','<<nx<<','<<nz<<','<<static_cast<std::size_t>(s.grid().nx)*s.grid().nz<<','<<(backend==BackendKind3D::Cuda?"CUDA":"CPU8")<<','<<precisionName(precision)<<','<<repeat<<','<<warm<<','<<timed<<','<<ms<<','<<ms/timed<<','<<d.residentDeviceBytes<<','<<d.residentHostArrayBytes<<','<<before<<','<<after<<','<<d.rhsRegisters<<','<<d.rhsLocalBytes<<','<<d.initializationMs<<','<<s.dt()<<",OK\n";csv.flush();std::cout<<"DONE step_ms="<<ms/timed<<" device_GiB="<<d.residentDeviceBytes/1073741824.<<std::endl;
    };
    if(capacity){for(bool q:{false,true})for(auto precision:{Precision::Float64,Precision::Float32})one(5001,2001,q,precision,0,BackendKind3D::Cuda);}
    else {for(int size:{1024,2048})for(bool q:{false,true})for(int rep=0;rep<3;++rep)for(auto precision:{Precision::Float64,Precision::Float32})one(size,size,q,precision,rep,BackendKind3D::Cuda);
        for(bool q:{false,true})for(int rep=0;rep<3;++rep)for(auto precision:{Precision::Float64,Precision::Float32})one(512,512,q,precision,rep,BackendKind3D::Cpu);}
}
}
int main(int argc,char** argv){try{const auto folder=std::filesystem::path(SEIS3D_OUTPUT_ROOT)/"validation/precision-2d";std::filesystem::create_directories(folder);
    const std::string_view mode=argc>1?argv[1]:"";if(mode=="--benchmark")benchmark(folder,false);else if(mode=="--capacity")benchmark(folder,true);else validation(folder);return 0;
}catch(const std::exception& e){std::cerr<<"FAIL: "<<e.what()<<'\n';return 1;}}
