#pragma once
#include "MenuWidgets.hpp"
#include "ModelTransfer2D.hpp"
#include "Presentation3D.hpp"
namespace SeisWave::Ui {
inline const char *moduleName2D(Module2D m) {
    using Seis3D::Curvilinear::Ui::language3D;
    bool zh = language3D() != 0;
    switch (m) {
    case Module2D::Elastic:
        return zh ? "弹性正演" : "Elastic forward";
    case Module2D::Viscoelastic:
        return zh ? "黏弹性正演" : "Viscoelastic forward";
    case Module2D::AcousticForward:
        return zh ? "声学正演" : "Acoustic forward";
    case Module2D::AcousticRtm:
        return zh ? "声学 RTM 与照明" : "Acoustic RTM / illumination";
    }
    return "2D";
}
struct ModuleNavigation2D {
    Module2D current{Module2D::Elastic};
    int requested{};
    bool copied{};
    ImRect rootRect, acousticRect, rtmRect;
    void draw(const Seis2D::Curvilinear::Parameters &model,
              const Seis2D::Curvilinear::ComputeSettings3D &compute,
              Seis2D::Curvilinear::BackendKind3D backend, std::string &error) {
        using namespace Seis3D::Curvilinear::Ui;
        const bool zh = language3D() != 0;
        auto root = std::string(zh ? "模块：" : "Module: ") + moduleName2D(current) + "###ModuleSelect2D";
        bool open = beginDropdownMenu(root.c_str());
        rootRect = {ImGui::GetItemRectMin(), ImGui::GetItemRectMax()};
        if (open) {
            bool acoustic = beginDropdownMenu(zh ? "声学 / P" : "Acoustics / P");
            acousticRect = {ImGui::GetItemRectMin(), ImGui::GetItemRectMax()};
            if (acoustic) {
                if (ImGui::MenuItem(moduleName2D(Module2D::AcousticForward), nullptr,
                                    current == Module2D::AcousticForward) &&
                    current != Module2D::AcousticForward)
                    requested = routeAcoustic2D;
                if (ImGui::MenuItem(moduleName2D(Module2D::AcousticRtm), nullptr,
                                    current == Module2D::AcousticRtm) &&
                    current != Module2D::AcousticRtm)
                    requested = routeRtm2D;
                rtmRect = {ImGui::GetItemRectMin(), ImGui::GetItemRectMax()};
                ImGui::EndMenu();
            }
            if (ImGui::MenuItem(moduleName2D(Module2D::Elastic), nullptr, current == Module2D::Elastic) &&
                current != Module2D::Elastic)
                requested = routeElastic2D;
            if (ImGui::MenuItem(moduleName2D(Module2D::Viscoelastic), nullptr,
                                current == Module2D::Viscoelastic) &&
                current != Module2D::Viscoelastic)
                requested = routeViscoelastic2D;
            ImGui::Separator();
            ImGui::TextDisabled("%s",
                                zh ? "切换使用各模块自己的参数" : "Each module keeps its own parameters");
            ImGui::EndMenu();
        }
        if (beginDropdownMenu(zh ? "复制模型到" : "Copy model to")) {
            for (Module2D target : {Module2D::AcousticForward, Module2D::AcousticRtm, Module2D::Elastic,
                                    Module2D::Viscoelastic}) {
                if (ImGui::MenuItem(moduleName2D(target), nullptr, false, target != current)) {
                    ModelTransfer2D transfer{current, model, compute, backend};
                    bool okay = true;
                    if (target == Module2D::Elastic || target == Module2D::Viscoelastic)
                        okay = prepareElasticTransfer(transfer, target, error);
                    else
                        transfer.model.attenuation.enabled = false;
                    if (okay) {
                        modelMailbox2D().post(target, transfer);
                        requested = moduleRoute(target);
                        copied = true;
                    }
                }
            }
            ImGui::Separator();
            ImGui::TextDisabled("%s",
                                zh ? "复制待应用模型/设备；波场与记录独立"
                                   : "Copy pending model/device inputs; fields and records stay separate");
            ImGui::EndMenu();
        }
    }
};
} // namespace SeisWave::Ui
