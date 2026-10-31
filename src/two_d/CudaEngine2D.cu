#include "../common/CudaSupport.cuh"
#include "CudaEngine2D.hpp"
#include <algorithm>
namespace Seis2D::Curvilinear {
namespace {
using SeisWave::Cuda::checked;
using SeisWave::Cuda::DeviceBuffer;
using SeisWave::Cuda::DeviceScope;
constexpr int blockSize = 128;
template<class Real> __device__ bool physical(const Point::MeshT<Real> &g, std::size_t q) {
    const int i = static_cast<int>(q % g.nx), k = static_cast<int>(q / g.nx);
    return i >= 3 && i < g.nx - 3 && k >= 3 && k < g.nz - 3;
}
template<class Real> __global__ void rhsKernel(Point::FieldsT<Real> in, Point::FieldsT<Real> out, Point::MeshT<Real> g, Real force, int step,
                          int stage) {
    const std::size_t q = static_cast<std::size_t>(blockIdx.x) * blockDim.x + threadIdx.x;
    if (q < g.count && physical(g, q))
        Point::rhs(in, out, g, q, force, step, stage);
}
template<class Real> __global__ void projectKernel(Point::FieldsT<Real> fields, Point::MeshT<Real> g) {
    const int i = blockIdx.x * blockDim.x + threadIdx.x + 3;
    if (i < g.nx - 3)
        Point::project(fields, g, i);
}
template<class Real> __global__ void combineKernel(Point::FieldsT<Real> out, Point::FieldsT<Real> base, Point::FieldsT<Real> rate, Point::FieldsT<Real> a,
                              Point::FieldsT<Real> b, Point::FieldsT<Real> c, Point::FieldsT<Real> d, std::size_t count, Real dt,
                              bool final,int fieldCount) {
    const std::size_t q = static_cast<std::size_t>(blockIdx.x) * blockDim.x + threadIdx.x;
    if (q < count)
        for (int f = 0; f < fieldCount; ++f)
            out.f[f][q] =
                base.f[f][q] + (final ? dt * (a.f[f][q] + 2 * b.f[f][q] + 2 * c.f[f][q] + d.f[f][q]) / Real(6)
                                      : dt * rate.f[f][q]);
}
template<class Real> __global__ void combinePml(Real* out,const Real* base,const Real* rate,const Real* a,const Real* b,const Real* c,const Real* d,std::size_t count,Real dt,bool final){
    const auto q=static_cast<std::size_t>(blockIdx.x)*blockDim.x+threadIdx.x;
    if(q<count)out[q]=base[q]+(final?dt*(a[q]+2*b[q]+2*c[q]+d[q])/Real(6):dt*rate[q]);
}
struct Stats {
    double energy{}, velocity{}, traction{};
    int bad{};
};
template<class Real> __global__ void statsKernel(Point::FieldsT<Real> w, Point::MeshT<Real> g, Stats *output) {
    __shared__ Stats shared[blockSize];
    Stats s{};
    const std::size_t q = static_cast<std::size_t>(blockIdx.x) * blockDim.x + threadIdx.x;
    if (q < g.count && physical(g, q)) {
        for (int f = 0; f < g.fieldCount; ++f)
            if (!isfinite(w.f[f][q]))
                s.bad = 1;
        if(!Point::finitePml(w,g,q))s.bad=1;
        s.energy = Point::energy(w, g, q);
        s.velocity = Point::value(w, g, q, 2);
        if (!isfinite(s.energy) || !isfinite(s.velocity))
            s.bad = 1;
        if (q / g.nx == 3) {
            const double nx = g.normals[2 * q], nz = g.normals[2 * q + 1],xx=w.f[2][q],zz=w.f[3][q],xz=w.f[4][q],
                         tx = xx * nx + xz * nz, tz = xz * nx + zz * nz,
                         norm = sqrt(xx * xx + zz * zz + 2 * xz * xz);
            s.traction = sqrt(tx * tx + tz * tz) / fmax(norm, 1e-25);
        }
    }
    shared[threadIdx.x] = s;
    __syncthreads();
    for (int stride = blockSize / 2; stride > 0; stride /= 2) {
        if (threadIdx.x < stride) {
            auto &a = shared[threadIdx.x];
            const auto b = shared[threadIdx.x + stride];
            a.energy += b.energy;
            a.velocity = fmax(a.velocity, b.velocity);
            a.traction = fmax(a.traction, b.traction);
            a.bad |= b.bad;
        }
        __syncthreads();
    }
    if (threadIdx.x == 0)
        output[blockIdx.x] = shared[0];
}
template<class Real> __global__ void sampleKernel(Point::FieldsT<Real> w, const std::size_t *indices, double *out, int count) {
    const int r = blockIdx.x * blockDim.x + threadIdx.x;
    if (r < count) {
        out[2 * r] = w.f[0][indices[r]];
        out[2 * r + 1] = w.f[1][indices[r]];
    }
}
template<class Real> __global__ void probeKernel(Point::FieldsT<Real> w,const std::size_t* indices,double* out,int count){
    const int r=blockIdx.x*blockDim.x+threadIdx.x;
    if(r<count)for(int f=0;f<5;++f)out[5*r+f]=w.f[f][indices[r]];
}
template<class Real> __global__ void frameKernel(Point::FieldsT<Real> w, Point::MeshT<Real> g, int component, float *out,int nx,int nz,int offsetX) {
    const int q = blockIdx.x * blockDim.x + threadIdx.x;
    if (q < nx * nz)
        out[q] = static_cast<float>(Point::value(w, g, Point::index(g, q % nx + 3+offsetX, q / nx + 3), component));
}
} // namespace

struct CudaEngine::Impl {
    virtual ~Impl()=default;
    virtual void step(double,double,double,double,int)=0;
    virtual void rollback()=0;virtual void reset()=0;virtual void diagnostics(Diagnostics&)=0;
    virtual void sample(std::vector<Receiver>&)=0;
    virtual void frame(int,std::vector<float>&,int,int,int)const=0;
    virtual Snapshot snapshot(bool)const=0;
    virtual void probes(const std::vector<std::size_t>&,std::vector<std::array<double,5>>&)const=0;
    virtual std::size_t residentBytes()const=0;
};
template<class Real> class TypedEngine final:public CudaEngine::Impl {
    int device{},blocks{},registers{};std::size_t localBytes{};
    Point::MeshT<Real> mesh;
    DeviceBuffer fields[7],pml[7],cpmlX,cpmlZ,a,b,jac,normals,lambda,mu,rho,source,damping,stats,receiverIndices,receiverSamples,display;
    DeviceBuffer sourceGradient[2],deltaP[3],deltaS[3];
    Point::FieldsT<Real> state[7];std::vector<Stats> hostStats;std::vector<double> samples;
    static Real* ptr(DeviceBuffer& buffer){return static_cast<Real*>(buffer.pointer);}
    static const Real* ptr(const DeviceBuffer& buffer){return static_cast<const Real*>(buffer.pointer);}
    void project(Point::FieldsT<Real> w){projectKernel<Real><<<(mesh.nx-6+127)/128,128>>>(w,mesh);checked(cudaGetLastError(),"2D surface launch");}
    void rhs(Point::FieldsT<Real> in,Point::FieldsT<Real> out,Real force,int step,int stage){rhsKernel<Real><<<blocks,blockSize>>>(in,out,mesh,force,step,stage);checked(cudaGetLastError(),"2D RHS launch");}
    void combine(Point::FieldsT<Real> out,Point::FieldsT<Real> rate,Real dt,bool final){
        combineKernel<Real><<<blocks,blockSize>>>(out,state[1],rate,state[2],state[3],state[4],state[5],mesh.count,dt,final,mesh.fieldCount);checked(cudaGetLastError(),"2D stage launch");
        if(mesh.cpmlCount){combinePml<Real><<<(mesh.cpmlCount+127)/128,128>>>(out.pml,state[1].pml,rate.pml,state[2].pml,state[3].pml,state[4].pml,state[5].pml,mesh.cpmlCount,dt,final);checked(cudaGetLastError(),"2D CFS-PML RK stage");}project(out);
    }
    void download(std::vector<double>& out,const Real* values,std::size_t count)const{
        out.resize(count);
        if constexpr(std::is_same_v<Real,double>)checked(cudaMemcpy(out.data(),values,count*sizeof(Real),cudaMemcpyDeviceToHost),"2D snapshot download");
        else {std::vector<Real> staging(count);checked(cudaMemcpy(staging.data(),values,count*sizeof(Real),cudaMemcpyDeviceToHost),"2D FP32 snapshot download");std::copy(staging.begin(),staging.end(),out.begin());}
    }
public:
    TypedEngine(const Point::MeshT<Real>& input,const std::vector<Receiver>& receivers,int chosen):device(chosen),mesh(input){
        DeviceScope scope(device);cudaFuncAttributes attributes{};checked(cudaFuncGetAttributes(&attributes,rhsKernel<Real>),"2D kernel compatibility");registers=attributes.numRegs;localBytes=attributes.localSizeBytes;
        blocks=static_cast<int>((mesh.count+127)/128);
        for(int s=0;s<7;++s){fields[s].allocate(mesh.fieldCount*mesh.count*sizeof(Real));for(int f=0;f<mesh.fieldCount;++f)state[s].f[f]=ptr(fields[s])+f*mesh.count;}
        if(mesh.cpmlCount){for(int s=0;s<7;++s){pml[s].allocate(mesh.cpmlCount*sizeof(Real));state[s].pml=ptr(pml[s]);}
            cpmlX.upload(input.cpmlX,3*(mesh.nx-6)*sizeof(Real));cpmlZ.upload(input.cpmlZ,3*(mesh.nz-6)*sizeof(Real));mesh.cpmlX=ptr(cpmlX);mesh.cpmlZ=ptr(cpmlZ);}
        const auto upload=[&](DeviceBuffer& buffer,const Real* host,std::size_t n){buffer.upload(host,n*sizeof(Real));return ptr(buffer);};
        mesh.a=upload(a,input.a,mesh.count);mesh.b=upload(b,input.b,mesh.count);mesh.jac=upload(jac,input.jac,mesh.count);mesh.normals=upload(normals,input.normals,2*mesh.count);
        mesh.lambda=upload(lambda,input.lambda,mesh.count);mesh.mu=upload(mu,input.mu,mesh.count);mesh.rho=upload(rho,input.rho,mesh.count);mesh.source=upload(source,input.source,mesh.count);mesh.damping=upload(damping,input.damping,mesh.count);
        for(int axis=0;axis<2;++axis)if(input.sourceGradient[axis])mesh.sourceGradient[axis]=upload(sourceGradient[axis],input.sourceGradient[axis],mesh.count);
        for(int l=0;l<mesh.mechanisms;++l){mesh.deltaP[l]=upload(deltaP[l],input.deltaP[l],mesh.count);mesh.deltaS[l]=upload(deltaS[l],input.deltaS[l],mesh.count);}
        stats.allocate(blocks*sizeof(Stats));hostStats.resize(blocks);std::vector<std::size_t> indices;
        for(const auto& r:receivers)indices.push_back(Point::index(mesh,r.x+3,r.z+3));
        receiverIndices.upload(indices.data(),indices.size()*sizeof(std::size_t));receiverSamples.allocate(indices.size()*2*sizeof(double));samples.resize(indices.size()*2);
        display.allocate(static_cast<std::size_t>(mesh.nx-6)*(mesh.nz-6)*sizeof(float));
    }
    void step(double time,double delta,double frequency,double scale,int number)override{
        DeviceScope scope(device);checked(cudaMemcpy(state[1].f[0],state[0].f[0],mesh.fieldCount*mesh.count*sizeof(Real),cudaMemcpyDeviceToDevice),"2D save step");
        if(mesh.cpmlCount)checked(cudaMemcpy(state[1].pml,state[0].pml,mesh.cpmlCount*sizeof(Real),cudaMemcpyDeviceToDevice),"2D CFS-PML save step");
        const auto force=[&](double t){const double v=3.14159265358979323846*frequency*(t-1.5/frequency);return static_cast<Real>(scale*(1-2*v*v)*std::exp(-v*v));};
        const Real dt=static_cast<Real>(delta);
        rhs(state[0],state[2],force(time),number,0);combine(state[6],state[2],dt/Real(2),false);
        rhs(state[6],state[3],force(time+delta/2),number,1);combine(state[6],state[3],dt/Real(2),false);
        rhs(state[6],state[4],force(time+delta/2),number,2);combine(state[6],state[4],dt,false);
        rhs(state[6],state[5],force(time+delta),number,3);combine(state[0],state[2],dt,true);
    }
    void rollback()override{DeviceScope scope(device);checked(cudaMemcpy(state[0].f[0],state[1].f[0],mesh.fieldCount*mesh.count*sizeof(Real),cudaMemcpyDeviceToDevice),"2D rollback");if(mesh.cpmlCount)checked(cudaMemcpy(state[0].pml,state[1].pml,mesh.cpmlCount*sizeof(Real),cudaMemcpyDeviceToDevice),"2D PML rollback");}
    void reset()override{DeviceScope scope(device);for(auto& f:fields)checked(cudaMemset(f.pointer,0,f.allocatedBytes),"2D reset");if(mesh.cpmlCount)for(auto& f:pml)checked(cudaMemset(f.pointer,0,f.allocatedBytes),"2D PML reset");checked(cudaDeviceSynchronize(),"2D clear completion");}
    void diagnostics(Diagnostics& d)override{
        DeviceScope scope(device);statsKernel<Real><<<blocks,blockSize>>>(state[0],mesh,static_cast<Stats*>(stats.pointer));checked(cudaGetLastError(),"2D diagnostics");checked(cudaMemcpy(hostStats.data(),stats.pointer,hostStats.size()*sizeof(Stats),cudaMemcpyDeviceToHost),"2D diagnostics readback");
        d.energy=d.maxVelocity=d.tractionRelative=0;d.finite=true;for(const auto& r:hostStats){d.energy+=r.energy;d.maxVelocity=std::max(d.maxVelocity,r.velocity);d.tractionRelative=std::max(d.tractionRelative,r.traction);d.finite&=!r.bad;}d.finite&=std::isfinite(d.energy);d.rhsRegisters=registers;d.rhsLocalBytes=localBytes;
    }
    void sample(std::vector<Receiver>& receivers)override{
        DeviceScope scope(device);sampleKernel<Real><<<(static_cast<int>(receivers.size())+127)/128,128>>>(state[0],static_cast<std::size_t*>(receiverIndices.pointer),static_cast<double*>(receiverSamples.pointer),static_cast<int>(receivers.size()));checked(cudaGetLastError(),"2D receiver launch");
        checked(cudaMemcpy(samples.data(),receiverSamples.pointer,samples.size()*sizeof(double),cudaMemcpyDeviceToHost),"2D receiver readback");for(std::size_t r=0;r<receivers.size();++r){receivers[r].vx.push_back(samples[2*r]);receivers[r].vz.push_back(samples[2*r+1]);}
    }
    void frame(int component,std::vector<float>& values,int nx,int nz,int offset)const override{
        DeviceScope scope(device);const int count=nx*nz;values.resize(count);frameKernel<Real><<<(count+127)/128,128>>>(state[0],mesh,component,static_cast<float*>(display.pointer),nx,nz,offset);checked(cudaGetLastError(),"2D display launch");checked(cudaMemcpy(values.data(),display.pointer,count*sizeof(float),cudaMemcpyDeviceToHost),"2D display readback");
    }
    void probes(const std::vector<std::size_t>& indices,std::vector<std::array<double,5>>& out)const override{
        out.resize(indices.size());if(indices.empty())return;
        DeviceScope scope(device);DeviceBuffer locations,values;
        locations.upload(indices.data(),indices.size()*sizeof(std::size_t));values.allocate(indices.size()*5*sizeof(double));
        probeKernel<Real><<<(static_cast<int>(indices.size())+127)/128,128>>>(state[0],static_cast<const std::size_t*>(locations.pointer),static_cast<double*>(values.pointer),static_cast<int>(indices.size()));
        checked(cudaGetLastError(),"2D compact probe launch");
        checked(cudaMemcpy(out.data(),values.pointer,indices.size()*5*sizeof(double),cudaMemcpyDeviceToHost),"2D compact probe readback");
    }
    Snapshot snapshot(bool memories)const override{
        DeviceScope scope(device);Snapshot result;for(int f=0;f<5;++f)download(result.fields[f],state[0].f[f],mesh.count);
        if(mesh.mechanisms){download(result.syy,state[0].f[5],mesh.count);if(memories){result.memory.resize(4*mesh.mechanisms);for(int f=6;f<mesh.fieldCount;++f)download(result.memory[f-6],state[0].f[f],mesh.count);}}
        if(memories&&mesh.cpmlCount)download(result.boundaryMemory,state[0].pml,mesh.cpmlCount);return result;
    }
    std::size_t residentBytes()const override{
        std::size_t n=0;for(const auto& f:fields)n+=f.allocatedBytes;for(const auto& f:pml)n+=f.allocatedBytes;
        for(const auto& f:sourceGradient)n+=f.allocatedBytes;for(const auto& f:deltaP)n+=f.allocatedBytes;for(const auto& f:deltaS)n+=f.allocatedBytes;
        for(const auto* f:{&cpmlX,&cpmlZ,&a,&b,&jac,&normals,&lambda,&mu,&rho,&source,&damping,&stats,&receiverIndices,&receiverSamples,&display})n+=f->allocatedBytes;return n;
    }
};
CudaEngine::CudaEngine(const Point::Mesh& mesh,const std::vector<Receiver>& receivers,int device):impl_(std::make_unique<TypedEngine<double>>(mesh,receivers,device)){}
CudaEngine::CudaEngine(const Point::MeshT<float>& mesh,const std::vector<Receiver>& receivers,int device):impl_(std::make_unique<TypedEngine<float>>(mesh,receivers,device)){}
CudaEngine::~CudaEngine()=default;
void CudaEngine::step(double t,double dt,double f,double scale,int n){impl_->step(t,dt,f,scale,n);}
void CudaEngine::rollback(){impl_->rollback();}void CudaEngine::reset(){impl_->reset();}
void CudaEngine::diagnostics(Diagnostics& d){impl_->diagnostics(d);}void CudaEngine::sample(std::vector<Receiver>& r){impl_->sample(r);}
void CudaEngine::frame(int c,std::vector<float>& v,int nx,int nz,int x)const{impl_->frame(c,v,nx,nz,x);}
Snapshot CudaEngine::snapshot(bool m)const{return impl_->snapshot(m);}std::size_t CudaEngine::residentBytes()const{return impl_->residentBytes();}
void CudaEngine::probes(const std::vector<std::size_t>& i,std::vector<std::array<double,5>>& v)const{impl_->probes(i,v);}
} // namespace Seis2D::Curvilinear
