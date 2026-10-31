#pragma once
#include "RuntimeStatus.hpp"
#include "imgui.h"
#include "imgui_internal.h"
#include <initializer_list>
#include <string>
namespace SeisWave::Ui {
inline bool beginWorkbenchMenuBar() {
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {10, 6});
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {12, 8});
    if (ImGui::BeginMainMenuBar())
        return true;
    ImGui::PopStyleVar(2);
    return false;
}
inline void endWorkbenchMenuBar() {
    drawRuntimeStatus();
    ImGui::EndMainMenuBar();
    ImGui::PopStyleVar(2);
}
// Native ImGui submenus already have a right-facing arrow; menu-bar entries do not.
// Reserve a real clickable arrow area, retaining the original stable widget ID.
inline bool beginDropdownMenu(const char *label, bool enabled = true) {
    auto *parent = ImGui::GetCurrentWindow();
    if (parent->DC.LayoutType != ImGuiLayoutType_Horizontal)
        return ImGui::BeginMenu(label, enabled);
    std::string text(label);
    auto marker = text.find("###");
    auto visible = text.substr(0, marker);
    auto identity = marker == std::string::npos ? text : text.substr(marker + 3);
    const auto padded = visible + "    ###" + identity;
    ImDrawListSplitter channels;
    channels.Split(parent->DrawList, 2);
    channels.SetCurrentChannel(parent->DrawList, 1);
    const bool open = ImGui::BeginMenu(padded.c_str(), enabled);
    const auto rect = ImGui::GetCurrentContext()->LastItemData.Rect;
    channels.SetCurrentChannel(parent->DrawList, 0);
    const bool hovered = enabled && rect.Contains(ImGui::GetIO().MousePos);
    const ImVec2 a{rect.Min.x + 3, rect.Min.y + 1}, b{rect.Max.x - 3, rect.Max.y - 1};
    parent->DrawList->AddRectFilled(a, b,
                                    ImGui::GetColorU32(open      ? ImGuiCol_ButtonActive
                                                       : hovered ? ImGuiCol_ButtonHovered
                                                                 : ImGuiCol_Button),
                                    4);
    parent->DrawList->AddRect(a, b, ImGui::GetColorU32(ImGuiCol_Border), 4);
    channels.Merge(parent->DrawList);
    const float font = ImGui::GetFontSize(), x = rect.Max.x - font * .43f,
                y = (rect.Min.y + rect.Max.y) * .5f;
    parent->DrawList->AddTriangleFilled({x - font * .17f, y - font * .09f},
                                        {x + font * .17f, y - font * .09f}, {x, y + font * .13f},
                                        ImGui::GetColorU32(enabled ? ImGuiCol_Text : ImGuiCol_TextDisabled));
    return open;
}
// Keep top-level controls reachable when a narrow window or a larger font no
// longer fits the full menu bar. Native nested menus retain keyboard navigation.
struct MenuBarOverflow {
    bool collapsed{}, open{};
    MenuBarOverflow(float requiredWidth, const char *label) {
        collapsed = workbenchMenuRightEdge() - ImGui::GetCursorPosX() < requiredWidth;
        open = !collapsed || beginDropdownMenu(label);
    }
    MenuBarOverflow(std::initializer_list<const char *> labels, const char *label)
        : MenuBarOverflow(width(labels), label) {}
    static float width(std::initializer_list<const char *> labels) {
        float result = 0;
        for (const auto *label : labels)
            result += ImGui::CalcTextSize(label).x + ImGui::GetFontSize() * 1.4f + 24;
        return result;
    }
    ~MenuBarOverflow() {
        if (collapsed && open)
            ImGui::EndMenu();
    }
};
} // namespace SeisWave::Ui
