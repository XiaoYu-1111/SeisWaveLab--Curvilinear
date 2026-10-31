#pragma once
#include "Presentation3D.hpp"
#include "imgui_internal.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <unordered_map>

namespace SeisWave::Ui {
// Stateless presentation shared by physics workspaces. Callers own all inputs and actions.
inline const char *parameterLabel2D(const char *en, const char *zh) {
    return Seis3D::Curvilinear::Ui::language3D() ? zh : en;
}
inline void parameterTabs2D(int &category) {
    using namespace Seis3D::Curvilinear::Ui;
    const char *en[]{"Model", "Survey", "Run", "Display", "Export"};
    const char *zh[]{"建模", "采集", "运行", "显示", "导出"};
    const float width =
        std::max(22.f, (ImGui::GetContentRegionAvail().x - 4 * ImGui::GetStyle().ItemSpacing.x) / 5);
    float longest = 1;
    for (int i = 0; i < 5; ++i)
        longest = std::max(longest, ImGui::CalcTextSize(parameterLabel2D(en[i], zh[i])).x);
    ImGui::PushFont(nullptr, std::clamp(ImGui::GetFontSize() *
                                            (width - 2 * ImGui::GetStyle().FramePadding.x - 4) / longest,
                                        12.f, ImGui::GetFontSize()));
    for (int i = 0; i < 5; ++i) {
        if (i)
            ImGui::SameLine();
        if (functionButton3D(parameterLabel2D(en[i], zh[i]),
                             i == 2   ? FunctionRole3D::Run
                             : i == 4 ? FunctionRole3D::Export
                                      : FunctionRole3D::Model,
                             {width, 0}))
            category = i;
        if (category == i)
            ImGui::GetWindowDrawList()->AddRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(),
                                                ImGui::ColorConvertFloat4ToU32(palette3D().accent), 4, 0,
                                                1.5f);
    }
    ImGui::PopFont();
}
inline void parameterGroupButtons2D(int &epoch, bool &expanded) {
    using namespace Seis3D::Curvilinear::Ui;
    if (functionButton3D(parameterLabel2D("Expand all", "全部展开"), FunctionRole3D::Display)) {
        ++epoch;
        expanded = true;
    }
    ImGui::SameLine();
    if (functionButton3D(parameterLabel2D("Collapse all", "全部折叠"), FunctionRole3D::Display)) {
        ++epoch;
        expanded = false;
    }
}
inline bool parameterGroup2D(const char *title, ImGuiTreeNodeFlags flags, int epoch, bool expanded,
                             std::unordered_map<ImGuiID, int> &applied) {
    const auto id = ImGui::GetID(title);
    if (epoch && applied[id] != epoch) {
        ImGui::SetNextItemOpen(expanded);
        applied[id] = epoch;
    }
    return ImGui::CollapsingHeader(title, flags);
}
struct ParameterSession2D {
    bool ready{}, dirty{};
    double frequency{}, dt{};
    std::string error;
    std::string activeText() const {
        if (!ready)
            return parameterLabel2D("Model not built yet", "模型尚未构建");
        char value[128];
        std::snprintf(value, sizeof(value), "%s f0 %.2f Hz | dt %.5f ms", parameterLabel2D("Active", "生效"),
                      frequency, dt * 1000);
        return value;
    }
    const char *stateText() const {
        if (!error.empty())
            return parameterLabel2D("Operation incomplete (hover for details)", "操作未完成（悬停查看详情）");
        return dirty ? parameterLabel2D("Pending changes: reset / apply or Run.",
                                        "参数待应用：重置并应用，或运行。")
                     : parameterLabel2D("Parameters applied", "参数已生效");
    }
    float height() const {
        const auto &style = ImGui::GetStyle();
        const float width = std::max(40.f, ImGui::GetContentRegionAvail().x);
        return std::ceil(ImGui::GetFontSize() + 2 * style.SeparatorTextPadding.y +
                         ImGui::CalcTextSize(activeText().c_str(), nullptr, false, width).y +
                         ImGui::CalcTextSize(stateText(), nullptr, false, width).y +
                         2.4f * ImGui::GetFrameHeight() + 5 * style.ItemSpacing.y + 6);
    }
};
struct ParameterActions2D {
    bool rebuild{}, undo{}, defaults{};
    ImRect rebuildRect, undoRect, defaultsRect;
};
inline ParameterActions2D parameterSession2D(const ParameterSession2D &session) {
    using namespace Seis3D::Curvilinear::Ui;
    ParameterActions2D actions;
    ImGui::SeparatorText(parameterLabel2D("Session / rebuild", "会话与重置"));
    ImGui::TextWrapped("%s", session.activeText().c_str());
    ImGui::PushStyleColor(ImGuiCol_Text, functionColor3D(!session.error.empty() ? FunctionRole3D::Danger
                                                         : session.dirty        ? FunctionRole3D::Warning
                                                                                : FunctionRole3D::Run));
    ImGui::TextWrapped("%s", session.stateText());
    if (!session.error.empty() && ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", session.error.c_str());
    ImGui::PopStyleColor();
    actions.rebuild = functionButton3D(parameterLabel2D("Reset / apply parameters", "重置并应用参数"),
                                       session.dirty ? FunctionRole3D::Warning : FunctionRole3D::Model,
                                       {-1, ImGui::GetFrameHeight() * 1.4f});
    actions.rebuildRect = {ImGui::GetItemRectMin(), ImGui::GetItemRectMax()};
    const float half = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) / 2;
    ImGui::BeginDisabled(!session.ready);
    actions.undo =
        functionButton3D(parameterLabel2D("Undo edits", "撤销修改"), FunctionRole3D::Display, {half, 0});
    actions.undoRect = {ImGui::GetItemRectMin(), ImGui::GetItemRectMax()};
    ImGui::EndDisabled();
    ImGui::SameLine();
    actions.defaults =
        functionButton3D(parameterLabel2D("Defaults", "恢复默认"), FunctionRole3D::Display, {half, 0});
    actions.defaultsRect = {ImGui::GetItemRectMin(), ImGui::GetItemRectMax()};
    return actions;
}
} // namespace SeisWave::Ui
