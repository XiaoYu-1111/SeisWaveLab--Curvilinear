#pragma once
#include "Seis2D/AcousticSolver2D.hpp"
namespace Seis2D::Acoustic {
struct Simulation::Impl {
    Options options;
    Grid grid;
    Diagnostics diagnostics;
    BackendInfo3D backend;
    std::vector<Receiver> receivers;
    std::vector<double> previousSamples;
    int step{}, total{};
    double dt{}, recordDt{};
    virtual ~Impl() = default;
    virtual void advance(int) = 0;
    virtual void reset() = 0;
    virtual std::vector<double> snapshot(bool includeAbsorber) const = 0;
};
std::unique_ptr<Simulation::Impl> makeDrpForward(const Options &);
} // namespace Seis2D::Acoustic
