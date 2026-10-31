#pragma once
#include "SeisCommon/FiniteDifference.hpp"
#include <cmath>
#include <cstddef>
#include <limits>
#include <type_traits>
#ifdef __CUDACC__
#define SEIS_2D_HD __host__ __device__ inline
#else
#define SEIS_2D_HD inline
#endif
namespace Seis2D::Curvilinear::Point {
inline constexpr int maxFields=18; // 5 elastic, or 6 plane-strain stresses/velocities + 4 per GSLS branch.
template<class Real> struct MeshT {
    int nx{}, nz{};
    std::size_t count{};
    Real h{};
    const Real *a{}, *b{}, *jac{}, *normals{}, *lambda{}, *mu{}, *rho{}, *source{}, *damping{};
    Real sourceDirection[3]{0, 0, 1};
    const Real *sourceGradient[2]{};
    int mechanisms{},fieldCount{5};
    const Real *cpmlX{},*cpmlZ{};
    int cpmlWidthX{},cpmlWidthZ{};
    std::size_t cpmlCount{};
    Real rates[3]{};
    const Real *deltaP[3]{},*deltaS[3]{};
};
template<class Real> struct FieldsT {
    Real *f[maxFields]{};
    Real *pml{};
};
template<class Real> SEIS_2D_HD Real root(Real x){if constexpr(std::is_same_v<Real,float>)return ::sqrtf(x);else return ::sqrt(x);}
using Mesh=MeshT<double>;
using Fields=FieldsT<double>;
template<class Real> SEIS_2D_HD std::size_t index(const MeshT<Real> &g, int i, int k) {
    return i + static_cast<std::size_t>(g.nx) * k;
}
template<class Real> SEIS_2D_HD Real derivative(const Real *v, const MeshT<Real> &g, std::size_t q, int axis, int dir,
                             int depth = -1) {
    int offsets[5];
    Real c[5];
    const int n = SeisWave::Difference::stencil(dir, depth, offsets, c);
    Real result = 0;
    for (int l = 0; l < n; ++l)
        result += c[l] * v[static_cast<std::ptrdiff_t>(q) + offsets[l] * (axis ? g.nx : 1)];
    return result / g.h;
}
// Four memories per active computational direction: two velocity derivatives
// and two directional momentum contributions. Corners own both directions.
template<class Real> SEIS_2D_HD std::ptrdiff_t pmlSlot(const MeshT<Real>& g,std::size_t q,int axis){
    if(!g.cpmlCount)return -1;
    const int i=int(q%g.nx)-3,k=int(q/g.nx)-3,nx=g.nx-6,nz=g.nz-6;
    if(axis==0){const int w=g.cpmlWidthX;int j=i<w?i:i>=nx-w?w+i-(nx-w):-1;
        return j<0?-1:4*(static_cast<std::ptrdiff_t>(k)*2*w+j);}
    return k<nz-g.cpmlWidthZ?-1:4*(static_cast<std::ptrdiff_t>(2*g.cpmlWidthX)*nz+static_cast<std::ptrdiff_t>(k-(nz-g.cpmlWidthZ))*nx+i);
}
template<class Real> SEIS_2D_HD Real stretched(const FieldsT<Real>& in,FieldsT<Real> out,const MeshT<Real>& g,std::size_t q,int axis,int field,Real derivative){
    const auto slot=pmlSlot(g,q,axis);if(slot<0)return derivative;
    const int at=axis?int(q/g.nx)-3:int(q%g.nx)-3;
    const Real* profile=(axis?g.cpmlZ:g.cpmlX)+3*at;
    const Real d=profile[0],kappa=profile[1],alpha=profile[2],psi=in.pml[slot+field];
    out.pml[slot+field]=-(d/kappa+alpha)*psi-d/(kappa*kappa)*derivative;
    return derivative/kappa+psi;
}
template<class Real> SEIS_2D_HD bool finitePml(const FieldsT<Real>& w,const MeshT<Real>& g,std::size_t q){
    Real maximum{};if constexpr(std::is_same_v<Real,float>)maximum=3.4028234663852886e38F;else maximum=1.7976931348623157e308;
    for(int axis=0;axis<2;++axis){const auto slot=pmlSlot(g,q,axis);if(slot>=0)for(int f=0;f<4;++f)if(!(w.pml[slot+f]<=maximum&&w.pml[slot+f]>=-maximum))return false;}
    return true;
}
template<class Real> SEIS_2D_HD void project(FieldsT<Real> w, const MeshT<Real> &g, int i) {
    const auto q = index(g, i, 3);
    const Real nx = g.normals[2 * q], nz = g.normals[2 * q + 1];
    const Real tx = w.f[2][q] * nx + w.f[4][q] * nz, tz = w.f[4][q] * nx + w.f[3][q] * nz,
                 nn = nx * tx + nz * tz;
    w.f[2][q] += -2 * nx * tx + nn * nx * nx;
    w.f[3][q] += -2 * nz * tz + nn * nz * nz;
    w.f[4][q] += -nx * tz - tx * nz + nn * nx * nz;
}
template<class Real> SEIS_2D_HD void surfaceDerivative(const FieldsT<Real> &w, const MeshT<Real> &g, std::size_t q, int dir, Real *result,const Real* correctedXi=nullptr) {
    const Real c[2]{g.a[q], g.b[q]}, c2 = c[0] * c[0] + c[1] * c[1], length = root(c2),
                                       n[2]{c[0] / length, c[1] / length};
    const Real dx = correctedXi?correctedXi[0]:derivative(w.f[0], g, q, 0, dir), dz = correctedXi?correctedXi[1]:derivative(w.f[1], g, q, 0, dir),
                 lambda = g.lambda[q], mu = g.mu[q];
    Real t[2]{(lambda + 2 * mu) * dx * c[0] + mu * dz * c[1], mu * dz * c[0] + lambda * dx * c[1]};
    for(int l=0;l<g.mechanisms;++l){const int m=6+4*l;t[0]-=w.f[m][q]*c[0]+w.f[m+2][q]*c[1];t[1]-=w.f[m+2][q]*c[0]+w.f[m+1][q]*c[1];}
    const Real normal = t[0] * n[0] + t[1] * n[1];
    for (int v = 0; v < 2; ++v)
        result[v] = -(t[v] - n[v] * normal) / (mu * c2) - n[v] * normal / ((lambda + 2 * mu) * c2);
}
template<class Real> SEIS_2D_HD Real flux(const FieldsT<Real> &w, const MeshT<Real> &g, std::size_t q, int axis, int component) {
    const Real x = axis ? g.a[q] : 1, z = axis ? g.b[q] : 0;
    return g.jac[q] * (x * w.f[component ? 4 : 2][q] + z * w.f[component ? 3 : 4][q]);
}
template<class Real> SEIS_2D_HD Real momentum(const FieldsT<Real> &w, const MeshT<Real> &g, std::size_t q, int component, const int *dirs,
                           int depth,int onlyAxis=-1) {
    Real result = 0;
    for (int axis = 0; axis < 2; ++axis) {
        if(onlyAxis>=0&&axis!=onlyAxis)continue;
        int offsets[5];
        Real c[5];
        const int n = SeisWave::Difference::stencil(dirs[axis], -1, offsets, c);
        for (int l = 0; l < n; ++l) {
            int offset = offsets[l];
            Real sign = 1;
            if (axis) {
                const int k = depth + offset;
                if (k == 0)
                    continue;
                if (k < 0) {
                    offset = -k - depth;
                    sign = -1;
                }
            }
            result +=
                sign * c[l] *
                flux(w, g, static_cast<std::ptrdiff_t>(q) + offset * (axis ? g.nx : 1), axis, component);
        }
    }
    return result / (g.h * g.jac[q] * g.rho[q]);
}
template<class Real> SEIS_2D_HD void rhs(const FieldsT<Real> &w, FieldsT<Real> out, const MeshT<Real> &g, std::size_t q, std::type_identity_t<Real> forcing, int step,
                    int stage) {
    const int depth = static_cast<int>(q / g.nx) - 3,
              dirs[2]{SeisWave::Difference::direction(step, stage, 0),
                      SeisWave::Difference::direction(step, stage, 2)};
    Real surface[2]{};
    if (depth == 0){
        Real xi[2]{};
        if(g.cpmlCount)for(int v=0;v<2;++v)xi[v]=stretched(w,out,g,q,0,v,derivative(w.f[v],g,q,0,dirs[0]));
        surfaceDerivative(w,g,q,dirs[0],surface,g.cpmlCount?xi:nullptr);
    }
    Real dx[5]{}, dz[5]{},xis[5]{},etas[5]{};
    for (int f = 0; f < 5; ++f) {
        const Real xi = derivative(w.f[f], g, q, 0, dirs[0]);
        const Real eta =
            f < 2 && depth == 0 ? surface[f] : derivative(w.f[f], g, q, 1, dirs[1], f < 2 ? depth : -1);
        xis[f]=xi;etas[f]=eta;
        const Real vx=g.cpmlCount&&f<2?stretched(w,out,g,q,0,f,xi):xi;
        const Real vz=g.cpmlCount&&f<2?stretched(w,out,g,q,1,f,eta):eta;
        dx[f] = vx + g.a[q] * vz;
        dz[f] = g.b[q] * vz;
    }
    out.f[0][q] = (dx[2] + dz[4]) / g.rho[q];
    out.f[1][q] = (dx[4] + dz[3]) / g.rho[q];
    if (depth <= (dirs[1] ? 3 : 1))
        for (int v = 0; v < 2; ++v)
            out.f[v][q] = momentum(w, g, q, v, dirs, depth);
    if(g.cpmlCount)for(int v=0;v<2;++v){
        const int f=v?4:2,other=v?3:4;
        Real x=xis[f]/g.rho[q],z=(g.a[q]*etas[f]+g.b[q]*etas[other])/g.rho[q];
        if(depth<=(dirs[1]?3:1)){x=momentum(w,g,q,v,dirs,depth,0);z=momentum(w,g,q,v,dirs,depth,1);}
        out.f[v][q]=stretched(w,out,g,q,0,2+v,x)+stretched(w,out,g,q,1,2+v,z);
    }
    for (int v = 0; v < 2; ++v)
        out.f[v][q] +=
            forcing *
            (g.sourceGradient[v] ? g.sourceGradient[v][q] : g.source[q] * g.sourceDirection[v == 0 ? 0 : 2]) /
            g.rho[q];
    const Real lambda = g.lambda[q], mu = g.mu[q], div = dx[0] + dz[1];
    out.f[2][q] = lambda * div + 2 * mu * dx[0];
    out.f[3][q] = lambda * div + 2 * mu * dz[1];
    out.f[4][q] = mu * (dz[0] + dx[1]);
    if(g.mechanisms){out.f[5][q]=lambda*div;
        for(int l=0;l<g.mechanisms;++l){const int m=6+4*l;const Real dp=g.deltaP[l][q],ds=g.deltaS[l][q],dl=dp-2*ds;
            const Real drive[4]{dl*div+2*ds*dx[0],dl*div+2*ds*dz[1],ds*(dz[0]+dx[1]),dl*div};
            out.f[2][q]-=w.f[m][q];out.f[3][q]-=w.f[m+1][q];out.f[4][q]-=w.f[m+2][q];out.f[5][q]-=w.f[m+3][q];
            for(int f=0;f<4;++f)out.f[m+f][q]=g.rates[l]*(drive[f]-w.f[m+f][q]);
        }
    }
    for (int f = 0; f < g.fieldCount; ++f)
        out.f[f][q] -= g.damping[q] * w.f[f][q];
}
template<class Real> SEIS_2D_HD Real value(const FieldsT<Real> &w, const MeshT<Real> &g, std::size_t q, int component) {
    return component == 0   ? w.f[1][q]
           : component == 1 ? w.f[0][q]
           : component == 2
               ? root(w.f[0][q] * w.f[0][q] + w.f[1][q] * w.f[1][q])
               : g.mechanisms ? -(w.f[2][q]+w.f[3][q]+w.f[5][q])/3
               : -(1 + g.lambda[q] / (2 * (g.lambda[q] + g.mu[q]))) * (w.f[2][q] + w.f[3][q]) / 3;
}
template<class Real> SEIS_2D_HD double energy(const FieldsT<Real> &w, const MeshT<Real> &g, std::size_t q) {
    const double l=g.lambda[q],m=g.mu[q],xx=w.f[2][q],zz=w.f[3][q],xz=w.f[4][q],vx=w.f[0][q],vz=w.f[1][q];
    return (.5*double(g.rho[q])*(vx*vx+vz*vz)+((l+2*m)*(xx*xx+zz*zz)-2*l*xx*zz)/(8*m*(l+m))+xz*xz/(2*m))*double(g.jac[q])*double(g.h)*double(g.h);
}
} // namespace Seis2D::Curvilinear::Point
#undef SEIS_2D_HD
