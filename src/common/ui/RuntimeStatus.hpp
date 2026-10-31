#pragma once
#include "AppRuntime.hpp"
#include "Presentation3D.hpp"
#include "imgui.h"
#include <array>
#include <cstdio>
#include <ctime>
#include <string>

namespace SeisWave::Ui {
inline std::array<ImVec4, 2> runtimeDisplayOptions() {
    std::array<ImVec4, 2> bounds{};
    const bool zh = Seis3D::Curvilinear::Ui::language3D() != 0;
    ImGui::Checkbox(zh ? "右上角系统时间" : "System clock (top right)", &systemClockVisible());
    auto a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    bounds[0] = {a.x, a.y, b.x, b.y};
    ImGui::Checkbox(zh ? "右上角刷新率 FPS" : "Frame rate FPS (top right)", &frameRateVisible());
    a = ImGui::GetItemRectMin();
    b = ImGui::GetItemRectMax();
    bounds[1] = {a.x, a.y, b.x, b.y};
    return bounds;
}
inline float runtimeStatusWidth() {
    if (!systemClockVisible() && !frameRateVisible())
        return 0;
    const auto text = std::string(systemClockVisible() ? "88:88:88" : "") +
                      (systemClockVisible() && frameRateVisible() ? " | " : "") +
                      (frameRateVisible() ? "9999.9 FPS" : "");
    return ImGui::CalcTextSize(text.c_str()).x + 2 * ImGui::GetStyle().FramePadding.x + 12;
}
inline float workbenchMenuRightEdge() {
    return ImGui::GetWindowWidth() - ImGui::GetStyle().WindowPadding.x - runtimeStatusWidth();
}
inline void drawRuntimeStatus() {
    const float width = runtimeStatusWidth();
    if (width == 0)
        return;
    static std::time_t last = -1;
    static char clock[16]{}, date[32]{};
    const auto now = std::time(nullptr);
    if (now != last) {
        std::tm local{};
#ifdef _WIN32
        localtime_s(&local, &now);
#else
        localtime_r(&now, &local);
#endif
        std::strftime(clock, sizeof(clock), "%H:%M:%S", &local);
        std::strftime(date, sizeof(date), "%Y-%m-%d %H:%M:%S", &local);
        last = now;
    }
    char fps[40]{};
    if (frameRateVisible())
        std::snprintf(fps, sizeof(fps), "%.1f FPS", ImGui::GetIO().Framerate);
    const auto text = std::string(systemClockVisible() ? clock : "") +
                      (systemClockVisible() && frameRateVisible() ? " | " : "") + fps;
    ImGui::SetCursorPosX(ImGui::GetWindowWidth() - ImGui::GetStyle().WindowPadding.x - width);
    const auto at = ImGui::GetCursorScreenPos();
    const ImVec2 end{at.x + width, at.y + ImGui::GetFrameHeight()};
    auto *draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(at, end, ImGui::GetColorU32(ImGuiCol_FrameBg), 4);
    draw->AddRect(at, end, ImGui::GetColorU32(ImGuiCol_Border), 4);
    const auto size = ImGui::CalcTextSize(text.c_str());
    draw->AddText({end.x - ImGui::GetStyle().FramePadding.x - size.x, at.y + (end.y - at.y - size.y) * .5f},
                  ImGui::GetColorU32(ImGuiCol_Text), text.c_str());
    ImGui::Dummy({width, end.y - at.y});
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s\n%s", date,
                          Seis3D::Curvilinear::Ui::language3D() ? "FPS：界面实际绘制帧率"
                                                                : "FPS: actual interface rendering rate");
}
} // namespace SeisWave::Ui
