#pragma once
#include "Seis2D/CurvilinearSolver2D.hpp"
#include <array>
#include <optional>
#include <string>
namespace SeisWave::Ui {
enum class Module2D { Elastic, Viscoelastic, AcousticForward, AcousticRtm };
inline constexpr int routeElastic2D = 102, routeAcoustic2D = 103, routeRtm2D = 101, routeViscoelastic2D = 104;
inline int moduleRoute(Module2D module) {
    switch (module) {
    case Module2D::Elastic:
        return routeElastic2D;
    case Module2D::Viscoelastic:
        return routeViscoelastic2D;
    case Module2D::AcousticForward:
        return routeAcoustic2D;
    case Module2D::AcousticRtm:
        return routeRtm2D;
    }
    return routeElastic2D;
}
struct ModelTransfer2D {
    Module2D source{Module2D::Elastic};
    Seis2D::Curvilinear::Parameters model;
    Seis2D::Curvilinear::ComputeSettings3D compute;
    Seis2D::Curvilinear::BackendKind3D backend{Seis2D::Curvilinear::BackendKind3D::Cpu};
};
// Value-only, one-shot mailboxes. Never stores a solver, worker, trace or GPU pointer.
class ModelMailbox2D {
    std::array<std::optional<ModelTransfer2D>, 4> pending_;

  public:
    void post(Module2D target, const ModelTransfer2D &value) { pending_[static_cast<int>(target)] = value; }
    std::optional<ModelTransfer2D> take(Module2D target) {
        auto &slot = pending_[static_cast<int>(target)];
        auto value = std::move(slot);
        slot.reset();
        return value;
    }
    bool pending(Module2D target) const { return pending_[static_cast<int>(target)].has_value(); }
};
ModelMailbox2D &modelMailbox2D();
// Pure parameter adaptation. No foreign solver allocation or propagation during navigation.
bool prepareElasticTransfer(ModelTransfer2D &, Module2D, std::string &);
bool loadInputModel2D(const std::filesystem::path &, ModelTransfer2D &, bool &acoustic, std::string &);
} // namespace SeisWave::Ui
