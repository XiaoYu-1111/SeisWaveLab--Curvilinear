#include "AcousticDrpModel2D.hpp"
#include <chrono>
#include <fstream>
#include <iomanip>
#include <iostream>
#ifdef _WIN32
#include <windows.h>

#include <psapi.h>
#endif
using namespace Seis2D::Acoustic;
using namespace Seis2D::Curvilinear;
namespace {
void check(bool v, const std::string &s) {
    if (!v)
        throw std::runtime_error(s);
}
auto folder() {
    auto p = std::filesystem::path(SEIS3D_OUTPUT_ROOT) / "validation/acoustic-drp-2026-10-31";
    std::filesystem::create_directories(p);
    return p;
}
double norm(const std::vector<double> &a) {
    double s = 0;
    for (auto v : a)
        s += v * v;
    return std::sqrt(s);
}
double difference(const std::vector<double> &a, const std::vector<double> &b) {
    check(a.size() == b.size(), "Shape mismatch");
    double n = 0, d = 0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        n += std::pow(a[i] - b[i], 2);
        d += b[i] * b[i];
    }
    return std::sqrt(n / std::max(d, 1e-100));
}
Options base() {
    auto o = defaultOptions();
    o.propagator = Propagator::CurvilinearDrp;
    o.model.layered = false;
    o.model.vp = 1200;
    o.model.density = 1800;
    o.model.nx = 161;
    o.model.nz = 121;
    o.model.spacing = 5;
    o.model.sourceX = 80;
    o.model.sourceZ = 40;
    o.model.receiverStart = 80;
    o.model.receiverCount = 1;
    o.model.receiverDepth = 40;
    o.model.frequency = 15;
    o.model.duration = 1.5;
    o.model.sampleIntervalMs = 1;
    o.model.precision = Precision::Float64;
    o.compute.cpuThreads = 4;
    return o;
}
Detail::Mesh geometry(const Options &o, bool full = false) {
    Detail::ModelSettings c;
    c.model = o.model;
    c.compute = o.compute;
    c.backend = o.backend;
    c.memoryMiB = 2048;
    c.absorberCells = o.absorberCells;
    c.geometryOnly = !full;
    return Detail::mesh(c);
}
double energy(const Drp::Operator<double> &op, const std::vector<double> &p, const std::vector<double> &x,
              const std::vector<double> &z, bool target, int pad) {
    double e = 0;
    for (int j = 0; j < op.nz; ++j)
        for (int i = 0; i < op.nx; ++i) {
            if (target && (i < pad || i >= op.nx - pad || j >= op.nz - pad))
                continue;
            const auto q = std::size_t(j) * op.nx + i;
            const auto g = op.geometry[q];
            const double w = (i == 0 || i == op.nx - 1 ? .5 : 1) * (j == 0 || j == op.nz - 1 ? .5 : 1);
            e += w / g.inverseJ * (p[q] * p[q] / g.bulk + (x[q] * x[q] + z[q] * z[q]) / g.inverseRho);
        }
    return e;
}
void energyIdentity(std::ostream &log) {
    auto o = base();
    o.drpDissipation = 0;
    o.damping = false;
    o.model.relief = 20;
    o.model.wavelength = 400;
    o.model.frequency = 10;
    o.model.layered = true;
    o.model.layers = {{150, 800, 0, 1500}, {0, 1600, 0, 2100}};
    const auto g = geometry(o);
    const auto op = Drp::makeOperator<double>(o, g);
    const int n = int(op.size());
    std::vector<double> s(7 * n), flux(2 * n), rhs(7 * n);
    for (int j = 0; j < op.nz; ++j)
        for (int i = 0; i < op.nx; ++i) {
            int q = j * op.nx + i;
            s[q] = (i && j && i < op.nx - 1 && j < op.nz - 1) ? std::sin(.37 * i + .21 * j) : 0;
            s[n + q] = std::cos(.17 * i - .22 * j) * 1e-6;
            s[2 * n + q] = std::sin(.19 * i + .32 * j) * 1e-6;
        }
    for (int q = 0; q < n; ++q)
        Drp::fluxNode(q, op.nx, op.nz, op.geometry.data(), s.data(), flux.data());
    double work = 0, absolute = 0;
    for (int q = 0; q < n; ++q) {
        Drp::rhsNode(q, op.nx, op.nz, op.geometry.data(), op.dx.data(), op.dz.data(), op.tx.data(),
                     op.tz.data(), op.fx.data(), op.fz.data(), op.px.data(), op.pz.data(), s.data(),
                     flux.data(), rhs.data(), -1, 0.);
        const int i = q % op.nx, j = q / op.nx;
        const auto m = op.geometry[q];
        const double w =
            (i == 0 || i == op.nx - 1 ? .5 : 1) * (j == 0 || j == op.nz - 1 ? .5 : 1) / m.inverseJ;
        for (double v : {w * s[q] * rhs[q] / m.bulk, w * s[n + q] * rhs[n + q] / m.inverseRho,
                         w * s[2 * n + q] * rhs[2 * n + q] / m.inverseRho}) {
            work += v;
            absolute += std::abs(v);
        }
    }
    log << "curved_variable_density_energy_residual=" << std::abs(work) / absolute << '\n';
    check(std::abs(work) / absolute < 1e-12, "Curved variable-density discrete energy identity failed");
    auto filtered = o;
    filtered.drpDissipation = .2;
    const auto fop = Drp::makeOperator<double>(filtered, g);
    double filterWork = 0;
    for (int q = 0; q < n; ++q) {
        const int i = q % op.nx, j = q / op.nx;
        const auto m = fop.geometry[q];
        filterWork -= s[q] * m.filterRate / (m.bulk * m.inverseJ) *
                      (Drp::derivative(fop.fx[i], s.data(), j * op.nx, 1) +
                       Drp::derivative(fop.fz[j], s.data(), i, op.nx));
    }
    check(filterWork < 0, "Selective pressure dissipation injected energy");
    log << "selective_filter_energy_work=" << filterWork << '\n';
    std::fill(s.begin(), s.end(), 0);
    std::fill(flux.begin(), flux.end(), 0);
    const int source = op.nx + o.absorberCells + 10;
    double integral = 0;
    for (int q = 0; q < n; ++q) {
        Drp::rhsNode(q, op.nx, op.nz, op.geometry.data(), op.dx.data(), op.dz.data(), op.tx.data(),
                     op.tz.data(), op.fx.data(), op.fz.data(), op.px.data(), op.pz.data(), s.data(),
                     flux.data(), rhs.data(), source,
                     op.geometry[source].inverseJ / (o.model.spacing * o.model.spacing));
        integral += rhs[q] / op.geometry[q].inverseJ * o.model.spacing * o.model.spacing;
    }
    check(std::abs(integral - 1) < 1e-12, "Clipped DRP source lost physical area normalization");
    log << "clipped_binomial_source_integral=" << integral << '\n';
}
double modal(int intervals, bool p1, bool gpu, std::ostream &csv) {
    auto o = base();
    const double h = 800. / intervals;
    o.absorberCells = 8;
    o.damping = false;
    o.model.nx = intervals + 1 - 16;
    o.model.nz = 3 * intervals / 4 + 1 - 8;
    o.model.spacing = h;
    o.model.frequency = 2;
    o.model.receiverStart = 1;
    o.model.receiverDepth = 1;
    o.model.sourceX = 2;
    o.model.sourceZ = 2;
    auto g = geometry(o, p1);
    const int nx = g.nx, nz = g.nz, n = nx * nz;
    std::vector<double> shape(n);
    const double ax = 3 * Detail::pi / 800, az = 2 * Detail::pi / 600;
    const double omega = 1200 * std::hypot(ax, az), time = .09;
    for (int j = 1; j < nz - 1; ++j)
        for (int i = 1; i < nx - 1; ++i)
            shape[j * nx + i] = std::sin(ax * i * h) * std::sin(az * j * h);
    std::vector<double> result;
    double dt{};
    int steps{};
    const auto start = std::chrono::steady_clock::now();
    if (!p1) {
        auto op = Drp::makeOperator<double>(o, g);
        steps = int(std::ceil(time / op.dt));
        op.dt = time / steps;
        dt = op.dt;
        auto eng = Drp::engine(op, gpu, 0);
        std::vector<double> s(7 * n);
        std::copy(shape.begin(), shape.end(), s.begin());
        eng->setState(s);
        for (int k = 0; k < steps; ++k)
            eng->step(-1, {});
        result = eng->field();
    } else {
        auto op = Detail::makeOperator<double>(g, false, 4);
        std::fill(op.a.begin(), op.a.end(), 1.);
        std::fill(op.b.begin(), op.b.end(), -1.);
        steps = int(std::ceil(time / g.dt));
        dt = time / steps;
        op.dt2 = dt * dt;
        auto eng = Detail::engine(op, gpu, 0);
        auto s = eng->state();
        for (int q = 0; q < n; ++q) {
            s.current[q] = shape[q] * op.rootMass[q];
            s.previous[q] = s.current[q] * std::cos(omega * dt);
        }
        eng->restore(s);
        for (int k = 0; k < steps; ++k)
            eng->step(false, -1, 0.);
        result = eng->field();
        for (int q = 0; q < n; ++q)
            result[q] /= op.rootMass[q];
    }
    const double elapsed =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    double a = 0, b = 0, projection = 0;
    for (int q = 0; q < n; ++q) {
        a += std::pow(result[q] - shape[q] * std::cos(omega * time), 2);
        b += shape[q] * shape[q];
        projection += result[q] * shape[q];
    }
    const double error = std::sqrt(a / b);
    csv << (p1 ? "P1" : "DRP7") << ',' << (gpu ? "CUDA" : "CPU") << ',' << nx << ',' << nz << ',' << h << ','
        << dt << ',' << steps << ',' << error << ',' << projection / b << ',' << elapsed << '\n';
    return error;
}
void reflection(std::ostream &log, bool gpu) {
    auto o = base();
    o.model.nx = 321;
    o.model.nz = 161;
    o.model.layered = true;
    o.model.layers = {{350, 1200, 0, 1800}, {0, 1800, 0, 2200}};
    const auto g = geometry(o);
    auto op = Drp::makeOperator<double>(o, g);
    const int n = int(op.size());
    const double end = .25;
    const int steps = int(std::ceil(end / op.dt));
    op.dt = end / steps;
    std::vector<double> s(7 * n);
    for (int j = 1; j < op.nz - 1; ++j)
        for (int i = 1; i < op.nx - 1; ++i) {
            const int q = j * op.nx + i;
            const double p = std::exp(-.5 * std::pow((j * 5. - 200) / 35, 2));
            s[q] = p;
            s[2 * n + q] = p / (1800 * 1200);
        }
    auto eng = Drp::engine(op, gpu, 0);
    eng->setState(s);
    std::ofstream trace(folder() / "normal_reflection.csv");
    trace << "t_s,p_Pa\n";
    const int receiver = 40 * op.nx + o.absorberCells + 160;
    for (int k = 0; k < steps; ++k) {
        eng->step(-1, {});
        if (k % 4 == 0)
            trace << (k + 1) * op.dt << ',' << eng->field()[receiver] << '\n';
    }
    const double measured = eng->field()[receiver],
                 expected = (2200. * 1800 - 1800. * 1200) / (2200. * 1800 + 1800. * 1200);
    log << "normal_reflection_expected=" << expected << " measured=" << measured << '\n';
    check(std::abs(measured - expected) < .025, "Variable-density normal reflection amplitude failed");
}
double absorption(bool damping, double relief, bool gpu, std::ostream &log) {
    auto o = base();
    o.damping = damping;
    o.model.relief = relief;
    o.model.wavelength = 500;
    const auto g = geometry(o);
    auto op = Drp::makeOperator<double>(o, g);
    const int n = int(op.size());
    std::vector<double> s(7 * n);
    for (int j = 1; j < op.nz - 1; ++j)
        for (int i = 1; i < op.nx - 1; ++i) {
            int q = j * op.nx + i;
            s[q] = std::exp(-.5 * (std::pow((g.x[q] - 400) / 35, 2) + std::pow((g.z[q] - 250) / 35, 2)));
        }
    auto eng = Drp::engine(op, gpu, 0);
    eng->setState(s);
    std::vector<double> zero(n);
    const double initial = energy(op, {s.begin(), s.begin() + n}, zero, zero, true, o.absorberCells);
    const int steps = int(std::ceil(1.5 / op.dt));
    for (int k = 0; k < steps; ++k)
        eng->step(-1, {});
    const auto pressure = eng->field();
    double top = 0;
    for (int i = 0; i < op.nx; ++i)
        top = std::max(top, std::abs(pressure[i]));
    const double ratio = energy(op, pressure, eng->field(1), eng->field(2), true, o.absorberCells) / initial;
    log << "target_residual_energy damping=" << damping << " relief=" << relief << " t=" << steps * op.dt
        << " ratio=" << ratio << " top_p=" << top << '\n';
    check(std::isfinite(ratio) && top == 0, "CPML nonfinite state / zero-pressure boundary failed");
    return ratio;
}
void integration(std::ostream &log, bool gpu) {
    auto o = base();
    o.model.nx = 81;
    o.model.nz = 61;
    o.model.receiverStart = 40;
    o.model.sourceX = 40;
    o.model.sourceZ = 20;
    o.model.receiverDepth = 20;
    o.model.duration = .18;
    o.model.relief = 8;
    o.model.wavelength = 250;
    o.model.layered = true;
    o.model.layers = {{80, 1000, 0, 1500}, {0, 1800, 0, 2200}};
    o.backend = gpu ? BackendKind3D::Cuda : BackendKind3D::Cpu;
    std::string e;
    Seis2D::Acoustic::Simulation a;
    check(a.initialize(o, e), e);
    check(a.advance(a.totalSteps(), e), e);
    const auto ref = a.pressureSnapshot();
    check(norm(ref) > 0, "No DRP pressure");
    auto file = folder() / "drp-model.vts";
    check(writeModelVts(a, file, e), e);
    Options loaded;
    check(readModelVts(file, loaded, e), e);
    check(loaded == a.options(), "DRP schema4 roundtrip changed model/algorithm");
    const auto before = a.step();
    auto invalid = o;
    invalid.model.sourceZ = 0;
    check(!a.initialize(invalid, e) && a.step() == before, "Failed DRP rebuild mutated active session");
    invalid = o;
    invalid.model.precision = Precision::Float32;
    invalid.model.layered = false;
    invalid.model.vp = 1e25;
    invalid.model.density = 1e25;
    check(!a.initialize(invalid, e) && a.step() == before &&
              e.find("floating-point precision") != std::string::npos,
          "Unrepresentable FP32 DRP material was accepted or changed active session");
    auto cpu = o;
    cpu.backend = BackendKind3D::Cpu;
    Seis2D::Acoustic::Simulation b;
    check(b.initialize(cpu, e), e);
    check(b.advance(b.totalSteps(), e), e);
    const double parity = difference(b.pressureSnapshot(), ref);
    check(parity < 1e-10, "DRP CPU/CUDA FP64 mismatch");
    auto single = o;
    single.model.precision = Precision::Float32;
    Seis2D::Acoustic::Simulation c;
    check(c.initialize(single, e), e);
    check(c.advance(c.totalSteps(), e), e);
    const double precision = difference(c.pressureSnapshot(), ref);
    check(precision < .002, "DRP FP32/FP64 mismatch");
    check(c.reset(e) && c.step() == 0 && norm(c.pressureSnapshot()) == 0, "DRP reset left field/PML history");
    log << "integration schema4 rollback reset CPU_GPU_L2=" << parity << " FP32_FP64_L2=" << precision
        << '\n';
}
void experiment(int frequency, int method, double h, double duration = 3) {
    check((frequency == 15 || frequency == 30) && (method == 0 || method == 1) && std::isfinite(h) &&
              h >= .25 && h <= 2 && std::isfinite(duration) && duration > 0 && duration <= 3,
          "Pilot requires 15/30 Hz, method 0/1, h=0.25..2 m, duration=0..3 s.");
    auto o = base();
    o.propagator = method ? Propagator::CurvilinearDrp : Propagator::P1;
    o.absorberCells = int(std::lround((frequency == 15 ? 48. : 24.) / h));
    o.backend = BackendKind3D::Cuda;
    o.model.precision = Precision::Float32;
    o.model.spacing = h;
    auto &p = o.model;
    p.nx = int(std::lround(600 / h)) + 1;
    p.nz = int(std::lround(300 / h)) + 1;
    p.sourceX = int(300 / h);
    p.sourceZ = int(40 / h);
    p.receiverStart = int(100 / h);
    p.receiverDepth = int(4 / h);
    p.receiverStride = int(20 / h);
    p.receiverCount = 21;
    p.frequency = frequency;
    p.duration = duration;
    p.sampleIntervalMs = .5;
    p.relief = 12;
    p.wavelength = 300;
    p.flattenDepth = 120;
    p.layered = true;
    p.layers = {{24, 500, 0, 1400}, {56, 1000, 0, 1700}, {0, 2000, 0, 2100}};
    const auto name = std::to_string(frequency) + "Hz-" + (method ? "DRP" : "P1") + "-h" + std::to_string(h) +
                      (duration == 3 ? "" : "-t" + std::to_string(duration));
    const auto dir = folder() / name;
    std::filesystem::create_directories(dir);
    Seis2D::Acoustic::Simulation s;
    std::string e;
    check(s.initialize(o, e), e);
    check(s.advance(s.totalSteps(), e), e);
    auto snapshot = s.pressureSnapshot();
    check(norm(snapshot) > 0, "Empty long-time pressure");
    auto result = dir / "result";
    if (std::filesystem::exists(result))
        result = dir / ("result-repeat-" +
                        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    check(exportResult(s, result, e), e);
    std::ofstream(dir / "result-path.txt") << result.filename().string() << '\n';
    std::ofstream info(dir / "benchmark.csv");
    info << std::setprecision(17);
    std::size_t peak = 0;
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS pmc{};
    if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc)))
        peak = pmc.PeakWorkingSetSize;
