#include "Seis2D/ModelIo2D.hpp"
#include "SeisCommon/AtomicFile.hpp"
#include "SeisCommon/SegyIo.hpp"
#include "MemoryBudget2D.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>

using namespace Seis2D::Curvilinear;
namespace {
using Clock = std::chrono::steady_clock;
void check(bool ok, const std::string &why) { if (!ok) throw std::runtime_error(why); }
double seconds(Clock::time_point start) { return std::chrono::duration<double>(Clock::now()-start).count(); }
struct Options {
    std::filesystem::path output, terrain;
    int group{-1}; bool test{}, attenuation{};
    double frequency{15}, width{10000}, depth{4000}, duration{3}, originX{};
};
void textFile(const std::filesystem::path &path, const std::string &text) {
    auto pending = path; pending += ".pending";
    { std::ofstream out(pending); out << text; out.close(); check(bool(out), "Cannot write " + path.string()); }
    SeisWave::replaceCompletedFile(pending, path);
}
void status(const Simulation &s, const std::filesystem::path &folder, const char *state, double elapsed) {
    const auto &d=s.diagnostics();
    std::ostringstream out;
    out << std::setprecision(17) << "{\n\"state\":\"" << state << "\",\"step\":" << s.step()
        << ",\"total_steps\":" << s.totalSteps() << ",\"time_s\":" << s.step()*s.dt()
        << ",\"dt_s\":" << s.dt() << ",\"record_dt_s\":" << s.recordDt()
        << ",\"samples\":" << s.receivers().front().vx.size() << ",\"elapsed_s\":" << elapsed
        << ",\"mean_step_ms\":" << (s.step()?d.totalStepMs/s.step():0)
        << ",\"finite\":" << (d.finite?"true":"false") << ",\"energy\":" << d.energy
        << ",\"peak_energy\":" << d.peakEnergy << ",\"traction_relative\":" << d.tractionRelative
        << ",\"max_velocity\":" << d.maxVelocity << ",\"ppw\":" << d.ppw
        << ",\"state_fields\":" << d.stateFields << ",\"max_q_fit_error\":" << d.maxRelativeQError << "\n}\n";
    textFile(folder/"status.json",out.str());
}
void records(const Simulation &s, const std::filesystem::path &folder, bool final) {
    std::string error;
    check(writeReceivers(s,folder/(final?"records/receivers_float64.csv":"records/receivers_partial.csv"),error),error);
    if (!final) return;
    const auto &p=s.parameters(); const auto &g=s.grid();
    const auto source=g.coordinates[g.index((p.sourceX<0?p.nx/2:p.sourceX)+ghost,p.sourceZ+ghost)];
    SeisWave::RecordDataView view; view.dt=s.recordDt(); view.components={"Vx","Vz"};
    for(const auto &r:s.receivers()) {
        SeisWave::TraceView trace; trace.position={r.position[0],0,r.position[1]};
        trace.source={source[0],0,source[1]}; trace.channels[0]=r.vx; trace.channels[1]=r.vz;
        view.traces.push_back(trace);
    }
    SeisWave::SegyExportOptions options;
    options.profile=SeisWave::SegyExportProfile::CompatibleIeee32; options.sampleGain=1;
    for(int component=0;component<2;++component) {
        const auto path=folder/"records"/(view.components[component]+".sgy");
        check(SeisWave::writeSegy(view,component,path,error,options),error);
        SeisWave::ImportedRecords read;
        check(SeisWave::readSegy(path,read,error),error);
        check(read.traces.size()==s.receivers().size() && read.dt==s.recordDt(),"SEG-Y readback geometry/dt mismatch");
        for(std::size_t r=0;r<read.traces.size();++r) {
            const auto raw=view.traces[r].channels[component];
            check(read.traces[r].samples.size()==raw.size(),"SEG-Y sample count mismatch");
            for(std::size_t t=0;t<raw.size();++t)
                check(read.traces[r].samples[t]==static_cast<double>(static_cast<float>(raw[t])),"SEG-Y sample readback mismatch");
        }
    }
}
void run(const Options &o) {
    check(o.group>=0 && o.group<4,"Group must be 0 (M00), 1 (M10), 2 (M01), 3 (M11)");
    auto relative=o.output.lexically_normal().lexically_relative(std::filesystem::path(SEIS3D_OUTPUT_ROOT));
    check(o.output.is_absolute() && !relative.empty() && relative!="." && *relative.begin()!="..","Output must be inside project out");
    check(!std::filesystem::exists(o.output) || std::filesystem::is_empty(o.output),"Refusing to overwrite an existing experiment");
    for(const char *dir:{"model","records","snapshots","figures"}) std::filesystem::create_directories(o.output/dir);
    Parameters p;
    check(std::isfinite(o.width) && o.width>=160 && o.width<=10000 &&
          std::isfinite(o.depth) && o.depth>=160 && o.depth<=4000 &&
          std::isfinite(o.duration) && o.duration>0 && o.duration<=3 &&
          std::isfinite(o.originX) && o.originX>=0 && o.originX+o.width<=10000,
          "Require a domain within the original 10 km by 4 km and a duration in (0,3] s");
    check(std::abs(o.width/2-std::round(o.width/2))<1e-10 &&
          std::abs(o.depth/2-std::round(o.depth/2))<1e-10,
          "Domain extents must be multiples of the 2 m grid spacing");
    p.nx=static_cast<int>(std::llround(o.width/2))+1;
    p.nz=static_cast<int>(std::llround(o.depth/2))+1;
    p.spacing=2; p.relief=0; p.flattenDepth=1000;
    check(std::isfinite(o.frequency) && o.frequency>0 && o.frequency<=1e6,"Invalid source frequency");
    p.frequency=o.frequency; p.duration=o.duration; p.cfl=.6; p.maxSteps=0;
    p.source.kind=SeisWave::SourceKind::IsotropicMoment; p.momentPerLength=2e6; p.sourceX=-1; p.sourceZ=15;
    // Keep the original global receiver line (20+40*r m), including its phase.
    const int firstTrace=std::max(0,static_cast<int>(std::ceil((o.originX-20)/40)));
    const double firstReceiver=20+40*firstTrace-o.originX;
    check(std::abs(firstReceiver/2-std::round(firstReceiver/2))<1e-10,"Receiver line must align with the grid");
    p.receiverStart=static_cast<int>(std::llround(firstReceiver/2)); p.receiverStride=20;
    p.receiverCount=static_cast<int>(std::floor((o.width-firstReceiver)/40))+1;
    p.receiverDepth=0; p.sampleIntervalMs=.5;
    p.absorbing=true; p.spongeX=p.spongeZ=64; p.spongeStrength=3; p.layered=true;
    const bool terrain=o.group%2==1, lvl=o.group>=2;
    // Trial Q values, not estimates from field measurements. The background
    // materials have the same Q in the with/without low-velocity-layer cases.
    p.layers=lvl?std::vector<Layer>{{20,1000,500,1800,20,12.5},{40,1600,800,2000,40,25},{40,2300,1250,2200,80,50},
                      {900,3000,1700,2400,150,90},{1000,3500,2000,2550,200,120},{0,4200,2400,2700,250,150}}
                :std::vector<Layer>{{1000,3000,1700,2400,150,90},{1000,3500,2000,2550,200,120},{0,4200,2400,2700,250,150}};
    p.attenuation.enabled=o.attenuation; p.attenuation.mechanisms=3;
    p.attenuation.referenceFrequency=p.frequency;
    p.attenuation.minFrequency=.2*p.frequency; p.attenuation.maxFrequency=3*p.frequency;
    p.importedSurface=terrain; p.surfaceDatum=1481.322148135886;
    std::string error;
    if(terrain && !o.test) {
        check(readSurfaceProfile(o.terrain,p.surfaceSamples,error),error);
        // Retain the complete input profile around the selected domain so that
        // cropping does not change endpoint slopes or interior interpolation.
        for(auto& sample:p.surfaceSamples) sample[0]-=o.originX;
    }
    // Explicit small end-to-end runner validation; never used for production.
    if(o.test) {
        p.nx=81; p.nz=81; p.flattenDepth=100; p.duration=.01;
        p.receiverStart=10; p.receiverCount=4; p.spongeX=p.spongeZ=8;
        if(terrain) p.surfaceSamples={{0,p.surfaceDatum},{80,p.surfaceDatum+2},{160,p.surfaceDatum}};
    }
    const ComputeSettings3D compute{1,0};
    Simulation s; const auto begin=Clock::now();
    try {
        check(s.initialize(p,error,BackendKind3D::Cuda,compute),error);
        const auto &g=s.grid(); const auto source=g.coordinates[g.index(p.nx/2+ghost,p.sourceZ+ghost)];
        check(s.backend().kind==BackendKind3D::Cuda && s.backend().cudaDevice==0,"Unexpected compute device");
        if(!o.test) {
            if(!o.attenuation && o.width==10000 && o.depth==4000 && o.duration==3)
                check(s.totalSteps()==(terrain?17992:14850),"Final report and solver step count differ");
            for(int depth:{500,1000}) if(depth<p.nz) for(int x:{0,p.nx/2,p.nx-1})
                check(std::abs(g.coordinates[g.index(x+ghost,depth+ghost)][1]-depth*2.)<1e-9,"Target reflector not horizontal");
        }
        std::ofstream geometry(o.output/"records/receiver_geometry.csv"); geometry<<std::setprecision(17)
            <<"trace_1based,receiver_0based,x_m,z_m,offset_m,in_side_sponge,main_analysis,source_x_m,source_z_m\n";
        for(std::size_t r=0;r<s.receivers().size();++r) {
            const auto &receiver=s.receivers()[r]; const double x=receiver.position[0];
            const bool edge=x<p.spongeX*p.spacing || x>(p.nx-1-p.spongeX)*p.spacing;
            geometry<<r+1<<','<<r<<','<<x<<','<<receiver.position[1]<<','<<x-source[0]<<','<<edge<<','<<!edge<<','<<source[0]<<','<<source[1]<<'\n';
        }
        geometry.close(); check(bool(geometry),"Receiver geometry write failed");
        const auto budget=Detail::memoryBudget(p,BackendKind3D::Cuda);
        std::ostringstream metadata; metadata<<std::setprecision(17)
            <<"{\"group_index\":"<<o.group<<",\"terrain\":"<<(terrain?"true":"false")<<",\"low_velocity_layer\":"<<(lvl?"true":"false")
            <<",\"validation_only\":"<<(o.test?"true":"false")<<",\"nx\":"<<p.nx<<",\"nz\":"<<p.nz
            <<",\"spacing_m\":2,\"frequency_hz\":"<<p.frequency<<",\"duration_s\":"<<p.duration<<",\"cfl\":0.6,\"moment_per_length_N\":2000000"
            <<",\"source_x_m\":"<<source[0]<<",\"source_z_m\":"<<source[1]<<",\"source_depth_eta_m\":30,\"flatten_depth_m\":"<<p.flattenDepth
            <<",\"surface_datum_m\":"<<p.surfaceDatum<<",\"cuda_device\":0,\"backend\":\"CUDA\",\"precision\":\"Float64\",\"record_dt_s\":0.0005"
            <<",\"receiver_count\":"<<p.receiverCount<<",\"receiver_spacing_m\":40,\"segy_revision\":1,\"segy_format\":5,\"segy_gain\":1"
            <<",\"global_origin_x_m\":"<<o.originX<<",\"width_m\":"<<o.width<<",\"depth_m\":"<<o.depth
            <<",\"attenuation\":"<<(o.attenuation?"true":"false")<<",\"mechanisms\":3,\"reference_hz\":"<<p.frequency
            <<",\"fit_min_hz\":"<<p.attenuation.minFrequency<<",\"fit_max_hz\":"<<p.attenuation.maxFrequency
            <<",\"budget_host_bytes\":"<<budget.hostBytes<<",\"budget_device_bytes\":"<<budget.deviceBytes
            <<",\"q_values_are_field_calibrated\":false,\"layers\":[";
        for(std::size_t i=0;i<p.layers.size();++i){const auto& l=p.layers[i];if(i)metadata<<',';
            metadata<<"{\"thickness_m\":"<<l.thickness<<",\"vp_m_s\":"<<l.vp<<",\"vs_m_s\":"<<l.vs
                    <<",\"density_kg_m3\":"<<l.density<<",\"qp\":"<<l.qp<<",\"qs\":"<<l.qs<<'}';}
        metadata<<"]}\n";
        textFile(o.output/"run_metadata.json",metadata.str());
        status(s,o.output,"saving_model",seconds(begin));
        std::cout<<"Initialized group="<<o.group<<" steps="<<s.totalSteps()<<" dt="<<std::setprecision(17)<<s.dt()<<std::endl;
        check(writeModelVts(s,o.output/"model/model.vts",compute,error),error);
        std::vector<double> targets=o.test?std::vector<double>{.005,.01}:std::vector<double>{.25,.5,.75,1,1.25,1.5,2,2.5,3};
        if(!o.test){std::erase_if(targets,[&](double t){return t>p.duration;});
            if(targets.empty() || targets.back()<p.duration)targets.push_back(p.duration);}
        SnapshotExportOptions snapshot; snapshot.components.fill(false); snapshot.components[0]=snapshot.components[1]=true; snapshot.includeMaterial=false;
        std::ofstream times(o.output/"snapshots/times.csv"); times<<std::setprecision(17)<<"file,target_time_s,actual_time_s,step\n";
        std::size_t next=0;
        while(!s.completed()) {
            const int target=std::min(s.totalSteps(),static_cast<int>(std::ceil(targets[next]/s.dt()-1e-9)));
            const int steps=std::min(100,target-s.step());
            check(steps>0,"Invalid snapshot schedule");
            check(s.advance(steps,error),error);
            check(s.diagnostics().finite,"Nonfinite wavefield");
            status(s,o.output,"running",seconds(begin));
            std::cout<<"step="<<s.step()<<'/'<<s.totalSteps()<<" time_s="<<s.step()*s.dt()<<" elapsed_s="<<seconds(begin)
                     <<" mean_ms="<<s.diagnostics().totalStepMs/s.step()<<std::endl;
            if(s.step()==target) {
                const auto name="wavefield_"+std::to_string(next+1)+".vts";
                status(s,o.output,"saving_snapshot",seconds(begin));
                check(writeSnapshot(s,o.output/"snapshots"/name,error,snapshot),error);
                times<<name<<','<<targets[next]<<','<<s.step()*s.dt()<<','<<s.step()<<'\n'; times.flush(); check(bool(times),"Snapshot index write failed");
                records(s,o.output,false); ++next;
            }
        }
        const auto expected=static_cast<std::size_t>(std::llround(p.duration/s.recordDt()))+1;
        check(s.step()==s.totalSteps() && next==targets.size(),"Run truncated");
        for(const auto &r:s.receivers()) check(r.vx.size()==expected && r.vz.size()==expected,"Record duration/sample count mismatch");
        check(s.diagnostics().peakEnergy>0,"Zero wavefield");
        status(s,o.output,"saving_records",seconds(begin)); records(s,o.output,true);
        status(s,o.output,"complete",seconds(begin));
        textFile(o.output/"COMPLETE.txt","Full duration reached; finite fields; record counts and SEG-Y sample readback verified. Numerical accuracy remains subject to refinement and boundary validation.\n");
        std::cout<<"COMPLETE group="<<o.group<<" elapsed_s="<<seconds(begin)<<std::endl;
    } catch(const std::exception &ex) {
        if(s.initialized()) {
            try { records(s,o.output,false); status(s,o.output,"failed",seconds(begin)); } catch(...) {}
        }
        textFile(o.output/"FAILED.txt",ex.what()); throw;
    }
}
} // namespace
int main(int argc,char **argv) {
    try {
        Options o;
        for(int i=1;i<argc;++i) {
            const std::string key=argv[i];
            if(key=="--self-test") {o.test=true; continue;}
            check(i+1<argc,"Missing option value"); const std::string value=argv[++i];
            const auto path=std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(value.data()),value.size()));
            if(key=="--output") o.output=path;
            else if(key=="--terrain") o.terrain=path;
            else if(key=="--group") o.group=std::stoi(value);
            else if(key=="--frequency") o.frequency=std::stod(value);
            else if(key=="--width-m") o.width=std::stod(value);
            else if(key=="--depth-m") o.depth=std::stod(value);
            else if(key=="--duration-s") o.duration=std::stod(value);
            else if(key=="--origin-x-m") o.originX=std::stod(value);
            else if(key=="--q") {check(value=="on"||value=="off","Q must be on or off");o.attenuation=value=="on";}
            else throw std::runtime_error("Unknown option: "+key);
        }
        run(o); return 0;
    } catch(const std::exception &ex) {std::cerr<<"FAILED: "<<ex.what()<<std::endl; return 1;}
}
