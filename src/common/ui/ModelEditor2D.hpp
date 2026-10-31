#pragma once
#include "ParameterPanel2D.hpp"
#include "Seis2D/ModelPresets2D.hpp"
#include "Seis2D/SurfacePresets.hpp"
#include "WorkspaceTools.hpp"
#include "implot.h"
#include <functional>
#include <limits>
namespace SeisWave::Ui {
using namespace Seis2D::Curvilinear;
using Seis3D::Curvilinear::Ui::FunctionRole3D;
struct ModelEditor2D {
    Parameters &staged;
    std::array<char, 2048> &surfacePath;
    std::string &error;
    std::string &status;
    bool acoustic{};
    std::function<void()> browseTerrain;
    std::function<void(Parameters &)> fitAcquisition;
    std::vector<Layer> layerDraft;
    bool openLayers{}, openTerrain{}, zeroSurfaceX{true};
    int layerPreset{-1}, terrainPreset{-1}, terrainResampledNx{};
    SurfaceSamples terrainPresetSource;
    double terrainResampledSpacing{};
    std::string layerError;
    ImVec2 dialogViewport{};
    std::vector<double> terrainPreviewX, terrainPreviewZ;
    ImRect layerUseRect, terrainDoneRect;
    static const char *editorLabel(const char *en, const char *zh) { return parameterLabel2D(en, zh); }
    static bool editorButton(const char *en, const char *zh, Seis3D::Curvilinear::Ui::FunctionRole3D role,
                             ImVec2 size = {0, 0}) {
        const std::string id = std::string(editorLabel(en, zh)) + "###2D" + en;
        return Seis3D::Curvilinear::Ui::functionButton3D(id.c_str(), role, size);
    }
    inline static constexpr SurfacePreset examples[]{
        {"flat_2km.csv", "Flat surface / 2 km", "平地 / 2 km"},
        {"ridge_2km.csv", "Smooth ridge / 2 km", "平滑丘陵 / 2 km"},
        {"irregular_2km.csv", "Irregular samples / 2 km", "不等距采样 / 2 km"},
        {"valley_20km.csv", "Wide valley / 20 km", "宽域山谷 / 20 km"},
        {"hill_valley_1km.csv", "Hill + valley (+/-100 m) / 1 km", "山丘 + 沟谷（±100 m）/ 1 km"}};
    ModelEditor2D(Parameters &p, std::array<char, 2048> &path, std::string &e, std::string &state,
                  bool pressure = false)
        : staged(p), surfacePath(path), error(e), status(state), acoustic(pressure) {}
    void updateTerrainPreview() {
        terrainPreviewX.clear();
        terrainPreviewZ.clear();
        if (staged.surfaceSamples.empty())
            return;
        const SurfaceProfile curve(staged.surfaceSamples);
        for (int i = 0; i <= 512; ++i) {
            const double x =
                staged.surfaceSamples.front()[0] +
                (staged.surfaceSamples.back()[0] - staged.surfaceSamples.front()[0]) * (i / 512.);
            terrainPreviewX.push_back(x);
            terrainPreviewZ.push_back(curve.sample(x)[0]);
        }
    }
    bool syncTerrainPreset() {
        if (terrainPreset < 0 || !staged.importedSurface)
            return true;
        try {
            if (staged.nx < 8 || staged.nx > maxAxisNodes || !std::isfinite(staged.spacing) ||
                staged.spacing <= 0)
                throw std::runtime_error("Terrain preset requires valid X nodes and positive spacing.");
            if (terrainResampledNx != staged.nx || terrainResampledSpacing != staged.spacing) {
                auto samples = resampleSurfaceProfile(terrainPresetSource, (staged.nx - 1) * staged.spacing,
                                                      static_cast<std::size_t>(staged.nx));
                staged.surfaceSamples = std::move(samples);
                terrainResampledNx = staged.nx;
                terrainResampledSpacing = staged.spacing;
                updateTerrainPreview();
            }
            return true;
        } catch (const std::exception &e) {
            error = e.what();
            return false;
        }
    }
    bool chooseTerrainPreset(int index) {
        if (index < 0 || index >= static_cast<int>(surfacePresets.size()))
            return false;
        const auto path = SeisWave::Ui::elevationExampleRoot() / surfacePresets[index].file;
        SurfaceSamples source;
        if (!readSurfaceProfile(path, source, error))
            return false;
        try {
            const auto pathText = path.u8string();
            if (pathText.size() >= surfacePath.size())
                throw std::runtime_error("Terrain preset path is too long.");
            if (staged.nx < 8 || staged.nx > maxAxisNodes || !std::isfinite(staged.spacing) ||
                staged.spacing <= 0)
                throw std::runtime_error("Set valid X nodes and spacing before choosing a terrain preset.");
            auto samples = resampleSurfaceProfile(source, (staged.nx - 1) * staged.spacing,
                                                  static_cast<std::size_t>(staged.nx));
            terrainPresetSource = std::move(source);
            staged.surfaceSamples = std::move(samples);
            terrainPreset = index;
            terrainResampledNx = staged.nx;
            terrainResampledSpacing = staged.spacing;
            staged.surfaceDatum = 0;
            staged.importedSurface = true;
            std::snprintf(surfacePath.data(), surfacePath.size(), "%s",
                          reinterpret_cast<const char *>(pathText.c_str()));
            updateTerrainPreview();
            error.clear();
            status = editorLabel(
                "Terrain preset staged; X follows model width, elevation is unchanged. Reset / apply.",
                "地表预设已选择，水平范围随模型宽度适配，高程保持不变；重置并应用后生效。");
            return true;
        } catch (const std::exception &e) {
            error = e.what();
            return false;
        }
    }
    bool setTerrainPresetWidth(double width) {
        const double intervals = width / staged.spacing;
        if (terrainPreset < 0 || !std::isfinite(width) || width <= 0 || !std::isfinite(staged.spacing) ||
            staged.spacing <= 0 || !std::isfinite(intervals) || intervals < 7 ||
            intervals > maxAxisNodes - 1) {
            error = editorLabel("This width requires X nodes outside 8..16384 or invalid spacing.",
                                "当前宽度所需 X 节点超出 8..16384，或网格间距无效。");
            return false;
        }
        staged.nx = static_cast<int>(std::round(intervals)) + 1;
        if (fitAcquisition)
            fitAcquisition(staged);
        if (!syncTerrainPreset())
            return false;
        error.clear();
        return true;
    }
    void terrainPresetControls() {
        ImGui::TextUnformatted(editorLabel("Surface presets (fit model width)", "地表预设（适配模型宽度）"));
        ImGui::SetNextItemWidth(-1);
        if (ImGui::BeginCombo(
                "##surfacePreset",
                terrainPreset < 0
                    ? editorLabel("Custom / file / analytic", "自定义 / 文件 / 解析地表")
                    : editorLabel(surfacePresets[terrainPreset].en, surfacePresets[terrainPreset].zh))) {
            if (ImGui::Selectable(editorLabel("Custom / file / analytic", "自定义 / 文件 / 解析地表"),
                                  terrainPreset < 0)) {
                terrainPreset = -1;
                terrainPresetSource.clear(); // Freeze current samples; do not stretch imported measurements.
            }
            for (int i = 0; i < static_cast<int>(surfacePresets.size()); ++i)
                if (ImGui::Selectable(editorLabel(surfacePresets[i].en, surfacePresets[i].zh),
                                      terrainPreset == i))
                    chooseTerrainPreset(i);
            ImGui::EndCombo();
        }
        if (terrainPreset < 0)
            return;
        for (int km : {1, 2, 4}) {
            if (km != 1)
                ImGui::SameLine();
            const auto name = std::to_string(km) + " km";
            if (ImGui::Button(name.c_str()))
                setTerrainPresetWidth(km * 1000.);
        }
        syncTerrainPreset();
        ImGui::TextWrapped(
            "%s",
            editorLabel("Original 1 km shape is stretched horizontally and PCHIP-sampled at grid nodes. "
                        "Elevation and datum stay unchanged; slopes change. Width buttons keep spacing and "
                        "round X nodes.",
                        "由原始 1 km "
                        "数据水平拉伸，并按网格节点保形插值。高程、基准保持不变，坡度会变化；宽度按钮保持间距"
                        "，X 节点取整，以待应用尺寸为准。"));
    }
    bool loadTerrain() {
        SurfaceSamples samples;
        if (!readSurfaceProfile(
                std::filesystem::path(std::u8string(reinterpret_cast<const char8_t *>(surfacePath.data()))),
                samples, error))
            return false;
        const double origin = zeroSurfaceX ? samples.front()[0] : 0;
        double sum = 0;
        for (auto &point : samples) {
            point[0] -= origin;
            sum += point[1];
        }
        const double datum = sum / static_cast<double>(samples.size());
        if (!std::isfinite(datum)) {
            error = "Nonfinite elevation datum.";
            return false;
        }
        staged.surfaceSamples = std::move(samples);
        terrainPreset = -1;
        terrainPresetSource.clear();
        staged.surfaceDatum = datum;
        staged.importedSurface = true;
        updateTerrainPreview();
        status = editorLabel("Profile loaded; review span and datum, then reset / apply.",
                             "高程剖面已载入；检查覆盖范围和高程基准后，重置并应用。");
        return true;
    }
    void modelDialogs() {
        const char *layersTitle =
            editorLabel("Layered material editor###2DLayerEditor", "分层介质编辑###2DLayerEditor");
        if (openLayers) {
            layerDraft = staged.layers;
            layerError.clear();
            layerPreset = -1;
            ImGui::OpenPopup(layersTitle);
            openLayers = false;
        }
        const auto *vp = ImGui::GetMainViewport();
        const bool resized = dialogViewport.x != vp->Size.x || dialogViewport.y != vp->Size.y;
        dialogViewport = vp->Size;
        ImGui::SetNextWindowPos(vp->GetCenter(), resized ? ImGuiCond_Always : ImGuiCond_Appearing,
                                {.5f, .5f});
        ImGui::SetNextWindowSize({std::min(vp->Size.x - 40, 1020.f), std::min(vp->Size.y - 70, 660.f)},
                                 ImGuiCond_Always);
        if (ImGui::BeginPopupModal(layersTitle, nullptr, ImGuiWindowFlags_NoSavedSettings)) {
            ImGui::SetNextItemWidth(-1);
            const auto &presets = materialPresets();
            if (ImGui::BeginCombo("##layerPreset",
                                  layerPreset < 0
                                      ? editorLabel("Choose a material preset", "选择介质预设")
                                      : editorLabel(presets[layerPreset].en, presets[layerPreset].zh))) {
                for (int i = 0; i < static_cast<int>(presets.size()); ++i)
                    if (ImGui::Selectable(editorLabel(presets[i].en, presets[i].zh), i == layerPreset)) {
                        layerPreset = i;
                        layerDraft = presets[i].layers;
                    }
                ImGui::EndCombo();
            }
            if (acoustic)
                ImGui::TextWrapped(
                    "%s", editorLabel(
                              "Acoustic: Vp and density are used; Vs is retained only for elastic transfer.",
                              "声学仅使用 Vp 和密度；Vs 保留用于弹性模式转换。"));
            ImGui::TextWrapped(
                "%s",
                editorLabel("Thickness uses computational depth. The final layer fills all remaining depth.",
                            "厚度按计算深度定义，末层自动填充剩余深度；相同深度索引保持相同物性。"));
            const float tableHeight =
                std::max(100.f, ImGui::GetContentRegionAvail().y - ImGui::GetFontSize() * 6 - 48);
            if (ImGui::BeginTable("##layerTable", staged.attenuation.enabled ? 7 : 5,
                                  ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
                                      ImGuiTableFlags_SizingStretchSame,
                                  {0, tableHeight})) {
                for (const char *name :
                     {editorLabel("Layer", "地层"), editorLabel("Thickness (m)", "厚度（m）"), "Vp (m/s)",
                      "Vs (m/s)", editorLabel("Density (kg/m3)", "密度（kg/m³）")})
                    ImGui::TableSetupColumn(name);
                if (staged.attenuation.enabled) {
                    ImGui::TableSetupColumn("Qp");
                    ImGui::TableSetupColumn("Qs");
                }
                ImGui::TableSetupScrollFreeze(0, 1);
                ImGui::TableHeadersRow();
                for (std::size_t i = 0; i < layerDraft.size(); ++i) {
                    ImGui::PushID(static_cast<int>(i));
                    auto &v = layerDraft[i];
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::Text("%zu", i + 1);
                    for (int c = 0; c < (staged.attenuation.enabled ? 6 : 4); ++c) {
                        ImGui::TableNextColumn();
                        ImGui::SetNextItemWidth(-1);
                        if (c == 0 && i + 1 == layerDraft.size())
                            ImGui::TextUnformatted(editorLabel("Auto", "自动"));
                        else {
                            double *value = c == 0   ? &v.thickness
                                            : c == 1 ? &v.vp
                                            : c == 2 ? &v.vs
                                            : c == 3 ? &v.density
                                            : c == 4 ? &v.qp
                                                     : &v.qs;
                            ImGui::PushID(c);
                            ImGui::InputDouble("##value", value, 0, 0, "%.2f");
                            ImGui::PopID();
                        }
                    }
                    ImGui::PopID();
                }
                ImGui::EndTable();
            }
            ImGui::BeginDisabled(layerDraft.size() >= 16);
            if (editorButton("Add layer", "增加地层", FunctionRole3D::Model)) {
                if (!layerDraft.empty())
                    layerDraft.back().thickness = 20;
                layerDraft.push_back({0, 4000, 2300, 2650});
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::BeginDisabled(layerDraft.size() <= 1);
            if (editorButton("Remove last", "删除末层", FunctionRole3D::Display)) {
                layerDraft.pop_back();
                layerDraft.back().thickness = 0;
            }
            ImGui::EndDisabled();
            double vs = 1e100;
            for (const auto &v : layerDraft)
                vs = std::min(vs, acoustic ? v.vp : v.vs);
            ImGui::TextWrapped(
                "%s %.2f | h <= %.3f m",
                editorLabel(acoustic ? "Minimum Vp" : "Minimum Vs", acoustic ? "最低 Vp" : "最低 Vs"), vs,
                vs / ((acoustic ? 12 : 20) * std::max(1.0, staged.frequency)));
            if (staged.attenuation.enabled)
                ImGui::TextWrapped("%s",
                                   editorLabel("These are reference-frequency velocities; review the "
                                               "dispersion-aware sampling diagnostic after rebuilding.",
                                               "这里使用参考频率速度；重建后请核对考虑频散的实际采样指标。"));
            if (!layerError.empty())
                ImGui::TextWrapped("%s", layerError.c_str());
            if (editorButton("Use layers", "采用层表", FunctionRole3D::Run)) {
                layerError.clear();
                bool valid = !layerDraft.empty();
                for (std::size_t i = 0; i < layerDraft.size(); ++i) {
                    const auto &v = layerDraft[i];
                    valid &= std::isfinite(v.thickness) && std::isfinite(v.vp) && std::isfinite(v.vs) &&
                             std::isfinite(v.density) && (acoustic ? v.vs >= 0 : v.vs > 0) && v.vp > 0 &&
                             v.density > 0 && std::isfinite(v.density * v.vp * v.vp) &&
                             (acoustic || v.vp * v.vp > 4 * v.vs * v.vs / 3) &&
                             (i + 1 == layerDraft.size() || v.thickness > 0);
                    if (valid && staged.attenuation.enabled) {
                        SeisWave::Rheology::Material m;
                        valid = SeisWave::Rheology::fitMaterial(v.vp, v.vs, v.density, v.qp, v.qs,
                                                                staged.attenuation, m, layerError);
                    }
                }
                if (valid) {
                    layerDraft.back().thickness = 0;
                    staged.layers = layerDraft;
                    staged.layered = true;
                    ImGui::CloseCurrentPopup();
                } else if (layerError.empty())
                    layerError =
                        editorLabel("Require finite positive material values, positive bulk modulus and "
                                    "positive finite-layer thickness.",
                                    "物性必须为有限正值、体积模量必须为正，非末层厚度必须大于零。");
            }
            layerUseRect = {ImGui::GetItemRectMin(), ImGui::GetItemRectMax()};
            ImGui::SameLine();
            if (editorButton("Cancel layers", "取消", FunctionRole3D::Display))
                ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
        const char *terrainTitle =
            editorLabel("External surface profile###2DTerrain", "外部地表高程###2DTerrain");
        if (openTerrain) {
            updateTerrainPreview();
            ImGui::OpenPopup(terrainTitle);
            openTerrain = false;
        }
        ImGui::SetNextWindowPos(vp->GetCenter(), resized ? ImGuiCond_Always : ImGuiCond_Appearing,
                                {.5f, .5f});
        ImGui::SetNextWindowSize({std::min(vp->Size.x - 40, 1020.f), std::min(vp->Size.y - 70, 800.f)},
                                 ImGuiCond_Always);
        if (ImGui::BeginPopupModal(terrainTitle, nullptr, ImGuiWindowFlags_NoSavedSettings)) {
            ImGui::BeginChild("##terrainFields", {0, -ImGui::GetFrameHeightWithSpacing() - 12},
                              ImGuiChildFlags_None, ImGuiWindowFlags_AlwaysVerticalScrollbar);
            ImGui::TextWrapped("%s",
                               editorLabel("CSV/TXT: x_m, elevation_m (positive up), optional header. "
                                           "Coordinates must increase strictly. PCHIP interpolation is used.",
                                           "CSV/TXT 两列：x_m, elevation_m（高程向上为正），可有表头；X "
                                           "必须严格递增。采用保形三次 Hermite 插值。"));
            terrainPresetControls();
            ImGui::SetNextItemWidth(-1);
            if (ImGui::BeginCombo("##terrainExample",
                                  editorLabel("Choose an elevation example…", "选择示例高程…"))) {
                for (const auto &example : examples)
                    if (ImGui::Selectable(editorLabel(example.en, example.zh))) {
                        const auto path = (SeisWave::Ui::elevationExampleRoot() / example.file).u8string();
                        if (path.size() < surfacePath.size()) {
                            std::snprintf(surfacePath.data(), surfacePath.size(), "%s",
                                          reinterpret_cast<const char *>(path.c_str()));
                            loadTerrain();
                        } else
                            error = editorLabel("Example path is too long.", "示例路径过长。");
                    }
                ImGui::EndCombo();
            }
            ImGui::SetNextItemWidth(-1);
            ImGui::InputText("##surfacePath", surfacePath.data(), surfacePath.size());
            if (editorButton("Browse terrain", "选择高程文件", FunctionRole3D::Model))
                if (browseTerrain)
                    browseTerrain();
            ImGui::SameLine();
            if (editorButton("Load terrain", "载入高程", FunctionRole3D::Model))
                loadTerrain();
            ImGui::Checkbox(editorLabel("Shift first X to zero on import", "导入时将首点 X 平移至零"),
                            &zeroSurfaceX);
            if (!staged.surfaceSamples.empty()) {
                ImGui::Checkbox(editorLabel("Use imported surface", "使用外部高程"), &staged.importedSurface);
                ImGui::TextUnformatted(editorLabel("Elevation datum (m)", "高程基准（m）"));
                ImGui::SetNextItemWidth(-1);
                ImGui::InputDouble("##elevationDatum", &staged.surfaceDatum, 1, 10, "%.3f");
                const auto &v = staged.surfaceSamples;
                ImGui::TextWrapped("%zu %s | X %.3f .. %.3f m | %s %.3f m", v.size(),
                                   editorLabel("samples", "个样点"), v.front()[0], v.back()[0],
                                   editorLabel("required span", "所需覆盖"),
                                   (staged.nx - 1) * staged.spacing);
                if (terrainPreset >= 0)
                    ImGui::TextWrapped(
                        "%s",
                        editorLabel(
                            "Preset datum starts at 0 m; width adaptation preserves your datum. "
                            "Model depth z = datum - elevation. Reset / apply rebuilds geometry and dt.",
                            "预设基准初始为 0 m；宽度适配保留已设基准。模型深度 "
                            "z=基准−高程，重置并应用会重建几何与 dt。"));
                else
                    ImGui::TextWrapped(
                        "%s", editorLabel("Initial datum is mean elevation. Model depth z = datum - "
                                          "elevation. Reset / apply rebuilds geometry, normals and dt.",
                                          "初始基准为样点高程平均值，模型深度 "
                                          "z=基准−高程。重置并应用会重新生成网格、法向和 dt。"));
                const auto &x = terrainPreviewX, &z = terrainPreviewZ;
                if (ImPlot::BeginPlot("##surfacePreview",
                                      {-1, std::max(180.f, ImGui::GetContentRegionAvail().y - 12)},
                                      ImPlotFlags_NoLegend)) {
                    ImPlot::SetupAxes("X (m)", editorLabel("Elevation (m)", "高程（m）"));
                    ImPlot::SetupAxisTicks(ImAxis_Y1, *std::min_element(z.begin(), z.end()) - 1,
                                           *std::max_element(z.begin(), z.end()) + 1, 3);
                    ImPlot::SetupAxesLimits(v.front()[0], v.back()[0],
                                            *std::min_element(z.begin(), z.end()) - 1,
                                            *std::max_element(z.begin(), z.end()) + 1, ImGuiCond_Always);
                    ImPlot::PlotLine("Profile", x.data(), z.data(), static_cast<int>(x.size()));
                    ImPlot::EndPlot();
                }
            }
            if (!error.empty())
                ImGui::TextWrapped("%s", error.c_str());
            ImGui::EndChild();
            if (editorButton("Done terrain", "完成", FunctionRole3D::Run))
                ImGui::CloseCurrentPopup();
            terrainDoneRect = {ImGui::GetItemRectMin(), ImGui::GetItemRectMax()};
            ImGui::EndPopup();
        }
    }
};
} // namespace SeisWave::Ui
