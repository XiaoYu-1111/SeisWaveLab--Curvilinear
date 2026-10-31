#pragma once
#include "Seis2D/CurvilinearSolver2D.hpp"
#include <cmath>
#include "CfsPml2D.hpp"

namespace Seis2D::Curvilinear::Detail {
struct MemoryBudget {
    double hostBytes{}, deviceBytes{};
};
// Additional storage for a candidate session, compared against currently free
// memory: the active session is already excluded by the OS/CUDA memory query.
inline MemoryBudget memoryBudget(const Parameters &p, BackendKind3D kind) {
    const auto domain=domainLayout(p);
    const double count = static_cast<double>(domain.nx + 2 * ghost) * (domain.nz + 2 * ghost);
    const bool cuda = kind == BackendKind3D::Cuda;
    const double scalar=static_cast<double>(precisionBytes(p.precision));
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
    const double pmlState=7.*cpmlScalars(p)*scalar;
    const double pmlProfiles=usesCfsPml(p)?3.*(domain.nx+domain.nz)*scalar:0;
    // Geometry/material construction uses FP64 temporarily, then releases each
    // double array while converting. Include that peak even for a FP32 GPU session.
    const double hostPerPoint=std::max(((cuda?18.:60.)+gradients+qExtra+(cuda?0:stateExtra))*scalar,
                                    (14.+gradients+qExtra)*sizeof(double)+2*scalar);
    return {pmlProfiles+(cuda?0:pmlState)+count*hostPerPoint+records,
            cuda ? pmlState+pmlProfiles+count*(48.+gradients+(p.attenuation.enabled?2.0*p.attenuation.mechanisms:0)+stateExtra)*scalar : 0};
}
} // namespace Seis2D::Curvilinear::Detail
