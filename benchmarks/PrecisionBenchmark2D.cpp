// Production solver benchmark: no kernel, diagnostic or sampling changes.
#include "Seis2D/CurvilinearSolver2D.hpp"
#include <windows.h>
#include <psapi.h>
#include <intrin.h>
#include <xmmintrin.h>
#include <omp.h>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <set>
#include <stdexcept>
#include <string>

using namespace Seis2D::Curvilinear;
using Clock=std::chrono::steady_clock;
void check(bool v,const std::string& e){if(!v)throw std::runtime_error(e);}
double milliseconds(Clock::time_point start){return std::chrono::duration<double,std::milli>(Clock::now()-start).count();}
double epoch(){return std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();}
std::string quote(const std::string& s){std::string x="\"";for(unsigned char c:s){if(c=='"'||c=='\\')x+='\\';if(c>=32)x+=static_cast<char>(c);}return x+'"';}
double cpuSeconds(){FILETIME c,e,k,u;check(GetProcessTimes(GetCurrentProcess(),&c,&e,&k,&u)!=0,"Process time query failed");const auto ticks=[](FILETIME f){return (static_cast<unsigned long long>(f.dwHighDateTime)<<32)|f.dwLowDateTime;};return (ticks(k)+ticks(u))*1e-7;}
struct Options {std::filesystem::path output;int size=256,threads=32,steps=100,warmup=10,preheat=1000;bool gpu{},q{},fp32{};};
int main(int argc,char** argv){try{
    Options o;
    for(int i=1;i<argc;i+=2){check(i+1<argc,"Missing value");const std::string a=argv[i],v=argv[i+1];
        if(a=="--output")o.output=std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(v.data()),v.size()));
        else if(a=="--size")o.size=std::stoi(v);else if(a=="--threads")o.threads=std::stoi(v);else if(a=="--steps")o.steps=std::stoi(v);
        else if(a=="--warmup")o.warmup=std::stoi(v);else if(a=="--preheat-ms")o.preheat=std::stoi(v);
        else if(a=="--backend"){check(v=="CPU"||v=="GPU","Expected CPU/GPU");o.gpu=v=="GPU";}
        else if(a=="--precision"){check(v=="FP32"||v=="FP64","Expected FP32/FP64");o.fp32=v=="FP32";}
        else if(a=="--case"){check(v=="elastic"||v=="gsls3","Expected elastic/gsls3");o.q=v=="gsls3";}
        else throw std::runtime_error("Unknown option: "+a);
    }
    const auto root=std::filesystem::weakly_canonical(SEIS3D_OUTPUT_ROOT),target=std::filesystem::weakly_canonical(o.output);
    const auto relative=target.lexically_relative(root);check(!o.output.empty()&&o.output.is_absolute()&&!relative.empty()&&*relative.begin()!=".."&&relative!=".","Output must be an absolute subdirectory of outer out");
    check(o.threads>=1&&o.threads<=omp_get_num_procs()&&o.steps>0&&o.warmup>=0&&o.steps+o.warmup<=20000&&o.size>=32&&o.size<=4096&&o.preheat>=0,"Invalid benchmark configuration");
    std::filesystem::create_directories(target);
    omp_set_dynamic(0);omp_set_num_threads(o.threads);int team=0;unsigned mxcsr=0;
