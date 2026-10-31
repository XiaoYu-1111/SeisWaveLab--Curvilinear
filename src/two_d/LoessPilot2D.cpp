#include "Seis2D/ModelIo2D.hpp"
#include "SeisCommon/AtomicFile.hpp"
#include "SeisCommon/SegyIo.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
using namespace Seis2D::Curvilinear;
namespace {
using Clock=std::chrono::steady_clock;
void check(bool okay,const std::string& why){if(!okay)throw std::runtime_error(why);}
std::filesystem::path path(const std::string& s){return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(s.data()),s.size()));}
void atomic(const std::filesystem::path& file,const std::string& value){auto pending=file;pending+=".pending";{std::ofstream o(pending);o<<value;o.close();check(bool(o),"Cannot save "+file.string());}SeisWave::replaceCompletedFile(pending,file);}
struct Config {std::string id;Parameters p;std::vector<double> snapshots;double left{},right{},bottom{},probeSpacing{},probeDtMs{};};
Config read(const std::filesystem::path& file){
    std::ifstream in(file);check(bool(in),"Cannot open configuration");
    auto take=[&](const char* wanted){std::string key;check(bool(in>>key)&&key==wanted,std::string("Expected configuration key: ")+wanted);};
    Config c;take("LOESS_PILOT");int version;in>>version;check(version==1,"Unknown configuration version");take("id");in>>c.id;
    check(!c.id.empty()&&std::all_of(c.id.begin(),c.id.end(),[](unsigned char ch){return std::isalnum(ch)||ch=='_'||ch=='-';}),"Unsafe case ID");
    auto& p=c.p;take("grid");double width,depth;in>>width>>depth>>p.spacing;
    const auto cell=[&](double metres){const double value=metres/p.spacing;check(std::isfinite(value)&&std::abs(value-std::round(value))<1e-8,"Coordinate must align with spacing");return static_cast<int>(std::llround(value));};
    check(p.spacing>0&&width>0&&depth>0,"Invalid grid");p.nx=cell(width)+1;p.nz=cell(depth)+1;
    take("time");in>>p.frequency>>p.duration>>p.cfl>>p.sampleIntervalMs;p.maxSteps=0;
    take("source");double sx,sz;in>>sx>>sz>>p.forcePerLength>>p.sourceSigmaM>>p.sourceRadiusM;p.sourceX=cell(sx);p.sourceZ=cell(sz);p.source.kind=SeisWave::SourceKind::DirectionalForce;p.source.direction={0,0,1};
    take("receivers");double start,stride,rz;in>>start>>p.receiverCount>>stride>>rz;p.receiverStart=cell(start);p.receiverStride=cell(stride);p.receiverDepth=cell(rz);
    take("terrain");std::string terrain;in>>terrain>>p.surfaceDatum>>p.flattenDepth;p.relief=0;p.importedSurface=true;std::string error;check(readSurfaceProfile(path(terrain),p.surfaceSamples,error),error);
    take("layers");int layers;in>>layers;check(layers>=1&&layers<=16,"Invalid layer count");p.layers.clear();p.layered=true;
    for(int i=0;i<layers;++i){Layer l;check(bool(in>>l.thickness>>l.vp>>l.vs>>l.density>>l.qp>>l.qs),"Incomplete layer");p.layers.push_back(l);}
    take("attenuation");int enabled;in>>enabled>>p.attenuation.mechanisms>>p.attenuation.minFrequency>>p.attenuation.maxFrequency>>p.attenuation.referenceFrequency;check(enabled==0||enabled==1,"Bad attenuation flag");p.attenuation.enabled=enabled!=0;
    take("boundary");double band;in>>band>>p.cpml.targetReflection>>p.cpml.kappaMax>>p.cpml.frequency;p.absorbing=true;p.externalAbsorption=true;p.absorber=Absorber::CfsPml;p.spongeX=p.spongeZ=cell(band);
    take("snapshots");int n;in>>n;check(n>=0&&n<=100,"Invalid snapshot count");for(int i=0;i<n;++i){double t;in>>t;check(t>0&&t<=p.duration&&(c.snapshots.empty()||t>c.snapshots.back()),"Invalid snapshot schedule");c.snapshots.push_back(t);}
    take("probes");in>>c.left>>c.right>>c.bottom>>c.probeSpacing>>c.probeDtMs;check(c.probeDtMs>=0&&c.probeSpacing>0,"Invalid probe setup");
    check(bool(in),"Truncated configuration");std::string extra;check(!(in>>extra),"Unexpected configuration trailer");p.precision=Precision::Float64;return c;
}
struct Probe{std::array<int,2> node;int line;double nx{},nz{},dl{};};
std::vector<Probe> probes(const Config& c,const Simulation& s){
    if(c.probeDtMs==0)return {};const auto& p=c.p;const auto& g=s.grid();const auto cell=[&](double x){return static_cast<int>(std::llround(x/p.spacing));};
    const int a=cell(c.left),b=cell(c.right),bottom=cell(c.bottom),step=cell(c.probeSpacing);check(a>=0&&b<p.nx&&a<b&&bottom<p.nz&&step>0,"Probe box outside model");
    std::vector<Probe> result;
    for(int line=0;line<3;++line){const auto begin=result.size();if(line<2){for(int k=0;k<=bottom;k+=step)result.push_back({{line?b:a,k},line,line?1.:-1.,0,0});}
        else for(int i=a;i<=b;i+=step)result.push_back({{i,bottom},line,0,1,0});
        const auto end=result.size();check(end-begin>=2,"Too few probe nodes");
        for(auto n=begin;n<end;++n){const auto location=[&](std::size_t q){const auto& node=result[q].node;return g.coordinates[g.modelIndex(node[0],node[1])];};
            const auto here=location(n),before=location(n==begin?n:n-1),after=location(n+1==end?n:n+1);
            result[n].dl=.5*(std::hypot(here[0]-before[0],here[1]-before[1])+std::hypot(after[0]-here[0],after[1]-here[1]));
            if(line==2){const double tx=after[0]-before[0],tz=after[1]-before[1],len=std::hypot(tx,tz);result[n].nx=-tz/len;result[n].nz=tx/len;}
        }
    }return result;
}
void records(const Simulation& s,const std::filesystem::path& root){
    std::string error;check(writeReceivers(s,root/"records/receivers_float64.csv",error),error);
    const auto& p=s.parameters();const auto source=s.grid().coordinates[s.grid().modelIndex(p.sourceX,p.sourceZ)];
    SeisWave::RecordDataView view;view.dt=s.recordDt();view.components={"Vx","Vz"};double peak=0;
    const auto expected=static_cast<std::size_t>(std::llround(p.duration/s.recordDt()))+1;
    for(const auto& r:s.receivers()){check(r.vx.size()==expected&&r.vz.size()==expected,"Incomplete record");SeisWave::TraceView trace;trace.position={r.position[0],0,r.position[1]};trace.source={source[0],0,source[1]};trace.channels[0]=r.vx;trace.channels[1]=r.vz;view.traces.push_back(trace);
        for(const auto* values:{&r.vx,&r.vz})for(double v:*values){check(std::isfinite(v),"Nonfinite raw record");peak=std::max(peak,std::abs(v));}}
    check(peak>1e-30,"All records are effectively zero");SeisWave::SegyExportOptions options;options.profile=SeisWave::SegyExportProfile::CompatibleIeee32;options.sampleGain=1;
    for(int f=0;f<2;++f){const auto file=root/"records"/(view.components[f]+".sgy");check(SeisWave::writeSegy(view,f,file,error,options),error);SeisWave::ImportedRecords read;check(SeisWave::readSegy(file,read,error),error);
        check(read.traces.size()==view.traces.size()&&std::abs(read.dt-view.dt)<1e-12,"SEG-Y geometry/sample interval");
        for(std::size_t r=0;r<read.traces.size();++r){check(read.traces[r].samples.size()==expected,"SEG-Y samples");for(std::size_t t=0;t<expected;++t)check(read.traces[r].samples[t]==static_cast<double>(static_cast<float>(view.traces[r].channels[f][t])),"SEG-Y readback differs from Float32 cast");}}
}
}
int main(int argc,char** argv){std::filesystem::path root;bool outputOwned=false;std::unique_ptr<Simulation> sim;std::string id;const auto begin=Clock::now();try{
    std::filesystem::path config;bool validate=false;auto backend=BackendKind3D::Cuda;
    for(int i=1;i<argc;++i){const std::string key=argv[i];if(key=="--validate"){validate=true;continue;}check(i+1<argc,"Missing option value");const std::string value=argv[++i];
        if(key=="--config")config=path(value);else if(key=="--output")root=std::filesystem::absolute(path(value)).lexically_normal();else if(key=="--backend"){check(value=="cpu"||value=="cuda","Bad backend");backend=value=="cpu"?BackendKind3D::Cpu:BackendKind3D::Cuda;}else throw std::runtime_error("Unknown option: "+key);}
    check(!root.empty()&&!config.empty(),"Usage: seis2d_loess_pilot --config FILE --output OUT_SUBDIR [--validate] [--backend cpu|cuda]");
    const auto relative=root.lexically_relative(std::filesystem::path(SEIS3D_OUTPUT_ROOT));check(!relative.empty()&&relative!="."&&*relative.begin()!="..","Output must be below outer out");
    check(!std::filesystem::exists(root)||std::filesystem::is_empty(root),"Refusing to overwrite existing case");std::filesystem::create_directories(root);outputOwned=true;
    const auto c=read(config);id=c.id;const auto& p=c.p;for(const char* folder:{"model","records","snapshots","probes","geometry"})std::filesystem::create_directories(root/folder);
    std::filesystem::copy_file(config,root/"config.cfg");const ComputeSettings3D compute{1,0};sim=std::make_unique<Simulation>();std::string error;
    check(sim->initialize(p,error,backend,compute),error);check(sim->backend().kind==backend,"Unexpected backend fallback");
    const auto status=[&](const char* state){const auto& d=sim->diagnostics();std::ostringstream o;o<<std::setprecision(17)<<"{\"state\":\""<<state<<"\",\"case_id\":\""<<id<<"\",\"step\":"<<sim->step()<<",\"total_steps\":"<<sim->totalSteps()<<",\"time_s\":"<<sim->step()*sim->dt()<<",\"dt_s\":"<<sim->dt()<<",\"record_dt_s\":"<<sim->recordDt()<<",\"samples\":"<<sim->receivers().front().vz.size()<<",\"finite\":"<<(d.finite?"true":"false")<<",\"elapsed_s\":"<<std::chrono::duration<double>(Clock::now()-begin).count()<<",\"traction_relative\":"<<d.tractionRelative<<",\"max_velocity\":"<<d.maxVelocity<<"}\n";atomic(root/"status.json",o.str());};
    check(sim->totalSteps()*sim->dt()>=p.duration-1e-10,"Duration would be truncated");check(std::abs(sim->diagnostics().sourceWeightSum-1)<1e-12,"Physical source normalization");
    std::ostringstream meta;const auto& d=sim->diagnostics();meta<<std::setprecision(17)<<"{\"case_id\":\""<<id<<"\",\"nx\":"<<p.nx<<",\"nz\":"<<p.nz<<",\"spacing_m\":"<<p.spacing<<",\"min_jacobian\":"<<sim->grid().minJacobian<<",\"max_edge_m\":"<<sim->grid().maxEdge<<",\"max_metric\":"<<sim->grid().maxMetric<<",\"max_Q_fit_error\":"<<d.maxRelativeQError<<",\"source_weight_sum\":"<<d.sourceWeightSum<<",\"source_rms_radius_m\":"<<d.sourceRmsRadiusM<<",\"device_array_bytes\":"<<d.residentDeviceBytes<<",\"host_array_bytes\":"<<d.residentHostArrayBytes<<",\"duration_s\":"<<p.duration<<",\"dt_s\":"<<sim->dt()<<",\"total_steps\":"<<sim->totalSteps()<<",\"model_schema\":6,\"precision\":\"Float64\"}\n";atomic(root/"metadata.json",meta.str());status("initialized");
    if(validate){atomic(root/"READY.txt","Initialized and preflighted without time stepping.\n");status("validated_not_run");return 0;}
    check(writeModelVts(*sim,root/"model/model.vts",compute,error),error);ModelDocument doc;check(readModelVts(root/"model/model.vts",doc,error)&&doc.parameters==p,"Model roundtrip: "+error);
    std::ofstream geo(root/"geometry/receivers.csv");geo<<std::setprecision(17)<<"trace_1based,i,k,x_m,z_m,normal_x,normal_z,source_x_m,source_z_m\n";const auto src=sim->grid().coordinates[sim->grid().modelIndex(p.sourceX,p.sourceZ)];
    for(std::size_t r=0;r<sim->receivers().size();++r){const auto& rec=sim->receivers()[r];const auto normal=sim->grid().normals[sim->grid().modelIndex(rec.x,rec.z)];geo<<r+1<<','<<rec.x<<','<<rec.z<<','<<rec.position[0]<<','<<rec.position[1]<<','<<normal[0]<<','<<normal[1]<<','<<src[0]<<','<<src[1]<<'\n';}geo.close();check(bool(geo),"Geometry write");
    const auto locations=probes(c,*sim);std::vector<std::array<int,2>> nodes;for(const auto& loc:locations)nodes.push_back(loc.node);
    std::ofstream probeGeo(root/"probes/geometry.csv");probeGeo<<std::setprecision(17)<<"probe_id,line_id,i,k,x_m,z_m,normal_x,normal_z,line_weight_m\n";
    for(std::size_t n=0;n<locations.size();++n){const auto& loc=locations[n];const auto r=sim->grid().coordinates[sim->grid().modelIndex(loc.node[0],loc.node[1])];probeGeo<<n<<','<<loc.line<<','<<loc.node[0]<<','<<loc.node[1]<<','<<r[0]<<','<<r[1]<<','<<loc.nx<<','<<loc.nz<<','<<loc.dl<<'\n';}probeGeo.close();
    std::ofstream binary(root/"probes/fields_f64.bin",std::ios::binary),probeIndex(root/"probes/index.csv"),flux(root/"probes/flux.csv");
    const char magic[8]={'S','W','P','5','F','6','4','\n'};const std::uint32_t count=static_cast<std::uint32_t>(nodes.size());binary.write(magic,8);binary.write(reinterpret_cast<const char*>(&count),4);
    probeIndex<<std::setprecision(17)<<"frame,target_time_s,actual_time_s,step\n";flux<<std::setprecision(17)<<"actual_time_s,left_outward_W_per_m,right_outward_W_per_m,bottom_outward_W_per_m\n";
    const auto sample=[&](int frame,double target){std::vector<std::array<double,5>> values;check(sim->readProbes(nodes,values,error),error);const double t=sim->step()*sim->dt();binary.write(reinterpret_cast<const char*>(&t),8);binary.write(reinterpret_cast<const char*>(values.data()),static_cast<std::streamsize>(values.size()*5*sizeof(double)));
        double power[3]{};for(std::size_t n=0;n<values.size();++n){const auto& v=values[n];for(double value:v)check(std::isfinite(value),"Nonfinite compact probe");const double sx=-v[2]*v[0]-v[4]*v[1],sz=-v[4]*v[0]-v[3]*v[1];power[locations[n].line]+=(sx*locations[n].nx+sz*locations[n].nz)*locations[n].dl;}
        probeIndex<<frame<<','<<target<<','<<t<<','<<sim->step()<<'\n';flux<<t<<','<<power[0]<<','<<power[1]<<','<<power[2]<<'\n';};
    if(!nodes.empty())sample(0,0);std::size_t nextSnapshot=0;int nextProbe=1;
    std::ofstream times(root/"snapshots/index.csv");times<<std::setprecision(17)<<"file,target_time_s,actual_time_s,step\n";auto last=Clock::now();
    const auto at=[&](double t){return std::min(sim->totalSteps(),static_cast<int>(std::ceil(t/sim->dt()-1e-9)));};
    while(!sim->completed()){
        int target=sim->totalSteps();if(nextSnapshot<c.snapshots.size())target=std::min(target,at(c.snapshots[nextSnapshot]));
        if(!nodes.empty()&&nextProbe*c.probeDtMs*.001<=p.duration+1e-12)target=std::min(target,at(nextProbe*c.probeDtMs*.001));
        check(target>sim->step(),"Non-advancing event schedule");check(sim->advance(std::min(16,target-sim->step()),error),error);
        if(!nodes.empty()&&nextProbe*c.probeDtMs*.001<=p.duration+1e-12&&sim->step()==at(nextProbe*c.probeDtMs*.001)){sample(nextProbe,nextProbe*c.probeDtMs*.001);++nextProbe;}
        if(nextSnapshot<c.snapshots.size()&&sim->step()==at(c.snapshots[nextSnapshot])){const auto name="wavefield_"+std::to_string(nextSnapshot+1)+".vts";SnapshotExportOptions options;options.components.fill(false);options.components[0]=options.components[1]=true;options.includeMaterial=false;
            status("exporting_snapshot");check(writeSnapshot(*sim,root/"snapshots"/name,error,options),error);times<<name<<','<<c.snapshots[nextSnapshot]<<','<<sim->step()*sim->dt()<<','<<sim->step()<<'\n';times.flush();++nextSnapshot;}
        if(std::chrono::duration<double>(Clock::now()-last).count()>=1){status("running");last=Clock::now();binary.flush();probeIndex.flush();flux.flush();}
    }
    check(nextSnapshot==c.snapshots.size(),"Missing wavefield snapshots");binary.close();probeIndex.close();flux.close();times.close();check(bool(binary)&&bool(probeIndex)&&bool(flux)&&bool(times),"Probe/snapshot index write failed");
    status("exporting_records");records(*sim,root);check(sim->diagnostics().finite,"Final nonfinite diagnostics");
    atomic(root/"COMPLETE.txt","Full duration, finite fields, requested snapshots, compact probes, model roundtrip and SEG-Y readback verified. This is not a convergence certificate.\n");status("complete");std::cout<<id<<" COMPLETE, steps="<<sim->step()<<", compute_s="<<sim->diagnostics().totalStepMs/1000<<'\n';return 0;
}catch(const std::exception& ex){std::cerr<<ex.what()<<'\n';if(outputOwned){try{atomic(root/"FAILED.txt",std::string(ex.what())+"\n");atomic(root/"status.json","{\"state\":\"failed\"}\n");}catch(...){}}return 1;}}
