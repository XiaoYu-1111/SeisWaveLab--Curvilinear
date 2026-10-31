#pragma once
#include "imgui.h"
#include "imgui_internal.h"
#include <string>
namespace SeisWave::Ui {
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
    const bool open = ImGui::BeginMenu(padded.c_str(), enabled);
    const auto rect = ImGui::GetCurrentContext()->LastItemData.Rect;
    const float font = ImGui::GetFontSize(), x = rect.Max.x - font * .43f,
                y = (rect.Min.y + rect.Max.y) * .5f;
    parent->DrawList->AddTriangleFilled({x - font * .17f, y - font * .09f},
                                        {x + font * .17f, y - font * .09f}, {x, y + font * .13f},
                                        ImGui::GetColorU32(enabled ? ImGuiCol_Text : ImGuiCol_TextDisabled));
    return open;
}
} // namespace SeisWave::Ui
