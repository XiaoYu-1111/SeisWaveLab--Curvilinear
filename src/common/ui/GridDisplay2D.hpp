#pragma once
#include <algorithm>
#include <cmath>

namespace SeisWave::Ui {
// Native node lines at a nominal 2 m interval. Larger grids / distant zooms
// decimate only the overlay, with the same logical stride in both directions.
inline int displayGridStride2D(int nx, int nz, double spacing, double pixelsPerCell) {
    const int base = std::max(1, static_cast<int>(std::ceil(2. / spacing)));
    const int budget = static_cast<int>(std::ceil(std::sqrt(double(nx - 1) * (nz - 1) / 200000.)));
    const int pixels = static_cast<int>(std::ceil(.75 / std::max(.001, pixelsPerCell)));
    const int stride = std::max({base, budget, pixels});
    return std::clamp(((stride + base - 1) / base) * base, 1, std::max(1, std::min(nx - 1, nz - 1)));
}
} // namespace SeisWave::Ui
