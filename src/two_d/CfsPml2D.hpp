#pragma once
#include "Seis2D/CurvilinearSolver2D.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
namespace Seis2D::Curvilinear::Detail {
struct DomainLayout {
    int nx{},nz{},offsetX{},widthX{},widthZ{};
};
inline DomainLayout domainLayout(const Parameters& p){
    DomainLayout d{p.nx,p.nz,0,std::clamp(p.spongeX,1,std::max(1,p.nx/3)),std::clamp(p.spongeZ,1,std::max(1,p.nz/3))};
    if(p.absorbing&&p.externalAbsorption){d.offsetX=d.widthX;d.nx+=2*d.widthX;d.nz+=d.widthZ;}
    return d;
}
inline bool acquisitionInAbsorber(const Parameters& p){
    if(!p.absorbing||p.externalAbsorption)return false;
    const auto d=domainLayout(p);const int sx=p.sourceX<0?p.nx/2:p.sourceX;
    const int support=p.source.kind==SeisWave::SourceKind::IsotropicMoment?4:2;
    if(sx-support<d.widthX||sx+support>=p.nx-d.widthX||p.sourceZ+support>=p.nz-d.widthZ)return true;
    for(int r=0;r<p.receiverCount;++r){const auto x=static_cast<long long>(p.receiverStart)+static_cast<long long>(r)*p.receiverStride;
        if(x<d.widthX||x>=p.nx-d.widthX||p.receiverDepth>=p.nz-d.widthZ)return true;}
    return false;
}
inline bool usesCfsPml(const Parameters& p){return p.absorbing&&p.absorber==Absorber::CfsPml;}
inline std::size_t cpmlScalars(const Parameters& p){
    if(!usesCfsPml(p))return 0;
    const auto d=domainLayout(p);
    return 4*(2*static_cast<std::size_t>(d.widthX)*d.nz+static_cast<std::size_t>(d.widthZ)*d.nx);
}
// d, kappa, alpha: O(Nx+Nz) profiles, no full-domain auxiliary allocation.
inline std::vector<double> cpmlProfile(int nodes,int width,double h,double vp,double reflection,double kappa,double frequency,bool both){
    std::vector<double> result(3*nodes);
    const double d0=-3*vp*std::log(reflection)/(2*width*h);
    for(int i=0;i<nodes;++i){
        const int distance=both?std::min(i,nodes-1-i):nodes-1-i;
        const double t=std::clamp(1.-double(distance)/width,0.,1.);
        result[3*i]=d0*t*t;result[3*i+1]=1+(kappa-1)*t*t;
        result[3*i+2]=t>0?3.14159265358979323846*frequency*(1-t):0;
    }
    return result;
}
inline double cpmlRate(const std::vector<double>& profile){
    double rate=0;for(std::size_t i=0;i<profile.size();i+=3)rate=std::max(rate,profile[i]/profile[i+1]+profile[i+2]);return rate;
}
}
