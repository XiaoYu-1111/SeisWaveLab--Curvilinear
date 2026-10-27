#pragma once
#include "Seis2D/CurvilinearSolver2D.hpp"
#include <cmath>

namespace Seis2D::Curvilinear::Detail {
struct MemoryBudget {
    double hostBytes{}, deviceBytes{};
};
// Additional storage for a candidate session, compared against currently free
// memory: the active session is already excluded by the OS/CUDA memory query.
inline MemoryBudget memoryBudget(const Parameters &p, BackendKind3D kind) {
    const double count = static_cast<double>(p.nx + 2 * ghost) * (p.nz + 2 * ghost);
    const bool cuda = kind == BackendKind3D::Cuda;
    const double gradients = p.source.kind == SeisWave::SourceKind::IsotropicMoment ? 2 : 0;
    const double samples = p.sampleIntervalMs > 0
                               ? std::floor(p.duration / (p.sampleIntervalMs * .001)) + 1
                               : (p.maxSteps ? p.maxSteps + 1 : maxTimeSteps + 1);
    // Before geometry/dt are available, per-step output must reserve against
    // the accepted-step ceiling, including full-duration (maxSteps == 0) runs.
    const double records = p.receiverCount * samples * 2 * sizeof(double);
    const double qExtra=p.attenuation.enabled?4.0+2.0*p.attenuation.mechanisms:0; // Qp/Qs, reference moduli and branch strengths.
    const double stateExtra=p.attenuation.enabled?7.0*(1+4*p.attenuation.mechanisms):0;
    // CUDA keeps geometry/material/source arrays on the host (14 + gradients
    // doubles/node), but all 35 RK field arrays live only on the device. Allow
    // four extra host doubles/node for staging, display and allocation overhead.
    // CPU retains the previous conservative 60 + gradients budget.
    return {count * ((cuda ? 18. : 60.) + gradients+qExtra+(cuda?0:stateExtra)) * sizeof(double) + records,
            cuda ? count * (48. + gradients+(p.attenuation.enabled?2.0*p.attenuation.mechanisms:0)+stateExtra) * sizeof(double) : 0};
}
} // namespace Seis2D::Curvilinear::Detail
