#include "ModelTransfer2D.hpp"
#include "Seis2D/AcousticSolver2D.hpp"
#include "Seis2D/Imaging2D.hpp"
#include <fstream>
#include <iostream>
#include <stdexcept>
using namespace SeisWave::Ui;
void require(bool value, const char *message) {
    if (!value)
        throw std::runtime_error(message);
}
int main() {
    try {
        ModelMailbox2D mailbox;
        ModelTransfer2D source;
        source.model.surfaceSamples = {{0, 10}, {100, 30}};
        source.model.layers = {{50, 1800, 900, 1800}, {0, 2300, 1200, 2000}};
        source.compute.cpuThreads = 1;
        mailbox.post(Module2D::AcousticForward, source);
        mailbox.post(Module2D::AcousticRtm, source);
        source.model.surfaceSamples[0][1] = 999;
        source.model.layers[0].vp = 999;
        auto forward = mailbox.take(Module2D::AcousticForward);
        require(forward && forward->model.surfaceSamples[0][1] == 10 && forward->model.layers[0].vp == 1800,
                "Model transfer aliases the source");
        forward->model.layers[0].vp = 1200;
        require(!mailbox.pending(Module2D::AcousticForward) && mailbox.pending(Module2D::AcousticRtm) &&
                    !mailbox.pending(Module2D::Elastic),
                "Taking one inbox changed another module");
        auto rtm = mailbox.take(Module2D::AcousticRtm);
        require(rtm && rtm->model.layers[0].vp == 1800, "Model copies share mutable material storage");
        auto invalid = *rtm;
        invalid.model.vs = 0;
        std::string error;
        require(!prepareElasticTransfer(invalid, Module2D::Elastic, error) && invalid.model.vs == 0,
                "Invalid elastic transfer was mutated or accepted");
        auto p = Seis2D::Acoustic::defaultOptions();
        p.model.nx = 41;
        p.model.nz = 35;
        p.model.sourceX = 20;
        p.model.sourceZ = 3;
        p.model.receiverStart = 1;
        p.model.receiverCount = 20;
        p.model.receiverStride = 2;
        p.model.duration = .25;
        p.compute.cpuThreads = 1;
        Seis2D::Acoustic::Simulation a, b, reference;
        require(a.initialize(p, error) && b.initialize(p, error), error.c_str());
        require(a.advance(5, error) && b.advance(3, error), error.c_str());
        auto bBefore = b.pressureSnapshot();
        auto alternate = p;
        alternate.model.frequency = 20;
        alternate.model.precision = Seis2D::Curvilinear::Precision::Float64;
        require(a.initialize(alternate, error) && a.advance(7, error), error.c_str());
        require(b.pressureSnapshot() == bBefore && b.step() == 3,
                "Rebuilding one acoustic solver changed another");
        Seis2D::Curvilinear::Simulation elastic;
        auto ep = p.model;
        ep.receiverCount = 20;
        require(elastic.initialize(ep, error, Seis2D::Curvilinear::BackendKind3D::Cpu, p.compute) &&
                    elastic.advance(5, error),
                error.c_str());
        auto elasticBefore = elastic.snapshot(true);
        auto imaging = Seis2D::Imaging::benchmarkOptions();
        imaging.model = p.model;
        imaging.model.layers = {{60, 1800, 900, 1800}, {0, 2300, 1200, 1800}};
        imaging.shots = 1;
        imaging.firstShot = 15;
        imaging.lastShot = 25;
        imaging.absorberCells = 8;
        imaging.compute.cpuThreads = 1;
        Seis2D::Imaging::Result image;
        require(Seis2D::Imaging::run(imaging, image, error), error.c_str());
        auto imageBefore = image.image;
        require(b.advance(5, error) && reference.initialize(p, error) && reference.advance(8, error),
                error.c_str());
        require(b.pressureSnapshot() == reference.pressureSnapshot(),
                "Another module changed acoustic continuation");
        require(elastic.snapshot(true).fields == elasticBefore.fields && elastic.step() == 5,
                "RTM/acoustic calculation changed elastic state");
        require(a.reset(error) && image.image == imageBefore, "Acoustic reset changed RTM result");
        auto folder = std::filesystem::path(SEIS3D_OUTPUT_ROOT) / "validation/module-isolation";
        std::filesystem::create_directories(folder);
        std::ofstream log(folder / "validation.txt");
        log << "PASS: deep-copy mailboxes / one-shot destination / explicit-only transfer / independent "
               "acoustic states / independent elastic state / immutable RTM result\n";
        std::cout << "Module isolation PASS\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
