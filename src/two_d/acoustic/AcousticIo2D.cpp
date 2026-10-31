#include "Seis2D/AcousticSolver2D.hpp"
#include "SeisCommon/AtomicFile.hpp"
#include "SeisCommon/SegyIo.hpp"
#include "SeisCommon/VtsModelIo.hpp"
#include <fstream>
#include <iomanip>
#include <locale>
namespace Seis2D::Acoustic {
namespace {
using namespace SeisWave::VtsModel;
Arrays metadata(const Simulation &s, bool snapshot) {
    const auto &o = s.options();
    const auto &p = o.model;
    Arrays a;
    const bool surveySchema = p.autoSurfaceReceivers || p.receiverTargetCount != 0;
    a[snapshot ? "SeisWaveAcousticSnapshotVersion" : "SeisWaveAcousticModelVersion"] = {
        surveySchema                                 ? 5.
        : o.propagator == Propagator::CurvilinearDrp ? 4.
        : o.memoryMiB == 0                           ? 3.
                                                     : 2.};
    if (o.propagator == Propagator::CurvilinearDrp || surveySchema)
        a["AcousticPropagator"] = {double(static_cast<int>(o.propagator)), o.drpDissipation};
    if (surveySchema)
        a["SurfaceReceiverLayout"] = {double(p.autoSurfaceReceivers), double(p.receiverTargetCount)};
    a["AcousticBoundary"] = {double(static_cast<int>(o.absorber)), o.cpml.targetReflection, o.cpml.kappaMax,
                             o.cpml.frequency};
    a["Grid"] = {double(p.nx), double(p.nz),   p.spacing,      p.relief,
                 p.wavelength, p.flattenDepth, p.surfaceDatum, double(p.importedSurface)};
    a["Material"] = {p.vp, p.vs, p.density, double(p.layered)};
    a["Survey"] = {double(p.sourceX),
                   double(p.sourceZ),
                   double(p.receiverStart),
                   double(p.receiverCount),
                   double(p.receiverStride),
                   double(p.receiverDepth),
                   p.frequency,
                   p.duration,
                   double(p.maxSteps),
                   p.sampleIntervalMs};
    a["Runtime"] = {double(static_cast<int>(p.precision)),
                    double(o.compute.cpuThreads),
                    double(o.compute.cudaDevice),
                    double(static_cast<int>(o.backend)),
                    double(o.absorberCells),
                    double(o.damping),
                    o.memoryMiB,
                    o.sourceStrength};
    a["Reference"] = {s.dt(), double(s.totalSteps()), s.recordDt()};
    auto &layers = a["Layers"];
    for (const auto &v : p.layers)
        layers.insert(layers.end(), {v.thickness, v.vp, v.vs, v.density, v.qp, v.qs});
    auto &surface = a["Surface"];
    for (const auto &v : p.surfaceSamples)
        surface.insert(surface.end(), {v[0], v[1]});
    if (snapshot)
        a["State"] = {double(s.step()), s.step() * s.dt()};
    return a;
}
void write(const Simulation &s, const std::filesystem::path &path, bool snapshot) {
    require(s.initialized(), "Initialize acoustic model before exporting.");
    auto a = metadata(s, snapshot);
    const auto &g = s.grid();
    Checksum hash;
    for (const auto &[name, v] : a) {
        hash.name(name);
        for (double x : v) {
            require(std::isfinite(x), "Nonfinite acoustic model metadata.");
            hash.number(x);
        }
    }
    std::vector<double> pressure;
    if (snapshot)
        pressure = s.pressureSnapshot();
    auto points = [&](const auto &emit) {
        for (std::size_t q = 0; q < g.x.size(); ++q)
            emit(q);
    };
    for (auto [name, v] : {std::pair{"Vp", &g.vp}, {"Density", &g.density}}) {
        hash.name(name);
        points([&](auto q) { hash.number((*v)[q]); });
    }
    if (snapshot) {
        hash.name("Pressure_Pa");
        for (double x : pressure)
            hash.number(x);
    }
    hash.name("Coordinates");
    points([&](auto q) {
        hash.number(g.x[q]);
        hash.number(0);
        hash.number(g.z[q]);
    });
    a["Checksum"] = {double(hash.value >> 32), double(hash.value & 0xffffffffULL)};
    auto pending = path;
    pending += ".tmp-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    try {
        std::ofstream f(pending);
        f.exceptions(std::ios::badbit | std::ios::failbit);
        f.imbue(std::locale::classic());
        f << std::setprecision(17);
        auto extent = "0 " + std::to_string(g.nx - 1) + " 0 0 0 " + std::to_string(g.nz - 1);
        f << "<?xml version=\"1.0\"?>\n<VTKFile type=\"StructuredGrid\" version=\"1.0\" "
             "byte_order=\"LittleEndian\"><StructuredGrid WholeExtent=\""
          << extent << "\"><FieldData>\n";
        for (const auto &[name, v] : a) {
            f << "<DataArray type=\"Float64\" Name=\"" << name
              << "\" NumberOfComponents=\"1\" NumberOfTuples=\"" << v.size() << "\" format=\"ascii\">\n";
            for (double x : v)
                f << x << ' ';
            f << "\n</DataArray>\n";
        }
        f << "</FieldData><Piece Extent=\"" << extent << "\"><PointData>\n";
        for (auto [name, v] : {std::pair{"Vp", &g.vp}, {"Density", &g.density}}) {
            f << "<DataArray type=\"Float64\" Name=\"" << name << "\" format=\"ascii\">\n";
            points([&](auto q) { f << (*v)[q] << ' '; });
            f << "\n</DataArray>\n";
        }
        if (snapshot) {
            f << "<DataArray type=\"Float64\" Name=\"Pressure_Pa\" format=\"ascii\">\n";
            for (double x : pressure)
                f << x << ' ';
            f << "\n</DataArray>\n";
        }
        f << "</PointData><Points><DataArray type=\"Float64\" Name=\"Coordinates\" NumberOfComponents=\"3\" "
             "format=\"ascii\">\n";
        points([&](auto q) { f << g.x[q] << " 0 " << g.z[q] << ' '; });
        f << "\n</DataArray></Points></Piece></StructuredGrid></VTKFile>\n";
        f.close();
        SeisWave::replaceCompletedFile(pending, path);
    } catch (...) {
        std::error_code ignored;
        std::filesystem::remove(pending, ignored);
        throw;
    }
}
} // namespace
bool writeModelVts(const Simulation &s, const std::filesystem::path &p, std::string &e) {
    try {
        write(s, p, false);
        e.clear();
        return true;
    } catch (const std::exception &x) {
        e = x.what();
        return false;
    }
}
bool readModelVts(const std::filesystem::path &path, Options &destination, std::string &error) {
    try {
        using namespace SeisWave::VtsModel;
        std::ifstream f(path);
        f.imbue(std::locale::classic());
        require(bool(f), "Cannot open acoustic model VTS.");
        auto head = tag(f);
        if (head.starts_with("<?xml"))
            head = tag(f);
        require(head.starts_with("<VTKFile ") && attribute(head, "type") == "StructuredGrid",
                "Expected acoustic StructuredGrid.");
        auto gridHeader = tag(f);
        expected(f, "<FieldData>");
        Arrays a;
        Checksum hash;
        while (true) {
            auto t = tag(f);
            if (t == "</FieldData>")
                break;
            require(t.starts_with("<DataArray ") && a.size() < 12, "Invalid acoustic metadata.");
            auto name = attribute(t, "Name");
            require(!a.contains(name), "Duplicate acoustic metadata.");
            const auto text = attribute(t, "NumberOfTuples");
            std::size_t end{};
            double count = std::stod(text, &end);
            require(end == text.size() && attribute(t, "NumberOfComponents") == "1",
                    "Invalid acoustic metadata dimensions.");
            int n = integer(count, 0, 2000000);
            a[name] = readArray(f, t, name == "Checksum" ? nullptr : &hash, n, true);
        }
        auto array = [&](const char *name, std::size_t count) -> const std::vector<double> & {
            auto it = a.find(name);
            require(it != a.end() && it->second.size() == count,
                    std::string("Missing acoustic model metadata: ") + name);
            return it->second;
        };
        const auto version = integer(array("SeisWaveAcousticModelVersion", 1)[0], 1, 5);
        Options next;
        if (version >= 4) {
            const auto &method = array("AcousticPropagator", 2);
            next.propagator = static_cast<Propagator>(integer(method[0], version >= 5 ? 0 : 1, 1));
            next.drpDissipation = method[1];
            require(next.drpDissipation >= 0 && next.drpDissipation <= 1, "Invalid DRP dissipation.");
        }
        if (version == 1)
            next.absorber = AbsorbingBoundary::Sponge;
        else {
            const auto &boundary = array("AcousticBoundary", 4);
            next.absorber = static_cast<AbsorbingBoundary>(integer(boundary[0], 0, 1));
            next.cpml = {boundary[1], boundary[2], boundary[3]};
            require(next.cpml.targetReflection >= 1e-10 && next.cpml.targetReflection <= .1 &&
                        next.cpml.kappaMax >= 1 && next.cpml.kappaMax <= 8 && next.cpml.frequency >= 0 &&
                        next.cpml.frequency <= 10000,
                    "Invalid acoustic CPML metadata.");
        }
        auto &p = next.model;
        if (version >= 5) {
            const auto &layout = array("SurfaceReceiverLayout", 2);
            p.autoSurfaceReceivers = integer(layout[0], 0, 1) != 0;
            p.receiverTargetCount = integer(layout[1], 0, 4096);
        }
        const auto &g = array("Grid", 8);
        p.nx = integer(g[0], 16, 16384);
        p.nz = integer(g[1], 16, 16384);
        p.spacing = g[2];
        p.relief = g[3];
        p.wavelength = g[4];
        p.flattenDepth = g[5];
        p.surfaceDatum = g[6];
        p.importedSurface = integer(g[7], 0, 1) != 0;
        const auto &m = array("Material", 4);
        p.vp = m[0];
        p.vs = m[1];
        p.density = m[2];
        p.layered = integer(m[3], 0, 1) != 0;
        const auto &survey = array("Survey", 10);
        p.sourceX = integer(survey[0], 1, p.nx - 2);
        p.sourceZ = integer(survey[1], 1, p.nz - 2);
        p.receiverStart = integer(survey[2], 0, p.nx - 1);
        p.receiverCount = integer(survey[3], 1, 4096);
        p.receiverStride = integer(survey[4], 1, 16384);
        p.receiverDepth = integer(survey[5], 1, p.nz - 2);
        p.frequency = survey[6];
        p.duration = survey[7];
        p.maxSteps = integer(survey[8], 0, 200000);
        p.sampleIntervalMs = survey[9];
        const auto &r = array("Runtime", 8);
        p.precision = static_cast<Precision>(integer(r[0], 0, 1));
        next.compute.cpuThreads = integer(r[1], 0, 64);
        next.compute.cudaDevice = integer(r[2], -1, 10000);
        next.backend = static_cast<BackendKind3D>(integer(r[3], 0, 1));
        next.absorberCells = integer(r[4], 8, 128);
        next.damping = integer(r[5], 0, 1) != 0;
        next.memoryMiB = r[6];
        require(std::isfinite(next.memoryMiB) && ((version >= 3 && next.memoryMiB == 0) ||
                                                  (next.memoryMiB >= 16 && next.memoryMiB <= 32768)),
                "Invalid acoustic RAM budget policy.");
        next.sourceStrength = r[7];
        const auto &layers = a.at("Layers");
        require(layers.size() % 6 == 0 && layers.size() <= 96, "Invalid acoustic layer table.");
        p.layers.clear();
        for (std::size_t j = 0; j < layers.size(); j += 6)
            p.layers.push_back(
                {layers[j], layers[j + 1], layers[j + 2], layers[j + 3], layers[j + 4], layers[j + 5]});
        const auto &surface = a.at("Surface");
        require(surface.size() % 2 == 0, "Invalid acoustic surface samples.");
        p.surfaceSamples.clear();
        for (std::size_t j = 0; j < surface.size(); j += 2)
            p.surfaceSamples.push_back({surface[j], surface[j + 1]});
        require(p.spacing > 0 && p.frequency > 0 && p.duration > 0 && p.duration <= 10 &&
                    array("Reference", 3)[0] > 0,
                "Invalid acoustic physical parameters.");
        auto extent = "0 " + std::to_string(p.nx - 1) + " 0 0 0 " + std::to_string(p.nz - 1);
        require(attribute(gridHeader, "WholeExtent") == extent, "Acoustic model extent mismatch.");
        auto piece = tag(f);
        require(attribute(piece, "Extent") == extent, "Acoustic piece extent mismatch.");
        expected(f, "<PointData>");
        std::size_t count = std::size_t(p.nx) * p.nz;
        for (const char *name : {"Vp", "Density"}) {
            auto t = tag(f);
            require(attribute(t, "Name") == name, "Missing acoustic point array.");
            readArray(f, t, &hash, count, false);
        }
        expected(f, "</PointData>");
        expected(f, "<Points>");
        auto t = tag(f);
        require(attribute(t, "Name") == "Coordinates" && attribute(t, "NumberOfComponents") == "3",
                "Invalid acoustic coordinate array.");
        readArray(f, t, &hash, 3 * count, false);
        expected(f, "</Points>");
        expected(f, "</Piece>");
        expected(f, "</StructuredGrid>");
        expected(f, "</VTKFile>");
        f >> std::ws;
        require(f.peek() == std::char_traits<char>::eof(), "Trailing data in acoustic model.");
        const auto &checksum = array("Checksum", 2);
        require(checksum[0] == double(hash.value >> 32) && checksum[1] == double(hash.value & 0xffffffffULL),
                "Acoustic model checksum mismatch.");
        destination = std::move(next);
        error.clear();
        return true;
    } catch (const std::exception &e) {
        error = e.what();
        return false;
    }
}
bool writePressureSegy(const Simulation &s, const std::filesystem::path &file, std::string &error,
                       bool ieee64) {
    if (!s.initialized()) {
        error = "Initialize acoustic simulation first.";
        return false;
    }
    SeisWave::RecordDataView data;
    data.units = "Pa";
    data.components = {"Pressure"};
    data.dt = s.recordDt();
    const auto &g = s.grid();
    const auto &p = s.options().model;
    const auto source = std::size_t(p.sourceZ) * g.nx + p.sourceX;
    for (const auto &r : s.receivers()) {
        SeisWave::TraceView trace;
        trace.position = {r.position[0], 0, r.position[1]};
        trace.source = {g.x[source], 0, g.z[source]};
        trace.channels[0] = r.pressure;
        data.traces.push_back(trace);
    }
    return SeisWave::writeSegy(data, 0, file, error, ieee64);
}
bool exportResult(const Simulation &s, const std::filesystem::path &folder, std::string &error,
                  bool segyIeee64) {
    try {
        using SeisWave::VtsModel::require;
        require(s.initialized(), "Initialize acoustic simulation first.");
        require(!std::filesystem::exists(folder), "Export directory already exists.");
        std::filesystem::create_directories(folder);
        require(writeModelVts(s, folder / "model_acoustic.vts", error), error);
        write(s, folder / "pressure.vts", true);
        const auto &records = s.receivers();
        std::ofstream csv(folder / "pressure_records.csv");
        csv.exceptions(std::ios::badbit | std::ios::failbit);
        csv.imbue(std::locale::classic());
        csv << std::setprecision(17)
            << "# physics=acoustic; component=pressure; unit=Pa; time_s=sample_index*record_dt\ntime_s";
        for (std::size_t r = 0; r < records.size(); ++r)
            csv << ",receiver_" << r;
        csv << '\n';
        for (std::size_t n = 0; n < records.front().pressure.size(); ++n) {
            csv << n * s.recordDt();
            for (const auto &r : records)
                csv << ',' << r.pressure[n];
            csv << '\n';
        }
        csv.close();
        require(writePressureSegy(s, folder / "pressure_records.sgy", error, segyIeee64), error);
        std::ofstream coordinates(folder / "receivers.csv");
        coordinates.exceptions(std::ios::badbit | std::ios::failbit);
        coordinates.imbue(std::locale::classic());
        coordinates << std::setprecision(17) << "receiver,x_m,z_positive_down_m\n";
        for (std::size_t r = 0; r < records.size(); ++r)
            coordinates << r << ',' << records[r].position[0] << ',' << records[r].position[1] << '\n';
        coordinates.close();
        std::ofstream info(folder / "experiment.txt");
        info.exceptions(std::ios::badbit | std::ios::failbit);
        info << std::setprecision(17) << "physics=variable-density acoustic pressure\nmethod="
             << (s.options().propagator == Propagator::P1
                     ? "P1 triangular FEM / lumped mass / second-order time"
                     : "experimental curvilinear central DRP7 / first-order p-v / classical RK4")
             << "\nboundary="
             << (!s.options().damping ? "outer Dirichlet only"
                 : s.options().absorber == AbsorbingBoundary::Cpml
                     ? (s.options().propagator == Propagator::P1
                            ? "CFS-CPML, weak-form convolution filters"
                            : "CFS-CPML, four ADE histories at every RK stage")
                     : (s.options().propagator == Propagator::P1 ? "legacy quadratic sponge"
                                                                 : "DRP cubic sponge"))
             << "\nprecision=" << precisionName(s.options().model.precision)
             << "\nsource_strength_Pa_m2_s2=" << s.options().sourceStrength
             << "\nsource=area-normalized nodal pressure acceleration; Ricker delay 1.5/f\nVs and Q are not "
                "used. No Rayleigh/S/conversion waves. Pressure is not Vz/Vx.\nCurrent RTM uses a different "
                "source normalization; absolute amplitudes are not interchangeable.\nCPU/GPU speed ratios "
                "from elastic benchmarks do not apply.\ndt_s="
             << s.dt() << "\nstep=" << s.step() << "\ntotal_steps=" << s.totalSteps()
             << "\nrecord_dt_s=" << s.recordDt() << "\ncompute_ms=" << s.diagnostics().totalStepMs
             << "\npressure_records.sgy=SEG-Y rev2; " << (segyIeee64 ? "IEEE64" : "IEEE32")
             << "; Pa; gain=1; no display AGC\n"
             << "\nresident_host_arrays_bytes=" << s.diagnostics().residentHostArrayBytes
             << "\nram_budget_policy=" << (s.options().memoryMiB == 0 ? "automatic" : "manual")
             << "\nresolved_ram_budget_MiB=" << s.diagnostics().ramBudgetMiB
             << "\nresident_device_arrays_bytes=" << s.diagnostics().residentDeviceBytes
             << "\nmodel_acoustic.vts reloads the model at t=0, pressure.vts is not a restart file.\n";
        if (s.options().propagator == Propagator::CurvilinearDrp)
            info << "drp_source_spatial=5x5 binomial, clipped/area normalized\ndrp_dissipation="
                 << s.options().drpDissipation
                 << "\ndrp_filter=sixth-difference pressure, energy-weighted; numerical, not material Q\n";
        info.close();
        std::ofstream complete(folder / "COMPLETE.txt");
        complete.exceptions(std::ios::badbit | std::ios::failbit);
        complete << "All acoustic model / field / record files written.\n";
        complete.close();
        error.clear();
        return true;
    } catch (const std::exception &e) {
        error = e.what();
        return false;
    }
}
} // namespace Seis2D::Acoustic
