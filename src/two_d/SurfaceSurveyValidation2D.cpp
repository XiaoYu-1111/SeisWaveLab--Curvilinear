#include "../common/ui/InitialModel2D.hpp"
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
void validateSurfaceForce(const std::filesystem::path &folder, bool gpu) {
    std::ofstream report(folder / "surface-force.txt");
    std::array<std::vector<double>, 4> references;
    std::string error;
    for (bool cuda : {false, true}) {
        if (cuda && !gpu)
            continue;
        int reference = 0;
        for (auto precision : {E::Precision::Float32, E::Precision::Float64})
            for (bool q : {false, true}) {
                E::Parameters p;
                SeisWave::Ui::applyInitialModel2D(p);
                check(p.nx == 500 && p.nz == 200 && p.spacing == 2 && p.frequency == 30 && p.sourceZ == 0 &&
                          p.sourceX == -1 && p.layers.size() == 3,
                      "Interactive default model mismatch");
                p.nx = 129;
                p.nz = 97;
                p.duration = .16;
                p.relief = 4;
                p.wavelength = 180;
                p.precision = precision;
                p.attenuation.enabled = q;
                p.externalAbsorption = true;
                p.absorber = E::Absorber::CfsPml;
                p.spongeX = p.spongeZ = 8;
                p.autoSurfaceReceivers = true;
                p.receiverTargetCount = 25;
                E::Simulation s;
                check(s.initialize(p, error, cuda ? E::BackendKind3D::Cuda : E::BackendKind3D::Cpu, {1, -1}),
                      error);
                check(s.parameters().sourceZ == 0 && std::abs(s.diagnostics().sourceWeightSum - 1) < 1e-5,
                      "Truncated surface-force kernel is not normalized");
                check(s.advance(s.totalSteps(), error), error);
                check(s.diagnostics().finite && s.diagnostics().maxVelocity > 0 &&
                          s.diagnostics().tractionRelative <
                              (precision == E::Precision::Float32 ? 1e-4 : 1e-10),
                      "Surface source lost finite propagation / traction-free projection");
                bool signal = false;
                for (const auto &r : s.receivers())
                    for (double value : r.vz)
                        signal |= value != 0;
                check(signal, "Surface force produced no receiver signal");
                const auto frame = s.snapshot().fields[1];
                double relative = 0;
                if (!cuda)
                    references[reference] = frame;
                else {
                    double norm = 0, diff = 0;
                    for (std::size_t i = 0; i < frame.size(); ++i) {
                        norm += references[reference][i] * references[reference][i];
                        const double d = frame[i] - references[reference][i];
                        diff += d * d;
                    }
                    relative = std::sqrt(diff / std::max(norm, 1e-100));
                    check(relative < 2e-4, "CPU/CUDA surface-force waveform mismatch");
                }
                ++reference;
                const auto file = folder / "surface-force-model.vts";
                E::ModelDocument loaded;
                check(E::writeModelVts(s, file, {1, -1}, error) && E::readModelVts(file, loaded, error) &&
                          loaded.parameters.sourceZ == 0,
                      error);
                auto bad = p;
                bad.sourceZ = -1;
                const auto step = s.step();
                check(!s.initialize(bad, error) && s.step() == step,
                      "Negative surface source accepted / changed active session");
                bad = p;
                bad.source.kind = SeisWave::SourceKind::IsotropicMoment;
                check(!s.initialize(bad, error),
                      "Surface moment source accepted without an interior stencil");
                report << (cuda ? "CUDA" : "CPU") << " / "
                       << (precision == E::Precision::Float32 ? "FP32" : "FP64") << " / "
                       << (q ? "GSLS" : "elastic")
                       << ": PASS row-0 force, normalized inward kernel, finite wavefield, receiver signal, "
                          "traction, VTS; CUDA relative error "
                       << relative << '\n';
            }
    }
    report << "PASS: free-surface-centered regularized directional body force; not a prescribed traction "
              "boundary source\n";
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
        validateSurfaceForce(folder, gpu);
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
