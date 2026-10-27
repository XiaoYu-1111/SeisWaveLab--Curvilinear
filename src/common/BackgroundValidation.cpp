#include "SeisCommon/BackgroundRunner.hpp"
#include "Seis2D/AsyncSimulation2D.hpp"
#include "Seis3D/AsyncSimulation3D.hpp"
#include <chrono>
#include <fstream>
#include <iostream>
#include <stdexcept>
using namespace Seis3D::Curvilinear;
namespace {
void check(bool okay,const std::string& message){if(!okay)throw std::runtime_error(message);}
}
int main(){try{
    std::string error;std::size_t polls=0;int backends=0;
    {SeisWave::BackgroundRunner<int> runner;check(runner.submit([](const std::atomic_bool&){std::this_thread::sleep_for(std::chrono::milliseconds(80));return 7;}),"Task rejected");
        const auto start=std::chrono::steady_clock::now();int ticks=0;while(std::chrono::steady_clock::now()-start<std::chrono::milliseconds(35)){check(!runner.poll(),"Task unexpectedly completed");++ticks;std::this_thread::yield();}check(ticks>=100,"UI could not continue during worker task");check(runner.wait()==7,"Worker result lost");}
    for(int kind=0;kind<2;++kind){const auto backend=static_cast<BackendKind3D>(kind);if(!queryBackend3D(backend).available)continue;++backends;
        SimulationParameters p;p.nx=36;p.ny=28;p.nz=32;p.maxSteps=24;p.topographyAmplitude=5;
        auto core=createSimulationBackend3D(backend,error);check(core&&core->initialize(p,error,{1}),error);AsyncSimulation3D live(std::move(core));
        CpuSimulation3D reference(backend);check(reference.initialize(p,error,{1}),error);
        DisplayFrame3D frame;DisplayRequest3D request{12,10,3,0};check(live.readDisplayFrame(request,frame,error),error);
        while(live.currentStep()<12){if(!live.busy())live.queue(std::min(3,12-live.currentStep()),1000);
            const auto oldStep=live.currentStep();const auto samples=live.receivers().front().samples.size();
            check(samples==static_cast<std::size_t>(oldStep+1)&&live.diagnostics().allFinite,"UI publication is inconsistent");
            check(live.grid().nx==p.nx+6&&live.parameters()==p,"Immutable geometry/parameters changed");
            check(live.poll(error),error);++polls;std::this_thread::sleep_for(std::chrono::milliseconds(1));}
        check(reference.advance(12,error),error);const auto a=reference.snapshot(),b=live.snapshot();
        check(a.vz==b.vz&&a.vx==b.vx&&a.sxx==b.sxx,"3D threaded numerical state differs");
        for(std::size_t i=0;i<live.receivers().size();++i)check(live.receivers()[i].samples==reference.receivers()[i].samples,"3D receiver publication differs");
        check(live.readDisplayFrame(request,frame,error)&&frame.revision==live.revision(),"3D display revision differs");
        check(live.queue(8,1000),"3D queue after publication failed");live.pause();live.synchronize();
        check(live.receivers().front().samples.size()==static_cast<std::size_t>(live.currentStep()+1),"Pause lost receiver samples");
        check(live.resetFields(error)&&live.currentStep()==0&&live.receivers().front().samples.size()==1,"3D reset while managed failed");
        namespace D=Seis2D::Curvilinear;D::Parameters q;q.nx=80;q.nz=64;q.relief=5;q.sourceZ=20;q.receiverCount=4;q.maxSteps=24;q.sampleIntervalMs=.5;
        D::AsyncSimulation live2;D::Simulation reference2;check(live2.initialize(q,error,backend,{1})&&reference2.initialize(q,error,backend,{1}),error);
        std::vector<float> image;check(live2.readFrame(0,image,error),error);
        while(live2.step()<12){if(!live2.busy())live2.queue(std::min(3,12-live2.step()),1000);check(live2.poll(error),error);check(live2.diagnostics().finite,"2D UI snapshot nonfinite");++polls;std::this_thread::sleep_for(std::chrono::milliseconds(1));}
        check(reference2.advance(12,error),error);check(live2.snapshot().fields==reference2.snapshot().fields,"2D threaded numerical state differs");
        for(std::size_t i=0;i<live2.receivers().size();++i)check(live2.receivers()[i].vz==reference2.receivers()[i].vz&&live2.receivers()[i].vx==reference2.receivers()[i].vx,"2D fractional-interval records differ");
        check(live2.queue(8,1000),"2D queue failed");check(live2.reset(error)&&live2.step()==0&&live2.receivers().front().vx.size()==1,"2D reset while busy failed");
    }
    const auto folder=std::filesystem::path(SEIS3D_OUTPUT_ROOT)/"validation/background";std::filesystem::create_directories(folder);
    std::ofstream(folder/"result.json")<<"{\"backends\":"<<backends<<",\"ui_polls\":"<<polls<<",\"exact_2d_3d_fields\":true,\"fractional_interval_records\":true,\"pause_reset\":true}\n";
    std::cout<<"Background PASS: "<<backends<<" backends, "<<polls<<" nonblocking UI polls, exact 2D/3D fields and raw records, pause/reset\n";
    return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
