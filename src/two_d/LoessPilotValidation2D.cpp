#include "Seis2D/ModelIo2D.hpp"
#include <algorithm>
#include <cmath>
#include <fstream>
#include <iostream>
#include <stdexcept>
using namespace Seis2D::Curvilinear;
namespace {
void check(bool ok,const std::string& why){if(!ok)throw std::runtime_error(why);}
Parameters fixture(){Parameters p;p.nx=81;p.nz=81;p.spacing=.5;p.relief=.15;p.wavelength=30;p.flattenDepth=20;
    p.sourceX=40;p.sourceZ=6;p.frequency=15;p.duration=.015;p.maxSteps=0;p.cfl=.5;p.forcePerLength=100;
    p.sourceSigmaM=.5;p.sourceRadiusM=1.5;p.receiverStart=20;p.receiverCount=9;p.receiverStride=5;p.receiverDepth=0;p.sampleIntervalMs=.25;
    p.absorber=Absorber::CfsPml;p.externalAbsorption=true;p.spongeX=p.spongeZ=12;
    p.attenuation.enabled=true;p.attenuation.mechanisms=3;p.attenuation.minFrequency=3;p.attenuation.maxFrequency=45;p.attenuation.referenceFrequency=15;
    p.layered=true;p.layers={{10,550,250,1550,40,25},{0,1500,700,2050,120,75}};return p;}
double relative(const std::vector<double>& a,const std::vector<double>& b){check(a.size()==b.size(),"length");double e=0,n=0;for(std::size_t i=0;i<a.size();++i){e+=(a[i]-b[i])*(a[i]-b[i]);n+=a[i]*a[i];}return std::sqrt(e/std::max(n,1e-100));}
}
int main(){try{
    const auto root=std::filesystem::path(SEIS3D_OUTPUT_ROOT)/"validation/loess-pilot";std::filesystem::create_directories(root);
    std::string error;auto p=fixture();Simulation cpu;check(cpu.initialize(p,error,BackendKind3D::Cpu,{1,0}),error);
    check(std::abs(cpu.diagnostics().sourceWeightSum-1)<1e-12,"physical source force normalization");
    check(cpu.advance(cpu.totalSteps(),error),error);
    const std::vector<std::array<int,2>> nodes{{0,0},{40,6},{80,80},{30,20},{40,6}};
    std::vector<std::array<double,5>> probes;check(cpu.readProbes(nodes,probes,error),error);auto snap=cpu.snapshot();
    for(std::size_t i=0;i<nodes.size();++i)for(int f=0;f<5;++f)check(probes[i][f]==snap.fields[f][cpu.grid().modelIndex(nodes[i][0],nodes[i][1])],"CPU compact probe differs from full snapshot");
    const auto saved=probes;check(!cpu.readProbes({{-1,0}},probes,error)&&probes==saved,"bad probe changed output");
    check(cpu.readProbes({},probes,error)&&probes.empty(),"empty probe");
    check(cpu.snapshot().fields==snap.fields,"probe readback changed state");
    double worst=0;int gpuCount=0;
    if(Seis3D::Curvilinear::queryBackend3D(BackendKind3D::Cuda,0).available){
        for(auto precision:{Precision::Float64,Precision::Float32}){auto next=p;next.precision=precision;Simulation gpu;check(gpu.initialize(next,error,BackendKind3D::Cuda,{1,0}),error);check(gpu.advance(gpu.totalSteps(),error),error);
            check(gpu.readProbes(nodes,probes,error),error);const auto whole=gpu.snapshot();
            for(std::size_t i=0;i<nodes.size();++i)for(int f=0;f<5;++f)check(probes[i][f]==whole.fields[f][gpu.grid().modelIndex(nodes[i][0],nodes[i][1])],"CUDA compact probe differs from snapshot");
            if(precision==Precision::Float64)for(int f=0;f<5;++f)worst=std::max(worst,relative(snap.fields[f],whole.fields[f]));
            check(gpu.readProbes({},probes,error)&&probes.empty(),"CUDA empty probe");++gpuCount;
        }
    }
    check(worst<1e-8,"CPU/CUDA physical-source mismatch");
    for(auto precision:{Precision::Float64,Precision::Float32}){auto next=p;next.precision=precision;Simulation s;check(s.initialize(next,error,BackendKind3D::Cpu,{1,0}),error);
        const auto file=root/(precision==Precision::Float64?"physical64.vts":"physical32.vts");check(writeModelVts(s,file,{1,0},error),error);ModelDocument doc;check(readModelVts(file,doc,error),error);check(doc.parameters==next,"schema6 physical kernel roundtrip");}
    auto flat=p;flat.relief=0;flat.attenuation.enabled=false;flat.layered=false;flat.vp=550;flat.vs=250;flat.density=1550;
    double radius[2]{};int index=0;
    for(double h:{.5,.25}){auto next=flat;next.spacing=h;next.nx=static_cast<int>(40/h)+1;next.nz=next.nx;next.sourceX=static_cast<int>(20/h);next.sourceZ=static_cast<int>(3/h);next.receiverStart=static_cast<int>(10/h);next.receiverStride=static_cast<int>(2.5/h);
        Simulation s;check(s.initialize(next,error,BackendKind3D::Cpu,{1,0}),error);radius[index++]=s.diagnostics().sourceRmsRadiusM;check(std::abs(s.diagnostics().sourceWeightSum-1)<1e-12,"force normalization by grid");}
    check(std::abs(radius[0]/radius[1]-1)<.02,"physical kernel changed width under refinement");
    auto bad=p;bad.sourceRadiusM=.1;check(!cpu.initialize(bad,error),"invalid physical radius accepted");
    bad=p;bad.source.kind=SeisWave::SourceKind::IsotropicMoment;check(!cpu.initialize(bad,error),"unsupported physical moment kernel accepted");
    std::ofstream(root/"summary.json")<<"{\"status\":\"PASS\",\"cuda_precisions_tested\":"<<gpuCount<<",\"cpu_cuda_l2\":"<<worst<<",\"kernel_rms_h050_m\":"<<radius[0]<<",\"kernel_rms_h025_m\":"<<radius[1]<<"}\n";
    std::cout<<"PASS physical force kernel, schema6 FP32/64, compact CPU/CUDA probes; CUDA precisions="<<gpuCount<<", L2="<<worst<<'\n';return 0;
}catch(const std::exception& ex){std::cerr<<ex.what()<<'\n';return 1;}}
