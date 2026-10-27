#include "SeisCommon/AcquisitionAnalysis.hpp"
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
using namespace SeisWave;
int main(){try{
    const auto check=[](bool okay,const std::string& message){if(!okay)throw std::runtime_error(message);};
    std::vector<double> samples{1,1,2,2,0,-4,0,0},zero(8);
    RecordDataView data;data.dt=.125;data.components={"Vz","Vx"};
    TraceView trace;trace.channels={samples,zero,{}};trace.position={25,0,0};data.traces.push_back(trace);
    trace.channels={zero,zero,{}};trace.position={50,0,0};data.traces.push_back(trace);
    std::string error;std::vector<TraceMetrics> metrics;const auto original=samples;
    check(analyzeAcquisition(data,0,{.25,.75},{0,.25},.5,metrics,error),error);
    check(metrics.size()==2&&metrics[0].signalSamples==4&&metrics[0].interferenceSamples==2,"Half-open sample counts differ");
    check(metrics[0].signalEnergy==3&&metrics[0].interferenceEnergy==.25&&metrics[0].energyRatio==12&&metrics[0].thresholdTime==.25,"Known energy/arrival values differ");
    check(std::abs(metrics[0].signalRms-std::sqrt(6.))<1e-14&&metrics[0].interferenceRms==1,"Known RMS differs");
    check(!metrics[1].energyRatio&&!metrics[1].rmsRatio&&!metrics[1].thresholdTime,"Zero trace produced a false ratio/arrival");
    check(!analyzeAcquisition(data,0,{.25,.75},{.5,.875},.5,metrics,error)&&metrics[0].signalEnergy==3,"Overlapping windows accepted/mutated result");
    check(analyzeAcquisition(data,2,{.25,.75},{0,.25},.5,metrics,error)&&metrics[0].signalEnergy==3,"Magnitude component differs");
    const auto folder=std::filesystem::path(SEIS3D_OUTPUT_ROOT)/"validation/acquisition-analysis";std::filesystem::create_directories(folder);
    check(writeAcquisitionCsv(data,0,{.25,.75},{0,.25},.5,folder/"known-metrics.csv",error),error);
    PaperRecordOptions options;options.title="Raw <test> & record";check(writePaperRecordSvg(data,0,folder/"known-record.svg",options,error),error);
    const auto read=[](const auto& file){std::ifstream in(file);return std::string(std::istreambuf_iterator<char>(in),{});};
    auto svg=read(folder/"known-record.svg");check(svg.find("Raw &lt;test&gt; &amp; record")!=svg.npos&&svg.find("shared_amplitude_reference=4")!=svg.npos,"SVG escaping/reference differs");
    samples[0]=std::numeric_limits<double>::quiet_NaN();
    check(!writePaperRecordSvg(data,0,folder/"known-record.svg",options,error)&&read(folder/"known-record.svg")==svg,"Invalid data overwrote an existing figure");
    samples=original;check(samples==original,"Analysis changed raw input");
    std::cout<<"Acquisition analysis PASS: exact half-open energy/RMS/threshold, zero traces, overlap rejection, magnitude, immutable samples, atomic SVG/CSV\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
