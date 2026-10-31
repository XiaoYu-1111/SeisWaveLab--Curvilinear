#include "AcousticKernel2D.hpp"
#include <cuda_runtime.h>
#include <string>
namespace Seis2D::Acoustic::Detail {
namespace {
void check(cudaError_t e) {
    if (e != cudaSuccess)
        throw std::runtime_error(std::string("Acoustic CUDA: ") + cudaGetErrorString(e));
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
    void put(const std::vector<T> &v) {
        if (v.size() != count)
            throw std::runtime_error("Acoustic CUDA buffer shape mismatch.");
        if (count)
            check(cudaMemcpy(p, v.data(), count * sizeof(T), cudaMemcpyHostToDevice));
    }
    std::vector<T> get() const {
        std::vector<T> v(count);
        if (count)
            check(cudaMemcpy(v.data(), p, count * sizeof(T), cudaMemcpyDeviceToHost));
        return v;
    }
    void zero() {
        if (count)
            check(cudaMemset(p, 0, count * sizeof(T)));
    }
};
template <class T>
__global__ void propagate(int count, int nx, T dt2, const T *h, const T *a, const T *b,
                          const unsigned char *active, const T *cur, const T *prev, T *next, bool adjoint) {
    int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= count)
        return;
    if (!active[i]) {
        next[i] = 0;
        return;
    }
    const int offsets[9]{-nx - 1, -nx, -nx + 1, -1, 0, 1, nx - 1, nx, nx + 1};
    T sum = 0;
    for (int k = 0; k < 9; ++k) {
        T v = h[std::size_t(i) * 9 + k];
        if (v != 0) {
            int q = i + offsets[k];
            sum += v * cur[q] * (adjoint ? a[q] : T(1));
        }
    }
    next[i] = T(2) * a[i] * cur[i] - dt2 * sum * (adjoint ? T(1) : a[i]) + b[i] * prev[i];
}
template <class T>
__global__ void cpmlFluxKernel(int count, const CpmlTriangle<T> *triangles, const T *current, const T *root,
                               T *history, T *flux) {
    const int t = blockIdx.x * blockDim.x + threadIdx.x;
    if (t < count)
        cpmlFlux(triangles[t], current, root, history + 4 * t, flux + 2 * t);
}
template <class T>
__global__ void cpmlNodeKernel(int count, int triangles, int nx, T dt2, const CpmlNode<T> *nodes,
                               const CpmlTriangle<T> *elements, const int *rows, const int *references,
                               const unsigned char *active, const T *h, const T *root, const T *current,
                               const T *previous, const T *flux, T *history, T *next) {
    const int n = blockIdx.x * blockDim.x + threadIdx.x;
    if (n >= count)
        return;
    const auto node = nodes[n];
    const int q = node.node;
    if (!active[q]) {
        next[q] = 0;
        return;
    }
    T force = 0;
    const int offsets[9]{-nx - 1, -nx, -nx + 1, -1, 0, 1, nx - 1, nx, nx + 1};
    for (int k = 0; k < 9; ++k) {
        const T value = h[std::size_t(q) * 9 + k];
        if (value != 0)
            force -= value * current[q + offsets[k]];
    }
    T correction = 0;
    for (int r = rows[n]; r < rows[n + 1]; ++r) {
        const int ref = references[r], t = ref / 3, corner = ref % 3;
        correction -= elements[t].dx[corner] * flux[2 * t] + elements[t].dz[corner] * flux[2 * t + 1];
    }
    force += correction / root[q];
    auto *memory = history + 4 * triangles + 2 * n;
    force = cpmlFilter(node.inverseZ, cpmlFilter(node.inverseX, force, memory[0]), memory[1]);
    next[q] = T(2) * current[q] - previous[q] + dt2 * force;
}
template <class T>
__global__ void inject(T *next, int source, T amplitude, int nr, const int *ids, const T *root,
                       const T *data) {
    int r = blockIdx.x * blockDim.x + threadIdx.x;
    if (r == 0 && source >= 0)
        next[source] += amplitude;
    // Sources and receivers are never injected together in this implementation.
    if (r < nr)
        next[ids[r]] += data[r] / root[ids[r]];
}
template <class T>
__global__ void samplePressure(int n, const int *ids, const T *root, const T *cur, T *data) {
    int r = blockIdx.x * blockDim.x + threadIdx.x;
    if (r < n)
        data[r] = cur[ids[r]] / root[ids[r]];
}
template <class Real> class GpuEngine final : public Engine<Real> {
    const Operator<Real> &op;
    int device;
    Buffer<Real> h, a, b, root, previous, current, next, samples, injection;
    Buffer<unsigned char> active;
    Buffer<int> ids, pmlRows, pmlReferences;
    Buffer<CpmlTriangle<Real>> pmlTriangles;
    Buffer<CpmlNode<Real>> pmlNodes;
    Buffer<Real> pml, flux;

  public:
    explicit GpuEngine(const Operator<Real> &o, int d)
        : op(o), device(d), h(o.h.size()), a(o.size()), b(o.size()), root(o.size()), previous(o.size()),
          current(o.size()), next(o.size()), samples(o.receivers.size()), injection(o.receivers.size()),
          active(o.size()), ids(o.receivers.size()), pmlRows(o.pmlRows.size()),
          pmlReferences(o.pmlReferences.size()), pmlTriangles(o.pmlTriangles.size()),
          pmlNodes(o.pmlNodes.size()), pml(o.pmlMemorySize()), flux(2 * o.pmlTriangles.size()) {
        h.put(o.h);
        a.put(o.a);
        b.put(o.b);
        root.put(o.rootMass);
        active.put(o.active);
        ids.put(o.receivers);
        pmlRows.put(o.pmlRows);
        pmlReferences.put(o.pmlReferences);
        pmlTriangles.put(o.pmlTriangles);
        pmlNodes.put(o.pmlNodes);
        clear();
    }
    ~GpuEngine() { cudaSetDevice(device); }
    void clear() override {
        check(cudaSetDevice(device));
        previous.zero();
        current.zero();
        pml.zero();
    }
    void step(bool adjoint, int source, Real amplitude, const std::vector<Real> &data = {}) override {
        check(cudaSetDevice(device));
        int count = static_cast<int>(op.size());
        propagate<<<(count + 255) / 256, 256>>>(count, op.nx, op.dt2, h.p, a.p, b.p, active.p, current.p,
                                                previous.p, next.p, adjoint);
        check(cudaGetLastError());
        if (pmlTriangles.count) {
            if (adjoint)
                throw std::runtime_error("Acoustic CPML is forward-only; RTM keeps its validated operator.");
            const int triangles = static_cast<int>(pmlTriangles.count),
                      nodes = static_cast<int>(pmlNodes.count);
            cpmlFluxKernel<<<(triangles + 255) / 256, 256>>>(triangles, pmlTriangles.p, current.p, root.p,
                                                             pml.p, flux.p);
            check(cudaGetLastError());
            cpmlNodeKernel<<<(nodes + 255) / 256, 256>>>(
                nodes, triangles, op.nx, op.dt2, pmlNodes.p, pmlTriangles.p, pmlRows.p, pmlReferences.p,
                active.p, h.p, root.p, current.p, previous.p, flux.p, pml.p, next.p);
            check(cudaGetLastError());
        }
        if (source >= 0 || !data.empty()) {
            int nr = static_cast<int>(data.size());
            if (nr) {
                if (nr != static_cast<int>(op.receivers.size()))
                    throw std::runtime_error("RTM injection shape mismatch.");
                injection.put(data);
            }
            inject<<<(std::max(1, nr) + 255) / 256, 256>>>(next.p, source, amplitude, nr, ids.p, root.p,
                                                           injection.p);
            check(cudaGetLastError());
        }
        std::swap(previous.p, current.p);
        std::swap(current.p, next.p);
    }
    State<Real> state() const override {
        check(cudaSetDevice(device));
        return {previous.get(), current.get(), pml.get()};
    }
    void restore(const State<Real> &s) override {
        check(cudaSetDevice(device));
        previous.put(s.previous);
        current.put(s.current);
        pml.put(s.pml);
    }
    std::vector<Real> field() const override {
        check(cudaSetDevice(device));
        return current.get();
    }
    std::vector<Real> sample() const override {
        check(cudaSetDevice(device));
        int n = static_cast<int>(op.receivers.size());
        samplePressure<<<(n + 255) / 256, 256>>>(n, ids.p, root.p, current.p, samples.p);
        check(cudaGetLastError());
        return samples.get();
    }
};
template <class Real> std::unique_ptr<Engine<Real>> create(const Operator<Real> &op, int device) {
    check(cudaSetDevice(device));
    return std::make_unique<GpuEngine<Real>>(op, device);
}
} // namespace
std::unique_ptr<Engine<float>> cudaEngine(const Operator<float> &op, int device) {
    return create(op, device);
}
std::unique_ptr<Engine<double>> cudaEngine(const Operator<double> &op, int device) {
    return create(op, device);
}
} // namespace Seis2D::Acoustic::Detail
