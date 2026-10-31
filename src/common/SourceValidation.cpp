#include "Seis2D/CurvilinearSolver2D.hpp"
#include "Seis3D/CurvilinearSolver.hpp"
#include "Seis3D/ReceiverIo.hpp"
#include "Seis3D/VtsWriter.hpp"
#include "SeisCommon/SourceMechanism.hpp"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <stdexcept>
using namespace SeisWave;
using namespace Seis3D::Curvilinear;
namespace {
void check(bool value, const char *message) {
    if (!value)
        throw std::runtime_error(message);
}
void compare(const std::vector<double> &a, const std::vector<double> &b) {
    check(a.size() == b.size(), "Source backend field sizes differ");
    double peak = 0, error = 0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        check(std::isfinite(a[i]) && std::isfinite(b[i]), "Nonfinite source field");
        peak = std::max(peak, std::abs(a[i]));
        error = std::max(error, std::abs(a[i] - b[i]));
    }
    check(error <= 1e-9 * std::max(peak, 1e-25), "Source CPU/CUDA mismatch");
}
void moments(int dimensions) {
    const int n = 21, c = 10;
    const double h = 2.5;
    const std::size_t count = dimensions == 2 ? n * n : n * n * n;
    const std::array<std::ptrdiff_t, 3> strides{1, n, n * n};
    std::vector<double> w(count);
    double sum = 0;
    for (int k = -2; k <= 2; ++k)
        for (int j = (dimensions == 3 ? -2 : 0); j <= (dimensions == 3 ? 2 : 0); ++j)
            for (int i = -2; i <= 2; ++i)
                sum += std::exp(-.5 * (i * i + j * j + k * k));
    const auto index = [&](int i, int j, int k) {
        return dimensions == 2 ? std::size_t(i + n * k) : std::size_t(i + n * (j + n * k));
    };
    for (int k = -2; k <= 2; ++k)
        for (int j = (dimensions == 3 ? -2 : 0); j <= (dimensions == 3 ? 2 : 0); ++j)
            for (int i = -2; i <= 2; ++i)
                w[index(c + i, c + j, c + k)] = std::exp(-.5 * (i * i + j * j + k * k)) / sum;
    double net[3]{}, tensor[3][3]{};
    for (int k = 3; k < n - 3; ++k)
        for (int j = (dimensions == 3 ? 3 : c); j <= (dimensions == 3 ? n - 4 : c); ++j)
            for (int i = 3; i < n - 3; ++i) {
                const auto q = index(i, j, k);
                const auto f = isotropicMomentPattern(
                    q, dimensions, strides, h, 1,
                    [&](std::ptrdiff_t p, int axis, int component) { return axis == component ? w[p] : 0.; });
                const double x[3]{(i - c) * h, (dimensions == 2 ? k - c : j - c) * h, (k - c) * h};
                for (int a = 0; a < dimensions; ++a) {
                    net[a] += f[a];
                    for (int b = 0; b < dimensions; ++b)
                        tensor[b][a] += x[b] * f[a];
                }
            }
    for (int a = 0; a < dimensions; ++a) {
        check(std::abs(net[a]) < 1e-13, "Explosion has nonzero resultant force");
        for (int b = 0; b < dimensions; ++b)
            check(std::abs(tensor[b][a] - (a == b ? 1. : 0.)) < 1e-12,
                  "Explosion tensor is not unit isotropic moment");
    }
    std::cout << "PASS " << dimensions
              << "D explosion: zero net force, unit diagonal moments, zero off-diagonal moments\n";
}
void injection(int dimensions, const SourceMechanism &mechanism) {
    constexpr int n = 35, center = 17;
    constexpr double h = 2.5, rho = 2200, force = 6e7, moment = 9e8, frequency = 30;
    double net[3]{}, tensor[3][3]{}, dt{};
    const auto accumulate = [&](const auto &grid, const auto &fields, int d) {
        const double volume = std::pow(h, d);
        for (int k = 0; k < n; ++k)
            for (int j = 0; j < (d == 3 ? n : 1); ++j)
                for (int i = 0; i < n; ++i) {
                    const auto q = d == 3
                                       ? std::size_t(i + 3) + (n + 6) * std::size_t(j + 3 + (n + 6) * (k + 3))
                                       : std::size_t(i + 3) + (n + 6) * std::size_t(k + 3);
                    const double x[]{(i - center) * h, (d == 3 ? j - center : k - center) * h,
                                     (k - center) * h};
                    for (int a = 0; a < d; ++a) {
                        const double massVelocity = rho * volume * grid.jacobian[q] * fields[a][q];
                        net[a] += massVelocity;
                        for (int b = 0; b < d; ++b)
                            tensor[b][a] += x[b] * massVelocity;
                    }
                }
    };
    std::string error;
    if (dimensions == 2) {
        Seis2D::Curvilinear::Parameters p;
        p.nx = p.nz = n;
        p.sourceX = p.sourceZ = center;
        p.receiverCount = 16;
        p.relief = 0;
        p.absorbing = false;
        p.maxSteps = 1;
        p.duration = .1;
        p.source = mechanism;
        p.forcePerLength = force;
        p.momentPerLength = moment;
        Seis2D::Curvilinear::Simulation s;
        check(s.initialize(p, error) && s.advance(1, error), error.c_str());
        const auto snap = s.snapshot();
        // Adapt the grid name while keeping the physical integration explicit.
        struct JacobianView {
            const Seis2D::Curvilinear::ScalarArray &jacobian;
        } g{s.grid().jacobian};
        accumulate(g, snap.fields, 2);
        dt = s.dt();
    } else {
        SimulationParameters p;
        p.nx = p.ny = p.nz = n;
        p.sourceX = p.sourceY = p.sourceZ = center;
        p.spacing = h;
        p.topographyAmplitude = 0;
        p.absorbingBoundary.enabled = false;
        p.maxSteps = 1;
        p.duration = .1;
        p.dominantFrequency = frequency;
        p.source = mechanism;
        p.sourceForce = force;
        p.sourceMoment = moment;
        CpuSimulation3D s;
        check(s.initialize(p, error, {1}) && s.advance(1, error), error.c_str());
        const auto snap = s.snapshot();
        const std::array fields{snap.vx, snap.vy, snap.vz};
        accumulate(s.grid(), fields, 3);
        dt = s.dt();
    }
    const auto ricker = [](double t) {
        const double a = 3.14159265358979323846 * frequency * (t - 1.5 / frequency);
        return (1 - 2 * a * a) * std::exp(-a * a);
    };
    const double quadrature = dt * (ricker(0) + 4 * ricker(dt / 2) + ricker(dt)) / 6;
    if (mechanism.kind == SourceKind::DirectionalForce) {
        const auto unit = unitSourceDirection(mechanism);
        for (int a = 0; a < dimensions; ++a)
            check(std::abs(net[a] - force * quadrature * unit[dimensions == 2 && a == 1 ? 2 : a]) <
                      std::abs(force * quadrature) * 1e-9,
                  "Integrated directional force / amplitude / units incorrect");
    } else {
        for (int a = 0; a < dimensions; ++a) {
            check(std::abs(net[a]) < std::abs(moment * quadrature / h) * 1e-10,
                  "Injected explosion has a nonzero net force");
            for (int b = 0; b < dimensions; ++b)
                check(std::abs(tensor[b][a] - (a == b ? moment * quadrature : 0.)) <
                          std::abs(moment * quadrature) * 1e-9,
                      "Injected moment amplitude / isotropy / units incorrect");
        }
    }
    std::cout << "PASS " << dimensions << "D integrated injection: kind=" << static_cast<int>(mechanism.kind)
              << " amplitude, direction, RK quadrature\n";
}
} // namespace
int main() {
    try {
        moments(2);
        moments(3);
        const auto cuda = queryBackend3D(BackendKind3D::Cuda);
        for (int mode = 0; mode < 4; ++mode) {
            SourceMechanism mechanism;
            if (mode == 1)
                mechanism.direction = {1, 0, 0};
            if (mode == 2)
                mechanism.direction = {1, 0, -2};
            if (mode == 3)
                mechanism.kind = SourceKind::IsotropicMoment;
            injection(2, mechanism);
            injection(3, mechanism);
            std::string error;
            {
                Seis2D::Curvilinear::Parameters p;
                p.nx = 40;
                p.nz = 48;
                p.sourceZ = 20;
                p.receiverCount = 18;
                p.frequency = 30;
                p.duration = .1;
                p.maxSteps = 0;
                p.relief = 3;
                p.source = mechanism;
                p.forcePerLength = 7e4;
                p.momentPerLength = 9e5;
                Seis2D::Curvilinear::Simulation cpu, gpu;
                check(cpu.initialize(p, error, {BackendKind3D::Cpu}, {1}) &&
                          cpu.advance(cpu.totalSteps(), error),
                      error.c_str());
                check(cpu.diagnostics().finite && cpu.diagnostics().energy > 0,
                      "2D source did not propagate");
                if (cuda.available) {
                    check(gpu.initialize(p, error, BackendKind3D::Cuda, {1, cuda.cudaDevice}) &&
                              gpu.advance(gpu.totalSteps(), error),
                          error.c_str());
                    const auto a = cpu.snapshot(), b = gpu.snapshot();
                    for (int f = 0; f < 5; ++f)
                        compare(a.fields[f], b.fields[f]);
                    for (std::size_t r = 0; r < cpu.receivers().size(); ++r) {
                        compare(cpu.receivers()[r].vx, gpu.receivers()[r].vx);
                        compare(cpu.receivers()[r].vz, gpu.receivers()[r].vz);
                    }
                }
                if (mode == 3) {
                    const auto folder = std::filesystem::path(SEIS3D_OUTPUT_ROOT) / "validation/sources";
                    std::filesystem::create_directories(folder);
                    check(Seis2D::Curvilinear::writeSnapshot(cpu, folder / "explosion-2d.vts", error) &&
                              Seis2D::Curvilinear::writeReceivers(cpu, folder / "explosion-2d.csv", error),
                          error.c_str());
                }
                auto bad = p;
                bad.source.kind = SourceKind::DirectionalForce;
                bad.source.direction = {0, 0, 0};
                const auto steps = cpu.step();
                check(!cpu.initialize(bad, error) && cpu.step() == steps,
                      "2D invalid direction discarded session");
                if (mode == 3) {
                    bad = p;
                    bad.sourceZ = 3;
                    check(!cpu.initialize(bad, error) && cpu.step() == steps,
                          "2D clipped moment support accepted");
                }
            }
            {
                SimulationParameters p;
                p.nx = 22;
                p.ny = 20;
                p.nz = 24;
                p.sourceZ = 12;
                p.spacing = 2.5;
                p.topographyAmplitude = 3;
                p.topographyWavelength = 100;
                p.dominantFrequency = 30;
                p.duration = .1;
                p.maxSteps = 0;
                p.source = mechanism;
                p.sourceForce = 6e7;
                p.sourceMoment = 9e8;
                if (mode == 2)
                    p.source.direction = {1, -2, 3};
                if (mode == 3) {
                    p.layeredMaterial = true;
                    p.layers = {{"upper", 20, 2600, 1500, 2200}, {"base", 0, 3000, 1700, 2400}};
                }
                CpuSimulation3D cpu;
                check(cpu.initialize(p, error, {1}) && cpu.advance(cpu.totalSteps(), error), error.c_str());
                check(cpu.diagnostics().allFinite && cpu.diagnostics().elasticEnergy > 0,
                      "3D source did not propagate");
                if (cuda.available) {
                    auto gpu = createSimulationBackend3D(BackendKind3D::Cuda, error);
                    check(gpu && gpu->initialize(p, error, {1, cuda.cudaDevice}) &&
                              gpu->advance(gpu->totalSteps(), error),
                          error.c_str());
                    const auto a = cpu.snapshot(), b = gpu->snapshot();
                    const std::vector<double> *aa[]{&a.vx,  &a.vy,  &a.vz,  &a.sxx, &a.syy,
                                                    &a.szz, &a.sxy, &a.sxz, &a.syz};
                    const std::vector<double> *bb[]{&b.vx,  &b.vy,  &b.vz,  &b.sxx, &b.syy,
                                                    &b.szz, &b.sxy, &b.sxz, &b.syz};
                    for (int f = 0; f < 9; ++f)
                        compare(*aa[f], *bb[f]);
                    for (std::size_t r = 0; r < cpu.receivers().size(); ++r) {
                        compare(cpu.receivers()[r].samples, gpu->receivers()[r].samples);
                        compare(cpu.receivers()[r].samplesVx, gpu->receivers()[r].samplesVx);
                        compare(cpu.receivers()[r].samplesVy, gpu->receivers()[r].samplesVy);
                    }
                }
                if (mode == 3) {
                    const auto folder = std::filesystem::path(SEIS3D_OUTPUT_ROOT) / "validation/sources";
                    std::filesystem::create_directories(folder);
                    check(writeVts(cpu.snapshot(), folder / "curvilinear_snapshot.vts", error) &&
                              writeReceiversCsv(cpu, folder / "receiver_traces.csv", error, true),
                          error.c_str());
                }
                auto bad = p;
                bad.source.kind = static_cast<SourceKind>(9);
                const auto steps = cpu.currentStep();
                check(!cpu.initialize(bad, error) && cpu.currentStep() == steps,
                      "3D invalid source discarded session");
                if (mode == 3) {
                    bad = p;
                    bad.sourceZ = 3;
                    check(!cpu.initialize(bad, error) && cpu.currentStep() == steps,
                          "3D clipped moment support accepted");
                }
            }
            std::cout << "PASS source mode " << mode
                      << ": 2D/3D curved propagation, CPU/CUDA=" << cuda.available << '\n';
        }
    } catch (const std::exception &e) {
        std::cerr << "FAIL: " << e.what() << '\n';
        return 1;
    }
}
