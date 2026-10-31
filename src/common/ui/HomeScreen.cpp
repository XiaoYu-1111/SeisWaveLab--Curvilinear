#include "HomeScreen.hpp"
#include "AppRuntime.hpp"
#include "Presentation3D.hpp"
#include "SeisCommon/ComputeDevices.hpp"
#include "imgui.h"
#include "imgui_internal.h"
#include <GLFW/glfw3.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
#include <stdexcept>

namespace SeisWave::Ui {
using namespace Seis3D::Curvilinear::Ui;
namespace {
const char *l(const char *en, const char *zh) { return language3D() ? zh : en; }
ImVec4 mix(ImVec4 a, ImVec4 b, float t) {
    return {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, 1};
}

// Procedural geometry illustrations; independent of the numerical sessions.
void illustration(ImDrawList *draw, ImVec2 at, ImVec2 size, bool spatial) {
    const auto point = [&](double x, double z) {
        return ImVec2(at.x + static_cast<float>(x) * size.x, at.y + static_cast<float>(z) * size.y);
    };
    const auto &theme = palette3D();
    const auto caption = [&](ImVec2 point, ImU32 color, const char *label) {
        draw->AddText(ImGui::GetFont(), std::clamp(size.y * .055f, 12.f, 20.f), point, color, label);
    };
    const auto base = ImGui::ColorConvertFloat4ToU32(theme.panel),
               shade = ImGui::ColorConvertFloat4ToU32(mix(theme.panel, theme.input, .65f));
    draw->AddRectFilledMultiColor(at, {at.x + size.x, at.y + size.y}, base, base, shade, shade);
    draw->PushClipRect(at, {at.x + size.x, at.y + size.y}, true);
    const auto ink = [&](float alpha) { return color3D(theme.text, alpha); };
    const int columns = std::clamp(static_cast<int>(size.x / 8), 48, 180);
    const int rows = std::clamp(static_cast<int>(size.y / 8), 32, 180);
    const auto curve = [&](auto mesh, bool vertical, double level, ImU32 color, float width = 1.f) {
        std::array<ImVec2, 129> points;
        for (int i = 0; i <= 128; ++i)
            points[i] = vertical ? mesh(level, i / 128.) : mesh(i / 128., level);
        draw->AddPolyline(points.data(), static_cast<int>(points.size()), color, 0, width);
    };
    if (!spatial) {
        const auto mesh = [&](double x, double z) {
            return point(.07 + .86 * x, .14 + .76 * z + .075 * std::cos(6.283185 * x) * std::pow(1 - z, 3));
        };
        for (int k = 0; k <= rows; ++k)
            curve(mesh, false, double(k) / rows, ink(k % 8 ? .16f : .34f));
        for (int i = 0; i <= columns; ++i)
            curve(mesh, true, double(i) / columns, ink(i % 8 ? .16f : .34f));
        curve(mesh, false, 0, ink(.85f), 2);
        const auto source = mesh(.5, .32);
        draw->AddCircle(source, 5, ink(.85f), 16, 1.5f);
        draw->AddCircleFilled(source, 1.7f, ink(.85f));
        caption(point(.94, .91), ink(.65f), "X");
        caption(point(.025, .88), ink(.65f), "Z");
        caption(point(.075, .025), ink(.8f), "X-Z  /  P-SV");
    } else {
        const auto project = [&](double x, double y, double z) {
            return point(
                .08 + .59 * x + .25 * y,
                .12 + .17 * y +
                    .65 * (z + .075 * std::cos(6.283185 * x) * std::cos(6.283185 * y) * std::pow(1 - z, 3)));
        };
        // Two orthogonal interior sections and a dense terrain-following surface.
        for (int section = 0; section < 2; ++section) {
            const auto mesh = [&](double u, double v) {
                return section ? project(.5, u, v) : project(u, .5, v);
            };
            for (int k = 0; k <= rows; ++k)
                curve(mesh, false, double(k) / rows, ink(k % 8 ? .11f : .24f));
            for (int i = 0; i <= columns; ++i)
                curve(mesh, true, double(i) / columns, ink(i % 8 ? .11f : .24f));
        }
        const auto surface = [&](double x, double y) { return project(x, y, 0); };
        for (int i = 0; i <= columns; ++i) {
            curve(surface, false, double(i) / columns, ink(i % 8 ? .25f : .55f));
            curve(surface, true, double(i) / columns, ink(i % 8 ? .25f : .55f));
        }
        for (double x : {0., 1.})
            for (double y : {0., 1.})
                draw->AddLine(project(x, y, 0), project(x, y, 1), ink(.6f), 1.5f);
        for (double t : {0., 1.}) {
            draw->AddLine(project(0, t, 1), project(1, t, 1), ink(.6f), 1.5f);
            draw->AddLine(project(t, 0, 1), project(t, 1, 1), ink(.6f), 1.5f);
        }
        draw->AddCircle(project(.5, .5, .32), 5, ink(.85f), 16, 1.5f);
        caption(point(.68, .83), ink(.65f), "X");
        caption(point(.93, .92), ink(.65f), "Y");
        caption(point(.04, .78), ink(.65f), "Z");
        caption(point(.055, .025), ink(.8f), "X-Y-Z  /  ELASTIC VOLUME");
    }
    draw->PopClipRect();
}
void flowingWave(ImDrawList *draw, ImVec2 at, ImVec2 size, double phase) {
    draw->AddRectFilled(at, {at.x + size.x, at.y + size.y}, color3D(palette3D().panel), 6);
    draw->PushClipRect(at, {at.x + size.x, at.y + size.y}, true);
    for (int layer = 0; layer < 4; ++layer) {
        std::array<ImVec2, 257> curve;
        for (int i = 0; i <= 256; ++i) {
            const double x = i / 256.;
            const double traveling = std::sin(25 * x - phase * 2.6 + layer * .55);
            const double envelope = .25 + .75 * std::pow(std::sin(3.141592653589793 * x), 2);
            const double ripple = std::sin(x * 8 + phase * .7 + layer) * .15;
            curve[i] = {at.x + static_cast<float>(x) * size.x,
                        at.y + size.y * (.5f + static_cast<float>(traveling * envelope * .24 + ripple * .3 +
                                                                  layer * .025))};
        }
        draw->AddPolyline(
            curve.data(), 257,
            color3D(functionColor3D(layer % 2 ? FunctionRole3D::Display : FunctionRole3D::Model),
                    .24f + layer * .15f),
            0, layer == 3 ? 2.f : 1.2f);
    }
    draw->PopClipRect();
}
void workflowStrip() {
    const char *en[]{"01  MODEL", "02  SURVEY", "03  SOLVE", "04  EXPLORE", "05  EXPORT"};
    const char *zh[]{"01  曲面建模", "02  震源与接收", "03  并行求解", "04  波场与记录", "05  数据导出"};
    if (ImGui::BeginTable("##homeWorkflow", 5, ImGuiTableFlags_SizingStretchSame)) {
        for (int i = 0; i < 5; ++i) {
            ImGui::TableNextColumn();
            ImGui::TextColored(functionColor3D(i == 2   ? FunctionRole3D::Run
                                               : i == 4 ? FunctionRole3D::Export
                                                        : FunctionRole3D::Display),
                               "%s", l(en[i], zh[i]));
        }
        ImGui::EndTable();
    }
}
} // namespace
Route runHomeScreen(bool smoke, int choice) {
    std::string error;
    AppRuntime runtime;
    if (!runtime.open("SeisWave Lab · Curvilinear", smoke, error))
        throw std::runtime_error(error);
    const auto inventory = Seis3D::Curvilinear::queryCudaDevices3D();
    int usable = 0;
    for (const auto &device : inventory.devices)
        usable += device.available ? 1 : 0;
    bool animate = true;
    Route result = Route::Home;
    std::array<ImRect, 2> buttons{};
    std::array<ImRect, 2> illustrations{};
    ImRect acousticEntry;
    std::array<ImRect, 2> acousticOptions;
    for (int frame = 0; !glfwWindowShouldClose(runtime.window()); ++frame) {
        if (smoke && !choice && frame % 8 == 0 && frame <= 40) {
            const int sizes[3][2]{{1600, 1000}, {1280, 800}, {960, 640}};
            const int c = frame / 8;
            glfwSetWindowSize(runtime.window(), sizes[c % 3][0], sizes[c % 3][1]);
            setLanguage3D(c < 3 ? 1 : 0);
            applyTheme3D(c < 3 ? 9 : 4);
        }
        auto &io = ImGui::GetIO();
        if (smoke && !choice && frame == 64) {
            glfwSetWindowSize(runtime.window(), 2560, 1494);
            setInterfaceFontSize(20);
            setLanguage3D(1);
            applyTheme3D(11);
        }
        if (smoke && !choice && frame == 48) {
            glfwSetWindowSize(runtime.window(), 960, 640);
            setInterfaceFontSize(28);
            setLanguage3D(1);
            applyTheme3D(0);
        }
        if (smoke && choice && frame == 2) {
            const auto at = choice >= 3 ? acousticEntry.GetCenter() : illustrations[choice - 1].GetCenter();
            io.AddMousePosEvent(at.x, at.y);
            io.AddMouseButtonEvent(0, true);
        }
        if (smoke && choice && frame == 3)
            io.AddMouseButtonEvent(0, false);
        if (smoke && choice >= 3 && frame == 6) {
            auto at = acousticOptions[choice == 3 ? 1 : 0].GetCenter();
            io.AddMousePosEvent(at.x, at.y);
            io.AddMouseButtonEvent(0, true);
        }
        if (smoke && choice >= 3 && frame == 7)
            io.AddMouseButtonEvent(0, false);
        runtime.begin();
        const auto *vp = ImGui::GetMainViewport();
        const bool compact = vp->Size.y < 760;
        ImGui::SetNextWindowPos(vp->Pos);
        ImGui::SetNextWindowSize(vp->Size);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {compact ? 16.f : 28.f, compact ? 12.f : 22.f});
        ImGui::Begin("Home###RootHome", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                         ImGuiWindowFlags_NoSavedSettings);
        ImGui::TextColored(palette3D().accent, "SEISWAVE LAB");
        ImGui::SameLine();
        ImGui::TextDisabled("CURVILINEAR");
        ImGui::SameLine(std::max(360.f, ImGui::GetWindowWidth() - 424));
        int language = language3D();
        ImGui::SetNextItemWidth(140);
        if (ImGui::Combo("##homeLang", &language, "English\0简体中文\0"))
            setLanguage3D(language);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(165);
        if (ImGui::BeginCombo("##homeTheme", themeName3D(themeIndex3D()))) {
            for (int i = 0; i < themeCount3D; ++i)
                if (ImGui::Selectable(themeName3D(i), themeIndex3D() == i))
                    applyTheme3D(i);
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        if (ImGui::Button(l("Aa", "字号")))
            ImGui::OpenPopup("HomeFont");
        if (ImGui::BeginPopup("HomeFont")) {
            float font = interfaceFontSize();
            if (ImGui::SliderFloat(l("Font size", "字体大小"), &font, 16, 28, "%.0f px"))
                setInterfaceFontSize(font);
            ImGui::Checkbox(l("Wave motion", "动态波场"), &animate);
            ImGui::EndPopup();
        }
        ImGui::Spacing();
        ImGui::PushFont(nullptr, compact ? 28.f : 36.f);
        ImGui::TextUnformatted(l("Curved-surface seismic wave simulation", "曲面地震波场模拟"));
        ImGui::PopFont();
        ImGui::TextWrapped("%s",
                           l("Surface-fitted grids. Elastic wave propagation. Two dimensions to explore.",
                             "地表贴体网格 · 弹性波传播 · 二维剖面与三维空间"));
        if (!compact) {
            ImGui::Spacing();
            workflowStrip();
        }
        ImGui::Spacing();
        const float cardHeight =
            std::max(285.f, ImGui::GetContentRegionAvail().y - (compact ? 108.f : 138.f));
        if (ImGui::BeginTable("##routes", 2, ImGuiTableFlags_SizingStretchSame)) {
            for (int dimension = 0; dimension < 2; ++dimension) {
                ImGui::TableNextColumn();
                ImGui::PushID(dimension);
                ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,
                                    {compact ? 12.f : 20.f, compact ? 12.f : 18.f});
                ImGui::BeginChild("##routeCard", {0, cardHeight}, ImGuiChildFlags_Borders);
                const auto role = dimension ? FunctionRole3D::Run : FunctionRole3D::Model;
                const auto origin = ImGui::GetWindowPos();
                const auto cardSize = ImGui::GetWindowSize();
                ImGui::GetWindowDrawList()->AddRectFilled(
                    origin, {origin.x + cardSize.x, origin.y + 4},
                    ImGui::ColorConvertFloat4ToU32(functionColor3D(role)));
                ImGui::PushFont(nullptr, compact ? 25.f : 30.f);
                ImGui::TextColored(functionColor3D(role), "%s",
                                   dimension ? l("3D  Curved volume", "3D  曲面体网格")
                                             : l("2D  Curved section", "2D  曲面剖面"));
                ImGui::PopFont();
                ImGui::TextUnformatted(
                    dimension ? l("X-Y-Z | Elastic waves | 9 fields", "X–Y–Z 空间 · 弹性波 · 九场求解")
                              : l("X-Z | Plane strain | P-SV, 5 fields", "X–Z 平面应变 · P–SV · 五场求解"));
                const auto at = ImGui::GetCursorScreenPos();
                const ImVec2 size{
                    ImGui::GetContentRegionAvail().x,
                    std::max(90.f, ImGui::GetContentRegionAvail().y -
                                       (compact ? (interfaceFontSize() > 22 ? 104.f : 152.f) : 206.f))};
                illustration(ImGui::GetWindowDrawList(), at, size, dimension != 0);
                if (ImGui::InvisibleButton("##enterIllustration", size))
                    result = dimension ? Route::ThreeD : Route::TwoD;
                illustrations[dimension] = {ImGui::GetItemRectMin(), ImGui::GetItemRectMax()};
                if (ImGui::IsItemHovered()) {
                    ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                    ImGui::SetTooltip("%s", dimension ? l("Enter 3D simulation", "进入三维曲面模拟")
                                                      : l("Enter 2D simulation", "进入二维曲面模拟"));
                }
                ImGui::TextDisabled("%s", l("Program-drawn geometry illustration", "程序绘制的曲网格示意"));
                ImGui::Spacing();
                if (!compact || interfaceFontSize() <= 22)
                    ImGui::TextWrapped(
                        "%s", dimension
                                  ? l("Curved surface mesh, orthogonal slices and a surface receiver array.",
                                      "起伏地表曲网格、正交波场切片、地表接收面阵与三分量记录。")
                                  : l("Terrain-following section, layered materials and a two-component "
                                      "receiver line.",
                                      "地形贴合剖面、分层介质、震源测线与双分量地震记录。"));
                if (!compact) {
                    ImGui::Spacing();
                    ImGui::TextDisabled("%s", dimension ? l("Spatial propagation and acquisition geometry",
                                                            "研究空间传播、波型转换与观测系统")
                                                        : l("Section-scale experiments and parameter studies",
                                                            "用于剖面传播机理与参数试验"));
                }
                ImGui::SetCursorPosY(std::max(ImGui::GetCursorPosY(), cardHeight - (compact ? 59.f : 76.f)));
                if (functionButton3D(dimension ? l("Enter 3D simulation", "进入三维曲面模拟")
                                               : l("Enter 2D simulation", "进入二维曲面模拟"),
                                     role, {-1, compact ? 44.f : 52.f}))
                    result = dimension ? Route::ThreeD : Route::TwoD;
                buttons[dimension] = {ImGui::GetItemRectMin(), ImGui::GetItemRectMax()};
                ImGui::EndChild();
                ImGui::PopStyleVar();
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
        ImGui::Spacing();
        ImGui::TextColored(functionColor3D(usable ? FunctionRole3D::Run : FunctionRole3D::Display),
                           "CPU / OpenMP  +  CUDA  |  %d %s", usable,
                           l("usable CUDA devices", "个可用 CUDA 设备"));
        ImGui::SameLine();
        ImGui::SetNextItemWidth(240);
        const auto acousticOrigin = ImGui::GetCursorScreenPos();
        acousticEntry = {acousticOrigin,
                         {acousticOrigin.x + 240, acousticOrigin.y + ImGui::GetFrameHeight()}};
        bool acousticMenu = ImGui::BeginCombo("##homeAcoustic", l("Acoustics / P", "声学 / P"));
        if (acousticMenu) {
            if (ImGui::Selectable(l("Acoustic forward", "声学正演")))
                result = Route::Acoustic;
            acousticOptions[0] = {ImGui::GetItemRectMin(), ImGui::GetItemRectMax()};
            if (ImGui::Selectable(l("RTM / illumination", "RTM 成像与照明")))
                result = Route::Imaging;
            acousticOptions[1] = {ImGui::GetItemRectMin(), ImGui::GetItemRectMax()};
            ImGui::EndCombo();
        }
        ImGui::TextWrapped("%s",
                           "XIAO YU  |  C++20 / CUDA / OpenMP / OpenGL / GLFW / Dear ImGui / ImPlot / CMake");
        const ImVec2 flowSize{ImGui::GetContentRegionAvail().x, compact ? 32.f : 58.f};
        flowingWave(ImGui::GetWindowDrawList(), ImGui::GetCursorScreenPos(), flowSize,
                    animate ? (smoke ? frame * .04 : ImGui::GetTime()) : 0);
        ImGui::Dummy(flowSize);
        ImGui::End();
        ImGui::PopStyleVar();
        runtime.present();
        if (smoke && choice >= 3 && (frame == 3 || frame == 5)) {
            int w, h;
            glfwGetFramebufferSize(runtime.window(), &w, &h);
            capture(outputRoot() / ("screenshots/module-ui/home-group-" + std::to_string(choice) + "-" +
                                    std::to_string(frame) + ".ppm"),
                    w, h);
        }
        if (smoke && choice && frame == (choice >= 3 ? 8 : 4)) {
            if (result != (choice == 1   ? Route::TwoD
                           : choice == 3 ? Route::Imaging
                           : choice == 4 ? Route::Acoustic
                                         : Route::ThreeD))
                throw std::runtime_error("Home card routed to the wrong dimension");
            std::cout << "Home route PASS: " << choice << '\n';
            return result;
        }
        if (smoke && !choice && frame % 8 == 5 && frame <= 45) {
            for (const auto &rect : buttons)
                if (rect.Min.y < 0 || rect.Max.y > vp->Size.y - 45 || rect.GetWidth() < 250)
                    throw std::runtime_error("Home entry button is clipped");
            if (buttons[0].GetCenter().x >= buttons[1].GetCenter().x)
                throw std::runtime_error("Home dimension order reversed");
            int width, height;
            glfwGetFramebufferSize(runtime.window(), &width, &height);
            const int index = frame / 8;
            if (!capture(outputRoot() / ("screenshots/two-d/home-" + std::to_string(index) + ".ppm"), width,
                         height))
                throw std::runtime_error("Home capture failed");
            if (!index)
                capture(outputRoot() / "screenshots/two-d/home.ppm", width, height);
            if (index == 5) {
                std::cout << "Home layout PASS: left curved 2D, right curved 3D; 6 bilingual layouts, "
                             "visible entry controls\n";
                // Also check the enlarged-font layout and changing footer frames.
            }
        }
        if (smoke && !choice && (frame == 53 || frame == 58)) {
            for (const auto &rect : buttons)
                if (rect.Max.y > vp->Size.y - 45)
                    throw std::runtime_error("Large-font Home entry is clipped");
            int w, h;
            glfwGetFramebufferSize(runtime.window(), &w, &h);
            if (!capture(outputRoot() / (frame == 53 ? "screenshots/two-d/home-motion-a.ppm"
                                                     : "screenshots/two-d/home-motion-b.ppm"),
                         w, h))
                throw std::runtime_error("Home animation capture failed");
            if (frame == 58) {
                std::cout << "Home PASS: 28 px font and program-drawn flowing wave footer\n";
            }
        }
        if (smoke && !choice && frame == 69) {
            int w, h;
            glfwGetFramebufferSize(runtime.window(), &w, &h);
            if (!capture(outputRoot() / "screenshots/two-d/home-maximized.ppm", w, h))
                throw std::runtime_error("Maximized Home capture failed");
            std::cout << "Home PASS: 2560x1494 responsive wire-grid illustrations\n";
            return result;
        }
        if (!smoke && result != Route::Home)
            return result;
    }
    return Route::Quit;
}
} // namespace SeisWave::Ui
