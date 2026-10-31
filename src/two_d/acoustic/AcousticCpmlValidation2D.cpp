#include "AcousticModel2D.hpp"
#include "Seis2D/AcousticSolver2D.hpp"
#include "SeisCommon/VtsModelIo.hpp"
#include <fstream>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <sstream>
using namespace Seis2D::Acoustic;
void check(bool ok, const std::string &message) {
    if (!ok)
        throw std::runtime_error(message);
}
double relative(const std::vector<double> &a, const std::vector<double> &b) {
    check(a.size() == b.size(), "Pressure shape mismatch");
    double n = 0, d = 0;
    for (std::size_t q = 0; q < a.size(); ++q) {
        n += (a[q] - b[q]) * (a[q] - b[q]);
        d += a[q] * a[q];
    }
    return std::sqrt(n / std::max(d, 1e-100));
}
void legacyFixture(const std::filesystem::path &modern, const std::filesystem::path &legacy,
                   const Seis2D::Acoustic::Simulation &model, int version = 1) {
    using namespace SeisWave::VtsModel;
    std::ifstream input(modern);
    const std::string original((std::istreambuf_iterator<char>(input)), {});
    const auto begin = original.find("<FieldData>") + 11, end = original.find("</FieldData>");
    std::istringstream fields(original.substr(begin, end - begin));
    Arrays arrays;
    while (fields.peek() != std::char_traits<char>::eof()) {
        fields >> std::ws;
        if (fields.peek() == std::char_traits<char>::eof())
            break;
        const auto header = tag(fields);
        const auto name = attribute(header, "Name");
        arrays[name] =
            readArray(fields, header, nullptr, std::stoull(attribute(header, "NumberOfTuples")), true);
    }
    if (version == 1)
        arrays.erase("AcousticBoundary");
    arrays["Runtime"][6] = 1536;
    arrays.erase("Checksum");
    arrays["SeisWaveAcousticModelVersion"] = {double(version)};
    Checksum hash;
    for (const auto &[name, values] : arrays) {
        hash.name(name);
        for (double value : values)
            hash.number(value);
    }
    const auto &grid = model.grid();
    for (auto [name, values] : {std::pair{"Vp", &grid.vp}, {"Density", &grid.density}}) {
        hash.name(name);
        for (std::size_t q = 0; q < values->size(); ++q)
            hash.number((*values)[q]);
    }
    hash.name("Coordinates");
    for (std::size_t q = 0; q < grid.x.size(); ++q) {
        hash.number(grid.x[q]);
        hash.number(0);
        hash.number(grid.z[q]);
    }
    arrays["Checksum"] = {double(hash.value >> 32), double(hash.value & 0xffffffffULL)};
    std::ofstream output(legacy);
    output << std::setprecision(17) << original.substr(0, begin);
    for (const auto &[name, values] : arrays) {
        output << "<DataArray type=\"Float64\" Name=\"" << name
               << "\" NumberOfComponents=\"1\" NumberOfTuples=\"" << values.size()
               << "\" format=\"ascii\">\n";
        for (double value : values)
            output << value << ' ';
        output << "\n</DataArray>\n";
    }
    output << original.substr(end);
}
struct Run {
    std::vector<std::vector<double>> traces;
    double dt{}, peak{};
};
Run solve(const Options &o) {
    Seis2D::Acoustic::Simulation s;
    std::string error;
    check(s.initialize(o, error) && s.advance(s.totalSteps(), error), error);
    Run result;
    result.dt = s.recordDt();
    for (const auto &r : s.receivers())
        result.traces.push_back(r.pressure);
    for (double v : s.pressureSnapshot()) {
        check(std::isfinite(v), "Nonfinite CPML field");
        result.peak = std::max(result.peak, std::abs(v));
    }
    return result;
}
Options fixture(int which, int margin = 0) {
    auto o = defaultOptions();
    auto &p = o.model;
    p.nx = 161 + 2 * margin;
    p.nz = 121 + margin;
    p.spacing = 5;
    p.frequency = which == 1 ? 15 : 20;
    p.duration = 1.0;
    p.maxSteps = 0;
    p.layered = false;
    p.vp = 1800;
    p.density = 1800;
    p.relief = 0;
    p.flattenDepth = 250;
    p.sourceX = 80 + margin;
    p.sourceZ = which == 1 ? 4 : 60;
    p.receiverDepth = which == 1 ? 8 : 60;
    p.receiverStart = 10 + margin;
    p.receiverCount = 3;
    p.receiverStride = 70;
    p.sampleIntervalMs = 1;
    p.precision = Precision::Float64;
    o.compute.cpuThreads = 4;
    if (which == 2) {
        p.sourceX = 12 + margin;
        p.sourceZ = 90;
        p.receiverDepth = 90;
    }
    if (which == 3) {
        p.importedSurface = true;
        p.surfaceDatum = 1000;
        p.sourceZ = 20;
        p.receiverDepth = 10;
        if (margin)
            p.surfaceSamples.push_back({0, 980});
        for (int j = 0; j <= 8; ++j)
            p.surfaceSamples.push_back(
                {margin * 5. + j * 100., 1000 - 20 * std::cos(2 * Seis2D::Acoustic::Detail::pi * j / 8)});
        if (margin)
            p.surfaceSamples.push_back({(p.nx - 1) * 5., 980});
    }
    if (which == 4) {
        p.layered = true;
        p.layers = {{100, 1200, 0, 1700}, {0, 2400, 0, 2100}};
        p.sourceZ = 8;
        p.receiverDepth = 8;
    }
    if (which == 5) {
        p.frequency = 30;
        p.sourceZ = 110;
        p.receiverDepth = 110;
        p.receiverStart = 10 + margin;
        p.receiverStride = 10;
    }
    o.backend = Seis3D::Curvilinear::queryBackend3D(BackendKind3D::Cuda).available ? BackendKind3D::Cuda
                                                                                   : BackendKind3D::Cpu;
    return o;
}
double errorEnergy(const Run &reference, const Run &actual) {
    check(reference.traces.size() == actual.traces.size(), "Reference receiver count mismatch");
    check(std::abs(reference.dt - actual.dt) <= 1e-12 * reference.dt,
          "Reference record sampling interval mismatch");
    double difference = 0, signal = 0;
    for (std::size_t r = 0; r < actual.traces.size(); ++r) {
        check(reference.traces[r].size() == actual.traces[r].size(), "Reference sample count mismatch");
        for (std::size_t n = 0; n < actual.traces[r].size(); ++n) {
            const double truth = reference.traces[r][n], delta = actual.traces[r][n] - truth;
            difference += delta * delta;
            signal += truth * truth;
        }
    }
    return std::sqrt(difference / std::max(signal, 1e-100));
}
std::string number(double value) {
    std::ostringstream text;
    text << std::scientific << std::setprecision(10) << value;
    return text.str();
}
void checkFullPressure(const Seis2D::Acoustic::Simulation &simulation) {
    const auto target = simulation.pressureSnapshot();
    const auto full = simulation.pressureSnapshot(true);
    const auto &g = simulation.grid();
    const int pad = simulation.options().absorberCells;
    check(full.size() == std::size_t(g.computeNx) * g.computeNz, "Full-domain pressure shape mismatch");
    double outside = 0, peak = 0;
    for (int z = 0; z < g.computeNz; ++z)
        for (int x = 0; x < g.computeNx; ++x) {
            const auto q = std::size_t(z) * g.computeNx + x;
            check(std::isfinite(full[q]), "Nonfinite absorber pressure");
            peak = std::max(peak, std::abs(full[q]));
            if (x >= pad && x < pad + g.nx && z < g.nz)
                check(full[q] == target[std::size_t(z) * g.nx + x - pad],
                      "Full-domain pressure changed target values or orientation");
            else
                outside = std::max(outside, std::abs(full[q]));
            if (x == 0 || x == g.computeNx - 1 || z == 0 || z == g.computeNz - 1)
                check(full[q] == 0, "Full pressure violated outer Dirichlet boundary");
        }
    check(outside > 1e-12 * peak && peak > 0, "Absorber pressure was cropped or padded with zeros");
    std::vector<float> display;
    std::string error;
    check(simulation.readPressure(display, error, true), error);
    check(display.size() == full.size(), "Full-domain display shape mismatch");
    for (std::size_t q = 0; q < full.size(); ++q)
        check(display[q] == static_cast<float>(full[q]), "Absorber display changed physical pressure");
}
int main(int argc, char **argv) {
    try {
        const auto folder = std::filesystem::path(SEIS3D_OUTPUT_ROOT) / "validation/acoustic-cpml-2d";
        std::filesystem::create_directories(folder);
        std::string error;
        std::ofstream log(folder / "validation.txt");
        log << std::setprecision(12);
        auto note = [&](const std::string &s) {
            log << s << std::endl;
            std::cout << s << std::endl;
        };
        auto small = fixture(0);
        small.model.nx = small.model.nz = 41;
        small.model.sourceX = 20;
        small.model.sourceZ = 15;
        small.model.receiverStart = 5;
        small.model.receiverCount = 5;
        small.model.receiverStride = 7;
        small.model.receiverDepth = 15;
        small.model.duration = .3;
        small.model.flattenDepth = 150;
        small.compute.cpuThreads = 1;
        small.backend = BackendKind3D::Cpu;
        Seis2D::Acoustic::Simulation s;
        check(s.initialize(small, error) && s.advance(80, error), error);
        const auto pressure = s.pressureSnapshot();
        check(s.reset(error) && s.advance(80, error), error);
        check(s.pressureSnapshot() == pressure, "CPML memories not reset");
        auto invalid = small;
        invalid.cpml.kappaMax = .5;
        check(!s.initialize(invalid, error) && s.pressureSnapshot() == pressure,
              "Invalid CPML rebuild lost old state");
        const auto model = folder / "cpml-model.vts";
        check(writeModelVts(s, model, error), error);
        Options restored;
        check(readModelVts(model, restored, error), error);
        check(restored == small, "CPML parameters did not roundtrip");
        legacyFixture(model, folder / "legacy-sponge-model.vts", s);
        Options old;
        check(readModelVts(folder / "legacy-sponge-model.vts", old, error), error);
        auto legacySponge = small;
        legacySponge.memoryMiB = 1536;
        legacySponge.absorber = AbsorbingBoundary::Sponge;
        Seis2D::Acoustic::Simulation loadedSponge, explicitSponge;
        check(old == legacySponge && loadedSponge.initialize(old, error) &&
                  explicitSponge.initialize(legacySponge, error),
              "Legacy schema 1 not mapped to original sponge");
        check(loadedSponge.advance(80, error) && explicitSponge.advance(80, error), error);
        check(loadedSponge.pressureSnapshot() == explicitSponge.pressureSnapshot(),
              "Legacy sponge replay changed");
        legacyFixture(model, folder / "legacy-cpml-model.vts", s, 2);
        Options legacyCpml;
        check(readModelVts(folder / "legacy-cpml-model.vts", legacyCpml, error), error);
        auto explicitCpml = small;
        explicitCpml.memoryMiB = 1536;
        check(legacyCpml == explicitCpml, "Legacy schema 2 RAM budget was not retained");
        for (auto precision : {Precision::Float64, Precision::Float32}) {
            auto cpu = small;
            cpu.model.precision = precision;
            if (precision == Precision::Float32)
                cpu.cpml.kappaMax = 3;
            Seis2D::Acoustic::Simulation c;
            check(c.initialize(cpu, error) && c.advance(160, error), error);
            checkFullPressure(c);
            const auto expected = c.pressureSnapshot();
            if (Seis3D::Curvilinear::queryBackend3D(BackendKind3D::Cuda).available) {
                auto gpu = cpu;
                gpu.backend = BackendKind3D::Cuda;
                Seis2D::Acoustic::Simulation g;
                check(g.initialize(gpu, error) && g.advance(160, error), error);
                checkFullPressure(g);
                const double delta = relative(expected, g.pressureSnapshot());
                note(std::string("CPU/CUDA ") + precisionName(precision) + " relative L2=" + number(delta));
                check(delta < (precision == Precision::Float64 ? 1e-10 : 2e-4), "CPML CPU/CUDA mismatch");
            }
            Seis2D::Acoustic::Detail::ModelSettings settings;
            settings.model = cpu.model;
            settings.compute = cpu.compute;
            settings.cpml = true;
            settings.cpmlSettings = cpu.cpml;
            auto mesh = Seis2D::Acoustic::Detail::mesh(settings);
            auto op = Seis2D::Acoustic::Detail::makeOperator<double>(mesh, false, 1);
            auto engine = Seis2D::Acoustic::Detail::engine(op, false, -1);
            const int source = cpu.model.sourceZ * mesh.nx + mesh.pad + cpu.model.sourceX;
            engine->step(false, source, 1);
            for (int n = 0; n < 70; ++n)
                engine->step(false, -1, 0);
            const auto checkpoint = engine->state();
            for (int n = 0; n < 30; ++n)
                engine->step(false, -1, 0);
            const auto continuation = engine->field();
            engine->restore(checkpoint);
            for (int n = 0; n < 30; ++n)
                engine->step(false, -1, 0);
            check(engine->field() == continuation, "CPML checkpoint omitted histories");
        }
        note("PASS: full-domain physical pressure / exact target crop / nonzero absorber / CPU-CUDA "
             "FP32-FP64");
        auto dry = small;
        dry.model.spacing = 2;
        dry.model.flattenDepth = 60;
        dry.model.frequency = 15;
        dry.model.layered = true;
        dry.model.layers = {{20, 500, 0, 1600}, {0, 2000, 0, 2100}};
        dry.model.sourceZ = 3;
        dry.model.receiverDepth = 3;
        dry.model.duration = .5;
        Seis2D::Acoustic::Simulation dry64, dry32;
        check(dry64.initialize(dry, error) && dry64.advance(dry64.totalSteps(), error), error);
        dry.model.precision = Precision::Float32;
        check(dry32.initialize(dry, error) && dry32.advance(dry32.totalSteps(), error), error);
        const double dryError = relative(dry64.pressureSnapshot(), dry32.pressureSnapshot());
        note("Dry loess Vp=500 m/s, h=2 m, 15 Hz FP32/FP64 pressure L2=" + number(dryError));
        check(dryError < .005, "Low-speed dry-loess CPML precision mismatch");
        auto tail = fixture(0);
        tail.model.nx = 81;
        tail.model.nz = 61;
        tail.model.sourceX = 40;
        tail.model.sourceZ = 10;
        tail.model.receiverStart = 5;
        tail.model.receiverStride = 35;
        tail.model.receiverDepth = 10;
        tail.model.duration = 5;
        tail.model.flattenDepth = 200;
        tail.model.precision = Precision::Float32;
        tail.cpml.kappaMax = 3;
        Seis2D::Acoustic::Simulation longRun;
        check(longRun.initialize(tail, error), error);
        check(longRun.advance(static_cast<int>(.2 / longRun.dt()), error), error);
        double early = 0;
        for (double v : longRun.pressureSnapshot())
            early = std::max(early, std::abs(v));
        check(longRun.advance(longRun.totalSteps() - longRun.step(), error), error);
        double late = 0;
        for (double v : longRun.pressureSnapshot()) {
            check(std::isfinite(v), "Unstable CPML tail");
            late = std::max(late, std::abs(v));
        }
        note("Five-second FP32 CPML, kappa=3, final/early peak=" + number(late / std::max(early, 1e-100)));
        check(late < .1 * early, "CPML tail failed to decay");
        note(
            "PASS: CPML reset / failed rebuild / checkpoint histories / auto schema 3 + legacy schemas 1/2 / "
            "backend precision");
        std::ofstream csv(folder / "reflection.csv");
        csv << "case,sponge_relative_l2,cpml_relative_l2,improvement_db,backend,precision,nx,nz,h_m,"
               "frequency_hz,duration_s,pad_cells,target_reflection,kappa_max,reference_margin_cells,source_"
               "x_index,source_z_index,receiver_start,receiver_stride,receiver_depth\n"
            << std::setprecision(17);
        const int only = argc > 1 ? std::stoi(argv[1]) : -1;
        const char *names[]{"homogeneous", "shallow_grazing", "corner",
                            "terrain",     "layered",         "bottom_oblique"};
        for (int which = 0; which < 6; ++which) {
            if (only >= 0 && only != which)
                continue;
            auto referenceOptions = fixture(which, 240);
            referenceOptions.damping = false;
            referenceOptions.absorberCells = 8;
            note(std::string("Running wide-domain reference: ") + names[which]);
            const auto truth = solve(referenceOptions);
            auto spongeOptions = fixture(which);
            spongeOptions.absorber = AbsorbingBoundary::Sponge;
            const auto sponge = solve(spongeOptions);
            auto cpmlOptions = spongeOptions;
            cpmlOptions.absorber = AbsorbingBoundary::Cpml;
            const auto cpml = solve(cpmlOptions);
            const double oldError = errorEnergy(truth, sponge), newError = errorEnergy(truth, cpml);
            const double improvement = 20 * std::log10(oldError / std::max(newError, 1e-100));
            csv << names[which] << ',' << oldError << ',' << newError << ',' << improvement << ','
                << (cpmlOptions.backend == BackendKind3D::Cuda ? "CUDA" : "CPU") << ",FP64,"
                << cpmlOptions.model.nx << ',' << cpmlOptions.model.nz << ',' << cpmlOptions.model.spacing
                << ',' << cpmlOptions.model.frequency << ',' << cpmlOptions.model.duration << ','
                << cpmlOptions.absorberCells << ',' << cpmlOptions.cpml.targetReflection << ','
                << cpmlOptions.cpml.kappaMax << ",240," << cpmlOptions.model.sourceX << ','
                << cpmlOptions.model.sourceZ << ',' << cpmlOptions.model.receiverStart << ','
                << cpmlOptions.model.receiverStride << ',' << cpmlOptions.model.receiverDepth << std::endl;
            note(std::string(names[which]) + ": sponge=" + std::to_string(oldError) +
                 " CPML=" + std::to_string(newError) + " improvement_dB=" + std::to_string(improvement));
            std::ofstream records(folder / (std::string(names[which]) + "_records.csv"));
            records << "time_s";
            for (int r = 0; r < 3; ++r)
                records << ",reference_R" << r << ",sponge_R" << r << ",cpml_R" << r;
            records << '\n' << std::setprecision(17);
            for (std::size_t n = 0; n < cpml.traces[0].size(); ++n) {
                records << n * cpml.dt;
                for (int r = 0; r < 3; ++r)
                    records << ',' << truth.traces[r][n] << ',' << sponge.traces[r][n] << ','
                            << cpml.traces[r][n];
                records << '\n';
            }
            check(improvement > 6 && newError < .05,
                  "CPML failed required reflection improvement (>6 dB, residual <5%)");
        }
        note("PASS: acoustic weak-form CFS-CPML reflection audit");
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << std::endl;
        return 1;
    }
}
