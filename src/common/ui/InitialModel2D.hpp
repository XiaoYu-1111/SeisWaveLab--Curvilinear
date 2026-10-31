#pragma once
#include "Seis2D/CurvilinearSolver2D.hpp"

namespace SeisWave::Ui {
// Interactive example only. Numerical API defaults and saved experiments stay unchanged.
inline void applyInitialModel2D(Seis2D::Curvilinear::Parameters &model, bool acoustic = false) {
    model.nx = 500;
    model.nz = 200;
    model.spacing = 2;
    model.frequency = 30;
    model.sourceX = -1;
    model.sourceZ = acoustic ? 1 : 0;
    model.maxSteps = 0;
    const double depth = (model.nz - 1) * model.spacing;
    model.relief = 0;
    model.layered = true;
    model.layers = {{.30 * depth, 1800, 900, 1900}, {.35 * depth, 2600, 1400, 2200}, {0, 3600, 2050, 2500}};
    model.vp = model.layers.front().vp;
    model.vs = model.layers.front().vs;
    model.density = model.layers.front().density;
}
} // namespace SeisWave::Ui
