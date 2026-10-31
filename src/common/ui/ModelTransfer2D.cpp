#include "ModelTransfer2D.hpp"
#include "Seis2D/AcousticSolver2D.hpp"
#include "Seis2D/ModelIo2D.hpp"
#include <algorithm>
#include <cmath>
namespace SeisWave::Ui {
ModelMailbox2D &modelMailbox2D() {
    static ModelMailbox2D value;
    return value;
}
bool loadInputModel2D(const std::filesystem::path &path, ModelTransfer2D &output, bool &acoustic,
                      std::string &error) {
    Seis2D::Acoustic::Options a;
    std::string acousticError;
    if (Seis2D::Acoustic::readModelVts(path, a, acousticError)) {
        output = {Module2D::AcousticForward, std::move(a.model), a.compute, a.backend};
        acoustic = true;
        error.clear();
        return true;
    }
    Seis2D::Curvilinear::ModelDocument e;
    if (Seis2D::Curvilinear::readModelVts(path, e, error)) {
        output = {e.parameters.attenuation.enabled ? Module2D::Viscoelastic : Module2D::Elastic,
                  std::move(e.parameters), e.compute, e.backend};
        acoustic = false;
        return true;
    }
    error = acousticError + " / " + error;
    return false;
}
bool prepareElasticTransfer(ModelTransfer2D &input, Module2D target, std::string &error) {
    auto copy = input;
    auto &p = copy.model;
    if (p.nx < 16 || p.nz < 16 || p.nx > 16384 || p.nz > 16384) {
        error = "Elastic target dimensions must be 16..16384.";
        return false;
    }
    auto valid = [](double vp, double vs, double rho) {
        return std::isfinite(vp) && std::isfinite(vs) && std::isfinite(rho) && vp > 0 && vs > 0 && rho > 0 &&
               vp * vp > 4 * vs * vs / 3;
    };
    if (!valid(p.vp, p.vs, p.density)) {
        error = "Elastic transfer requires positive Vs and Vp^2 > 4 Vs^2 / 3; edit Vs before copying.";
        return false;
    }
    if (p.layered)
        for (const auto &l : p.layers)
            if (!valid(l.vp, l.vs, l.density)) {
                error = "An elastic layer requires positive Vs and Vp^2 > 4 Vs^2 / 3; edit layer Vs before "
                        "copying.";
                return false;
            }
    const int margin = p.source.kind == SeisWave::SourceKind::IsotropicMoment ? 5 : 2;
    p.sourceX = std::clamp(p.sourceX < 0 ? p.nx / 2 : p.sourceX, margin, p.nx - margin - 1);
    p.sourceZ = std::clamp(p.sourceZ, margin, p.nz - margin - 1);
    p.receiverStart = std::clamp(p.receiverStart, 0, p.nx - 1);
    p.receiverStride = std::max(1, p.receiverStride);
    p.receiverDepth = std::clamp(p.receiverDepth, 0, p.nz - 1);
    p.receiverCount =
        std::clamp(p.receiverCount, 1, std::min(1024, 1 + (p.nx - 1 - p.receiverStart) / p.receiverStride));
    p.receiverTargetCount=std::clamp(p.receiverTargetCount,0,1024);
    p.attenuation.enabled = target == Module2D::Viscoelastic;
    if (!SeisWave::Rheology::validateSettings(p.attenuation, error))
        return false;
    input = std::move(copy);
    error.clear();
    return true;
}
} // namespace SeisWave::Ui
