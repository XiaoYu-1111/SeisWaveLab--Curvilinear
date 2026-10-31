#pragma once
#include "imgui.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <span>
#include <string>

namespace SeisWave::Ui {
struct ScalarLegend {
    std::string title;
    double low{}, high{};
    std::function<ImU32(double)> color; // The same normalized palette as the rendered field.
};
inline float scalarLegendFontSize() {
    return ImGui::GetWindowSize().y < 360 ? std::min(18.f, ImGui::GetFontSize()) : ImGui::GetFontSize();
}
inline int scalarLegendColumns(int count, float width) {
    const float minimum = std::max(170.f, 7.5f * scalarLegendFontSize());
    return std::clamp(static_cast<int>(width / minimum), 1, std::max(1, count));
}
inline float scalarLegendHeight(int count, float width) {
    if (!count)
        return 0;
    const int columns = scalarLegendColumns(count, width), rows = (count + columns - 1) / columns;
    return rows * (2 * (scalarLegendFontSize() + ImGui::GetStyle().ItemSpacing.y) + 14);
}
// Fixed-height cards keep legends aligned and leave the remaining height to the field.
// Narrow detached windows wrap whole cards; labels never overlap the next palette.
inline void drawScalarLegendStrip(const char *id, std::span<const ScalarLegend> legends) {
    if (legends.empty())
        return;
    ImGui::PushFont(nullptr, scalarLegendFontSize());
    const int columns =
        scalarLegendColumns(static_cast<int>(legends.size()), ImGui::GetContentRegionAvail().x);
    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, {10, 0});
    if (ImGui::BeginTable(id, columns, ImGuiTableFlags_SizingStretchSame)) {
        for (const auto &legend : legends) {
            ImGui::TableNextColumn();
            const auto at = ImGui::GetCursorScreenPos();
            const float width = std::max(1.f, ImGui::GetContentRegionAvail().x);
            const float line = ImGui::GetTextLineHeightWithSpacing(), height = 2 * line + 14;
            auto *draw = ImGui::GetWindowDrawList();
            const ImU32 text = ImGui::GetColorU32(ImGuiCol_Text);
            draw->PushClipRect(at, {at.x + width, at.y + height}, true);
            draw->AddText(at, text, legend.title.c_str());
            const float y = at.y + line;
            for (int i = 0; i < 128; ++i)
                draw->AddRectFilled({at.x + width * i / 128, y}, {at.x + width * (i + 1) / 128, y + 8},
                                    legend.color(i / 127.));
            draw->AddRect({at.x, y}, {at.x + width, y + 8}, ImGui::GetColorU32(ImGuiCol_Border));
            const int ticks = width >= 310 ? 3 : 2;
            for (int i = 0; i < ticks; ++i) {
                const double t = double(i) / (ticks - 1);
                char label[48];
                const double value = legend.low + (legend.high - legend.low) * t;
                std::snprintf(label, sizeof(label), width < 280 ? "%.2g" : "%.4g", value == 0 ? 0. : value);
                const float labelWidth = ImGui::CalcTextSize(label).x;
                const float x = at.x + width * static_cast<float>(t);
                draw->AddLine({x, y + 8}, {x, y + 11}, text);
                draw->AddText(
                    {std::clamp(x - labelWidth * .5f, at.x, at.x + std::max(0.f, width - labelWidth)),
                     y + 12},
                    text, label);
            }
            draw->PopClipRect();
            ImGui::Dummy({width, height});
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s\n%.6g .. %.6g", legend.title.c_str(), legend.low, legend.high);
        }
        ImGui::EndTable();
    }
    ImGui::PopStyleVar();
    ImGui::PopFont();
}
} // namespace SeisWave::Ui
