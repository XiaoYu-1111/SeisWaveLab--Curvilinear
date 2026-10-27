#include "../common/CudaSupport.cuh"
#include "CudaEngine2D.hpp"
#include <algorithm>
namespace Seis2D::Curvilinear {
namespace {
using SeisWave::Cuda::checked;
using SeisWave::Cuda::DeviceBuffer;
using SeisWave::Cuda::DeviceScope;
constexpr int blockSize = 128;
__device__ bool physical(const Point::Mesh &g, std::size_t q) {
    const int i = static_cast<int>(q % g.nx), k = static_cast<int>(q / g.nx);
    return i >= 3 && i < g.nx - 3 && k >= 3 && k < g.nz - 3;
}
__global__ void rhsKernel(Point::Fields in, Point::Fields out, Point::Mesh g, double force, int step,
                          int stage) {
    const std::size_t q = static_cast<std::size_t>(blockIdx.x) * blockDim.x + threadIdx.x;
    if (q < g.count && physical(g, q))
        Point::rhs(in, out, g, q, force, step, stage);
}
__global__ void projectKernel(Point::Fields fields, Point::Mesh g) {
    const int i = blockIdx.x * blockDim.x + threadIdx.x + 3;
    if (i < g.nx - 3)
        Point::project(fields, g, i);
}
__global__ void combineKernel(Point::Fields out, Point::Fields base, Point::Fields rate, Point::Fields a,
                              Point::Fields b, Point::Fields c, Point::Fields d, std::size_t count, double dt,
                              bool final,int fieldCount) {
    const std::size_t q = static_cast<std::size_t>(blockIdx.x) * blockDim.x + threadIdx.x;
    if (q < count)
        for (int f = 0; f < fieldCount; ++f)
            out.f[f][q] =
                base.f[f][q] + (final ? dt * (a.f[f][q] + 2 * b.f[f][q] + 2 * c.f[f][q] + d.f[f][q]) / 6
                                      : dt * rate.f[f][q]);
}
struct Stats {
    double energy{}, velocity{}, traction{};
    int bad{};
};
__global__ void statsKernel(Point::Fields w, Point::Mesh g, Stats *output) {
    __shared__ Stats shared[blockSize];
    Stats s{};
    const std::size_t q = static_cast<std::size_t>(blockIdx.x) * blockDim.x + threadIdx.x;
    if (q < g.count && physical(g, q)) {
        for (int f = 0; f < g.fieldCount; ++f)
            if (!isfinite(w.f[f][q]))
                s.bad = 1;
        s.energy = Point::energy(w, g, q);
        s.velocity = Point::value(w, g, q, 2);
        if (!isfinite(s.energy) || !isfinite(s.velocity))
            s.bad = 1;
        if (q / g.nx == 3) {
            const double nx = g.normals[2 * q], nz = g.normals[2 * q + 1],
                         tx = w.f[2][q] * nx + w.f[4][q] * nz, tz = w.f[4][q] * nx + w.f[3][q] * nz,
                         norm =
                             sqrt(w.f[2][q] * w.f[2][q] + w.f[3][q] * w.f[3][q] + 2 * w.f[4][q] * w.f[4][q]);
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
__global__ void sampleKernel(Point::Fields w, const std::size_t *indices, double *out, int count) {
    const int r = blockIdx.x * blockDim.x + threadIdx.x;
    if (r < count) {
        out[2 * r] = w.f[0][indices[r]];
        out[2 * r + 1] = w.f[1][indices[r]];
    }
}
__global__ void frameKernel(Point::Fields w, Point::Mesh g, int component, float *out) {
    const int q = blockIdx.x * blockDim.x + threadIdx.x, nx = g.nx - 6;
    if (q < nx * (g.nz - 6))
        out[q] = static_cast<float>(Point::value(w, g, Point::index(g, q % nx + 3, q / nx + 3), component));
}
} // namespace
struct CudaEngine::Impl {
    int device{}, blocks{};
    Point::Mesh mesh;
    DeviceBuffer sourceGradient[2];
    DeviceBuffer deltaP[3],deltaS[3];
    DeviceBuffer fields[7], a, b, jac, normals, lambda, mu, rho, source, damping, stats, receiverIndices,
        receiverSamples, display;
    std::vector<Stats> hostStats;
    std::vector<double> samples;
    Point::Fields state[7];
    void project(Point::Fields w) {
        projectKernel<<<(mesh.nx - 6 + 127) / 128, 128>>>(w, mesh);
        checked(cudaGetLastError(), "2D surface launch");
    }
    void rhs(Point::Fields in, Point::Fields out, double force, int step, int stage) {
        rhsKernel<<<blocks, blockSize>>>(in, out, mesh, force, step, stage);
        checked(cudaGetLastError(), "2D RHS launch");
    }
    void combine(Point::Fields out, Point::Fields rate, double dt, bool final) {
        combineKernel<<<blocks, blockSize>>>(out, state[1], rate, state[2], state[3], state[4], state[5],
                                             mesh.count, dt, final,mesh.fieldCount);
        checked(cudaGetLastError(), "2D stage launch");
        project(out);
    }
};
CudaEngine::CudaEngine(const Point::Mesh &mesh, const std::vector<Receiver> &receivers, int device)
    : impl_(std::make_unique<Impl>()) {
    auto &s = *impl_;
    s.device = device;
    DeviceScope scope(device);
    cudaFuncAttributes attributes{};
    checked(cudaFuncGetAttributes(&attributes, rhsKernel), "2D kernel compatibility");
    s.mesh = mesh;
    s.blocks = static_cast<int>((mesh.count + 127) / 128);
    for (int state = 0; state < 7; ++state) {
        s.fields[state].allocate(mesh.fieldCount * mesh.count * sizeof(double));
        for (int f = 0; f < mesh.fieldCount; ++f)
            s.state[state].f[f] = s.fields[state].doubles() + f * mesh.count;
    }
    s.a.upload(mesh.a, mesh.count * sizeof(double));
    s.mesh.a = s.a.doubles();
    s.b.upload(mesh.b, mesh.count * sizeof(double));
    s.mesh.b = s.b.doubles();
    s.jac.upload(mesh.jac, mesh.count * sizeof(double));
    s.mesh.jac = s.jac.doubles();
    s.normals.upload(mesh.normals, 2 * mesh.count * sizeof(double));
    s.mesh.normals = s.normals.doubles();
    s.lambda.upload(mesh.lambda, mesh.count * sizeof(double));
    s.mesh.lambda = s.lambda.doubles();
    s.mu.upload(mesh.mu, mesh.count * sizeof(double));
    s.mesh.mu = s.mu.doubles();
    s.rho.upload(mesh.rho, mesh.count * sizeof(double));
    s.mesh.rho = s.rho.doubles();
    s.source.upload(mesh.source, mesh.count * sizeof(double));
    s.mesh.source = s.source.doubles();
    for (int a = 0; a < 2; ++a)
        if (mesh.sourceGradient[a]) {
            s.sourceGradient[a].upload(mesh.sourceGradient[a], mesh.count * sizeof(double));
            s.mesh.sourceGradient[a] = s.sourceGradient[a].doubles();
        }
    s.damping.upload(mesh.damping, mesh.count * sizeof(double));
    s.mesh.damping = s.damping.doubles();
    for(int l=0;l<mesh.mechanisms;++l){s.deltaP[l].upload(mesh.deltaP[l],mesh.count*sizeof(double));s.deltaS[l].upload(mesh.deltaS[l],mesh.count*sizeof(double));s.mesh.deltaP[l]=s.deltaP[l].doubles();s.mesh.deltaS[l]=s.deltaS[l].doubles();}
    s.stats.allocate(s.blocks * sizeof(Stats));
    s.hostStats.resize(s.blocks);
    std::vector<std::size_t> indices;
    for (const auto &r : receivers)
        indices.push_back(Point::index(mesh, r.x + 3, r.z + 3));
    s.receiverIndices.upload(indices.data(), indices.size() * sizeof(std::size_t));
    s.receiverSamples.allocate(indices.size() * 2 * sizeof(double));
    s.samples.resize(indices.size() * 2);
    s.display.allocate(static_cast<std::size_t>(mesh.nx - 6) * (mesh.nz - 6) * sizeof(float));
}
CudaEngine::~CudaEngine() = default;
void CudaEngine::step(double time, double dt, double frequency, double scale, int step) {
    auto &s = *impl_;
    DeviceScope scope(s.device);
    checked(cudaMemcpy(s.fields[1].pointer, s.fields[0].pointer, s.mesh.fieldCount * s.mesh.count * sizeof(double),
                       cudaMemcpyDeviceToDevice),
            "2D save step");
    const auto force = [&](double t) {
        const double a = 3.14159265358979323846 * frequency * (t - 1.5 / frequency);
        return scale * (1 - 2 * a * a) * std::exp(-a * a);
    };
    s.rhs(s.state[0], s.state[2], force(time), step, 0);
    s.combine(s.state[6], s.state[2], dt / 2, false);
    s.rhs(s.state[6], s.state[3], force(time + dt / 2), step, 1);
    s.combine(s.state[6], s.state[3], dt / 2, false);
    s.rhs(s.state[6], s.state[4], force(time + dt / 2), step, 2);
    s.combine(s.state[6], s.state[4], dt, false);
    s.rhs(s.state[6], s.state[5], force(time + dt), step, 3);
    s.combine(s.state[0], s.state[2], dt, true);
}
void CudaEngine::rollback() {
    auto &s = *impl_;
    DeviceScope scope(s.device);
    checked(cudaMemcpy(s.fields[0].pointer, s.fields[1].pointer, s.mesh.fieldCount * s.mesh.count * sizeof(double),
                       cudaMemcpyDeviceToDevice),
            "2D rollback");
}
void CudaEngine::reset() {
    auto &s = *impl_;
    DeviceScope scope(s.device);
    for (auto &b : s.fields)
        checked(cudaMemset(b.pointer, 0, s.mesh.fieldCount * s.mesh.count * sizeof(double)), "2D reset");
    checked(cudaDeviceSynchronize(), "2D clear completion");
}
void CudaEngine::diagnostics(Diagnostics &d) {
    auto &s = *impl_;
    DeviceScope scope(s.device);
    statsKernel<<<s.blocks, blockSize>>>(s.state[0], s.mesh, static_cast<Stats *>(s.stats.pointer));
    checked(cudaGetLastError(), "2D diagnostics");
    checked(cudaMemcpy(s.hostStats.data(), s.stats.pointer, s.hostStats.size() * sizeof(Stats),
                       cudaMemcpyDeviceToHost),
            "2D diagnostic readback");
    d.energy = d.maxVelocity = d.tractionRelative = 0;
    d.finite = true;
    for (const auto &row : s.hostStats) {
        d.energy += row.energy;
        d.maxVelocity = std::max(d.maxVelocity, row.velocity);
        d.tractionRelative = std::max(d.tractionRelative, row.traction);
        d.finite &= !row.bad;
    }
    d.finite &= std::isfinite(d.energy);
}
void CudaEngine::sample(std::vector<Receiver> &receivers) {
    auto &s = *impl_;
    DeviceScope scope(s.device);
    sampleKernel<<<(static_cast<int>(receivers.size()) + 127) / 128, 128>>>(
        s.state[0], static_cast<std::size_t *>(s.receiverIndices.pointer), s.receiverSamples.doubles(),
        static_cast<int>(receivers.size()));
    checked(cudaGetLastError(), "2D receiver launch");
    checked(cudaMemcpy(s.samples.data(), s.receiverSamples.pointer, s.samples.size() * sizeof(double),
                       cudaMemcpyDeviceToHost),
            "2D receiver readback");
    for (std::size_t r = 0; r < receivers.size(); ++r) {
        receivers[r].vx.push_back(s.samples[2 * r]);
        receivers[r].vz.push_back(s.samples[2 * r + 1]);
    }
}
void CudaEngine::frame(int component, std::vector<float> &values) const {
    auto &s = *impl_;
    DeviceScope scope(s.device);
    const int count = (s.mesh.nx - 6) * (s.mesh.nz - 6);
    values.resize(count);
    frameKernel<<<(count + 127) / 128, 128>>>(s.state[0], s.mesh, component,
                                              static_cast<float *>(s.display.pointer));
    checked(cudaGetLastError(), "2D display launch");
    checked(cudaMemcpy(values.data(), s.display.pointer, count * sizeof(float), cudaMemcpyDeviceToHost),
            "2D compact display readback");
}
Snapshot CudaEngine::snapshot(bool includeMemories) const {
    auto &s = *impl_;
    DeviceScope scope(s.device);
    Snapshot result;
    if(s.mesh.mechanisms){result.syy.resize(s.mesh.count);checked(cudaMemcpy(result.syy.data(),s.state[0].f[5],s.mesh.count*sizeof(double),cudaMemcpyDeviceToHost),"2D Syy download");if(includeMemories)result.memory.resize(4*s.mesh.mechanisms);if(includeMemories)for(int f=6;f<s.mesh.fieldCount;++f){auto& out=result.memory[f-6];out.resize(s.mesh.count);checked(cudaMemcpy(out.data(),s.state[0].f[f],s.mesh.count*sizeof(double),cudaMemcpyDeviceToHost),"2D GSLS memory download");}}
    for (int f = 0; f < 5; ++f) {
        result.fields[f].resize(s.mesh.count);
        checked(cudaMemcpy(result.fields[f].data(), s.state[0].f[f], s.mesh.count * sizeof(double),
                           cudaMemcpyDeviceToHost),
                "2D snapshot download");
    }
    return result;
}
} // namespace Seis2D::Curvilinear
