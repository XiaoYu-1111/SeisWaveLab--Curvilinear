#include "Seis3D/AsyncSimulation3D.hpp"
#include "SeisCommon/BackgroundRunner.hpp"
#include <chrono>
#include <optional>
#include <utility>
namespace Seis3D::Curvilinear {
namespace {
using Clock=std::chrono::steady_clock;
double milliseconds(Clock::time_point t){return std::chrono::duration<double,std::milli>(Clock::now()-t).count();}
struct Publication {
    Diagnostics3D diagnostics;BackendMemory3D memory;int step{};std::uint64_t revision{};
    std::vector<ReceiverTrace3D> delta;std::array<std::optional<DisplayFrame3D>,2> frames;
    std::string error;double computeMs{},transferMs{};int advanced{};
};
}
struct AsyncSimulation3D::Impl {
    std::unique_ptr<ISimulationBackend3D> core;
    mutable SeisWave::BackgroundRunner<Publication> worker;
    Diagnostics3D diagnostics;BackendMemory3D memory;BackendInfo3D backend;
    int step{},total{};double dt{};std::uint64_t revision{};bool initialized{};
    std::vector<ReceiverTrace3D> receivers;
    mutable std::array<std::optional<DisplayRequest3D>,2> requests;
    mutable std::array<std::optional<DisplayFrame3D>,2> frames;
    BackgroundTiming3D timing;std::string workerError;
    explicit Impl(std::unique_ptr<ISimulationBackend3D> value):core(std::move(value)){refresh();}
    void refresh() {
        initialized=core->initialized();if(!initialized)return;
        diagnostics=core->diagnostics();memory=core->residentMemory();backend=core->backendInfo();
        step=core->currentStep();total=core->totalSteps();dt=core->dt();revision=core->revision();
        receivers=core->receivers();frames={};
    }
    void apply(Publication&& p) {
        diagnostics=p.diagnostics;memory=p.memory;step=p.step;revision=p.revision;
        for(std::size_t r=0;r<receivers.size();++r) {
            const auto& d=p.delta[r];auto& out=receivers[r];
            out.samples.insert(out.samples.end(),d.samples.begin(),d.samples.end());
            out.samplesVx.insert(out.samplesVx.end(),d.samplesVx.begin(),d.samplesVx.end());
            out.samplesVy.insert(out.samplesVy.end(),d.samplesVy.begin(),d.samplesVy.end());
        }
        for(int i=0;i<2;++i)if(p.frames[i])frames[i]=std::move(p.frames[i]);
        timing.computeMs+=p.computeMs;timing.transferMs+=p.transferMs;++timing.batches;timing.steps+=p.advanced;
        workerError=std::move(p.error);
    }
};
AsyncSimulation3D::AsyncSimulation3D(std::unique_ptr<ISimulationBackend3D> core):impl_(std::make_unique<Impl>(std::move(core))){}
AsyncSimulation3D::~AsyncSimulation3D(){try{synchronize();}catch(...){}}
void AsyncSimulation3D::synchronize(){auto& s=*impl_;if(auto p=s.worker.wait())s.apply(std::move(*p));}
void AsyncSimulation3D::pause(){impl_->worker.cancel();}
bool AsyncSimulation3D::busy()const{return impl_->worker.busy();}
bool AsyncSimulation3D::poll(std::string& error){auto& s=*impl_;try{if(auto p=s.worker.poll())s.apply(std::move(*p));}catch(const std::exception& e){error=e.what();return false;}if(!s.workerError.empty()){error=std::exchange(s.workerError,{});return false;}return true;}
BackgroundTiming3D AsyncSimulation3D::backgroundTiming()const{return impl_->timing;}
bool AsyncSimulation3D::queue(int maximum,double budget) {
    auto& s=*impl_;if(!s.initialized||completed()||s.worker.busy())return false;
    std::vector<std::size_t> counts;for(const auto& r:s.receivers)counts.push_back(r.samples.size());
    auto requests=s.requests;
    if(s.frames[1]&&requests[1]&&s.frames[1]->request==*requests[1])requests[1].reset();
    return s.worker.submit([&s,counts=std::move(counts),requests,maximum=std::clamp(maximum,1,8),budget](const std::atomic_bool& cancel){
        Publication p;const auto begin=Clock::now();const int before=s.core->currentStep();
        for(int n=0;n<maximum&&!cancel&&!s.core->completed();++n){if(!s.core->advance(1,p.error))break;if(milliseconds(begin)>=budget)break;}
        p.computeMs=milliseconds(begin);p.advanced=s.core->currentStep()-before;
        const auto transfer=Clock::now();p.diagnostics=s.core->diagnostics();p.memory=s.core->residentMemory();
        p.step=s.core->currentStep();p.revision=s.core->revision();
        const auto& source=s.core->receivers();p.delta.resize(source.size());
        for(std::size_t r=0;r<source.size();++r){const auto at=counts[r];auto& d=p.delta[r];
            d.samples.assign(source[r].samples.begin()+at,source[r].samples.end());
            d.samplesVx.assign(source[r].samplesVx.begin()+at,source[r].samplesVx.end());
            d.samplesVy.assign(source[r].samplesVy.begin()+at,source[r].samplesVy.end());}
        for(int i=0;i<2;++i)if(requests[i]){DisplayFrame3D f;std::string error;if(s.core->readDisplayFrame(*requests[i],f,error))p.frames[i]=std::move(f);else if(p.error.empty())p.error=error;}
        p.transferMs=milliseconds(transfer);return p;
    });
}
bool AsyncSimulation3D::initialize(const SimulationParameters& p,std::string& e,const ComputeSettings3D& c){synchronize();auto& s=*impl_;const bool okay=s.core->initialize(p,e,c);s.refresh();s.requests={};s.timing={};s.workerError.clear();return okay;}
bool AsyncSimulation3D::advance(int n,std::string& e){synchronize();const bool okay=impl_->core->advance(n,e);impl_->refresh();return okay;}
bool AsyncSimulation3D::resetFields(std::string& e){synchronize();const bool okay=impl_->core->resetFields(e);impl_->refresh();impl_->timing={};impl_->workerError.clear();return okay;}
bool AsyncSimulation3D::initialized()const noexcept{return impl_->initialized;}
bool AsyncSimulation3D::completed()const noexcept{return impl_->initialized&&impl_->step>=impl_->total;}
int AsyncSimulation3D::currentStep()const noexcept{return impl_->step;}
int AsyncSimulation3D::totalSteps()const noexcept{return impl_->total;}
double AsyncSimulation3D::dt()const noexcept{return impl_->dt;}
std::uint64_t AsyncSimulation3D::revision()const noexcept{return impl_->revision;}
BackendInfo3D AsyncSimulation3D::backendInfo()const noexcept{return impl_->backend;}
BackendMemory3D AsyncSimulation3D::residentMemory()const noexcept {
    auto memory=impl_->memory;for(const auto& r:impl_->receivers)memory.hostAvailableBytes+=(r.samples.capacity()+r.samplesVx.capacity()+r.samplesVy.capacity())*sizeof(double);
    for(const auto& frame:impl_->frames)if(frame)memory.hostAvailableBytes+=frame->bytes;return memory;
}
const Grid3D& AsyncSimulation3D::grid()const{return impl_->core->grid();}
const SimulationParameters& AsyncSimulation3D::parameters()const{return impl_->core->parameters();}
const Diagnostics3D& AsyncSimulation3D::diagnostics()const{return impl_->diagnostics;}
const std::vector<ReceiverTrace3D>& AsyncSimulation3D::receivers()const{return impl_->receivers;}
bool AsyncSimulation3D::readDisplayFrame(const DisplayRequest3D& r,DisplayFrame3D& out,std::string& error)const {
    auto& s=*impl_;const int slot=r.component>=5?1:0;s.requests[slot]=r;
    if(s.frames[slot]&&s.frames[slot]->request==r&&(slot==1||s.frames[slot]->revision==s.revision)){out=*s.frames[slot];error.clear();return true;}
    if(s.worker.busy()){error.clear();return false;} // UI shows a pending view, never mislabeled old data.
    DisplayFrame3D f;if(!s.core->readDisplayFrame(r,f,error))return false;s.frames[slot]=std::move(f);out=*s.frames[slot];return true;
}
SimulationResult AsyncSimulation3D::snapshot()const {const_cast<AsyncSimulation3D*>(this)->synchronize();return impl_->core->snapshot();}
}
