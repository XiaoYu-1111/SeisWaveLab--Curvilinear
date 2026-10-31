#include "AcousticDrp2D.hpp"
#include <cuda_runtime.h>
#include <string>
namespace Seis2D::Acoustic::Drp {
namespace {
void check(cudaError_t e) {
    if (e != cudaSuccess)
        throw std::runtime_error(std::string("DRP CUDA: ") + cudaGetErrorString(e));
}
template <class T> struct Buffer {
    T *p{};
    std::size_t count{};
    explicit Buffer(std::size_t n) : count(n) {
        if (n)
            check(cudaMalloc(reinterpret_cast<void **>(&p), n * sizeof(T)));
    }
    ~Buffer() {
        if (p)
            cudaFree(p);
    }
    Buffer(const Buffer &) = delete;
    Buffer &operator=(const Buffer &) = delete;
    void put(const std::vector<T> &v) {
        if (v.size() != count)
            throw std::runtime_error("DRP buffer shape mismatch");
        if (count)
            check(cudaMemcpy(p, v.data(), count * sizeof(T), cudaMemcpyHostToDevice));
    }
    std::vector<T> get(std::size_t offset = 0, std::size_t n = 0) const {
        if (!n)
            n = count;
        if (offset + n > count)
            throw std::runtime_error("DRP buffer range");
        std::vector<T> v(n);
        if (n)
            check(cudaMemcpy(v.data(), p + offset, n * sizeof(T), cudaMemcpyDeviceToHost));
        return v;
    }
    void zero() {
        if (count)
            check(cudaMemset(p, 0, count * sizeof(T)));
    }
};
template <class T> __global__ void fluxKernel(int nx, int nz, const Geometry<T> *geo, const T *u, T *flux) {
    const int q = blockIdx.x * blockDim.x + threadIdx.x;
    if (q < nx * nz)
        fluxNode(q, nx, nz, geo, u, flux);
}
template <class T>
__global__ void rhsKernel(int nx, int nz, const Geometry<T> *geo, const Row<T> *dx, const Row<T> *dz,
                          const Row<T> *tx, const Row<T> *tz, const Row<T> *fx, const Row<T> *fz,
                          const AxisPml<T> *px, const AxisPml<T> *pz, const T *u, const T *flux, T *rhs,
                          int source, T rate) {
    const int q = blockIdx.x * blockDim.x + threadIdx.x;
    if (q < nx * nz)
        rhsNode(q, nx, nz, geo, dx, dz, tx, tz, fx, fz, px, pz, u, flux, rhs, source, rate);
}
template <class T> __global__ void stageKernel(int n, int stage, T dt, T *u, T *temp, const T *rhs, T *sum) {
    const int q = blockIdx.x * blockDim.x + threadIdx.x;
    if (q >= n)
        return;
    if (stage == 3)
        u[q] += dt / T(6) * (sum[q] + rhs[q]);
    else {
        sum[q] = (stage == 0 ? T(0) : sum[q]) + T(stage == 0 ? 1 : 2) * rhs[q];
        temp[q] = u[q] + dt * T(stage == 2 ? 1 : .5) * rhs[q];
    }
}
template <class T> __global__ void sampleKernel(int n, const int *ids, const T *u, T *out) {
    const int r = blockIdx.x * blockDim.x + threadIdx.x;
    if (r < n)
        out[r] = u[ids[r]];
}
template <class T> class GpuEngine final : public Engine<T> {
    const Operator<T> &op;
    int device;
    Buffer<Geometry<T>> geo;
    Buffer<Row<T>> dx, dz, tx, tz, fx, fz;
    Buffer<AxisPml<T>> px, pz;
    Buffer<int> ids;
    Buffer<T> u, stage, rhs, sum, flux, samples;

  public:
    GpuEngine(const Operator<T> &o, int d)
        : op(o), device(d), geo(o.size()), dx(o.nx), dz(o.nz), tx(o.nx), tz(o.nz), fx(o.nx), fz(o.nz),
          px(o.nx), pz(o.nz), ids(o.receivers.size()), u(7 * o.size()), stage(u.count), rhs(u.count),
          sum(u.count), flux(2 * o.size()), samples(o.receivers.size()) {
        geo.put(o.geometry);
        dx.put(o.dx);
        dz.put(o.dz);
        tx.put(o.tx);
        tz.put(o.tz);
        fx.put(o.fx);
        fz.put(o.fz);
        px.put(o.px);
        pz.put(o.pz);
        ids.put(o.receivers);
        clear();
    }
    ~GpuEngine() override { cudaSetDevice(device); }
    void clear() override {
        check(cudaSetDevice(device));
        u.zero();
    }
    void setState(const std::vector<T> &s) override {
        check(cudaSetDevice(device));
        u.put(s);
    }
    void step(int source, const std::array<T, 4> &rates) override {
        check(cudaSetDevice(device));
        const int n = int(op.size());
        for (int k = 0; k < 4; ++k) {
            const auto *s = k ? stage.p : u.p;
            fluxKernel<<<(n + 255) / 256, 256>>>(op.nx, op.nz, geo.p, s, flux.p);
            rhsKernel<<<(n + 255) / 256, 256>>>(op.nx, op.nz, geo.p, dx.p, dz.p, tx.p, tz.p, fx.p, fz.p, px.p,
                                                pz.p, s, flux.p, rhs.p, source, rates[k]);
            stageKernel<<<(7 * n + 255) / 256, 256>>>(7 * n, k, op.dt, u.p, stage.p, rhs.p, sum.p);
        }
        check(cudaGetLastError());
    }
    std::vector<T> field(int c = 0) const override {
        if (c < 0 || c > 2)
            throw std::runtime_error("Invalid DRP field");
        check(cudaSetDevice(device));
        return u.get(c * op.size(), op.size());
    }
    std::vector<T> sample() const override {
        check(cudaSetDevice(device));
        const int n = int(op.receivers.size());
        sampleKernel<<<(n + 255) / 256, 256>>>(n, ids.p, u.p, samples.p);
        check(cudaGetLastError());
        return samples.get();
    }
};
template <class T> std::unique_ptr<Engine<T>> create(const Operator<T> &op, int d) {
    check(cudaSetDevice(d));
    return std::make_unique<GpuEngine<T>>(op, d);
}
} // namespace
std::unique_ptr<Engine<float>> cudaEngine(const Operator<float> &o, int d) { return create(o, d); }
std::unique_ptr<Engine<double>> cudaEngine(const Operator<double> &o, int d) { return create(o, d); }
} // namespace Seis2D::Acoustic::Drp
