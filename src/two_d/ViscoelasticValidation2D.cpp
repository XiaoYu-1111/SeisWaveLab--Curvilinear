#include "Seis2D/CurvilinearSolver2D.hpp"
#include "Seis2D/ModelIo2D.hpp"
#include "PointKernels2D.hpp"
#include "MemoryBudget2D.hpp"
#include <algorithm>
#include <complex>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <stdexcept>
using namespace Seis2D::Curvilinear;
namespace R=SeisWave::Rheology;
namespace {
constexpr double pi=3.14159265358979323846;
void check(bool v,const std::string& message){if(!v)throw std::runtime_error(message);}
void compare(const std::vector<double>& a,const std::vector<double>& b){check(a.size()==b.size(),"State lengths differ");double peak=0,error=0;for(std::size_t i=0;i<a.size();++i){check(std::isfinite(a[i])&&std::isfinite(b[i]),"Nonfinite state");peak=std::max(peak,std::abs(a[i]));error=std::max(error,std::abs(a[i]-b[i]));}check(error<=1e-8*std::max(peak,1e-25),"CPU/CUDA GSLS mismatch");}
void compare(const Snapshot& a,const Snapshot& b){for(int f=0;f<5;++f)compare(a.fields[f],b.fields[f]);compare(a.syy,b.syy);check(a.memory.size()==b.memory.size(),"Memory counts differ");for(std::size_t l=0;l<a.memory.size();++l)compare(a.memory[l],b.memory[l]);}
// Independent complex-frequency dispersion relation for a spatially periodic
// Fourier eigenmode. This is not obtained by running the FD solver as reference.
std::complex<double> eigenvalue(const R::Material& m,bool shear,double rho,double k){
    const double speed=std::sqrt((shear?m.sRelaxed:m.pRelaxed)/rho);std::complex<double> s{-.01*k*speed,k*speed};
    for(int it=0;it<60;++it){std::complex<double> dc=0;for(int l=0;l<m.mechanisms;++l)dc+=(shear?m.deltaS[l]:m.deltaP[l])*m.rates[l]/std::pow(s+m.rates[l],2);
        const auto ds=(s*s+k*k*R::modulus(m,shear,s)/rho)/(2.*s+k*k*dc/rho);s-=ds;if(std::abs(ds)<1e-12*std::abs(s))break;}
    check(s.real()<0&&s.imag()>0,"Non-passive eigenmode");check(std::abs(s*s+k*k*R::modulus(m,shear,s)/rho)<1e-8*std::norm(s),"Complex dispersion root failed");return s;
}
double periodic(const R::Material& material,bool shear,int nodes,const std::filesystem::path& folder,std::ostream& report){
    const int nx=nodes+6,nz=16,row=8;const std::size_t count=static_cast<std::size_t>(nx)*nz;constexpr double wavelength=200,rho=2000;
    const double h=wavelength/nodes,k=2*pi/wavelength;const auto s=eigenvalue(material,shear,rho,k);const std::complex<double> ik{0,k};
    const double targetTime=4*pi/s.imag(),safe=std::min(.25*h/std::sqrt(material.pUnrelaxed/rho),.2/material.rates[material.mechanisms-1]);
    const int steps=static_cast<int>(std::ceil(targetTime/safe));const double dt=targetTime/steps;
    using State=std::array<std::vector<double>,Point::maxFields>;State state[7];const int fields=6+4*material.mechanisms;
    for(auto& a:state)for(int f=0;f<fields;++f)a[f].resize(count);
    std::vector<double> zero(count),one(count,1),lambda(count,material.pUnrelaxed-2*material.sUnrelaxed),mu(count,material.sUnrelaxed),density(count,rho),normals(count*2);
    std::array<std::vector<double>,3> dp,ds;Point::Mesh mesh{nx,nz,count,h,zero.data(),one.data(),one.data(),normals.data(),lambda.data(),mu.data(),density.data(),zero.data(),zero.data()};
    mesh.mechanisms=material.mechanisms;mesh.fieldCount=fields;
    for(int l=0;l<material.mechanisms;++l){dp[l].assign(count,material.deltaP[l]);ds[l].assign(count,material.deltaS[l]);mesh.rates[l]=material.rates[l];mesh.deltaP[l]=dp[l].data();mesh.deltaS[l]=ds[l].data();}
    const auto view=[&](State& a){Point::Fields w;for(int f=0;f<fields;++f)w.f[f]=a[f].data();return w;};
    std::array<std::complex<double>,Point::maxFields> amplitude{};amplitude[shear?1:0]=1;
    const auto mp=R::modulus(material,false,s),ms=R::modulus(material,true,s),ml=mp-2.*ms;
    if(shear)amplitude[4]=ik*ms/s;else{amplitude[2]=ik*mp/s;amplitude[3]=amplitude[5]=ik*ml/s;}
    for(int l=0;l<material.mechanisms;++l){const int m=6+4*l;const auto factor=ik*material.rates[l]/(s+material.rates[l]);if(shear)amplitude[m+2]=factor*material.deltaS[l];else{amplitude[m]=factor*material.deltaP[l];amplitude[m+1]=amplitude[m+3]=factor*(material.deltaP[l]-2*material.deltaS[l]);}}
    for(int i=0;i<nodes;++i)for(int f=0;f<fields;++f)state[0][f][Point::index(mesh,i+3,row)]=std::real(amplitude[f]*std::exp(ik*(i*h)));
    const auto wrap=[&](State& a){for(int f=0;f<fields;++f){for(int i=0;i<nx;++i){const int source=3+(i-3+nodes)%nodes;a[f][Point::index(mesh,i,row)]=a[f][Point::index(mesh,source,row)];}for(int z=0;z<nz;++z)if(z!=row)std::copy_n(a[f].data()+Point::index(mesh,0,row),nx,a[f].data()+Point::index(mesh,0,z));}};
    const auto rhs=[&](State& a,State& out,int step,int stage){wrap(a);for(int i=3;i<nodes+3;++i)Point::rhs(view(a),view(out),mesh,Point::index(mesh,i,row),0.,step,stage);};
    wrap(state[0]);
    {auto curved=mesh;std::vector<double> metricA(count,.2),metricB(count,.9),surfaceNormals(count*2);for(std::size_t q=0;q<count;++q){surfaceNormals[2*q]=.2/std::hypot(.2,.9);surfaceNormals[2*q+1]=.9/std::hypot(.2,.9);}curved.a=metricA.data();curved.b=metricB.data();curved.normals=surfaceNormals.data();
        const auto q=Point::index(mesh,10,3);Point::rhs(view(state[0]),view(state[2]),curved,q,0.,0,0);const double normalX=surfaceNormals[2*q],normalZ=surfaceNormals[2*q+1],tx=state[2][2][q]*normalX+state[2][4][q]*normalZ,tz=state[2][4][q]*normalX+state[2][3][q]*normalZ;
        const double scale=std::hypot(state[2][2][q],std::hypot(state[2][3][q],state[2][4][q]));check(std::hypot(tx,tz)<1e-12*std::max(scale,1.),"Curved traction-rate closure omitted memory stresses");}
    const auto combine=[&](State& out,const State& rate,double weight,bool final){for(int f=0;f<fields;++f)for(int i=3;i<nodes+3;++i){const auto q=Point::index(mesh,i,row);out[f][q]=state[1][f][q]+(final?dt*(state[2][f][q]+2*state[3][f][q]+2*state[4][f][q]+state[5][f][q])/6:weight*rate[f][q]);}};
    for(int step=0;step<steps;++step){state[1]=state[0];rhs(state[0],state[2],step,0);combine(state[6],state[2],dt/2,false);rhs(state[6],state[3],step,1);combine(state[6],state[3],dt/2,false);rhs(state[6],state[4],step,2);combine(state[6],state[4],dt,false);rhs(state[6],state[5],step,3);combine(state[0],state[2],0,true);}
    std::complex<double> measured=0;const auto exact=std::exp(s*targetTime);std::ofstream profile;
    if(nodes==128){profile.open(folder/(shear?"s-plane-wave.csv":"p-plane-wave.csv"));profile<<std::setprecision(17)<<"x_m,numerical,analytic\n";}
    double velocityError=0,velocityNorm=0;
    for(int i=0;i<nodes;++i){const double x=i*h,value=state[0][shear?1:0][Point::index(mesh,i+3,row)],analytic=std::real(exact*std::exp(ik*x)),other=state[0][shear?0:1][Point::index(mesh,i+3,row)];measured+=value*std::exp(-ik*x)*(2./nodes);velocityError+=(value-analytic)*(value-analytic)+other*other;velocityNorm+=analytic*analytic;if(profile)profile<<x<<','<<value<<','<<analytic<<'\n';}
    check(std::sqrt(velocityError/velocityNorm)<.001,"Full P/S velocity vector error exceeds 0.1%");
    const double error=std::abs(measured-exact)/std::abs(exact);report<<(shear?"S":"P")<<','<<nodes<<','<<h<<','<<dt<<','<<steps<<','<<targetTime<<','<<std::abs(measured)<<','<<std::abs(exact)<<','<<std::arg(measured/exact)<<','<<error<<'\n';return error;
}
}
int main(){try{
    const auto folder=std::filesystem::path(SEIS3D_OUTPUT_ROOT)/"validation/viscoelastic";std::filesystem::create_directories(folder);std::string error;
    R::Settings settings;settings.enabled=true;settings.minFrequency=3;settings.maxFrequency=30;settings.referenceFrequency=10;settings.qp=40;settings.qs=25;
    R::Material m;check(R::fitMaterial(2000,1000,2000,40,25,settings,m,error),error);
    for(int count:{1,2,3}){auto candidate=settings;candidate.mechanisms=count;R::Material test;check(R::fitMaterial(2000,1000,2000,40,25,candidate,test,error),error);check(std::abs(R::phaseVelocity(test,false,2000,10)/2000-1)<1e-13,"Mechanism count changed reference calibration");}
    {R::Material elastic;check(R::fitMaterial(2000,1000,2000,1e9,1e9,settings,elastic,error),error);check(std::abs(elastic.pUnrelaxed/(2000.*2000*2000)-1)<1e-7,"High-Q elastic modulus limit failed");}
    check(std::abs(R::phaseVelocity(m,false,2000,10)/2000-1)<1e-13&&std::abs(R::phaseVelocity(m,true,2000,10)/1000-1)<1e-13,"Reference phase velocity differs");
    auto invalid=m;check(!R::fitMaterial(2000,1000,2000,10000,5,settings,invalid,error)&&invalid.pRelaxed==m.pRelaxed,"Non-passive Q pair accepted/mutated result");
    std::ofstream curves(folder/"q-dispersion.csv");curves<<std::setprecision(17)<<"frequency_hz,target_qp,target_qs,fit_qp,fit_qs,vp_phase_m_s,vs_phase_m_s\n";
    for(int i=0;i<=160;++i){const double f=std::pow(200.,i/160.);curves<<f<<",40,25,"<<R::qualityFactor(m,false,f)<<','<<R::qualityFactor(m,true,f)<<','<<R::phaseVelocity(m,false,2000,f)<<','<<R::phaseVelocity(m,true,2000,f)<<'\n';}
    std::ofstream convergence(folder/"plane-wave-convergence.csv");convergence<<std::setprecision(17)<<"mode,nodes,h_m,dt_s,steps,time_s,numerical_amplitude,analytic_amplitude,phase_error_rad,relative_complex_error\n";
    double maximumFineError=0;
    for(bool shear:{false,true}){double previous=1;for(int n:{32,64,128}){const double e=periodic(m,shear,n,folder,convergence);std::cout<<(shear?"S":"P")<<" N="<<n<<" error="<<e<<'\n';check(e<.01,"Plane-wave amplitude/phase error exceeds 1%");previous=e;}check(previous<.001,"Fine plane-wave amplitude/phase differs by more than 0.1%");maximumFineError=std::max(maximumFineError,previous);}
    Parameters p;p.nx=96;p.nz=80;p.spacing=2.5;p.relief=5;p.wavelength=180;p.sourceZ=15;p.receiverCount=4;p.receiverStart=15;p.receiverStride=12;p.receiverDepth=2;p.frequency=25;p.duration=.15;p.maxSteps=200;p.sampleIntervalMs=.5;
    p.attenuation=settings;p.attenuation.minFrequency=5;p.attenuation.maxFrequency=100;p.attenuation.referenceFrequency=25;p.layered=true;p.layers={{60,1500,650,1800,40,25},{0,2300,1150,2300,100,60}};
    Simulation cpu;check(cpu.initialize(p,error,BackendKind3D::Cpu,{1}),error);check(cpu.advance(160,error),error);const auto reference=cpu.snapshot(true);
    check(reference.memory.size()==12&&reference.syy.size()==reference.fields[0].size()&&cpu.diagnostics().finite&&cpu.diagnostics().tractionRelative<1e-10,"GSLS state/traction failed");
    check(cpu.snapshot().memory.empty(),"Ordinary export unnecessarily copied memory variables");
    {std::vector<float> pressure;check(cpu.readFrame(3,pressure,error),error);for(int k=0;k<p.nz;++k)for(int i=0;i<p.nx;++i){const auto q=cpu.grid().index(i+ghost,k+ghost);const float expected=static_cast<float>(-(reference.fields[2][q]+reference.fields[3][q]+reference.syy[q])/3);check(pressure[i+static_cast<std::size_t>(p.nx)*k]==expected,"Pressure did not use actual Syy");}}
    check(writeModelVts(cpu,folder/"gsls-model.vts",{1},error),error);ModelDocument document;check(readModelVts(folder/"gsls-model.vts",document,error)&&document.parameters==p,error);Simulation restored;check(restored.initialize(document.parameters,error,BackendKind3D::Cpu,{1})&&restored.advance(160,error),error);compare(reference,restored.snapshot(true));
    auto bad=p;bad.attenuation.qp=0;check(!cpu.initialize(bad,error)&&cpu.step()==160,"Invalid Q destroyed session");
    check(writeSnapshot(cpu,folder/"gsls-wavefield.vts",error)&&writeReceivers(cpu,folder/"gsls-records.csv",error),error);
    int backends=1;
    if(Seis3D::Curvilinear::queryBackend3D(BackendKind3D::Cuda).available){Simulation gpu;check(gpu.initialize(p,error,BackendKind3D::Cuda,{1})&&gpu.advance(160,error),error);compare(reference,gpu.snapshot(true));for(std::size_t r=0;r<cpu.receivers().size();++r){compare(cpu.receivers()[r].vx,gpu.receivers()[r].vx);compare(cpu.receivers()[r].vz,gpu.receivers()[r].vz);}check(gpu.reset(error),error);const auto reset=gpu.snapshot(true);for(const auto& a:reset.memory)for(double v:a)check(v==0,"GPU reset left memory variables");++backends;}
    check(cpu.reset(error),error);for(const auto& a:cpu.snapshot(true).memory)for(double v:a)check(v==0,"CPU reset left memory variables");
    std::ofstream(folder/"validation.json")<<std::setprecision(17)<<"{\"backends\":"<<backends<<",\"mechanisms\":3,\"reference_velocity_error_below\":1e-13,\"max_fitted_Q_relative_error\":"<<m.maxRelativeQError<<",\"max_fine_plane_wave_error\":"<<maximumFineError<<",\"passive_pair_rejection\":true,\"curved_free_surface\":true,\"Q_model_roundtrip\":true,\"fractional_records\":true,\"memory_reset\":true}\n";
    std::cout<<"GSLS PASS: analytical P/S amplitude/phase, reference frequency, passive Q pairs, "<<backends<<" backends, curved traction-rate closure, embedded Q model, raw records and reset\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
