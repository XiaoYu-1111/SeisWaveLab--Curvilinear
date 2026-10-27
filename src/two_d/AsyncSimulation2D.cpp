#include "Seis2D/AsyncSimulation2D.hpp"
#include "SeisCommon/BackgroundRunner.hpp"
#include <algorithm>
#include <chrono>
namespace Seis2D::Curvilinear {
namespace {
using Clock=std::chrono::steady_clock;
double elapsed(Clock::time_point begin){return std::chrono::duration<double,std::milli>(Clock::now()-begin).count();}
struct Publication {Diagnostics diagnostics;int step{};std::vector<Receiver> delta;std::vector<float> frame;int component{-1};double computeMs{},transferMs{};int advanced{};std::string error;};
}
struct AsyncSimulation::Impl {
    Simulation core;mutable SeisWave::BackgroundRunner<Publication> worker;
    Diagnostics diagnostics;BackendInfo3D backend;int step{},total{};double dt{},recordDt{};bool initialized{};
    std::vector<Receiver> receivers;mutable std::vector<float> frame;mutable int requested{-1},component{-1},frameStep{-1};
    BackgroundTiming timing;std::string workerError;
    void refresh(){initialized=core.initialized();if(!initialized)return;diagnostics=core.diagnostics();backend=core.backend();step=core.step();total=core.totalSteps();dt=core.dt();recordDt=core.recordDt();receivers=core.receivers();frameStep=-1;}
    void apply(Publication&& p){diagnostics=p.diagnostics;step=p.step;for(std::size_t i=0;i<receivers.size();++i){auto& out=receivers[i];const auto& d=p.delta[i];out.vx.insert(out.vx.end(),d.vx.begin(),d.vx.end());out.vz.insert(out.vz.end(),d.vz.begin(),d.vz.end());}
        if(p.component>=0){frame=std::move(p.frame);component=p.component;frameStep=step;}
        timing.computeMs+=p.computeMs;timing.transferMs+=p.transferMs;++timing.batches;timing.steps+=p.advanced;workerError=std::move(p.error);
    }
};
AsyncSimulation::AsyncSimulation():impl_(std::make_unique<Impl>()){}
AsyncSimulation::~AsyncSimulation(){try{synchronize();}catch(...){}}
void AsyncSimulation::synchronize(){if(auto p=impl_->worker.wait())impl_->apply(std::move(*p));}
void AsyncSimulation::pause(){impl_->worker.cancel();}
bool AsyncSimulation::busy()const{return impl_->worker.busy();}
bool AsyncSimulation::poll(std::string& e){auto& s=*impl_;try{if(auto p=s.worker.poll())s.apply(std::move(*p));}catch(const std::exception& exception){e=exception.what();return false;}if(!s.workerError.empty()){e=std::exchange(s.workerError,{});return false;}return true;}
BackgroundTiming AsyncSimulation::backgroundTiming()const{return impl_->timing;}
bool AsyncSimulation::queue(int count,double budget){auto& s=*impl_;if(!s.initialized||completed()||s.worker.busy())return false;
    std::vector<std::size_t> counts;for(const auto& r:s.receivers)counts.push_back(r.vx.size());const int requested=s.requested;
    return s.worker.submit([&s,counts=std::move(counts),requested,count=std::clamp(count,1,8),budget](const std::atomic_bool& cancel){
        Publication p;auto begin=Clock::now();const int before=s.core.step();
        for(int i=0;i<count&&!cancel&&!s.core.completed();++i){if(!s.core.advance(1,p.error))break;if(elapsed(begin)>=budget)break;}
        p.computeMs=elapsed(begin);p.advanced=s.core.step()-before;begin=Clock::now();p.step=s.core.step();p.diagnostics=s.core.diagnostics();
        const auto& source=s.core.receivers();p.delta.resize(source.size());
        for(std::size_t i=0;i<source.size();++i){p.delta[i].vx.assign(source[i].vx.begin()+counts[i],source[i].vx.end());p.delta[i].vz.assign(source[i].vz.begin()+counts[i],source[i].vz.end());}
        if(requested>=0){std::string error;if(s.core.readFrame(requested,p.frame,error))p.component=requested;else if(p.error.empty())p.error=error;}
        p.transferMs=elapsed(begin);return p;
    });
}
bool AsyncSimulation::initialize(const Parameters& p,std::string& e,BackendKind3D b,const ComputeSettings3D& c){synchronize();const bool okay=impl_->core.initialize(p,e,b,c);impl_->refresh();impl_->requested=-1;impl_->timing={};impl_->workerError.clear();return okay;}
bool AsyncSimulation::advance(int n,std::string& e){synchronize();const bool okay=impl_->core.advance(n,e);impl_->refresh();return okay;}
bool AsyncSimulation::reset(std::string& e){synchronize();const bool okay=impl_->core.reset(e);impl_->refresh();impl_->timing={};impl_->workerError.clear();return okay;}
bool AsyncSimulation::initialized()const{return impl_->initialized;}
bool AsyncSimulation::completed()const{return impl_->initialized&&impl_->step>=impl_->total;}
int AsyncSimulation::step()const{return impl_->step;}
int AsyncSimulation::totalSteps()const{return impl_->total;}
double AsyncSimulation::dt()const{return impl_->dt;}
double AsyncSimulation::recordDt()const{return impl_->recordDt;}
const Parameters& AsyncSimulation::parameters()const{return impl_->core.parameters();}
const Grid& AsyncSimulation::grid()const{return impl_->core.grid();}
const Diagnostics& AsyncSimulation::diagnostics()const{return impl_->diagnostics;}
const std::vector<Receiver>& AsyncSimulation::receivers()const{return impl_->receivers;}
BackendInfo3D AsyncSimulation::backend()const{return impl_->backend;}
bool AsyncSimulation::readFrame(int c,std::vector<float>& out,std::string& e)const{auto& s=*impl_;s.requested=c;if(s.frameStep==s.step&&s.component==c){out=s.frame;e.clear();return true;}if(s.worker.busy()){e.clear();return false;}if(!s.core.readFrame(c,s.frame,e))return false;s.component=c;s.frameStep=s.step;out=s.frame;return true;}
Snapshot AsyncSimulation::snapshot(bool includeMemories)const{const_cast<AsyncSimulation*>(this)->synchronize();return impl_->core.snapshot(includeMemories);}
AsyncSimulation::operator const Simulation&()const{const_cast<AsyncSimulation*>(this)->synchronize();return impl_->core;}
}
