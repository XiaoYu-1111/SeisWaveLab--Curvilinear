#include "CfsPml2D.hpp"
#include "Seis2D/AsyncSimulation2D.hpp"
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

using namespace Seis2D::Curvilinear;
namespace {
void check(bool value, const std::string &message) {
    if (!value)
        throw std::runtime_error(message);
}
void same(const std::vector<float> &a, const std::vector<float> &b, double tolerance) {
    check(a.size() == b.size(), "Display shape mismatch");
    double peak = 1e-30, delta = 0;
    for (std::size_t q = 0; q < a.size(); ++q) {
        check(std::isfinite(a[q]) && std::isfinite(b[q]), "Nonfinite display value");
        peak = std::max(peak, std::abs(double(a[q])));
        delta = std::max(delta, std::abs(double(a[q]) - b[q]));
    }
    check(delta <= tolerance * peak, "CPU/CUDA or cached frame mismatch");
}
Parameters fixture(Precision precision, Absorber absorber, bool q) {
    Parameters p;
    p.nx = 64;
    p.nz = 56;
    p.spacing = 2;
    p.vp = 550;
    p.vs = 250;
    p.density = 1550;
    p.relief = 1;
    p.wavelength = 80;
    p.flattenDepth = 60;
    p.frequency = 20;
    p.duration = .18;
    p.maxSteps = 0;
    p.cfl = .5;
    p.sourceX = 8;
    p.sourceZ = 15;
    p.receiverStart = 8;
    p.receiverCount = 8;
    p.receiverStride = 6;
    p.receiverDepth = 2;
    p.spongeX = p.spongeZ = 12;
    p.externalAbsorption = true;
    p.absorber = absorber;
    p.precision = precision;
    p.attenuation.enabled = q;
    p.attenuation.referenceFrequency = 20;
    p.attenuation.maxFrequency = 80;
    p.forcePerLength = 100;
    return p;
}
std::vector<float> verifyFrame(const Simulation &s, int component) {
    std::string error;
    std::vector<float> crop, full;
    check(s.readFrame(component, crop, error) && s.readFrame(component, full, error, true), error);
    const auto &p = s.parameters();
    const auto domain = Detail::domainLayout(p);
    check(crop.size() == std::size_t(p.nx) * p.nz && full.size() == std::size_t(domain.nx) * domain.nz,
          "Target/full display dimensions wrong");
    double outside = 0;
    for (int k = 0; k < domain.nz; ++k)
        for (int i = 0; i < domain.nx; ++i) {
            const auto q = std::size_t(k) * domain.nx + i;
            check(std::isfinite(full[q]), "Nonfinite absorber frame");
            if (i >= domain.offsetX && i < p.nx + domain.offsetX && k < p.nz)
                check(full[q] == crop[std::size_t(k) * p.nx + i - domain.offsetX],
                      "Target crop or full-domain orientation changed");
            else
                outside = std::max(outside, std::abs(double(full[q])));
        }
    check(outside > 0, "Exterior frame was padded with zeros");
    return full;
}
void asynchronous(const Parameters &p, BackendKind3D backend) {
    ComputeSettings3D settings;
    settings.cpuThreads = 1;
    AsyncSimulation live;
    Simulation reference;
    std::string error;
    check(live.initialize(p, error, backend, settings) && reference.initialize(p, error, backend, settings),
          error);
    std::vector<float> image;
    check(live.readFrame(0, image, error) && image.size() == std::size_t(p.nx) * p.nz, error);
    const auto domain = Detail::domainLayout(p);
    check(live.readFrame(0, image, error, true) && image.size() == std::size_t(domain.nx) * domain.nz,
          "Same-step cache returned target data for full frame");
    for (int batch = 0; batch < 3; ++batch) {
        check(live.queue(8, 1000), "Display worker failed to queue");
        // Change the request while the worker owns its previous request snapshot.
        live.readFrame(3, image, error, batch % 2 == 0);
        live.synchronize();
        check(live.poll(error), error);
    }
    check(reference.advance(live.step(), error), error);
    for (bool full : {false, true}) {
        std::vector<float> expected;
        check(live.readFrame(3, image, error, full) && reference.readFrame(3, expected, error, full), error);
        same(image, expected, 0);
    }
    const auto actual = live.snapshot(true), expected = reference.snapshot(true);
    check(actual.fields == expected.fields && actual.syy == expected.syy &&
              actual.memory == expected.memory && actual.boundaryMemory == expected.boundaryMemory,
          "Background display request changed solver state");
}
} // namespace
int main() {
    try {
        const auto folder =
            std::filesystem::path(SEIS3D_OUTPUT_ROOT) / "validation/absorber-display-2026-10-31";
        std::filesystem::create_directories(folder);
        std::ofstream log(folder / "native-validation.txt");
        const auto gpu = Seis3D::Curvilinear::queryBackend3D(BackendKind3D::Cuda);
        ComputeSettings3D settings;
        settings.cpuThreads = 1;
        for (auto precision : {Precision::Float64, Precision::Float32}) {
            for (auto absorber : {Absorber::Sponge, Absorber::CfsPml})
                for (bool q : {false, true}) {
                    const auto p = fixture(precision, absorber, q);
                    Simulation cpu, cuda;
                    std::string error;
                    check(cpu.initialize(p, error, BackendKind3D::Cpu, settings) &&
                              cpu.advance(cpu.totalSteps(), error),
                          error);
                    const auto before = cpu.snapshot(true);
                    if (gpu.available)
                        check(cuda.initialize(p, error, BackendKind3D::Cuda, settings) &&
                                  cuda.advance(cuda.totalSteps(), error),
                              error);
                    for (int component = 0; component < 4; ++component) {
                        const auto full = verifyFrame(cpu, component);
                        if (gpu.available)
                            same(full, verifyFrame(cuda, component),
                                 precision == Precision::Float32 ? 5e-4 : 1e-6);
                    }
                    const auto after = cpu.snapshot(true);
                    check(before.fields == after.fields && before.syy == after.syy &&
                              before.memory == after.memory && before.boundaryMemory == after.boundaryMemory,
                          "Display readback changed state");
                    log << "PASS: " << (precision == Precision::Float32 ? "FP32" : "FP64")
                        << " absorber=" << int(absorber) << " GSLS=" << q
                        << " Vz/Vx/speed/pressure exact target crop; nonzero exterior; CUDA=" << gpu.available
                        << '\n';
                }
            const auto p = fixture(precision, Absorber::CfsPml, true);
            asynchronous(p, BackendKind3D::Cpu);
            if (gpu.available)
                asynchronous(p, BackendKind3D::Cuda);
        }
        log << "PASS: asynchronous full/target/component cache requests and raw state invariance\n";
        std::cout << "Absorber full-domain display validation PASS\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
