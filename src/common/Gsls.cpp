#include "SeisCommon/Gsls.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <vector>
namespace SeisWave::Rheology {
namespace {
constexpr double pi=3.14159265358979323846;
void require(bool v,const char* message){if(!v)throw std::runtime_error(message);}
std::array<double,3> strengths(double q,const Material& m,const Settings& settings) {
    constexpr int samples=81;double design[samples][3]{},target=1/q;
    for(int i=0;i<samples;++i){const double w=2*pi*settings.minFrequency*std::pow(settings.maxFrequency/settings.minFrequency,double(i)/(samples-1));
        for(int l=0;l<m.mechanisms;++l){const double x=w/m.rates[l];design[i][l]=(x-target*x*x)/(1+x*x);}}
    std::array<double,3> best{};double bestCost=std::numeric_limits<double>::infinity();
    // Exhaustive active-set NNLS, at most 3 mechanisms. No external fitting dependency.
    for(int mask=1;mask<(1<<m.mechanisms);++mask){int ids[3]{},n=0;for(int l=0;l<m.mechanisms;++l)if(mask&(1<<l))ids[n++]=l;
        double a[3][4]{};for(int i=0;i<n;++i)for(int row=0;row<samples;++row){a[i][n]+=design[row][ids[i]]*target;for(int j=0;j<n;++j)a[i][j]+=design[row][ids[i]]*design[row][ids[j]];}
        bool valid=true;for(int k=0;k<n;++k){int pivot=k;for(int i=k+1;i<n;++i)if(std::abs(a[i][k])>std::abs(a[pivot][k]))pivot=i;
            if(std::abs(a[pivot][k])<1e-20){valid=false;break;}for(int j=0;j<=n;++j)std::swap(a[k][j],a[pivot][j]);const double d=a[k][k];for(int j=k;j<=n;++j)a[k][j]/=d;
            for(int i=0;i<n;++i)if(i!=k){const double f=a[i][k];for(int j=k;j<=n;++j)a[i][j]-=f*a[k][j];}}
        std::array<double,3> candidate{};for(int i=0;i<n&&valid;++i){valid=std::isfinite(a[i][n])&&a[i][n]>=0;candidate[ids[i]]=a[i][n];}if(!valid)continue;
        double cost=0;for(int row=0;row<samples;++row){double residual=-target;for(int l=0;l<m.mechanisms;++l)residual+=design[row][l]*candidate[l];cost+=residual*residual;}
        if(cost<bestCost){bestCost=cost;best=candidate;}}
    require(std::isfinite(bestCost),"No passive GSLS fit for this Q/frequency band.");return best;
}
}
bool validateSettings(const Settings& s,std::string& error){try{
    require(s.mechanisms>=1&&s.mechanisms<=maxMechanisms,"GSLS mechanism count must be 1..3.");
    require(std::isfinite(s.minFrequency)&&std::isfinite(s.maxFrequency)&&std::isfinite(s.referenceFrequency)&&s.minFrequency>0&&s.maxFrequency>s.minFrequency&&s.maxFrequency<=1e6&&s.minFrequency>=.01&&s.referenceFrequency>=s.minFrequency&&s.referenceFrequency<=s.maxFrequency,"GSLS requires 0.01 <= fmin < fmax <= 1e6 Hz and fref within the band.");
    for(double q:{s.qp,s.qs})require(std::isfinite(q)&&q>=5&&q<=1e9,"Qp and Qs must be finite and in 5..1e9; disable attenuation for the elastic limit.");
    error.clear();return true;
}catch(const std::exception& e){error=e.what();return false;}}
std::complex<double> modulus(const Material& m,bool shear,std::complex<double> s){std::complex<double> c=shear?m.sRelaxed:m.pRelaxed;
    for(int l=0;l<m.mechanisms;++l)c+=(shear?m.deltaS[l]:m.deltaP[l])*s/(s+m.rates[l]);return c;}
double qualityFactor(const Material& m,bool shear,double f){const auto c=modulus(m,shear,{0,2*pi*f});return c.real()/std::abs(c.imag());}
double phaseVelocity(const Material& m,bool shear,double rho,double f){return 1/std::sqrt(rho/modulus(m,shear,{0,2*pi*f})).real();}
bool fitMaterial(double vp,double vs,double rho,double qp,double qs,const Settings& settings,Material& result,std::string& error){try{
    if(!validateSettings(settings,error))return false;
    require(std::isfinite(vp)&&std::isfinite(vs)&&std::isfinite(rho)&&rho>0&&vs>0&&vp*vp>4*vs*vs/3,"Invalid GSLS reference velocities/density.");
    for(double q:{qp,qs})require(std::isfinite(q)&&q>=5&&q<=1e9,"Layer Qp/Qs must be in 5..1e9.");
    Material m;m.mechanisms=settings.mechanisms;
    for(int l=0;l<m.mechanisms;++l)m.rates[l]=2*pi*(m.mechanisms==1?std::sqrt(settings.minFrequency*settings.maxFrequency):settings.minFrequency*std::pow(settings.maxFrequency/settings.minFrequency,double(l)/(m.mechanisms-1)));
    const auto p=strengths(qp,m,settings),s=strengths(qs,m,settings);const std::complex<double> w{0,2*pi*settings.referenceFrequency};
    const auto scale=[&](const auto& weights,double velocity){std::complex<double> shape=1.;for(int l=0;l<m.mechanisms;++l)shape+=weights[l]*w/(w+m.rates[l]);const double inv=std::real(1./std::sqrt(shape));return rho*velocity*velocity*inv*inv;};
    m.pRelaxed=scale(p,vp);m.sRelaxed=scale(s,vs);m.pUnrelaxed=m.pRelaxed;m.sUnrelaxed=m.sRelaxed;
    require(m.pRelaxed>4*m.sRelaxed/3,"Qp/Qs produce nonpositive relaxed bulk modulus.");
    for(int l=0;l<m.mechanisms;++l){m.deltaP[l]=m.pRelaxed*p[l];m.deltaS[l]=m.sRelaxed*s[l];
        require(m.deltaP[l]+1e-12*m.pRelaxed>=4*m.deltaS[l]/3,"Qp/Qs violate passive bulk relaxation (Qp cannot be arbitrarily large relative to Qs).");
        m.pUnrelaxed+=m.deltaP[l];m.sUnrelaxed+=m.deltaS[l];}
    require(std::isfinite(m.pUnrelaxed)&&std::isfinite(m.sUnrelaxed),"GSLS moduli overflow.");
    for(int i=0;i<=160;++i){const double f=settings.minFrequency*std::pow(settings.maxFrequency/settings.minFrequency,i/160.);m.maxRelativeQError=std::max({m.maxRelativeQError,std::abs(qualityFactor(m,false,f)/qp-1),std::abs(qualityFactor(m,true,f)/qs-1)});}
    result=m;error.clear();return true;
}catch(const std::exception& e){error=e.what();return false;}}
}
