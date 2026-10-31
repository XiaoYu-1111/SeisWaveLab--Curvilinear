#include "Seis2D/AcousticSolver2D.hpp"
#include <algorithm>
#include <cmath>
#include <tuple>
namespace Seis2D::Acoustic {
double effectiveRamBudgetMiB(const Options &o, const Seis3D::Curvilinear::BackendMemory3D &memory) {
    if (o.memoryMiB != 0)
        return o.memoryMiB;
    return memory.hostAvailableBytes ? std::min(32768., .7 * double(memory.hostAvailableBytes) / 1048576.)
                                     : 1536.;
}
Capacity estimateCapacity(const Options &o, const Seis3D::Curvilinear::BackendMemory3D &memory) {
    Capacity result;
    result.freeRamMiB = memory.hostAvailableBytes / 1048576.;
    result.freeDeviceMiB = memory.deviceAvailableBytes / 1048576.;
    if (!std::isfinite(o.memoryMiB) || (o.memoryMiB != 0 && (o.memoryMiB < 16 || o.memoryMiB > 32768)) ||
        o.absorberCells < 8 || o.absorberCells > 128 ||
        (o.propagator != Propagator::P1 && o.propagator != Propagator::CurvilinearDrp))
        return result;
    result.ramBudgetMiB = effectiveRamBudgetMiB(o, memory);
    const auto &p = o.model;
    const double real = p.precision == Precision::Float32 ? 4 : 8, pad = o.absorberCells;
    const double samples = p.sampleIntervalMs > 0
                               ? std::floor(std::max(0., p.duration) / (p.sampleIntervalMs * .001)) + 1
                               : std::clamp(p.maxSteps > 0 ? p.maxSteps : 200000, 1, 200000) + 1.;
    const double records = samples * std::max(1, p.receiverCount) * sizeof(double);
    const double ramLimit = result.ramBudgetMiB * 1048576.,
                 free = memory.hostAvailableBytes ? double(memory.hostAvailableBytes) : 1e100;
    const auto costs = [&](int nx, int nz) {
        const double count = (nx + 2 * pad) * (nz + pad);
        const double triangles = 2 * ((nx + 2 * pad - 1) * (nz + pad - 1) - double(nx - 1) * (nz - 1));
        const double nodes = count - double(nx - 2) * (nz - 1);
        const double cpml = o.damping && o.absorber == AbsorbingBoundary::Cpml
                                ? triangles * (140 + 25 * real) + nodes * (48 + 12 * real)
                                : 0;
        const bool drp = o.propagator == Propagator::CurvilinearDrp;
        const double axisBytes = 2 * (nx + 2 * pad + nz + pad) * (real == 4 ? 256 : 384);
        const double build =
            drp ? count * (real == 4 ? 288 : 448) + axisBytes : count * (real == 4 ? 256 : 384) + cpml;
        const double host = build + records;
        const double device = (drp ? count * 35 * real + axisBytes : count * (16 * real + 8) + cpml) * 1.25;
        Capacity::Limit limit = Capacity::Limit::None;
        if (nx < 16 || nz < 16 || nx > 16384 || nz > 16384)
            limit = Capacity::Limit::Axis;
        else if (o.backend == BackendKind3D::Cuda && !memory.deviceAvailableBytes)
            limit = Capacity::Limit::DeviceUnavailable;
        else if (o.memoryMiB > 0 && (build >= ramLimit || host >= ramLimit))
            limit = Capacity::Limit::ManualRam;
        else if (build >= std::min(ramLimit, .7 * free) || host >= std::min(ramLimit, .75 * free))
            limit = Capacity::Limit::SystemRam;
        else if (o.backend == BackendKind3D::Cuda && device >= memory.deviceAvailableBytes)
            limit = Capacity::Limit::DeviceMemory;
        return std::tuple{limit == Capacity::Limit::None, host, device, count, limit};
    };
    if (p.nx >= 16 && p.nz >= 16 && p.nx <= 16384 && p.nz <= 16384) {
        const auto [fits, host, device, count, limit] = costs(p.nx, p.nz);
        result.limit = limit;
        result.withinBudget = fits;
        result.estimatedRamMiB = host / 1048576.;
        result.estimatedDeviceMiB = o.backend == BackendKind3D::Cuda ? device / 1048576. : 0;
        result.computeNodes = std::size_t(count);
    } else
        result.limit = Capacity::Limit::Axis;
    const auto maximum = [&](auto predicate) {
        if (!predicate(16))
            return 0;
        int low = 16, high = 16384;
        while (low < high) {
            const int mid = low + (high - low + 1) / 2;
            if (predicate(mid))
                low = mid;
            else
                high = mid - 1;
        }
        return low;
    };
    result.maxNxGivenNz =
        p.nz >= 16 && p.nz <= 16384 ? maximum([&](int n) { return std::get<0>(costs(n, p.nz)); }) : 0;
    result.maxNzGivenNx =
        p.nx >= 16 && p.nx <= 16384 ? maximum([&](int n) { return std::get<0>(costs(p.nx, n)); }) : 0;
    const double ratio = double(std::max(16, p.nz)) / std::max(16, p.nx);
    const auto nzFor = [&](int nx) {
        const double desired = std::round(nx * ratio);
        return desired > 16384 ? 16385 : std::max(16, int(desired));
    };
    result.aspectNx = maximum([&](int n) { return std::get<0>(costs(n, nzFor(n))); });
    result.aspectNz = result.aspectNx ? nzFor(result.aspectNx) : 0;
    result.aspectNodes = std::size_t(result.aspectNx) * result.aspectNz;
    return result;
}
} // namespace Seis2D::Acoustic
