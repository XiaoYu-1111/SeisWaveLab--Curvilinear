#pragma once
#include "imgui.h"
#include <algorithm>
#include <span>
#include <vector>

namespace SeisWave::Ui {
// VS-like functional groups, with consistent button gaps and larger group spacing.
// All measurements use the active font, and entire groups wrap at narrow widths.
struct CommandStrip2D {
    std::vector<float> widths;
    std::vector<int> groups;
    std::vector<const char *> names;
    bool first{true};
    CommandStrip2D(std::span<const float> w, std::initializer_list<int> g,
                   std::initializer_list<const char *> n)
        : widths(w.begin(), w.end()), groups(g), names(n) {}
    float groupWidth(int i) const {
        float total = ImGui::CalcTextSize(names[groups[i]]).x + 12;
        for (int j = i; j < int(widths.size()) && groups[j] == groups[i]; ++j)
            total += widths[j] + (j == i ? 0 : 8);
        return total;
    }
    int rows(float available) const {
        int count = 1;
        float used = 0;
        for (int i = 0; i < int(widths.size());) {
            const float width = groupWidth(i), gap = used > 0 ? 22.f : 0.f;
            if (width > available) {
                if (used > 0) {
                    ++count;
                    used = 0;
                }
                used = ImGui::CalcTextSize(names[groups[i]]).x + 12 + widths[i];
                const int group = groups[i++];
                while (i < int(groups.size()) && groups[i] == group) {
                    if (used + 8 + widths[i] > available) {
                        ++count;
                        used = widths[i];
                    } else
                        used += 8 + widths[i];
                    ++i;
                }
                continue;
            }
            if (used > 0 && used + gap + width > available) {
                ++count;
                used = 0;
            }
            used += (used > 0 ? 22.f : 0.f) + width;
            const int group = groups[i];
            while (i < int(groups.size()) && groups[i] == group)
                ++i;
        }
        return count;
    }
    void next(int i) {
        const bool start = i == 0 || groups[i] != groups[i - 1];
        if (!first) {
            ImGui::SameLine(0, start ? 12.f : 8.f);
            const float needed = start ? groupWidth(i) : widths[i];
            if (ImGui::GetContentRegionAvail().x < needed) {
                ImGui::NewLine();
                first = true;
            }
        }
        if (start) {
            if (!first) {
                const auto at = ImGui::GetCursorScreenPos();
                ImGui::GetWindowDrawList()->AddLine({at.x, at.y + 4},
                                                    {at.x, at.y + ImGui::GetFrameHeight() - 4},
                                                    ImGui::GetColorU32(ImGuiCol_Separator));
                ImGui::Dummy({1, ImGui::GetFrameHeight()});
                ImGui::SameLine(0, 9);
            }
            ImGui::AlignTextToFramePadding();
            ImGui::TextDisabled("%s", names[groups[i]]);
            ImGui::SameLine(0, 12);
        }
        first = false;
    }
};
} // namespace SeisWave::Ui
