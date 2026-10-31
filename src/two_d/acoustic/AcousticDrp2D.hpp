#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <stdexcept>
#include <vector>
#ifdef __CUDACC__
#define SEIS_DRP_HD __host__ __device__ inline
#else
#define SEIS_DRP_HD inline
#endif
// Separate first-order acoustic solver. No elastic/GSLS state and no RTM adjoint.
namespace Seis2D::Acoustic::Drp {
template <class T> struct Row {
    int count{}, index[12]{};
    T coefficient[12]{};
};
// Tam-Shen seven-point DRP. Correct the last decimal's first-moment rounding.
inline std::array<double, 3> coefficients() {
    std::array<double, 3> c{.770882380518, -.166705904415, .020843142770};
    const double moment = 2 * (c[0] + 2 * c[1] + 3 * c[2]);
    for (auto &v : c)
        v /= moment;
    return c;
}
template <class T> void add(Row<T> &r, int index, T c) {
    for (int j = 0; j < r.count; ++j)
        if (r.index[j] == index) {
            r.coefficient[j] += c;
            return;
        }
    if (r.count >= 12)
        throw std::runtime_error("DRP boundary row overflow");
    r.index[r.count] = index;
    r.coefficient[r.count++] = c;
}
template <class T> void rows(int n, double h, std::vector<Row<T>> &gradient, std::vector<Row<T>> &transpose) {
    gradient.resize(n);
    transpose.resize(n);
    const auto c = coefficients();
    // Odd pressure extension about the outer zero-pressure nodes. The divergence
    // is the EXACT transpose, not a guessed reflected-velocity stencil.
    for (int i = 0; i < n; ++i)
        for (int k = -3; k <= 3; ++k) {
            int j = i + k, parity = 1;
            if (j < 0) {
                j = -j;
                parity = -1;
            }
            if (j >= n) {
                j = 2 * (n - 1) - j;
                parity = -1;
            }
            if (j > 0 && j < n - 1)
                add(gradient[i], j, T(parity * (k == 0 ? 0 : (k < 0 ? -1 : 1) * c[std::abs(k) - 1]) / h));
        }
    for (int i = 0; i < n; ++i)
        for (int k = 0; k < gradient[i].count; ++k)
            add(transpose[gradient[i].index[k]], i, gradient[i].coefficient[k]);
}
template <class T> std::vector<Row<T>> filterRows(int n) {
    const double b[7]{-1, 6, -15, 20, -15, 6, -1};
    std::vector<Row<T>> result(n);
    for (int i = 1; i < n - 1; ++i)
        for (int k = -3; k <= 3; ++k) {
            int j = i + k, parity = 1;
            if (j < 0) {
                j = -j;
                parity = -1;
            }
            if (j >= n) {
                j = 2 * (n - 1) - j;
                parity = -1;
            }
            if (j > 0 && j < n - 1)
                add(result[i], j, T(parity * b[k + 3]));
        }
    return result;
}
template <class T> struct AxisPml {
    T inverse{1}, a{}, b{}, sponge{};
};
template <class T> struct Geometry {
    T inverseJ{}, slope{}, inverseRho{}, bulk{}, filterRate{};
};
template <class T> struct Operator {
    int nx{}, nz{}, threads{1};
    T dt{};
    std::vector<Row<T>> dx, dz, tx, tz, fx, fz;
    std::vector<AxisPml<T>> px, pz;
    std::vector<Geometry<T>> geometry;
    std::vector<int> receivers;
    std::size_t size() const { return geometry.size(); }
    std::size_t coefficientBytes() const {
        return geometry.size() * sizeof(Geometry<T>) +
               (dx.size() + dz.size() + tx.size() + tz.size() + fx.size() + fz.size()) * sizeof(Row<T>) +
               (px.size() + pz.size()) * sizeof(AxisPml<T>) + receivers.size() * sizeof(int);
    }
};
template <class T> SEIS_DRP_HD T derivative(const Row<T> &r, const T *v, int origin, int stride) {
    T value = 0;
    for (int j = 0; j < r.count; ++j)
        value += r.coefficient[j] * v[origin + r.index[j] * stride];
    return value;
}
template <class T>
SEIS_DRP_HD void fluxNode(int q, int nx, int nz, const Geometry<T> *g, const T *u, T *flux) {
    const int n = nx * nz, i = q % nx, j = q / nx;
    const T weight = T((i == 0 || i == nx - 1 ? .5 : 1) * (j == 0 || j == nz - 1 ? .5 : 1));
    flux[q] = weight * u[n + q] / g[q].inverseJ;
    flux[n + q] = weight * (u[2 * n + q] - g[q].slope * u[n + q]);
}
// Layout: p,vx,vz,psi(p_xi),psi(p_eta),psi(div_xi),psi(div_eta).
// The four ADE histories are integrated at every RK stage, including alpha/kappa.
template <class T>
SEIS_DRP_HD void rhsNode(int q, int nx, int nz, const Geometry<T> *geo, const Row<T> *dx, const Row<T> *dz,
                         const Row<T> *tx, const Row<T> *tz, const Row<T> *fx, const Row<T> *fz,
                         const AxisPml<T> *px, const AxisPml<T> *pz, const T *u, const T *flux, T *rhs,
                         int source, T sourceRate) {
    const int n = nx * nz, i = q % nx, j = q / nx;
    const auto g = geo[q];
    const auto x = px[i];
    const auto z = pz[j];
    const T gp = derivative(dx[i], u, j * nx, 1), hp = derivative(dz[j], u, i, nx);
    const T dv = derivative(tx[i], flux, j * nx, 1), dw = derivative(tz[j], flux + n, i, nx);
    const T a = x.inverse * gp + u[3 * n + q], b = z.inverse * hp + u[4 * n + q];
    const T damp = x.sponge + z.sponge;
    rhs[q] = (i == 0 || i == nx - 1 || j == 0 || j == nz - 1)
                 ? T(0)
                 : g.bulk * g.inverseJ * (x.inverse * dv + u[5 * n + q] + z.inverse * dw + u[6 * n + q]) -
                       damp * u[q] -
                       g.filterRate * (derivative(fx[i], u, j * nx, 1) + derivative(fz[j], u, i, nx));
    // Binomial 5x5 source suppresses the collocated Nyquist branch. Normalize the
    // clipped footprint and EACH physical nodal area; the continuum source is unchanged.
    if (source >= 0 && i > 0 && i < nx - 1 && j > 0 && j < nz - 1) {
        const int sx = source % nx, sz = source / nx, offsetX = i - sx, offsetZ = j - sz;
        if (offsetX >= -2 && offsetX <= 2 && offsetZ >= -2 && offsetZ <= 2) {
            const int weights[5]{1, 4, 6, 4, 1};
            int wx = 0, wz = 0;
            for (int k = -2; k <= 2; ++k) {
                if (sx + k > 0 && sx + k < nx - 1)
                    wx += weights[k + 2];
                if (sz + k > 0 && sz + k < nz - 1)
                    wz += weights[k + 2];
            }
            rhs[q] += sourceRate * T(weights[offsetX + 2] * weights[offsetZ + 2]) / T(wx * wz) * g.inverseJ /
                      geo[source].inverseJ;
        }
    }
    rhs[n + q] = -g.inverseRho * (a - g.slope * g.inverseJ * b) - damp * u[n + q];
    rhs[2 * n + q] = -g.inverseRho * g.inverseJ * b - damp * u[2 * n + q];
    rhs[3 * n + q] = x.a * gp - x.b * u[3 * n + q];
    rhs[4 * n + q] = z.a * hp - z.b * u[4 * n + q];
    rhs[5 * n + q] = x.a * dv - x.b * u[5 * n + q];
    rhs[6 * n + q] = z.a * dw - z.b * u[6 * n + q];
}
template <class T> struct Engine {
    virtual ~Engine() = default;
    virtual void clear() = 0;
    virtual void step(int source, const std::array<T, 4> &sourceRates) = 0;
    virtual std::vector<T> field(int component = 0) const = 0;
    virtual std::vector<T> sample() const = 0;
    virtual void setState(const std::vector<T> &) = 0; // independent analytic benchmarks only
};
template <class T> class CpuEngine final : public Engine<T> {
    const Operator<T> &op;
    std::vector<T> u, stage, rhs, sum, flux;

  public:
    explicit CpuEngine(const Operator<T> &o)
        : op(o), u(7 * o.size()), stage(u.size()), rhs(u.size()), sum(u.size()), flux(2 * o.size()) {}
    void clear() override { std::fill(u.begin(), u.end(), T(0)); }
    void setState(const std::vector<T> &s) override {
        if (s.size() != u.size())
            throw std::runtime_error("Invalid acoustic DRP state");
        u = s;
    }
    void step(int source, const std::array<T, 4> &rates) override {
        const int n = static_cast<int>(op.size()), threads = op.threads;
        const int nx = op.nx, nz = op.nz;
        for (int k = 0; k < 4; ++k) {
            const auto *s = k ? stage.data() : u.data();
#ifdef SEIS3D_HAS_OPENMP
#pragma omp parallel for num_threads(threads) schedule(static)
#endif
            for (int q = 0; q < n; ++q)
                fluxNode(q, nx, nz, op.geometry.data(), s, flux.data());
#ifdef SEIS3D_HAS_OPENMP
#pragma omp parallel for num_threads(threads) schedule(static)
#endif
            for (int q = 0; q < n; ++q)
                rhsNode(q, nx, nz, op.geometry.data(), op.dx.data(), op.dz.data(), op.tx.data(), op.tz.data(),
                        op.fx.data(), op.fz.data(), op.px.data(), op.pz.data(), s, flux.data(), rhs.data(),
                        source, rates[k]);
#ifdef SEIS3D_HAS_OPENMP
#pragma omp parallel for num_threads(threads) schedule(static)
#endif
            for (int q = 0; q < 7 * n; ++q) {
                if (k == 3)
                    u[q] += op.dt / T(6) * (sum[q] + rhs[q]);
                else {
                    sum[q] = (k == 0 ? T(0) : sum[q]) + T(k == 0 ? 1 : 2) * rhs[q];
                    stage[q] = u[q] + op.dt * T(k == 2 ? 1 : .5) * rhs[q];
                }
            }
        }
    }
    std::vector<T> field(int c = 0) const override {
        if (c < 0 || c > 2)
            throw std::runtime_error("Invalid DRP field component");
        return {u.begin() + c * op.size(), u.begin() + (c + 1) * op.size()};
    }
    std::vector<T> sample() const override {
        std::vector<T> result;
        result.reserve(op.receivers.size());
        for (int q : op.receivers)
            result.push_back(u[q]);
        return result;
    }
};
std::unique_ptr<Engine<float>> cudaEngine(const Operator<float> &, int);
std::unique_ptr<Engine<double>> cudaEngine(const Operator<double> &, int);
template <class T> std::unique_ptr<Engine<T>> engine(const Operator<T> &o, bool gpu, int device) {
    if (!gpu)
        return std::make_unique<CpuEngine<T>>(o);
#ifdef SEIS3D_HAS_CUDA
    return cudaEngine(o, device);
#else
    (void)device;
    throw std::runtime_error("CUDA is unavailable in this build");
#endif
}
} // namespace Seis2D::Acoustic::Drp
#undef SEIS_DRP_HD
