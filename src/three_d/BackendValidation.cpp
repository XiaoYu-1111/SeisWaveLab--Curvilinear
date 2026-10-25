#include "Seis3D/CurvilinearSolver.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <iostream>
#include <stdexcept>
using namespace Seis3D::Curvilinear;
void check(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
void compare(const std::vector<double>& a,const std::vector<double>& b,const char* label) {
    check(a.size()==b.size(),"Backend buffer sizes differ");double peak=0,difference=0;
    for(std::size_t q=0;q<a.size();++q){check(std::isfinite(a[q])&&std::isfinite(b[q]),"Nonfinite comparison");peak=std::max(peak,std::abs(a[q]));difference=std::max(difference,std::abs(a[q]-b[q]));}
    const double relative=difference/std::max(peak,1e-25);std::cout<<label<<" max relative error="<<relative<<'\n';check(relative<1e-9,"CPU/CUDA numerical mismatch");
}
int main() {
    const auto availability=queryBackend3D(BackendKind3D::Cuda);
    if(!availability.available){std::cout<<"SKIP: "<<availability.description<<'\n';return 77;}
    std::cout<<"CUDA device: "<<availability.description<<'\n';
    try {
        for(double relief:{0.0,30.0}) {
            SimulationParameters p;p.maxSteps=0;p.nx=18;p.ny=16;p.nz=24;p.topographyAmplitude=relief;p.duration=0.25;
            if(relief>0){p.sourceX=6;p.sourceY=5;p.sourceZ=4;}
            std::string error;CpuSimulation3D cpu;auto gpu=createSimulationBackend3D(BackendKind3D::Cuda,error);
            check(gpu&&cpu.initialize(p,error,{1})&&gpu->initialize(p,error),error.c_str());
            check(gpu->backendInfo().kind==BackendKind3D::Cuda,"CUDA silently fell back to CPU");
            check(cpu.dt()==gpu->dt()&&cpu.totalSteps()==gpu->totalSteps(),"Backend time grids differ");
            double cpuMs=0,gpuMs=0;
            while(!cpu.completed()) {
                const auto start=std::chrono::steady_clock::now();check(cpu.advance(31,error),error.c_str());
                const auto middle=std::chrono::steady_clock::now();check(gpu->advance(31,error),error.c_str());
                const auto end=std::chrono::steady_clock::now();cpuMs+=std::chrono::duration<double,std::milli>(middle-start).count();gpuMs+=std::chrono::duration<double,std::milli>(end-middle).count();
            }
            const auto a=cpu.snapshot(),b=gpu->snapshot();
            const std::vector<double>* af[]{&a.vx,&a.vy,&a.vz,&a.sxx,&a.syy,&a.szz,&a.sxy,&a.sxz,&a.syz};
            const std::vector<double>* bf[]{&b.vx,&b.vy,&b.vz,&b.sxx,&b.syy,&b.szz,&b.sxy,&b.sxz,&b.syz};
            for(int f=0;f<9;++f)compare(*af[f],*bf[f],"field");
            for(std::size_t r=0;r<cpu.receivers().size();++r){compare(cpu.receivers()[r].samples,gpu->receivers()[r].samples,"receiver Vz");compare(cpu.receivers()[r].samplesVx,gpu->receivers()[r].samplesVx,"receiver Vx");compare(cpu.receivers()[r].samplesVy,gpu->receivers()[r].samplesVy,"receiver Vy");}
            check(b.allFinite&&b.diagnostics.elasticEnergy>0&&b.diagnostics.maxTractionRateRelative<1e-9,"CUDA diagnostics failed");
            check(std::abs(a.diagnostics.elasticEnergy-b.diagnostics.elasticEnergy)/a.diagnostics.elasticEnergy<1e-9,"CUDA energy reduction mismatch");
            DisplayRequest3D request{p.nx/2,p.ny/2,3,0};DisplayFrame3D fa,fb;
            for(int c=0;c<5;++c) {
                request.component=c;check(cpu.readDisplayFrame(request,fa,error)&&gpu->readDisplayFrame(request,fb,error),error.c_str());
                check(fa.bytes==fb.bytes&&fb.bytes==sizeof(float)*(p.nx*p.nz+p.ny*p.nz+p.nx*p.ny),"CUDA copied full volume for display");
                for(int plane=0;plane<3;++plane) {
                    double peak=0,diff=0;check(fa.planes[plane].values.size()==fb.planes[plane].values.size(),"Slice dimensions mismatch");
                    for(std::size_t q=0;q<fa.planes[plane].values.size();++q){peak=std::max(peak,std::abs(static_cast<double>(fa.planes[plane].values[q])));diff=std::max(diff,std::abs(static_cast<double>(fa.planes[plane].values[q])-fb.planes[plane].values[q]));}
                    check(diff<=1e-6*std::max(peak,1e-25),"CUDA compact slice mismatch");
                }
            }
            const auto revision=gpu->revision();auto invalid=p;invalid.nx=maxPhysicalNodesPerAxis3D+1;
            check(!gpu->initialize(invalid,error)&&gpu->revision()==revision,"CUDA invalid initialization discarded session");
            check(gpu->advance(100,error)&&gpu->currentStep()==cpu.currentStep(),"CUDA overran completion");
            p.maxSteps=3;check(gpu->initialize(p,error)&&gpu->advance(100,error)&&gpu->currentStep()==3&&gpu->receivers().front().samples.size()==4,"CUDA reset/step cap failed");
            std::cout<<"PASS CPU/CUDA relief="<<relief<<" CPU serial="<<cpuMs<<" ms CUDA="<<gpuMs<<" ms\n";
        }
        for(auto kind:{BackendKind3D::Cpu,BackendKind3D::Cuda}) {
            std::string error;auto s=createSimulationBackend3D(kind,error);SimulationParameters p;p.density=1e-300;p.maxSteps=1;
            check(s&&s->initialize(p,error),"Overflow guard case initialization failed");const auto revision=s->revision();
            check(!s->advance(1,error)&&s->currentStep()==0&&s->revision()==revision&&s->diagnostics().allFinite&&s->receivers().front().samples.size()==1,"Nonfinite RK step did not retain last accepted state");
            check(!s->advance(1,error),"Faulted backend resumed without rebuilding");
            const auto retained=s->snapshot();check(retained.maxAbsVz==0&&retained.steps==0,"Rollback metadata did not match retained fields");
            p.density=2200;check(s->initialize(p,error)&&s->advance(1,error),"Rebuild failed to clear fault");
        }
        std::cout<<"PASS: CPU and CUDA nonfinite rollback and fault recovery\n";
    } catch(const std::exception& e){std::cerr<<"FAIL: "<<e.what()<<'\n';return 1;}
}
