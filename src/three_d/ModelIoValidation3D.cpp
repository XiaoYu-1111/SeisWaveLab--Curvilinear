#include "Seis3D/MaterialModels3D.hpp"
#include "Seis3D/ModelIo3D.hpp"
#include "Seis3D/ReceiverIo.hpp"
#include "Seis3D/VtsWriter.hpp"
#include <algorithm>
#include <cmath>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
using namespace Seis3D::Curvilinear;
namespace {
void check(bool ok, const std::string &error) {
    if (!ok)
        throw std::runtime_error(error);
}
std::string read(const std::filesystem::path &p) {
    std::ifstream f(p);
    return {std::istreambuf_iterator<char>(f), {}};
}
std::vector<double> array(const std::string &s, const char *name) {
    const auto at = s.find(std::string("Name=\"") + name + "\"");
    check(at != s.npos, "Missing exported array");
    const auto begin = s.find('>', at) + 1, end = s.find('<', begin);
    std::istringstream in(s.substr(begin, end - begin));
    std::vector<double> v;
    double x;
    while (in >> x)
        v.push_back(x);
    return v;
}
} // namespace
int main() {
    try {
        const auto root = std::filesystem::path(SEIS3D_OUTPUT_ROOT) / "validation/model-3d";
        std::filesystem::create_directories(root);
        std::string error;
        SimulationParameters p;
        p.nx = 24;
        p.ny = 16;
        p.nz = 28;
        p.spacing = 2.5;
        p.topographyAmplitude = 1;
        p.topographyWavelength = 50;
        p.dominantFrequency = 30;
        p.duration = .003;
        p.maxSteps = 17;
        p.sourceX = 12;
        p.sourceY = 8;
        p.sourceZ = 6;
        p.receiverArray = {ReceiverArrayKind3D::GridXY, 3, 3, 1, 6, 4, 2, 2};
        p.absorbingBoundary = {true, {4, 3, 5}, 2.2};
        p.source.direction = {1, 2, 3};
        p.layers = {{"黄土 loess", 10, 1500, 700, 1800}, {"基岩 basement", 0, 3000, 1700, 2200}};
        const bool cuda = queryBackend3D(BackendKind3D::Cuda).available;
        for (int kind = 0; kind < (cuda ? 2 : 1); ++kind)
            for (int mode = 0; mode < 4; ++mode) {
                p.layeredMaterial = mode != 0;
                p.layerDepthMode = static_cast<LayerDepthMode3D>(mode ? mode - 1 : 0);
                p.source.kind =
                    mode % 2 ? SeisWave::SourceKind::IsotropicMoment : SeisWave::SourceKind::DirectionalForce;
                ComputeSettings3D settings;
                settings.cpuThreads = 1;
                auto source = createSimulationBackend3D(static_cast<BackendKind3D>(kind), error);
                check(source && source->initialize(p, error, settings), error);
                check(source->advance(2, error), error);
                const auto path = root / std::filesystem::path(u8"三维模型.vts");
                check(writeModelVts3D(*source, path, settings, error), error);
                check(writeModelVts3D(*source, path, settings, error), "Overwrite: " + error);
                ModelDocument3D doc;
                check(readModelVts3D(path, doc, error), error);
                check(doc.parameters == p && doc.compute == settings &&
                          doc.backend == static_cast<BackendKind3D>(kind) && doc.referenceDt == source->dt(),
                      "Roundtrip parameters/settings/dt differ");
                auto restored = createSimulationBackend3D(doc.backend, error);
                check(restored && restored->initialize(doc.parameters, error, doc.compute), error);
                check(restored->currentStep() == 0 && restored->dt() == source->dt() &&
                          restored->grid().coordinates == source->grid().coordinates,
                      "Restored geometry/time mismatch");
                check(restored->advance(2, error), error);
                const auto a = source->snapshot(), b = restored->snapshot();
                check(a.vx == b.vx && a.vy == b.vy && a.vz == b.vz && a.sxx == b.sxx,
                      "Reproduced solver differs");
                VtsExportOptions3D options;
                options.fields.fill(false);
                options.fields[0] = options.fields[4] = true;
                options.includeMaterial = false;
                options.includeVelocityVector = false;
                check(writeVts(a, root / "selected.vts", options, error), error);
                auto text = read(root / "selected.vts");
                check(text.find("Float32") == text.npos && text.find("Name=\"Vz\"") == text.npos &&
                          text.find("Name=\"Vp\"") == text.npos &&
                          text.find("Name=\"Velocity\"") == text.npos,
                      "Unselected fields leaked");
                const auto vx = array(text, "Vx"), pressure = array(text, "MeanNormalStress");
                std::size_t n = 0;
                const auto &g = a.grid;
                for (int k = 0; k < p.nz; ++k)
                    for (int j = 0; j < p.ny; ++j)
                        for (int i = 0; i < p.nx; ++i) {
                            const auto q = g.index(i + g.ghost, j + g.ghost, k + g.ghost);
                            check(vx.at(n) == a.vx[q] &&
                                      pressure.at(n) == -(a.sxx[q] + a.syy[q] + a.szz[q]) / 3.,
                                  "Snapshot value/order mismatch");
                            ++n;
                        }
                check(n == vx.size(), "Snapshot tuple count differs");
                options.fields.fill(false);
                check(!writeVts(a, root / "selected.vts", options, error) &&
                          read(root / "selected.vts") == text,
                      "Empty selection overwrote output");
                check(writeVts(a, root / "legacy.vts", error), error);
                check(writeReceiversCsv(*source, root / "selected.csv",
                                        std::array<bool, 3>{true, true, false}, error),
                      error);
                const auto csv = read(root / "selected.csv");
                check(csv.find("receiver_1_Vx,receiver_1_Vy") != csv.npos && csv.find("_Vz") == csv.npos,
                      "Receiver component headers differ");
                std::istringstream lines(csv);
                std::string line;
                int row = -1;
                while (std::getline(lines, line)) {
                    if (line.starts_with('#'))
                        continue;
                    if (row++ == -1)
                        continue;
                    std::replace(line.begin(), line.end(), ',', ' ');
                    std::istringstream values(line);
                    double time;
                    values >> time;
                    const auto sample = static_cast<std::size_t>(row - 1);
                    check(time == sample * source->dt(), "CSV time differs");
                    for (const auto &trace : source->receivers()) {
                        double x, y;
                        check(static_cast<bool>(values >> x >> y) && x == trace.samplesVx[sample] &&
                                  y == trace.samplesVy[sample],
                              "CSV selected values differ");
                    }
                    values >> std::ws;
                    check(values.eof(), "CSV extra components");
                }
                check(row == 3, "CSV sample count differs");
                check(!writeReceiversCsv(*source, root / "selected.csv",
                                         std::array<bool, 3>{false, false, false}, error) &&
                          read(root / "selected.csv") == csv,
                      "Empty receiver selection overwrote output");
                auto corrupt = read(path);
                const auto at = corrupt.find("Name=\"Parameter_spacing\"");
                const auto pos = corrupt.find('>', at) + 1;
                corrupt[pos] = '3';
                std::ofstream(root / "corrupt.vts") << corrupt;
                const auto saved = doc.parameters;
                check(!readModelVts3D(root / "corrupt.vts", doc, error) && doc.parameters == saved,
                      "Corrupt model accepted/mutated output");
                std::ofstream(root / "truncated.vts") << read(path).substr(0, 1024);
                check(!readModelVts3D(root / "truncated.vts", doc, error), "Truncated model accepted");
            }
        for(int kind=0;kind<(cuda?2:1);++kind) {
            auto terrain=p;terrain.source.kind=SeisWave::SourceKind::DirectionalForce;
            auto& dem=terrain.surfaceGrid;dem.nx=513;dem.ny=257;dem.spacing=.25;
            dem.originX=19300000.125;dem.originY=4146000.75;dem.datum=1500;
            for(int j=0;j<dem.ny;++j)for(int i=0;i<dem.nx;++i)dem.elevations.push_back(1500+.4*std::sin(i*.02)*std::cos(j*.03));
            auto a=createSimulationBackend3D(static_cast<BackendKind3D>(kind),error);
            check(a&&a->initialize(terrain,error,{1}),error);check(a->advance(2,error),error);
            const auto file=root/"embedded-dem.vts";check(writeModelVts3D(*a,file,{1},error),error);
            ModelDocument3D loaded;check(readModelVts3D(file,loaded,error),error);
            check(loaded.parameters==terrain,"Embedded original DEM/parameters differ");
            auto b=createSimulationBackend3D(loaded.backend,error);
            check(b&&b->initialize(loaded.parameters,error,loaded.compute),error);
            check(b->grid().coordinates==a->grid().coordinates&&b->dt()==a->dt(),"Embedded DEM geometry/dt reproduction differs");
            check(b->advance(2,error),error);
            check(a->snapshot().vz==b->snapshot().vz,"Embedded DEM wavefield reproduction differs");
            auto damaged=read(file);auto at=damaged.find("Name=\"SurfaceGridElevations\"");
            damaged[damaged.find('>',at)+1]='2';std::ofstream(root/"bad-dem.vts")<<damaged;
            const auto before=loaded.parameters;
            check(!readModelVts3D(root/"bad-dem.vts",loaded,error)&&loaded.parameters==before,"Corrupt embedded DEM changed loaded project");
        }
        {
            SimulationParameters wide;wide.nx=310;wide.ny=8;wide.nz=12;wide.topographyAmplitude=0;wide.maxSteps=2;
            wide.receiverArray={ReceiverArrayKind3D::XLine,300,3,1,4,1,1,1};
            auto s=createSimulationBackend3D(BackendKind3D::Cpu,error);check(s&&s->initialize(wide,error,{1}),error);
            check(writeModelVts3D(*s,root/"wide-grid.vts",{1},error),error);
            ModelDocument3D loaded;check(readModelVts3D(root/"wide-grid.vts",loaded,error)&&loaded.parameters==wide,"Dynamic-axis receiver geometry failed roundtrip");
        }
        std::ofstream(root / "summary.txt")
            << "PASS CPU" << (cuda ? " and CUDA" : "")
            << ": uniform / three layer depth conventions, UTF-8, full parameters, overwrite, t=0 restart, "
               "exact geometry/dt/wavefield, selected Float64 components, corruption and truncation "
               "rejection; schema-2 embedded 513x257 DEM, exact reconstructed waves, corrupt DEM rejection and receiver indices above 299.\n";
        std::cout << read(root / "summary.txt");
        return 0;
    } catch (const std::exception &ex) {
        std::cerr << ex.what() << '\n';
        return 1;
    }
}