#pragma omp parallel num_threads(o.threads)
    {
        _mm_setcsr(_mm_getcsr()&~0x8040u); // Keep IEEE subnormals; no FTZ/DAZ shortcut.
#pragma omp single
        {team=omp_get_num_threads();mxcsr=_mm_getcsr();}
    }
    check(team==o.threads,"OpenMP actual team differs");const bool priority=SetPriorityClass(GetCurrentProcess(),ABOVE_NORMAL_PRIORITY_CLASS)!=0;
    SYSTEM_POWER_STATUS power{};check(GetSystemPowerStatus(&power)!=0,"Cannot query AC power");check(power.ACLineStatus==1,"Connect AC power before formal performance test");
    MEMORYSTATUSEX ram{};ram.dwLength=sizeof(ram);check(GlobalMemoryStatusEx(&ram)!=0,"Cannot query RAM");
    int regs[4];char brand[49]{};for(int i=0;i<3;++i){__cpuid(regs,static_cast<int>(0x80000002u+i));std::memcpy(brand+16*i,regs,16);}
    DWORD_PTR processMask{},systemMask{};GetProcessAffinityMask(GetCurrentProcess(),&processMask,&systemMask);
    Parameters p;p.nx=p.nz=o.size;p.precision=o.fp32?Precision::Float32:Precision::Float64;p.spacing=2;p.vp=1500;p.vs=700;p.density=1800;
    p.relief=5;p.wavelength=500;p.frequency=15;p.cfl=.5;p.duration=10;p.maxSteps=o.warmup+o.steps;p.sourceX=o.size/2;p.sourceZ=20;p.forcePerLength=100;
    p.absorber=Absorber::CfsPml;p.externalAbsorption=true;p.spongeX=p.spongeZ=16;p.receiverCount=32;p.receiverStart=0;p.receiverStride=(o.size-1)/31;p.receiverDepth=1;p.sampleIntervalMs=.25;
    p.attenuation.enabled=o.q;p.attenuation.mechanisms=3;p.attenuation.qp=40;p.attenuation.qs=25;p.attenuation.minFrequency=5;p.attenuation.maxFrequency=60;p.attenuation.referenceFrequency=20;
    std::string error;Simulation sim;ComputeSettings3D settings;settings.cpuThreads=o.threads;const auto kind=o.gpu?BackendKind3D::Cuda:BackendKind3D::Cpu;
    const auto freeBefore=Seis3D::Curvilinear::queryBackendMemory3D(kind);check(sim.initialize(p,error,kind,settings),error);
    check(sim.backend().kind==kind&&sim.parameters().precision==p.precision&&sim.grid().vp.precision()==p.precision,"Unexpected backend/precision");
    if(!o.gpu)check(sim.backend().workerThreads==o.threads,"Solver did not use requested CPU workers");
    // Precondition the selected device with the same production solver, untimed.
    const auto heat=Clock::now();while(milliseconds(heat)<o.preheat){if(sim.completed())check(sim.reset(error),error);check(sim.advance(1,error),error);}
    const double preheatMs=milliseconds(heat);
    check(sim.reset(error)&&sim.advance(o.warmup,error),error);
    const auto freeAfter=Seis3D::Curvilinear::queryBackendMemory3D(kind);const double cpuStart=cpuSeconds(),startEpoch=epoch();const auto start=Clock::now();
    check(sim.advance(o.steps,error),error);const double wall=milliseconds(start),endEpoch=epoch(),cpu=cpuSeconds()-cpuStart;
    check(sim.completed()&&sim.diagnostics().finite,"Incomplete or nonfinite timed simulation");
    PROCESS_MEMORY_COUNTERS_EX memory{};memory.cb=sizeof(memory);check(GetProcessMemoryInfo(GetCurrentProcess(),reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory),sizeof(memory))!=0,"Process memory query failed");
    const auto& d=sim.diagnostics();
    std::ofstream out(target/"trial.json");out<<std::setprecision(17)<<"{\n\"size\":"<<o.size<<",\"logical_nodes\":"<<static_cast<std::size_t>(o.size)*o.size<<",\"compute_nodes\":"<<sim.grid().coordinates.size()<<",\"case\":"<<quote(o.q?"GSLS3_PML":"elastic_PML")
       <<",\"backend\":"<<quote(o.gpu?"GPU":"CPU")<<",\"precision\":"<<quote(precisionName(p.precision))<<",\"requested_threads\":"<<o.threads<<",\"actual_omp_team\":"<<team<<",\"solver_workers\":"<<sim.backend().workerThreads
       <<",\"warmup_steps\":"<<o.warmup<<",\"preheat_ms\":"<<preheatMs<<",\"timed_steps\":"<<o.steps<<",\"timed_start_epoch\":"<<startEpoch<<",\"timed_end_epoch\":"<<endEpoch<<",\"wall_ms\":"<<wall<<",\"step_ms\":"<<wall/o.steps
       <<",\"process_cpu_s\":"<<cpu<<",\"cpu_busy_cores\":"<<cpu/(wall/1000)<<",\"cpu_util_of_32_percent\":"<<100*cpu/(wall/1000)/o.threads
       <<",\"dt_s\":"<<sim.dt()<<",\"completed_step\":"<<sim.step()<<",\"host_array_bytes\":"<<d.residentHostArrayBytes<<",\"device_payload_bytes\":"<<d.residentDeviceBytes
       <<",\"working_set_bytes\":"<<memory.WorkingSetSize<<",\"peak_working_set_bytes_before_snapshot\":"<<memory.PeakWorkingSetSize<<",\"private_bytes\":"<<memory.PrivateUsage
       <<",\"free_ram_before_bytes\":"<<freeBefore.hostAvailableBytes<<",\"free_vram_before_bytes\":"<<freeBefore.deviceAvailableBytes<<",\"free_vram_warmed_bytes\":"<<freeAfter.deviceAvailableBytes
       <<",\"initialization_ms\":"<<d.initializationMs<<",\"rhs_registers\":"<<d.rhsRegisters<<",\"rhs_local_bytes\":"<<d.rhsLocalBytes<<",\"traction\":"<<d.tractionRelative<<",\"energy\":"<<d.energy
       <<",\"cpu\":"<<quote(brand)<<",\"ram_bytes\":"<<ram.ullTotalPhys<<",\"logical_processors\":"<<omp_get_num_procs()<<",\"ac_power\":"<<int(power.ACLineStatus)<<",\"process_affinity\":"<<processMask<<",\"system_affinity\":"<<systemMask<<",\"mxcsr\":"<<mxcsr<<",\"above_normal_priority\":"<<(priority?"true":"false")<<",\"msvc\":"<<_MSC_VER<<"}\n";
    out.close();
    // Explicit sparse field check plus ALL receiver samples, excluded from timing.
    const auto shot=sim.snapshot();std::set<std::size_t> indices;const auto n=sim.grid().coordinates.size();
    for(std::size_t i=0;i<1024;++i)indices.insert(i*(n-1)/1023);
    for(int k=std::max(0,p.sourceZ-12);k<std::min(p.nz,p.sourceZ+13);++k)for(int i=p.sourceX-12;i<=p.sourceX+12;++i)indices.insert(sim.grid().modelIndex(i,k));
    std::ofstream sample(target/"samples.csv");sample<<std::setprecision(17)<<"component,index,value\n";
    for(int f=0;f<(o.q?6:5);++f){const auto& values=f==5?shot.syy:shot.fields[f];for(auto q:indices)sample<<f<<','<<q<<','<<values[q]<<'\n';}
    for(std::size_t r=0;r<sim.receivers().size();++r)for(std::size_t i=0;i<sim.receivers()[r].vz.size();++i){sample<<6+2*r<<','<<i<<','<<sim.receivers()[r].vz[i]<<'\n';sample<<7+2*r<<','<<i<<','<<sim.receivers()[r].vx[i]<<'\n';}
    std::cout<<(o.gpu?"GPU":"CPU32")<<' '<<precisionName(p.precision)<<' '<<(o.q?"GSLS3":"elastic")<<' '<<o.size<<" ms/step="<<wall/o.steps<<" device_GiB="<<d.residentDeviceBytes/1073741824.<<" cpu_busy_cores="<<cpu/(wall/1000)<<std::endl;return 0;
}catch(const std::exception& e){std::cerr<<"FAIL: "<<e.what()<<std::endl;return 1;}}
