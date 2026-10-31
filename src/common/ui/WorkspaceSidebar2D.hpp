#pragma once
#include "ParameterPanel2D.hpp"
#include "Presentation3D.hpp"
#include "WorkspaceTools.hpp"
#include <algorithm>
#include <array>
namespace SeisWave::Ui {
// Keep captions above full-width selectors, including the native down arrow.
inline bool choiceField2D(const char *label, int *value, const char *items) {
    ImGui::TextUnformatted(label);
    ImGui::SetNextItemWidth(-1);
    const auto id = std::string("##") + label;
    return ImGui::Combo(id.c_str(), value, items);
}
// Preserve the current dock tree and detached windows while scaling split proportions.
inline void resizeDockTree2D(ImGuiDockNode *node, ImVec2 scale) {
    if (!node)
        return;
    node->SizeRef = {node->SizeRef.x * scale.x, node->SizeRef.y * scale.y};
    for (auto *child : node->ChildNodes)
        resizeDockTree2D(child, scale);
}
inline void resizeDockSpace2D(ImGuiID id, ImVec2 &previous, ImVec2 size) {
    if (previous.x > 0 && previous.y > 0 && (previous.x != size.x || previous.y != size.y))
        resizeDockTree2D(ImGui::DockBuilderGetNode(id), {size.x / previous.x, size.y / previous.y});
    previous = size;
}
// Presentation state belongs to one workspace instance, never the 2D/3D global panel preference.
struct WorkspaceSidebar2D {
    int category{};
    bool floating{}, center{true};
    WorkspaceTools views;
    void navigation() { parameterTabs2D(category); }
    void prepare(const char *title) {
        if (!floating)
            return;
        const auto *vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowDockID(0, ImGuiCond_Always);
        if (center) {
            ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Always, {.5f, .5f});
            ImGui::SetNextWindowSize(
                {std::min(720.f, vp->WorkSize.x * .85f), std::min(850.f, vp->WorkSize.y * .85f)},
                ImGuiCond_Always);
            center = false;
        }
        (void)title;
    }
    void panelMenu() {
        using namespace Seis3D::Curvilinear::Ui;
        if (ImGui::MenuItem(language3D() ? "左侧参数面板" : "Dock parameters", nullptr, !floating)) {
            floating = false;
        }
        if (ImGui::MenuItem(language3D() ? "居中悬浮参数" : "Floating parameters", nullptr, floating)) {
            floating = true;
            center = true;
        }
    }
};
} // namespace SeisWave::Ui