#endif
    info << "frequency,method,h,nx,nz,dt,steps,init_ms,compute_ms,host_array_bytes,device_array_bytes,"
            "process_peak_working_set_bytes\n";
    info << frequency << ',' << (method ? "DRP7" : "P1") << ',' << h << ',' << p.nx << ',' << p.nz << ','
         << s.dt() << ',' << s.totalSteps() << ',' << s.diagnostics().initializationMs << ','
         << s.diagnostics().totalStepMs << ',' << s.diagnostics().residentHostArrayBytes << ','
         << s.diagnostics().residentDeviceBytes << ',' << peak << '\n';
    std::cout << "PASS long-time " << name << " steps=" << s.totalSteps()
              << " ms=" << s.diagnostics().totalStepMs << '\n';
}
} // namespace
int main(int argc, char **argv) {
    try {
        if ((argc == 5 || argc == 6) && std::string(argv[1]) == "--experiment") {
            experiment(std::stoi(argv[2]), std::stoi(argv[3]), std::stod(argv[4]),
                       argc == 6 ? std::stod(argv[5]) : 3.);
            return 0;
        }
        const bool gpu = Seis3D::Curvilinear::queryBackend3D(BackendKind3D::Cuda).available;
        std::ofstream log(folder() / "validation.txt");
        log << std::setprecision(14);
        energyIdentity(log);
        std::ofstream csv(folder() / "analytic_modes.csv");
        csv << std::setprecision(14)
            << "method,backend,nx,nz,h,dt,steps,L2_initial_normalized,mode_projection,total_ms\n";
        const double coarse = modal(40, false, false, csv), fine = modal(80, false, false, csv),
                     finer = modal(160, false, gpu, csv);
        log << "DRP_modal_errors=" << coarse << ',' << fine << ',' << finer << " ratios=" << coarse / fine
            << ',' << fine / finer << '\n';
        check(fine < coarse / 6 && finer < fine / 6,
              "DRP analytic refinement did not converge at high order");
        modal(40, true, false, csv);
        modal(80, true, false, csv);
        modal(160, true, gpu, csv);
        reflection(log, gpu);
        const double rigid = absorption(false, 0, gpu, log), flat = absorption(true, 0, gpu, log),
                     curved = absorption(true, 20, gpu, log);
        check(rigid > .2 && flat < .02 && curved < .03,
              "CPML residual energy exceeds initial acceptance threshold");
        integration(log, gpu);
        log << "PASS: DRP independent forward; energy, analytic refinement, impedance, free surface, CPML, "
               "precision, CUDA, schema4; RTM unchanged\n";
        std::cout << "PASS: acoustic DRP validation; GPU=" << gpu << '\n';
        return 0;
    } catch (const std::exception &e) {
        std::cerr << "FAIL: " << e.what() << '\n';
        return 1;
    }
}
