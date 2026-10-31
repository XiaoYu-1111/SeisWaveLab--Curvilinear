#pragma once
#include "CurvilinearSolver2D.hpp"
#include <algorithm>
#include <cmath>
namespace Seis2D::Curvilinear {
// Uniform horizontal grid spacing, following the mapped terrain row. Integer-node
// quantization may adjust the requested count to cover the available surface span.
inline bool setUniformSurfaceReceivers(Parameters &p, bool acoustic, int receiverLimit, int sideMargin = 0) {
    if (p.nx < 2 || p.nx > 16384 || p.nz < 2 || receiverLimit < 1)
        return false;
    const int margin = std::clamp(sideMargin, 0, (p.nx - 2) / 2), span = p.nx - 1 - 2 * margin;
    const int limit = std::min(receiverLimit, span + 1), wanted = std::clamp(p.receiverCount, 1, limit);
    int count = 1, stride = 1, start = margin + span / 2;
    if (wanted > 1) {
        const int minimumStride = std::max(1, (span + limit - 2) / (limit - 1));
        stride = std::max(minimumStride, std::max(1, int(std::lround(double(span) / (wanted - 1)))));
        count = span / stride + 1;
        start = margin + (span - (count - 1) * stride) / 2;
    }
    p.receiverStart = start;
    p.receiverCount = count;
    p.receiverStride = stride;
    p.receiverDepth = acoustic ? 1 : 0;
    return true;
}
// Idempotent automatic policy: never feed the quantized count back as the target.
inline bool synchronizeSurfaceReceivers(Parameters &p, bool acoustic, int limit, int margin = 0) {
    if (!p.autoSurfaceReceivers)
        return true;
    if (p.nx < 2 || p.nx > maxAxisNodes || p.nz < 2 || p.nz > maxAxisNodes || p.receiverTargetCount < 0 ||
        p.receiverTargetCount > limit)
        return false;
    const int target = p.receiverTargetCount > 0 ? p.receiverTargetCount : p.receiverCount;
    if (target < 1 || target > limit)
        return false;
    const int oldCount = p.receiverCount;
    p.receiverCount = target;
    if (!setUniformSurfaceReceivers(p, acoustic, limit, margin)) {
        p.receiverCount = oldCount;
        return false;
    }
    p.receiverTargetCount = target;
    return true;
}
} // namespace Seis2D::Curvilinear
