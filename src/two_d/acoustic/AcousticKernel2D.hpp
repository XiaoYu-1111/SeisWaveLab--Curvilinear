#pragma once
#include "AcousticCpml2D.hpp"
#include <algorithm>
#include <memory>
#include <stdexcept>
#include <vector>
namespace Seis2D::Acoustic::Detail {
template <class Real> struct Operator {
    int nx{}, nz{}, threads{1};
    Real dt2{};
    std::vector<Real> h, a, b, rootMass;
    std::vector<unsigned char> active;
    std::vector<int> receivers;
    std::vector<CpmlTriangle<Real>> pmlTriangles;
    std::vector<CpmlNode<Real>> pmlNodes;
    std::vector<int> pmlRows, pmlReferences;
    std::size_t pmlMemorySize() const { return 4 * pmlTriangles.size() + 2 * pmlNodes.size(); }
    std::size_t pmlArrayBytes() const {
        return pmlTriangles.size() * sizeof(CpmlTriangle<Real>) + pmlNodes.size() * sizeof(CpmlNode<Real>) +
               (pmlRows.size() + pmlReferences.size()) * sizeof(int);
    }
    std::size_t size() const { return a.size(); }
};
template <class Real> struct State {
    std::vector<Real> previous, current, pml;
};
template <class Real> struct Engine {
    virtual ~Engine() = default;
    virtual void clear() = 0;
    virtual void step(bool adjoint, int source, Real amplitude, const std::vector<Real> &injection = {}) = 0;
    virtual State<Real> state() const = 0;
    virtual void restore(const State<Real> &) = 0;
    virtual std::vector<Real> field() const = 0;
    virtual std::vector<Real> sample() const = 0;
};
// H is symmetric mass-scaled FEM stiffness. A=diag(a)*(2I-dt^2 H).
// A^T=(2I-dt^2 H)*diag(a), not A in the damping strip.
template <class Real>
inline Real nextValue(const Operator<Real> &op, const Real *u, const Real *previous, int i, bool adjoint) {
    if (!op.active[i])
        return 0;
    const int offsets[9]{-op.nx - 1, -op.nx, -op.nx + 1, -1, 0, 1, op.nx - 1, op.nx, op.nx + 1};
    Real hu = 0;
    for (int j = 0; j < 9; ++j) {
        const auto v = op.h[std::size_t(i) * 9 + j];
        if (v != 0) {
            const int q = i + offsets[j];
            hu += v * u[q] * (adjoint ? op.a[q] : Real(1));
        }
    }
    return Real(2) * op.a[i] * u[i] - op.dt2 * hu * (adjoint ? Real(1) : op.a[i]) + op.b[i] * previous[i];
}
template <class Real> class CpuEngine final : public Engine<Real> {
    const Operator<Real> &op;
    std::vector<Real> previous, current, next, pml, flux;

  public:
    explicit CpuEngine(const Operator<Real> &o)
        : op(o), previous(o.size()), current(o.size()), next(o.size()), pml(o.pmlMemorySize()),
          flux(2 * o.pmlTriangles.size()) {}
    void clear() override {
        std::fill(previous.begin(), previous.end(), Real(0));
        std::fill(current.begin(), current.end(), Real(0));
        std::fill(pml.begin(), pml.end(), Real(0));
    }
    void step(bool adjoint, int source, Real amplitude, const std::vector<Real> &injection = {}) override {
        const auto count = static_cast<int>(op.size());
#ifdef SEIS3D_HAS_OPENMP
#pragma omp parallel for schedule(static) num_threads(op.threads) if (op.threads > 1 && count >= 16384)
#endif
        for (int i = 0; i < count; ++i)
            next[i] = nextValue(op, current.data(), previous.data(), i, adjoint);
        if (!op.pmlTriangles.empty()) {
            if (adjoint)
                throw std::runtime_error("Acoustic CPML is forward-only; RTM keeps its validated operator.");
            const int triangles = static_cast<int>(op.pmlTriangles.size());
#ifdef SEIS3D_HAS_OPENMP
#pragma omp parallel for schedule(static) num_threads(op.threads) if (op.threads > 1 && triangles >= 1024)
#endif
            for (int t = 0; t < triangles; ++t)
                cpmlFlux(op.pmlTriangles[t], current.data(), op.rootMass.data(), pml.data() + 4 * t,
                         flux.data() + 2 * t);
            const int nodes = static_cast<int>(op.pmlNodes.size());
#ifdef SEIS3D_HAS_OPENMP
#pragma omp parallel for schedule(static) num_threads(op.threads) if (op.threads > 1 && nodes >= 1024)
#endif
            for (int n = 0; n < nodes; ++n) {
                const auto &node = op.pmlNodes[n];
                const int q = node.node;
                if (!op.active[q]) {
                    next[q] = 0;
                    continue;
                }
                Real force = 0;
                const int offsets[9]{-op.nx - 1, -op.nx, -op.nx + 1, -1, 0, 1, op.nx - 1, op.nx, op.nx + 1};
                for (int k = 0; k < 9; ++k) {
                    const Real h = op.h[std::size_t(q) * 9 + k];
                    if (h != 0)
                        force -= h * current[q + offsets[k]];
                }
                Real correction = 0;
                for (int r = op.pmlRows[n]; r < op.pmlRows[n + 1]; ++r) {
                    const int ref = op.pmlReferences[r], t = ref / 3, corner = ref % 3;
                    correction -= op.pmlTriangles[t].dx[corner] * flux[2 * t] +
                                  op.pmlTriangles[t].dz[corner] * flux[2 * t + 1];
                }
                force += correction / op.rootMass[q];
                auto *memory = pml.data() + 4 * triangles + 2 * n;
                force = cpmlFilter(node.inverseZ, cpmlFilter(node.inverseX, force, memory[0]), memory[1]);
                next[q] = Real(2) * current[q] - previous[q] + op.dt2 * force;
            }
        }
        if (source >= 0)
            next[source] += amplitude;
        for (std::size_t r = 0; r < injection.size(); ++r)
            next[op.receivers[r]] += injection[r] / op.rootMass[op.receivers[r]];
        previous.swap(current);
        current.swap(next);
    }
    State<Real> state() const override { return {previous, current, pml}; }
    void restore(const State<Real> &s) override {
        if (s.previous.size() != previous.size() || s.current.size() != current.size() ||
            s.pml.size() != pml.size())
            throw std::runtime_error("Acoustic checkpoint shape mismatch.");
        previous = s.previous;
        current = s.current;
        pml = s.pml;
    }
    std::vector<Real> field() const override { return current; }
    std::vector<Real> sample() const override {
        std::vector<Real> d;
        for (int q : op.receivers)
            d.push_back(current[q] / op.rootMass[q]);
        return d;
    }
};
#ifdef SEIS3D_HAS_CUDA
std::unique_ptr<Engine<float>> cudaEngine(const Operator<float> &, int);
std::unique_ptr<Engine<double>> cudaEngine(const Operator<double> &, int);
#endif
template <class Real> std::unique_ptr<Engine<Real>> engine(const Operator<Real> &op, bool gpu, int device) {
#ifdef SEIS3D_HAS_CUDA
    if (gpu)
        return cudaEngine(op, device);
#else
    (void)device;
    if (gpu)
        throw std::runtime_error("CUDA unavailable in this build.");
#endif
    return std::make_unique<CpuEngine<Real>>(op);
}
} // namespace Seis2D::Acoustic::Detail
