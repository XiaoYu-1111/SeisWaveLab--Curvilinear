#include "CudaEngine3D.hpp"
#include "../common/CudaSupport.cuh"
#include <cuda_runtime.h>
#include <algorithm>
#include <stdexcept>
#include <vector>
namespace Seis3D::Curvilinear {
namespace {
constexpr int blockSize=128;
using SeisWave::Cuda::checked;
using SeisWave::Cuda::DeviceScope;
using SeisWave::Cuda::DeviceBuffer;
__device__ bool physical(Point::Mesh g,int& i,int& j,int& k,std::size_t q) {
    i=static_cast<int>(q%g.nx);j=static_cast<int>((q/g.nx)%g.ny);k=static_cast<int>(q/(static_cast<std::size_t>(g.nx)*g.ny));
    return i>=ghostWidth3D&&i<g.nx-ghostWidth3D&&j>=ghostWidth3D&&j<g.ny-ghostWidth3D&&k>=ghostWidth3D&&k<g.nz-ghostWidth3D;
}
__global__ void surfaceKernel(Point::Fields w,Point::Mesh g) {
    const int i=static_cast<int>(blockIdx.x*blockDim.x+threadIdx.x)%(g.nx-2*ghostWidth3D)+ghostWidth3D;
    const int j=static_cast<int>(blockIdx.x*blockDim.x+threadIdx.x)/(g.nx-2*ghostWidth3D)+ghostWidth3D;
    if(j>=g.ny-ghostWidth3D)return;
    Point::project(w,g,i,j);
}
__global__ void rhsKernel(Point::Fields w,Point::Fields out,Point::Mesh g,double source,int step,int stage) {
    const std::size_t q=static_cast<std::size_t>(blockIdx.x)*blockDim.x+threadIdx.x;int i,j,k;
    if(q<g.count&&physical(g,i,j,k,q))Point::rhs(w,out,g,q,source,step,stage);
}
__global__ void combineKernel(Point::Fields out,Point::Fields base,Point::Fields a,Point::Fields b,Point::Fields c,Point::Fields d,std::size_t count,double dt,bool final) {
    const std::size_t q=static_cast<std::size_t>(blockIdx.x)*blockDim.x+threadIdx.x;if(q>=count)return;
    for(int f=0;f<9;++f)out.f[f][q]=final?base.f[f][q]+(dt/6)*a.f[f][q]+(dt/3)*b.f[f][q]+(dt/3)*c.f[f][q]+(dt/6)*d.f[f][q]:base.f[f][q]+dt*a.f[f][q];
}
struct Stats {double peak[5],energy,rateNorm,rateTraction;int bad;};
__device__ Stats merge(Stats a,const Stats& b) {
    for(int c=0;c<5;++c)a.peak[c]=fmax(a.peak[c],b.peak[c]);
    a.energy+=b.energy;a.rateNorm=fmax(a.rateNorm,b.rateNorm);a.rateTraction=fmax(a.rateTraction,b.rateTraction);a.bad|=b.bad;return a;
}
__global__ void statsKernel(Point::Fields w,Point::Fields rate,Point::Mesh g,Stats* output) {
    __shared__ Stats shared[blockSize];Stats s{};
    const std::size_t q=static_cast<std::size_t>(blockIdx.x)*blockDim.x+threadIdx.x;int i,j,k;
    if(q<g.count&&physical(g,i,j,k,q)) {
        for(int f=0;f<9;++f)if(!isfinite(w.f[f][q]))s.bad=1;
        const double v=sqrt(w.f[0][q]*w.f[0][q]+w.f[1][q]*w.f[1][q]+w.f[2][q]*w.f[2][q]);
        s.peak[0]=fabs(w.f[2][q]);s.peak[1]=fabs(w.f[0][q]);s.peak[2]=fabs(w.f[1][q]);s.peak[3]=v;s.peak[4]=fabs((w.f[3][q]+w.f[4][q]+w.f[5][q])/3);
        s.energy=Point::energyDensity(w,g,q);if(!isfinite(s.energy)||!isfinite(v))s.bad=1;
        if(k==ghostWidth3D) {
            double sigma[9],t[3]{};Point::stress(rate,q,sigma);const double* n=g.normals+3*q;
            for(int a=0;a<3;++a)for(int b=0;b<3;++b)t[a]+=sigma[3*a+b]*n[b];
            s.rateTraction=sqrt(t[0]*t[0]+t[1]*t[1]+t[2]*t[2]);
            double norm=0;for(int f=0;f<9;++f)norm+=sigma[f]*sigma[f];s.rateNorm=sqrt(norm);
            if(!isfinite(s.rateNorm)||!isfinite(s.rateTraction))s.bad=1;
        }
    }
    shared[threadIdx.x]=s;__syncthreads();
    for(int stride=blockSize/2;stride>0;stride/=2){if(threadIdx.x<stride)shared[threadIdx.x]=merge(shared[threadIdx.x],shared[threadIdx.x+stride]);__syncthreads();}
    if(threadIdx.x==0)output[blockIdx.x]=shared[0];
}
__global__ void receiverKernel(Point::Fields w,const std::size_t* indices,double* values,int n) {
    const int r=blockIdx.x*blockDim.x+threadIdx.x;if(r<n)for(int c=0;c<3;++c)values[3*r+c]=w.f[c][indices[r]];
}
__global__ void sliceKernel(Point::Fields w,Point::Mesh g,DisplayRequest3D request,float* output,int plane,int width,int height) {
    const int q=static_cast<int>(blockIdx.x*blockDim.x+threadIdx.x);if(q>=width*height)return;
    const int a=q%width,b=q/width;
    const auto p=Point::index(g,(plane==1?request.x:a)+ghostWidth3D,(plane==0?request.y:plane==1?a:b)+ghostWidth3D,(plane==2?request.z:b)+ghostWidth3D);
    const double value=request.component==0?w.f[2][p]:request.component==1?w.f[0][p]:request.component==2?w.f[1][p]:request.component==4?-(w.f[3][p]+w.f[4][p]+w.f[5][p])/3:sqrt(w.f[0][p]*w.f[0][p]+w.f[1][p]*w.f[1][p]+w.f[2][p]*w.f[2][p]);
    const double density=Point::localDensity(g,p),mu=Point::localMu(g,p);
    output[q]=static_cast<float>(request.component<5?value:request.component==5?sqrt((Point::localLambda(g,p)+2*mu)/density):request.component==6?sqrt(mu/density):density);
}
}
struct CudaEngine3D::Impl {
    Point::Mesh mesh;
    DeviceBuffer sourceGradient[3];
    DeviceBuffer fields[7],metrics,normals,jacobian,source,damping,materialLambda,materialMu,materialDensity,statsBuffer,receiverIndices,receiverValues,sliceBuffer;
    Point::Fields state,base,k1,k2,k3,k4,stage;
    std::vector<Stats> hostStats;
    std::vector<double> hostReceivers;
    int blocks{},surfaceBlocks{},device{-1};
    Point::Fields view(DeviceBuffer& buffer) const {Point::Fields w;for(int f=0;f<9;++f)w.f[f]=buffer.doubles()+f*mesh.count;return w;}
    void project(Point::Fields w){surfaceKernel<<<surfaceBlocks,blockSize>>>(w,mesh);checked(cudaGetLastError(),"surface projection launch");}
    void rhs(Point::Fields w,Point::Fields out,double value,int step,int stage){rhsKernel<<<blocks,blockSize>>>(w,out,mesh,value,step,stage);checked(cudaGetLastError(),"RHS launch");}
};
CudaEngine3D::CudaEngine3D(const Point::Mesh& mesh,const std::vector<ReceiverTrace3D>& receivers,int device):impl_(std::make_unique<Impl>()) {
    auto& s=*impl_;s.device=device;DeviceScope scope(device);s.mesh=mesh;
    std::size_t freeBytes=0,totalBytes=0;checked(cudaMemGetInfo(&freeBytes,&totalBytes),"memory query");
    const double required=mesh.count*((mesh.materialLambda?81.0:78.0)+(mesh.sourceGradient[0]?3:0))*sizeof(double)+(mesh.count+127)/128.0*sizeof(Stats)+receivers.size()*4.0*sizeof(double);
    if(required>freeBytes*0.9)throw std::runtime_error("Insufficient free GPU memory for the new session (current session is retained). Reduce the grid or choose CPU.");
    for(auto& buffer:s.fields)buffer.allocate(9*mesh.count*sizeof(double));
    s.metrics.upload(mesh.metrics,9*mesh.count*sizeof(double));s.mesh.metrics=s.metrics.doubles();
    s.normals.upload(mesh.normals,3*mesh.count*sizeof(double));s.mesh.normals=s.normals.doubles();
    s.jacobian.upload(mesh.jacobian,mesh.count*sizeof(double));s.mesh.jacobian=s.jacobian.doubles();
    s.source.upload(mesh.source,mesh.count*sizeof(double));s.mesh.source=s.source.doubles();
    for(int a=0;a<3;++a)if(mesh.sourceGradient[a]) {
        s.sourceGradient[a].upload(mesh.sourceGradient[a],mesh.count*sizeof(double));
        s.mesh.sourceGradient[a]=s.sourceGradient[a].doubles();
    }
    s.damping.upload(mesh.damping,mesh.count*sizeof(double));s.mesh.damping=s.damping.doubles();
    if(mesh.materialLambda){s.materialLambda.upload(mesh.materialLambda,mesh.count*sizeof(double));s.materialMu.upload(mesh.materialMu,mesh.count*sizeof(double));s.materialDensity.upload(mesh.materialDensity,mesh.count*sizeof(double));s.mesh.materialLambda=s.materialLambda.doubles();s.mesh.materialMu=s.materialMu.doubles();s.mesh.materialDensity=s.materialDensity.doubles();}
    s.state=s.view(s.fields[0]);s.base=s.view(s.fields[1]);s.k1=s.view(s.fields[2]);s.k2=s.view(s.fields[3]);s.k3=s.view(s.fields[4]);s.k4=s.view(s.fields[5]);s.stage=s.view(s.fields[6]);
    s.blocks=static_cast<int>((mesh.count+blockSize-1)/blockSize);s.surfaceBlocks=((mesh.nx-2*ghostWidth3D)*(mesh.ny-2*ghostWidth3D)+blockSize-1)/blockSize;
    s.hostStats.resize(s.blocks);s.statsBuffer.allocate(s.blocks*sizeof(Stats));
    std::vector<std::size_t> indices;for(const auto& r:receivers)indices.push_back(r.gridIndex);
    s.receiverIndices.upload(indices.data(),indices.size()*sizeof(std::size_t));s.receiverValues.allocate(indices.size()*3*sizeof(double));s.hostReceivers.resize(indices.size()*3);
    const auto maxPlane=std::max({(mesh.nx-2*ghostWidth3D)*(mesh.ny-2*ghostWidth3D),(mesh.nx-2*ghostWidth3D)*(mesh.nz-2*ghostWidth3D),(mesh.ny-2*ghostWidth3D)*(mesh.nz-2*ghostWidth3D)});
    s.sliceBuffer.allocate(maxPlane*sizeof(float));
}
CudaEngine3D::~CudaEngine3D()=default;
BackendMemory3D CudaEngine3D::residentMemory() const noexcept {
    BackendMemory3D m;if(!impl_)return m;const auto& s=*impl_;
    m.hostAvailableBytes=sizeof(Impl)+s.hostStats.capacity()*sizeof(Stats)+s.hostReceivers.capacity()*sizeof(double);
    for(const auto& b:s.fields)m.deviceAvailableBytes+=b.allocatedBytes;
    for(const auto& b:s.sourceGradient)m.deviceAvailableBytes+=b.allocatedBytes;
    for(const auto* b:{&s.metrics,&s.normals,&s.jacobian,&s.source,&s.damping,&s.materialLambda,&s.materialMu,&s.materialDensity,&s.statsBuffer,&s.receiverIndices,&s.receiverValues,&s.sliceBuffer})m.deviceAvailableBytes+=b->allocatedBytes;
    return m;
}
void CudaEngine3D::step(double time,double dt,double frequency,int step) {
    auto& s=*impl_;DeviceScope scope(s.device);checked(cudaMemcpy(s.fields[1].pointer,s.fields[0].pointer,9*s.mesh.count*sizeof(double),cudaMemcpyDeviceToDevice),"save step");
    const auto forcing=[&](double t){const double tau=3.14159265358979323846*frequency*(t-1.5/frequency);return s.mesh.sourceAmplitude/(s.mesh.density*s.mesh.h*s.mesh.h*s.mesh.h)*(1-2*tau*tau)*std::exp(-tau*tau);};
    s.rhs(s.state,s.k1,forcing(time),step,0);
    combineKernel<<<s.blocks,blockSize>>>(s.stage,s.base,s.k1,s.k2,s.k3,s.k4,s.mesh.count,0.5*dt,false);s.project(s.stage);
    s.rhs(s.stage,s.k2,forcing(time+0.5*dt),step,1);
    combineKernel<<<s.blocks,blockSize>>>(s.stage,s.base,s.k2,s.k2,s.k3,s.k4,s.mesh.count,0.5*dt,false);s.project(s.stage);
    s.rhs(s.stage,s.k3,forcing(time+0.5*dt),step,2);
    combineKernel<<<s.blocks,blockSize>>>(s.stage,s.base,s.k3,s.k2,s.k3,s.k4,s.mesh.count,dt,false);s.project(s.stage);
    s.rhs(s.stage,s.k4,forcing(time+dt),step,3);
    combineKernel<<<s.blocks,blockSize>>>(s.state,s.base,s.k1,s.k2,s.k3,s.k4,s.mesh.count,dt,true);s.project(s.state);
}
void CudaEngine3D::rollback(){auto& s=*impl_;DeviceScope scope(s.device);checked(cudaMemcpy(s.fields[0].pointer,s.fields[1].pointer,9*s.mesh.count*sizeof(double),cudaMemcpyDeviceToDevice),"restore step");}
void CudaEngine3D::resetFields(){auto& s=*impl_;DeviceScope scope(s.device);for(auto& field:s.fields)checked(cudaMemset(field.pointer,0,9*s.mesh.count*sizeof(double)),"clear fields");checked(cudaDeviceSynchronize(),"clear completion");}
void CudaEngine3D::diagnostics(Diagnostics3D& d) {
    auto& s=*impl_;DeviceScope scope(s.device);statsKernel<<<s.blocks,blockSize>>>(s.state,s.k4,s.mesh,static_cast<Stats*>(s.statsBuffer.pointer));checked(cudaGetLastError(),"diagnostics launch");
    checked(cudaMemcpy(s.hostStats.data(),s.statsBuffer.pointer,s.hostStats.size()*sizeof(Stats),cudaMemcpyDeviceToHost),"compact diagnostics download");
    double norm=0,traction=0;d.displayPeaks={};d.elasticEnergy=0;d.allFinite=true;
    for(const auto& stats:s.hostStats){for(int c=0;c<5;++c)d.displayPeaks[c]=std::max(d.displayPeaks[c],stats.peak[c]);d.elasticEnergy+=stats.energy;norm=std::max(norm,stats.rateNorm);traction=std::max(traction,stats.rateTraction);d.allFinite=d.allFinite&&!stats.bad;}
    d.maxAbsVelocity=d.displayPeaks[3];d.maxTractionRateRelative=traction/std::max(norm,1e-30);d.allFinite=d.allFinite&&std::isfinite(d.elasticEnergy);d.peakEnergy=std::max(d.peakEnergy,d.elasticEnergy);
}
void CudaEngine3D::sample(std::vector<ReceiverTrace3D>& receivers) {
    auto& s=*impl_;DeviceScope scope(s.device);receiverKernel<<<(static_cast<int>(receivers.size())+31)/32,32>>>(s.state,static_cast<std::size_t*>(s.receiverIndices.pointer),s.receiverValues.doubles(),static_cast<int>(receivers.size()));checked(cudaGetLastError(),"receiver launch");
    checked(cudaMemcpy(s.hostReceivers.data(),s.receiverValues.pointer,s.hostReceivers.size()*sizeof(double),cudaMemcpyDeviceToHost),"receiver download");
    for(std::size_t r=0;r<receivers.size();++r){receivers[r].samples.push_back(s.hostReceivers[3*r+2]);receivers[r].samplesVx.push_back(s.hostReceivers[3*r]);receivers[r].samplesVy.push_back(s.hostReceivers[3*r+1]);}
}
void CudaEngine3D::slices(const DisplayRequest3D& request,DisplayFrame3D& frame) {
    auto& s=*impl_;DeviceScope scope(s.device);const int nx=s.mesh.nx-2*ghostWidth3D,ny=s.mesh.ny-2*ghostWidth3D,nz=s.mesh.nz-2*ghostWidth3D;
    const int widths[3]{nx,ny,nx},heights[3]{nz,nz,ny};frame.bytes=0;
    for(int p=0;p<3;++p) {
        auto& out=frame.planes[p];out.width=widths[p];out.height=heights[p];out.values.resize(static_cast<std::size_t>(out.width)*out.height);
        sliceKernel<<<(out.width*out.height+blockSize-1)/blockSize,blockSize>>>(s.state,s.mesh,request,static_cast<float*>(s.sliceBuffer.pointer),p,out.width,out.height);checked(cudaGetLastError(),"slice launch");
        const auto bytes=out.values.size()*sizeof(float);checked(cudaMemcpy(out.values.data(),s.sliceBuffer.pointer,bytes,cudaMemcpyDeviceToHost),"slice download");frame.bytes+=bytes;
    }
}
void CudaEngine3D::download(std::array<std::vector<double>,9>& fields) const {
    const auto& s=*impl_;DeviceScope scope(s.device);for(int f=0;f<9;++f){fields[f].resize(s.mesh.count);checked(cudaMemcpy(fields[f].data(),s.state.f[f],s.mesh.count*sizeof(double),cudaMemcpyDeviceToHost),"explicit volume download");}
}
}
