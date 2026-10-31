#include "AppRuntime.hpp"
#include "Presentation3D.hpp"
#include "Seis2D/ModelIo2D.hpp"
#include "SeisCommon/AtomicFile.hpp"
#include "SeisCommon/SegyIo.hpp"
#include "imgui.h"
#include "implot.h"
#include <GLFW/glfw3.h>
#include <algorithm>
#include <chrono>
#include <cctype>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <unordered_map>

using namespace Seis2D::Curvilinear;
namespace {
using Clock = std::chrono::steady_clock;
struct Case { std::string id; Parameters p; };
void check(bool value, const std::string &message) { if (!value) throw std::runtime_error(message); }
std::string quote(const std::string &s) {
    std::string out="\"";
    for(char c:s) { if(c=='"'||c=='\\') out+='\\'; if(c=='\n')out+="\\n";else if(c=='\r')out+="\\r";else out+=c; }
    return out+'"';
}
void atomicText(const std::filesystem::path &path,const std::string &text) {
    auto pending=path; pending+=".pending";
    std::ofstream out(pending);out<<text;out.close();check(bool(out),"Cannot save "+path.string());
    SeisWave::replaceCompletedFile(pending,path);
}
bool safeName(const std::string &s) {
    return !s.empty() && std::all_of(s.begin(),s.end(),[](unsigned char c){return std::isalnum(c)||c=='_';});
}
std::vector<Case> readQueue(const std::filesystem::path &path,std::string &session) {
    std::ifstream in(path); std::string magic; int version=0;
    check(bool(in>>magic>>version>>session)&&magic=="SEIS_MASW_QUEUE"&&version==1&&safeName(session),"Invalid MASW queue header");
    std::vector<Case> cases; std::string token;
    while(in>>token) {
        check(token=="CASE","Expected CASE in MASW queue"); Case c; int layerCount=0;auto &p=c.p;
        check(bool(in>>c.id>>p.nx>>p.nz>>p.spacing>>p.frequency>>p.duration>>p.cfl>>p.maxSteps
            >>p.sourceX>>p.sourceZ>>p.receiverStart>>p.receiverCount>>p.receiverStride>>p.receiverDepth
            >>p.sampleIntervalMs>>p.spongeX>>p.spongeZ>>p.spongeStrength>>p.forcePerLength>>layerCount),"Incomplete MASW queue case");
        check(safeName(c.id)&&layerCount>0&&layerCount<=16,"Invalid case name/layer count");
        p.relief=0;p.layered=true;p.absorbing=true;p.source.direction={0,0,1};p.source.kind=SeisWave::SourceKind::DirectionalForce;
        p.layers.clear();
        for(int i=0;i<layerCount;++i) {Layer l;check(bool(in>>l.thickness>>l.vp>>l.vs>>l.density),"Incomplete layer");p.layers.push_back(l);}
        cases.push_back(std::move(c));
    }
    check(in.eof()&&!cases.empty(),"Empty or malformed MASW queue");return cases;
}
SeisWave::RecordDataView recordsView(const Simulation &s) {
    SeisWave::RecordDataView data;data.dt=s.recordDt();data.components={"Vx","Vz"};
    const auto &p=s.parameters();const auto &g=s.grid();
    const auto source=g.coordinates[g.index((p.sourceX<0?p.nx/2:p.sourceX)+ghost,p.sourceZ+ghost)];
    for(const auto &r:s.receivers()) {SeisWave::TraceView t;t.position={r.position[0],0,r.position[1]};t.source={source[0],0,source[1]};t.channels[0]=r.vx;t.channels[1]=r.vz;data.traces.push_back(t);}
    return data;
}
void saveRecords(const Simulation &s,const std::filesystem::path &folder,bool complete) {
    std::string error;
    check(writeReceivers(s,folder/"records"/(complete?"receivers_float64.csv":"receivers_partial.csv"),error),error);
    if(!complete)return;
    const auto view=recordsView(s);SeisWave::SegyExportOptions options;
    options.profile=SeisWave::SegyExportProfile::CompatibleIeee32;options.sampleGain=1;
    for(int component=0;component<2;++component) {
        const auto file=folder/"records"/(view.components[component]+".sgy");
        check(SeisWave::writeSegy(view,component,file,error,options),error);
        SeisWave::ImportedRecords loaded;check(SeisWave::readSegy(file,loaded,error),error);
        check(loaded.dt==view.dt&&loaded.traces.size()==view.traces.size(),"SEG-Y sampling/trace readback mismatch");
        for(std::size_t r=0;r<view.traces.size();++r) {
            const auto raw=view.traces[r].channels[component];
            check(raw.size()==loaded.traces[r].samples.size(),"SEG-Y sample count mismatch");
            for(std::size_t i=0;i<raw.size();++i)
                check(loaded.traces[r].samples[i]==static_cast<double>(static_cast<float>(raw[i])),"SEG-Y sample value mismatch");
        }
    }
}
struct Session {
    std::vector<Case> cases;std::size_t current{};std::unique_ptr<Simulation> sim;
    std::filesystem::path root,folder;std::ofstream times;std::vector<double> targets;
    std::size_t nextSnapshot{};ComputeSettings3D settings{1,0};BackendKind3D backend{BackendKind3D::Cuda};
    bool running{true},finished{},caseSaved{},failed{};std::string phase="initializing",error;
    Clock::time_point started=Clock::now(),lastStatus=Clock::now();
    void status() {
        std::ostringstream out;out<<std::setprecision(17)<<"{\"state\":"<<quote(phase)<<",\"case_id\":"<<quote(cases[current].id)
            <<",\"case_index\":"<<current<<",\"case_count\":"<<cases.size()<<",\"running\":"<<(running?"true":"false")
            <<",\"error\":"<<quote(error)<<",\"elapsed_s\":"<<std::chrono::duration<double>(Clock::now()-started).count();
        if(sim&&sim->initialized())out<<",\"step\":"<<sim->step()<<",\"total_steps\":"<<sim->totalSteps()<<",\"time_s\":"<<sim->step()*sim->dt()
            <<",\"dt_s\":"<<sim->dt()<<",\"record_dt_s\":"<<sim->recordDt()<<",\"samples\":"<<sim->receivers().front().vz.size()
            <<",\"energy\":"<<sim->diagnostics().energy<<",\"max_velocity\":"<<sim->diagnostics().maxVelocity
            <<",\"finite\":"<<(sim->diagnostics().finite?"true":"false")<<",\"backend\":"<<quote(sim->backend().deviceName);
        out<<"}\n";atomicText(root/"SESSION_STATUS.json",out.str());if(!folder.empty())atomicText(folder/"status.json",out.str());lastStatus=Clock::now();
    }
    void beginCase() {
        folder=root/cases[current].id;check(!std::filesystem::exists(folder),"Refusing to overwrite existing case");
        for(const char *name:{"model","records","snapshots","figures"})std::filesystem::create_directories(folder/name);
        phase="initializing";sim.reset();status();sim=std::make_unique<Simulation>();
        check(sim->initialize(cases[current].p,error,backend,settings),error);
        phase="saving_model";status();check(writeModelVts(*sim,folder/"model/model.vts",settings,error),error);
        ModelDocument document;check(readModelVts(folder/"model/model.vts",document,error)&&document.parameters==sim->parameters(),"Model VTS readback mismatch: "+error);
        const auto &p=sim->parameters();const double sourceX=(p.sourceX<0?p.nx/2:p.sourceX)*p.spacing;
        std::ostringstream meta;meta<<std::setprecision(17)<<"{\"case_id\":"<<quote(cases[current].id)<<",\"purpose\":\"validation\",\"nx\":"<<p.nx<<",\"nz\":"<<p.nz
            <<",\"spacing_m\":"<<p.spacing<<",\"frequency_hz\":"<<p.frequency<<",\"duration_s\":"<<p.duration<<",\"cfl\":"<<p.cfl
            <<",\"source_x_m\":"<<sourceX<<",\"source_depth_m\":"<<p.sourceZ*p.spacing<<",\"force_N_per_m\":"<<p.forcePerLength
            <<",\"receiver_count\":"<<p.receiverCount<<",\"receiver_spacing_m\":"<<p.receiverStride*p.spacing
            <<",\"sponge_x_m\":"<<std::min(p.spongeX,p.nx/3)*p.spacing<<",\"sponge_z_m\":"<<std::min(p.spongeZ,p.nz/3)*p.spacing<<"}\n";
        atomicText(folder/"run_metadata.json",meta.str());
        std::ofstream geometry(folder/"records/receiver_geometry.csv");geometry<<std::setprecision(17)<<"trace_1based,x_m,z_m,offset_m\n";
        for(std::size_t i=0;i<sim->receivers().size();++i) {const auto &r=sim->receivers()[i];geometry<<i+1<<','<<r.position[0]<<','<<r.position[1]<<','<<r.position[0]-sourceX<<'\n';}
        geometry.close();check(bool(geometry),"Geometry write failed");
        targets.clear();for(double t:{.1,.2,.4,.6,.8,1.,1.5,2.,2.5})if(t<p.duration-1e-10)targets.push_back(t);
        if(targets.empty()&&p.duration>.005)targets.push_back(p.duration/2);targets.push_back(p.duration);
        times.open(folder/"snapshots/times.csv");times<<std::setprecision(17)<<"file,target_time_s,actual_time_s,step\n";
        nextSnapshot=0;caseSaved=false;phase="running";status();
    }
    int nextTarget() const {return std::min(sim->totalSteps(),static_cast<int>(std::ceil(targets[nextSnapshot]/sim->dt()-1e-9)));}
    void snapshot() {
        phase="saving_snapshot";status();SnapshotExportOptions options;options.components.fill(false);options.components[0]=options.components[1]=true;options.includeMaterial=false;
        const auto name="wavefield_"+std::to_string(nextSnapshot+1)+".vts";
        check(writeSnapshot(*sim,folder/"snapshots"/name,error,options),error);
        times<<name<<','<<targets[nextSnapshot]<<','<<sim->step()*sim->dt()<<','<<sim->step()<<'\n';times.flush();check(bool(times),"Snapshot index write failed");
        ++nextSnapshot;phase="running";status();
    }
    void finishCase() {
        phase="saving_records";status();const auto &p=sim->parameters();
        const auto count=static_cast<std::size_t>(std::llround(p.duration/sim->recordDt()))+1;
        check(sim->completed()&&sim->step()==sim->totalSteps()&&std::abs(sim->step()*sim->dt()-p.duration)<1e-10,"Truncated simulation");
        for(const auto &r:sim->receivers())check(r.vx.size()==count&&r.vz.size()==count,"Truncated record");
        check(sim->diagnostics().finite&&sim->diagnostics().peakEnergy>0,"Nonfinite or zero wavefield");
        saveRecords(*sim,folder,true);times.close();
        atomicText(folder/"COMPLETE.txt","Complete duration, finite fields, raw records, model and SEG-Y readback verified. Numerical accuracy still requires phase/boundary/convergence analysis.\n");
        caseSaved=true;phase="case_complete";status();
    }
    void advance() {
        if(failed||finished||!running)return;
        if(!sim) {beginCase();return;}
        if(caseSaved) {
            if(current+1==cases.size()) {finished=true;running=false;phase="validation_data_complete";status();return;}
            ++current;folder.clear();sim.reset();beginCase();return;
        }
        const auto begin=Clock::now();
        while(!sim->completed()&&sim->step()<nextTarget()) {
            check(sim->advance(1,error),error);
            if(std::chrono::duration<double,std::milli>(Clock::now()-begin).count()>24)break;
        }
        if(sim->step()==nextTarget())snapshot();
        if(sim->completed())finishCase();
        if(std::chrono::duration<double>(Clock::now()-lastStatus).count()>=1)status();
    }
};
void heatmap(const char *title,const std::vector<float> &values,int rows,int cols,double length,double depth,double lo,double hi,int cmap,float height) {
    static std::unordered_map<std::string,float> correction;
    height=std::max(100.f,height+correction[title]);
    ImPlot::PushColormap(cmap);
    if(ImPlot::BeginPlot(title,{-1,height},ImPlotFlags_Equal)) {
        ImPlot::SetupAxes("X (m)","Depth (m)",0,ImPlotAxisFlags_Invert);
        ImPlot::SetupAxesLimits(0,length,0,depth,ImGuiCond_Always);
        ImPlot::PlotHeatmap("##field",values.data(),rows,cols,lo,hi,nullptr,{0,0},{length,depth});
        const auto size=ImPlot::GetPlotSize();
        // Axis-label margins depend on DPI and font. Correct the NEXT frame's
        // height from the actual plot rectangle, preserving equal m/pixel.
        correction[title]=std::clamp(correction[title]+static_cast<float>(size.x*depth/length)-size.y,-50.f,220.f);
        ImPlot::EndPlot();
    }
    ImPlot::PopColormap();
}
int selfTest() {
    for(auto backend:{BackendKind3D::Cpu,BackendKind3D::Cuda}) {
        if(backend==BackendKind3D::Cuda&&!Seis3D::Curvilinear::queryBackend3D(backend).available)continue;
        Session session;session.backend=backend;Case c;c.id=backend==BackendKind3D::Cpu?"CPU":"CUDA";
        auto &p=c.p;p.nx=p.nz=41;p.spacing=1;p.relief=0;p.sourceZ=2;p.receiverCount=3;p.duration=.01;p.maxSteps=0;p.sampleIntervalMs=.5;p.layered=true;p.layers={{0,550,250,1550}};
        session.cases={c};session.root=SeisWave::Ui::outputRoot()/"validation"/("masw-live-"+c.id+"-"+std::to_string(Clock::now().time_since_epoch().count()));std::filesystem::create_directories(session.root);
        while(!session.finished)session.advance();
        check(std::filesystem::exists(session.folder/"COMPLETE.txt")&&session.sim->receivers().front().vz.size()==21,"MASW live export self-test failed");
        std::cout<<"PASS MASW live: "<<c.id<<" model/snapshots/21 samples/raw CSV/IEEE32 SEG-Y readback/status\n";
    }
    return 0;
}
} // namespace
int main(int argc,char **argv) {
    if(argc>1&&std::string(argv[1])=="--version"){std::cout<<"SeisWaveLab_MASW v"<<SeisWave::Ui::applicationVersion()<<'\n';return 0;}
    if(argc>1&&std::string(argv[1])=="--self-test") {try{return selfTest();}catch(const std::exception &e){std::cerr<<e.what()<<'\n';return 1;}}
    Session session;
    try {
        std::string name;const auto config=SeisWave::Ui::outputRoot()/"surface_wave_masw/live_session.cfg";
        session.cases=readQueue(config,name);session.root=SeisWave::Ui::outputRoot()/"surface_wave_masw/runs"/name;
        check(!std::filesystem::exists(session.root),"Refusing to overwrite an existing session; choose a new session name");
        std::filesystem::create_directories(session.root);std::filesystem::copy_file(config,session.root/"session.cfg");
        check(Seis3D::Curvilinear::queryBackend3D(BackendKind3D::Cuda).available,"CUDA is unavailable; refusing an unintended long CPU run");
        SeisWave::Ui::AppRuntime runtime;std::string error;check(runtime.open("SeisWave Lab | MASW live validation | 20 Hz",false,error),error);
        Seis3D::Curvilinear::Ui::setLanguage3D(1);Seis3D::Curvilinear::Ui::applyTheme3D(9);
        SeisWave::Ui::setInterfaceFontSize(20);
        bool fixedScale=false;double range=1e-6,peak=1e-30;std::vector<float> frame,small,material;int rows=0,cols=0;
        auto lastFrame=Clock::now()-std::chrono::seconds(1);int previousCase=-1;
        while(!glfwWindowShouldClose(runtime.window())) {
            runtime.begin();const auto *vp=ImGui::GetMainViewport();ImGui::SetNextWindowPos(vp->WorkPos);ImGui::SetNextWindowSize(vp->WorkSize);
            ImGui::Begin("MASW实时波场与自动保存",nullptr,ImGuiWindowFlags_NoMove|ImGuiWindowFlags_NoResize|ImGuiWindowFlags_NoCollapse);
            ImGui::Text("验证批次 %zu / %zu | %s | %s",session.current+1,session.cases.size(),session.cases[session.current].id.c_str(),session.phase.c_str());
            if(ImGui::Button(session.running?"暂停计算":"继续计算")&&!session.finished&&!session.failed){session.running=!session.running;session.phase=session.running?"running":"paused";session.status();}
            ImGui::SameLine();if(ImGui::Checkbox("固定当前物理幅度色标",&fixedScale)&&fixedScale)range=std::max(peak,1e-30);
            ImGui::SameLine();ImGui::TextUnformatted("Vz / RdBu细节压缩；原始记录不归一化");
            if(session.sim&&session.sim->initialized()) {
                auto &s=*session.sim;const auto &p=s.parameters();
                char progress[180];std::snprintf(progress,sizeof(progress),"%d/%d  t=%.4f/%.2fs  dt=%.5fms  samples=%zu",s.step(),s.totalSteps(),s.step()*s.dt(),p.duration,s.dt()*1000,s.receivers().front().vz.size());
                ImGui::ProgressBar(static_cast<float>(s.step())/s.totalSteps(),{-1,0},progress);
                ImGui::Text("%s | %dx%d | h %.3fm | 侧边 %.2fm / 底部 %.2fm | beta %.1f",s.backend().deviceName.c_str(),p.nx,p.nz,p.spacing,std::min(p.spongeX,p.nx/3)*p.spacing,std::min(p.spongeZ,p.nz/3)*p.spacing,p.spongeStrength);
                if(previousCase!=static_cast<int>(session.current)||std::chrono::duration<double>(Clock::now()-lastFrame).count()>.3) {
                    check(s.readFrame(0,frame,error),error);cols=std::min(641,p.nx);rows=std::min(121,p.nz);small.resize(static_cast<std::size_t>(cols)*rows);material.resize(small.size());peak=1e-30;
                    for(float value:frame)peak=std::max(peak,std::abs(static_cast<double>(value)));
                    const double displayRange=fixedScale?range:peak;
                    for(int r=0;r<rows;++r)for(int c=0;c<cols;++c) {
                        const int i=c*(p.nx-1)/(cols-1),k=(rows-1-r)*(p.nz-1)/(rows-1);const auto q=static_cast<std::size_t>(r)*cols+c;
                        const double value=frame[i+static_cast<std::size_t>(p.nx)*k];const double magnitude=std::log1p(std::abs(value)/(displayRange*.001))/std::log1p(1000.);
                        small[q]=static_cast<float>(std::copysign(magnitude,value));material[q]=static_cast<float>(s.grid().vs[s.grid().index(i+ghost,k+ghost)]);
                    }
                    lastFrame=Clock::now();previousCase=static_cast<int>(session.current);
                }
                const double length=(p.nx-1)*p.spacing,depth=(p.nz-1)*p.spacing;
                const float height=static_cast<float>(std::clamp((ImGui::GetContentRegionAvail().x-110)*depth/length+70,140.,350.));
                const auto mm=std::minmax_element(material.begin(),material.end());
                heatmap("Vs速度模型（长度/深度1:1）",material,rows,cols,length,depth,*mm.first,*mm.second,ImPlotColormap_Viridis,height);
                heatmap("Vz波场（长度/深度1:1）",small,rows,cols,length,depth,-1,1,ImPlotColormap_RdBu,height);
                ImGui::Text("色标 ± %.5g m/s | %s；定量吸收以原始幅度与参考域残差为准",fixedScale?range:peak,fixedScale?"固定尺度":"逐帧自动缩放");
                if(ImPlot::BeginPlot("原始Vz接收记录（未做AGC）",{-1,180})) {
                    ImPlot::SetupAxes("Time (s)","Vz (m/s)",0,ImPlotAxisFlags_AutoFit);ImPlot::SetupAxisLimits(ImAxis_X1,0,p.duration,ImGuiCond_Always);
                    for(int id:{0,p.receiverCount/3,p.receiverCount*2/3,p.receiverCount-1}) {
                        const auto &r=s.receivers()[id];std::vector<double> t(r.vz.size());for(std::size_t i=0;i<t.size();++i)t[i]=i*s.recordDt();
                        const auto label=std::to_string(static_cast<int>(r.position[0]-p.sourceX*p.spacing))+"m";
                        ImPlot::PlotLine(label.c_str(),t.data(),r.vz.data(),static_cast<int>(t.size()));
                    }
                    ImPlot::EndPlot();
                }
            } else ImGui::TextUnformatted("正在准备模型与CUDA；数据将自动保存。");
            ImGui::TextWrapped("输出：%s",session.root.string().c_str());
            if(!session.error.empty())ImGui::TextWrapped("已暂停，错误：%s",session.error.c_str());
            if(session.finished)ImGui::TextUnformatted("本批验证数据已保存。尚需边界/频散/网格验收，再决定正式A/B分析。");
            ImGui::End();runtime.present();
            try{session.advance();}catch(const std::exception &e){session.error=e.what();session.failed=true;session.running=false;session.phase="failed";session.status();if(session.sim&&session.sim->initialized())saveRecords(*session.sim,session.folder,false);}
        }
        if(!session.finished&&!session.caseSaved&&session.sim&&session.sim->initialized()){session.running=false;session.phase="interrupted";session.status();saveRecords(*session.sim,session.folder,false);}
        return session.failed?1:0;
    }catch(const std::exception &e){std::cerr<<e.what()<<'\n';if(!session.root.empty())atomicText(session.root/"START_FAILED.txt",e.what());return 1;}
}
