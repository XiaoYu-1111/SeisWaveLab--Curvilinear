#include "CfsPml2D.hpp"
#include "Seis2D/AcousticSolver2D.hpp"
#include "Seis2D/ModelIo2D.hpp"
#include "Seis2D/SurfaceSurvey2D.hpp"
#include <cmath>
#include <fstream>
#include <iostream>
#include <stdexcept>
namespace E = Seis2D::Curvilinear;
namespace A = Seis2D::Acoustic;
void check(bool ok, const std::string &message) {
    if (!ok)
        throw std::runtime_error(message);
}
int main() {
    try {
        const auto folder = std::filesystem::path(SEIS3D_OUTPUT_ROOT) / "validation/auto-surface-2026-10-31";
        std::filesystem::create_directories(folder);
        std::ofstream log(folder / "native.txt");
        E::ComputeSettings3D compute;
        compute.cpuThreads = 1;
        std::string error;
        const bool gpu = Seis3D::Curvilinear::queryBackend3D(E::BackendKind3D::Cuda).available;
        for (bool cuda : {false, true}) {
            if (cuda && !gpu)
                continue;
            const auto backend = cuda ? E::BackendKind3D::Cuda : E::BackendKind3D::Cpu;
            for (auto precision : {E::Precision::Float32, E::Precision::Float64})
                for (bool q : {false, true})
                    for (bool external : {false, true}) {
                        E::Parameters p;
                        p.nx = 81;
                        p.nz = 61;
                        p.spacing = 5;
                        p.frequency = 10;
                        p.duration = .04;
                        p.maxSteps = 8;
                        p.vp = 2500;
                        p.vs = 1300;
                        p.density = 2100;
                        p.sourceX = 20;
                        p.sourceZ = 12;
                        p.relief = 6;
                        p.wavelength = 200;
                        p.precision = precision;
                        p.externalAbsorption = external;
                        p.spongeX = 8;
                        p.spongeZ = 8;
                        p.attenuation.enabled = q;
                        p.autoSurfaceReceivers = true;
                        p.receiverTargetCount = 13;
                        p.receiverStart = 999;
                        p.receiverCount = 1000;
                        p.receiverDepth = 999; // stale after resize: policy repairs it
                        if (external) {
                            p.importedSurface = true;
                            p.surfaceDatum = 1000;
                            p.surfaceSamples = {{0, 1000}, {100, 1010}, {200, 995}, {400, 1000}};
                        }
                        E::Simulation s;
                        check(s.initialize(p, error, backend, compute), error);
                        check(s.advance(8, error), error);
                        const auto active = s.parameters();
                        check(active.receiverTargetCount == 13 && active.receiverDepth == 0,
                              "Elastic canonical policy not published");
                        const int margin = external ? 0 : E::Detail::domainLayout(active).widthX;
                        for (const auto &r : s.receivers()) {
                            const auto pos = s.grid().coordinates[s.grid().modelIndex(r.x, 0)];
                            check(r.x >= margin && r.x <= p.nx - 1 - margin && r.z == 0 &&
                                      std::hypot(r.position[0] - pos[0], r.position[1] - pos[1]) < 1e-4,
                                  "Elastic/GSLS receiver not on mapped free surface or inside absorber");
                        }
                        auto again = active;
                        for (int n = 0; n < 10; ++n)
                            check(E::synchronizeSurfaceReceivers(again, false, 1024, margin) &&
                                      again == active,
                                  "Repeated elastic quantization drift");
                        auto file = folder / "elastic-auto.vts";
                        check(E::writeModelVts(s, file, compute, error), error);
                        E::ModelDocument doc;
                        check(E::readModelVts(file, doc, error) && doc.parameters == active, error);
                        auto resized = doc.parameters;
                        resized.nx = 41;
                        resized.receiverStart = 999;
                        check(s.initialize(resized, error, backend, compute), error);
                        check(s.parameters().receiverTargetCount == 13, "Resize lost desired elastic count");
                        auto manual = s.parameters();
                        manual.autoSurfaceReceivers = false;
                        manual.receiverStart = 2;
                        manual.receiverCount = 3;
                        manual.receiverStride = 4;
                        manual.receiverDepth = 3;
                        check(s.initialize(manual, error, backend, compute), error);
                        check(s.receivers().size() == 3 && s.receivers()[1].x == 6 && s.receivers()[1].z == 3,
                              "Elastic manual line overwritten");
                        file = folder / "elastic-manual.vts";
                        check(E::writeModelVts(s, file, compute, error) &&
                                  E::readModelVts(file, doc, error) && doc.parameters == manual,
                              error);
                        manual.receiverTargetCount = 0;
                        file = folder / "elastic-legacy.vts";
                        check(s.initialize(manual, error, backend, compute), error);
                        check(E::writeModelVts(s, file, compute, error) &&
                                  E::readModelVts(file, doc, error) && !doc.parameters.autoSurfaceReceivers &&
                                  doc.parameters.receiverTargetCount == 0,
                              error);
                    }
            for (auto precision : {E::Precision::Float32, E::Precision::Float64})
                for (auto method : {A::Propagator::P1, A::Propagator::CurvilinearDrp}) {
                    auto o = A::defaultOptions();
                    o.backend = backend;
                    o.compute = compute;
                    o.propagator = method;
                    auto &p = o.model;
                    p.nx = 81;
                    p.nz = 61;
                    p.spacing = 5;
                    p.layered = false;
                    p.vp = 1800;
                    p.frequency = 10;
                    p.sourceX = 20;
                    p.sourceZ = 12;
                    p.duration = .04;
                    p.maxSteps = 8;
                    p.relief = 6;
                    p.wavelength = 200;
                    p.precision = precision;
                    p.autoSurfaceReceivers = true;
                    p.receiverTargetCount = 13;
                    p.receiverStart = 999;
                    p.receiverCount = 1000;
                    p.receiverDepth = 999;
                    if (precision == E::Precision::Float64) {
                        p.importedSurface = true;
                        p.surfaceDatum = 1000;
                        p.surfaceSamples = {{0, 1000}, {100, 1010}, {200, 995}, {400, 1000}};
                    }
                    A::Simulation s;
                    check(s.initialize(o, error) && s.advance(8, error), error);
                    const auto active = s.options();
                    const auto &g = s.grid();
                    check(active.model.receiverTargetCount == 13 && active.model.receiverDepth == 1,
                          "Acoustic canonical policy not published");
                    for (std::size_t r = 0; r < s.receivers().size(); ++r) {
                        const auto x = active.model.receiverStart + int(r) * active.model.receiverStride;
                        const auto k = std::size_t(g.nx) + x;
                        check(std::hypot(s.receivers()[r].position[0] - g.x[k],
                                         s.receivers()[r].position[1] - g.z[k]) < 1e-4,
                              "P1/DRP receiver not on first mapped row");
                    }
                    auto file = folder / "acoustic-auto.vts";
                    check(A::writeModelVts(s, file, error), error);
                    A::Options loaded;
                    check(A::readModelVts(file, loaded, error) && loaded == active, error);
                    loaded.model.nx = 41;
                    loaded.model.receiverStart = 999;
                    check(s.initialize(loaded, error), error);
                    check(s.options().model.receiverTargetCount == 13 && s.options().model.receiverDepth == 1,
                          "Acoustic resize lost target/depth");
                    auto manual = s.options();
                    manual.model.autoSurfaceReceivers = false;
                    manual.model.receiverStart = 2;
                    manual.model.receiverCount = 3;
                    manual.model.receiverStride = 4;
                    manual.model.receiverDepth = 3;
                    check(s.initialize(manual, error), error);
                    check(s.receivers().size() == 3 && s.receivers()[1].position[0] == 30,
                          "Acoustic manual line overwritten");
                    file = folder / "acoustic-manual.vts";
                    check(A::writeModelVts(s, file, error) && A::readModelVts(file, loaded, error) &&
                              loaded == manual,
                          error);
                }
            log << (cuda ? "CUDA" : "CPU")
                << ": PASS elastic/GSLS + P1/DRP, FP32/FP64, internal/external, stale layout repair, "
                   "repeated alignment, resize, manual, VTS roundtrip, imported DEM\n";
        }
        log << "PASS: automatic forward surface survey and model reproducibility; numerical defaults remain "
               "manual\n";
        std::cout << "PASS: automatic forward surface survey\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
