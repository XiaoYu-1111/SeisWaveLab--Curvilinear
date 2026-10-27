#include "SeisCommon/AcquisitionAnalysis.hpp"
#include "SeisCommon/AtomicFile.hpp"
#include <algorithm>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <locale>
#include <stdexcept>
namespace SeisWave {
namespace {
void require(bool okay,const char* message){if(!okay)throw std::runtime_error(message);}
void validate(const RecordDataView& d,int component) {
    require(!d.traces.empty()&&d.components.size()>0&&d.components.size()<=3,"No valid raw records.");
    require(component>=0&&component<=static_cast<int>(d.components.size()),"Invalid record component.");
    require(std::isfinite(d.dt)&&d.dt>0,"Invalid record time sampling.");
    for(const auto& t:d.traces){require(std::isfinite(t.startTime)&&t.channels[0].size()>=2,"At least two samples per trace are required.");
        for(std::size_t c=0;c<d.components.size();++c){require(t.channels[c].size()==t.channels[0].size(),"Component sample counts differ.");for(double v:t.channels[c])require(std::isfinite(v),"Nonfinite raw record value.");}
        for(double v:t.position)require(std::isfinite(v),"Invalid receiver coordinate.");
    }
    for(std::size_t r=0;r<d.traces.size();++r)for(std::size_t i=0;i<d.sampleCount(r);++i)require(std::isfinite(d.value(r,component,i)),"Nonfinite derived component.");
}
std::string xml(const std::string& text){std::string out;for(char c:text){if(c=='&')out+="&amp;";else if(c=='<')out+="&lt;";else if(c=='>')out+="&gt;";else if(c=='\"')out+="&quot;";else if(c=='\'')out+="&apos;";else out+=c;}return out;}
std::string componentName(const RecordDataView& d,int c){return c==static_cast<int>(d.components.size())?"Velocity magnitude":d.components[c];}
template<class Emit>bool atomic(const std::filesystem::path& path,std::string& error,Emit emit){std::filesystem::path pending;
    try{if(!path.parent_path().empty())std::filesystem::create_directories(path.parent_path());pending=path;pending+=".tmp-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
        std::ofstream out(pending,std::ios::binary);out.imbue(std::locale::classic());require(bool(out),"Cannot create analysis output.");out<<std::setprecision(17);emit(out);out.close();require(bool(out),"Analysis output write failed.");replaceCompletedFile(pending,path);error.clear();return true;
    }catch(const std::exception& e){if(!pending.empty()){std::error_code ignored;std::filesystem::remove(pending,ignored);}error=e.what();return false;}}
}
bool analyzeAcquisition(const RecordDataView& d,int component,TimeWindow signal,TimeWindow interference,double fraction,std::vector<TraceMetrics>& result,std::string& error){try{
    validate(d,component);for(auto w:{signal,interference})require(std::isfinite(w.begin)&&std::isfinite(w.end)&&w.end>w.begin,"Time windows must have finite increasing bounds.");
    require(signal.end<=interference.begin||interference.end<=signal.begin,"Signal and interference windows must not overlap.");
    require(std::isfinite(fraction)&&fraction>0&&fraction<=1,"Threshold fraction must be in (0,1].");
    std::vector<TraceMetrics> next;next.reserve(d.traces.size());
    for(std::size_t r=0;r<d.traces.size();++r){TraceMetrics m;m.trace=r;
        for(std::size_t i=0;i<d.sampleCount(r);++i){const double value=d.value(r,component,i),time=d.traces[r].startTime+i*d.dt;m.peak=std::max(m.peak,std::abs(value));
            require(std::isfinite(value*value),"Record energy overflows Float64.");
            if(time>=signal.begin&&time<signal.end){m.signalEnergy+=value*value*d.dt;++m.signalSamples;}
            if(time>=interference.begin&&time<interference.end){m.interferenceEnergy+=value*value*d.dt;++m.interferenceSamples;}}
        require(m.signalSamples>0&&m.interferenceSamples>0,"A requested time window contains no samples on a trace.");
        require(std::isfinite(m.signalEnergy)&&std::isfinite(m.interferenceEnergy),"Integrated record energy overflows.");
        m.signalRms=std::sqrt(m.signalEnergy/(m.signalSamples*d.dt));m.interferenceRms=std::sqrt(m.interferenceEnergy/(m.interferenceSamples*d.dt));
        if(m.interferenceEnergy>0){const double ratio=m.signalEnergy/m.interferenceEnergy;if(std::isfinite(ratio))m.energyRatio=ratio;}
        if(m.interferenceRms>0){const double ratio=m.signalRms/m.interferenceRms;if(std::isfinite(ratio))m.rmsRatio=ratio;}
        if(m.peak>0)for(std::size_t i=0;i<d.sampleCount(r);++i)if(std::abs(d.value(r,component,i))>=m.peak*fraction){m.thresholdTime=d.traces[r].startTime+i*d.dt;break;}
        next.push_back(m);
    }
    result=std::move(next);error.clear();return true;
}catch(const std::exception& e){error=e.what();return false;}}
bool writeAcquisitionCsv(const RecordDataView& d,int c,TimeWindow signal,TimeWindow interference,double fraction,const std::filesystem::path& path,std::string& error){std::vector<TraceMetrics> metrics;if(!analyzeAcquisition(d,c,signal,interference,fraction,metrics,error))return false;
    return atomic(path,error,[&](std::ostream& out){
        out<<"# Raw samples; half-open physical-second windows; window energy/RMS ratios are not field SNR.\n";
        out<<"# component="<<componentName(d,c)<<"; amplitude_unit="<<d.units<<"; dt_s="<<d.dt<<"; signal="<<signal.begin<<':'<<signal.end<<"; interference="<<interference.begin<<':'<<interference.end<<"; threshold_fraction="<<fraction<<'\n';
        out<<"receiver_index,x_m,y_m,z_m,peak,signal_samples,interference_samples,signal_energy,interference_energy,signal_rms,interference_rms,energy_ratio,rms_ratio,threshold_time_s\n";
        for(const auto& m:metrics){const auto& t=d.traces[m.trace];out<<m.trace+1;for(double x:t.position)out<<','<<x;out<<','<<m.peak<<','<<m.signalSamples<<','<<m.interferenceSamples<<','<<m.signalEnergy<<','<<m.interferenceEnergy<<','<<m.signalRms<<','<<m.interferenceRms<<',';
            if(m.energyRatio)out<<*m.energyRatio;out<<',';if(m.rmsRatio)out<<*m.rmsRatio;out<<',';if(m.thresholdTime)out<<*m.thresholdTime;out<<'\n';}
    });
}
bool writePaperRecordSvg(const RecordDataView& d,int c,const std::filesystem::path& path,const PaperRecordOptions& options,std::string& error){try{
    validate(d,c);require(options.maxTraces>=1&&options.maxTraces<=1024,"Paper trace limit must be 1..1024.");
    require(std::isfinite(options.amplitudeReference)&&options.amplitudeReference>=0&&std::isfinite(options.gain)&&options.gain>0,"Invalid paper amplitude reference/gain.");
    const int count=std::min(options.maxTraces,static_cast<int>(d.traces.size()));std::vector<std::size_t> selected;
    for(int i=0;i<count;++i)selected.push_back(count==1?0:static_cast<std::size_t>(i)*(d.traces.size()-1)/(count-1));
    double first=1e300,last=-1e300,peak=0;
    for(auto r:selected){first=std::min(first,d.traces[r].startTime);last=std::max(last,d.traces[r].startTime+(d.sampleCount(r)-1)*d.dt);for(std::size_t i=0;i<d.sampleCount(r);++i)peak=std::max(peak,std::abs(d.value(r,c,i)));}
    require(std::isfinite(first)&&std::isfinite(last)&&last>first,"Invalid paper time extent.");
    const double reference=options.amplitudeReference>0?options.amplitudeReference:std::max(peak,1e-300);
    return atomic(path,error,[&](std::ostream& out){
        out<<"<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"180mm\" height=\"130mm\" viewBox=\"0 0 1200 866\">\n";
        out<<"<title>"<<xml(options.title)<<"</title><metadata>Raw "<<xml(componentName(d,c))<<"; dt_s="<<d.dt<<"; shared_amplitude_reference="<<reference<<' '<<xml(d.units)<<"; gain="<<options.gain<<"; receiver subset="<<count<<'/'<<d.traces.size()<<"; no AGC; no sample decimation.</metadata>\n";
        out<<"<rect width=\"1200\" height=\"866\" fill=\"white\"/><defs><clipPath id=\"plot\"><rect x=\"105\" y=\"85\" width=\"1050\" height=\"640\"/></clipPath></defs><g font-family=\"Times New Roman,serif\" fill=\"black\" font-size=\"23\">\n";
        out<<"<text x=\"630\" y=\"40\" text-anchor=\"middle\" font-size=\"27\">"<<xml(options.title)<<" — "<<xml(componentName(d,c))<<"</text>\n";
        for(int tick=0;tick<=5;++tick){const double y=85+640.*tick/5,time=first+(last-first)*tick/5;out<<"<path d=\"M105 "<<y<<" H1155\" stroke=\"#dddddd\" stroke-width=\"0.6\"/><text x=\"91\" y=\""<<y+7<<"\" text-anchor=\"end\">"<<std::setprecision(4)<<time<<std::setprecision(17)<<"</text>\n";}
        const double spacing=1050./count;
        out<<"<g clip-path=\"url(#plot)\" fill=\"none\" stroke=\"#161616\" stroke-width=\"0.65\" stroke-linejoin=\"round\">\n";
        for(int k=0;k<count;++k){const auto r=selected[k];const double x=105+(k+.5)*spacing;out<<"<polyline points=\"";
            for(std::size_t i=0;i<d.sampleCount(r);++i){const double t=d.traces[r].startTime+i*d.dt;out<<std::fixed<<std::setprecision(3)<<x+std::clamp(d.value(r,c,i)/reference*options.gain,-8.,8.)*spacing*.43<<','<<85+(t-first)/(last-first)*640<<' ';}out<<"\"/>\n";}
        out<<"</g><rect x=\"105\" y=\"85\" width=\"1050\" height=\"640\" fill=\"none\" stroke=\"black\" stroke-width=\"1.2\"/>\n";
        for(int k=0;k<count;++k)if(k==0||k==count-1||k%std::max(1,count/8)==0)out<<"<text x=\""<<105+(k+.5)*spacing<<"\" y=\"760\" text-anchor=\"middle\">"<<selected[k]+1<<"</text>\n";
        out<<"<text x=\"630\" y=\"804\" text-anchor=\"middle\">Receiver index</text><text x=\"28\" y=\"405\" text-anchor=\"middle\" transform=\"rotate(-90 28 405)\">Time (s)</text>";
        out<<"<text x=\"105\" y=\"846\" font-size=\"18\">Shared reference: "<<std::scientific<<std::setprecision(4)<<reference<<' '<<xml(d.units)<<"; gain "<<options.gain<<"; raw samples; no AGC</text></g></svg>\n";
    });
}catch(const std::exception& e){error=e.what();return false;}}
}
