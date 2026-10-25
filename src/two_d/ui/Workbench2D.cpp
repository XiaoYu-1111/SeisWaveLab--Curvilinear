#include "Workbench2D.hpp"
#include "AppRuntime.hpp"
#include "GlassMaterial.hpp"
#include "ParameterPanelHost.hpp"
#include "ThemeValidation.hpp"
#include "Presentation3D.hpp"
#include "RecordPalettes3D.hpp"
#include "RecordView.hpp"
#include "SeisCommon/SegyIo.hpp"
#include "SourceControls.hpp"
#ifdef _WIN32
// clang-format off
#include <windows.h>
#include <commdlg.h>
// clang-format on
#endif
#include "Seis2D/CurvilinearSolver2D.hpp"
#include "Seis2D/ModelPresets2D.hpp"
#include "Seis2D/ModelIo2D.hpp"
#include "Seis2D/SurfacePresets.hpp"
#include "imgui.h"
#include "imgui_internal.h"
#include "implot.h"
#include <GLFW/glfw3.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <iomanip>
#include <numeric>
#include <string_view>
#include <unordered_map>
namespace Seis2D::Curvilinear::Ui {
using namespace Seis3D::Curvilinear::Ui;
namespace {
struct ElevationExample {
    const char *file, *en, *zh;
};
constexpr ElevationExample elevationExamples[]{
    {"flat_2km.csv", "Flat surface / 2 km", "平地 / 2 km"},
    {"ridge_2km.csv", "Smooth ridge / 2 km", "平滑丘陵 / 2 km"},
    {"irregular_2km.csv", "Irregular samples / 2 km", "不等距采样 / 2 km"},
    {"valley_20km.csv", "Wide valley / 20 km", "宽域山谷 / 20 km"},
    {"hill_valley_1km.csv", "Hill + valley (+/-100 m) / 1 km", "山丘 + 沟谷（±100 m）/ 1 km"}};
const char *l(const char *en, const char *zh) {
    return language3D() ? zh : en;
}
std::string label(const char *en, const char *zh) {
    return std::string(l(en, zh)) + "###2D" + en;
}
bool button(const char *en, const char *zh, FunctionRole3D role, ImVec2 size = {0, 0}) {
    const auto text = label(en, zh);
    return functionButton3D(text.c_str(), role, size);
}
ImU32 paletteColor(double t, int palette) {
    const auto &stops = Seis3D::Curvilinear::RecordPalettes::wavePalette(palette);
    const double x = std::clamp(t, 0.0, 1.0) * 4;
    const int i = std::min(3, static_cast<int>(x));
    const float f = static_cast<float>(x - i);
    return ImGui::ColorConvertFloat4ToU32({stops[i][0] * (1 - f) + stops[i + 1][0] * f,
                                           stops[i][1] * (1 - f) + stops[i + 1][1] * f,
                                           stops[i][2] * (1 - f) + stops[i + 1][2] * f, 1});
}
void quad(ImDrawList *draw, const ImVec2 *points, const ImU32 *colors) {
    draw->PrimReserve(6, 4);
    const auto base = static_cast<ImDrawIdx>(draw->_VtxCurrentIdx);
    for (int i : {0, 1, 2, 0, 2, 3})
        draw->PrimWriteIdx(static_cast<ImDrawIdx>(base + i));
    for (int i = 0; i < 4; ++i)
        draw->PrimWriteVtx(points[i], ImGui::GetFontTexUvWhitePixel(), colors[i]);
}
void fitAcquisition(Parameters &p) {
    p.sourceX = p.sourceX < 0 ? -1 : std::clamp(p.sourceX, 2, std::max(2, p.nx - 3));
    const int margin =
        p.source.kind == SeisWave::SourceKind::IsotropicMoment && p.nx >= 11 && p.nz >= 11 ? 5 : 2;
    p.sourceX = p.sourceX < 0 ? -1 : std::clamp(p.sourceX, margin, std::max(margin, p.nx - 1 - margin));
    p.sourceZ = std::clamp(p.sourceZ, margin, std::max(margin, p.nz - 1 - margin));
    p.receiverStart = std::clamp(p.receiverStart, 0, std::max(0, p.nx - 1));
    p.receiverDepth = std::clamp(p.receiverDepth, 0, std::max(0, p.nz - 1));
    p.receiverStride = std::max(1, p.receiverStride);
    p.receiverCount =
        std::max(1, std::min({p.receiverCount, 1024, 1 + (p.nx - 1 - p.receiverStart) / p.receiverStride}));
}
struct Workbench {
    Simulation simulation;
    Parameters staged;
    ComputeSettings3D settings, activeSettings;
    int selectedBackend{}, category{}, component{}, wavePalette{1}, modelPalette{1}, modelComponent{},
        selectedReceiver{};
    bool running{}, home{}, help{}, fit{true}, grid{true}, sponge{}, normalized{true}, showWave{true},
        showModel{true};
    float gain{1};
    std::string error, status;
    std::vector<float> frame;
    std::vector<double> terrainPreviewX, terrainPreviewZ;
    std::array<char, 2048> vts{}, csv{}, segy{}, importPath{}, surfacePath{}, modelPath{};
    std::vector<Layer> layerDraft;
    bool openLayers{}, openTerrain{}, zeroSurfaceX{true};
    int layerPreset{-1};
    int terrainPreset{-1}, activeTerrainPreset{-1};
    SurfaceSamples terrainPresetSource, activeTerrainPresetSource;
    int terrainResampledNx{};
    double terrainResampledSpacing{};
    std::string layerError;
    ImRect layerEditRect, layerUseRect, terrainDoneRect;
    int renderStrideX{1}, renderStrideZ{1};
    ImVec2 dialogViewport{};
    SeisWave::Ui::RecordView recordView;
    SeisWave::ImportedRecords imported;
    bool useImported{};
    int segyProfile{};
    double segyGain{1}, segyIntervalMs{.5};
    std::array<bool, 3> exportComponents{true, false, false};
    bool snapshotFollowDisplay{true}, snapshotMaterial{};
    std::array<bool, 7> snapshotComponents{false, true, false, false, false, false, false};
    std::uint64_t recordGeneration{1}, importedRevision{1};
    ImRect rebuildRect, frequencyRect, sourceKindRect, sourceMomentRect;
    ImRect snapshotToggleRect, snapshotSaveRect;
    std::array<ImRect, 7> snapshotComponentRects;
    int lastHeight{};
    Seis3D::Curvilinear::CudaDeviceInventory3D inventory;
    ImRect homeRect;
    bool layout{true}, domainVisible{}, focusParameters{};
    int lastWidth{};
    float recordHeight{};
    SeisWave::Ui::ParameterPanelHost parameterHost{2};
    int floatingCategory{},lastParameterMode{};
    bool parameterWindow{true}, waveWindow{true}, recordWindow{true}, summaryWindow{true};
    int groupEpoch{};
    bool groupsExpanded{true};
    std::unordered_map<ImGuiID,int> groupApplied;
    bool parameterGroup(const char *title, ImGuiTreeNodeFlags flags = 0) {
        const auto id = ImGui::GetID(title);
        if (groupEpoch && groupApplied[id] != groupEpoch) {
            ImGui::SetNextItemOpen(groupsExpanded);
            groupApplied[id] = groupEpoch;
        }
        return ImGui::CollapsingHeader(title, flags);
    }
    Workbench() {
        inventory = Seis3D::Curvilinear::queryCudaDevices3D();
        selectedBackend = Seis3D::Curvilinear::selectCudaDevice3D(inventory).available ? 1 : 0;
        const auto output = SeisWave::Ui::outputRoot() / "2d";
        std::filesystem::create_directories(output);
        const auto setPath = [&](auto &field, const char *file) {
            const auto name = (output / file).u8string();
            std::snprintf(field.data(), field.size(), "%s", reinterpret_cast<const char *>(name.c_str()));
        };
        setPath(vts, "wavefield.vts"); setPath(modelPath, "model.vts");
        setPath(csv, "receivers.csv"); setPath(segy, "receivers.sgy");
    }
    bool apply() {
        running = false;
        if (!syncTerrainPreset())
            return false;
        if (!simulation.initialize(staged, error, static_cast<BackendKind3D>(selectedBackend), settings))
            return false;
        activeTerrainPreset = terrainPreset;
        activeTerrainPresetSource = terrainPresetSource;
        activeSettings = settings;
        ++recordGeneration;
        recordView.invalidate();
        useImported = false;
        running = false;
        fit = true;
        selectedReceiver =
            std::clamp(selectedReceiver, 0, static_cast<int>(simulation.receivers().size()) - 1);
        status = l("Fresh 2D session initialized.", "二维会话已从零时刻重建。");
        return true;
    }
    bool dirty() const {
        return staged != simulation.parameters() || settings != activeSettings ||
               selectedBackend != static_cast<int>(simulation.backend().kind);
    }
    SeisWave::RecordDataView liveRecords() const {
        SeisWave::RecordDataView data;
        data.dt = simulation.recordDt();
        data.revision = (recordGeneration << 32) + simulation.step() + 1;
        data.components = {"Vz", "Vx"};
        const auto &p = simulation.parameters();
        const auto source = simulation.grid().coordinates[simulation.grid().index(
            (p.sourceX < 0 ? p.nx / 2 : p.sourceX) + ghost, p.sourceZ + ghost)];
        for (const auto &receiver : simulation.receivers()) {
            SeisWave::TraceView t;
            t.position = {receiver.position[0], 0, receiver.position[1]};
            t.source = {source[0], 0, source[1]};
            t.channels[0] = receiver.vz;
            t.channels[1] = receiver.vx;
            data.traces.push_back(t);
        }
        return data;
    }
    SeisWave::RecordDataView displayedRecords() const {
        return useImported ? imported.view((1ULL << 63) + importedRevision) : liveRecords();
    }
    bool prepareRun(bool continuous) {
        if (dirty()) {
            if (!apply()) {
                running = false;
                return false;
            }
        } else if (simulation.completed()) {
            if (!simulation.reset(error))
                return false;
            ++recordGeneration;
            recordView.invalidate();
        }
        useImported = false;
        running = continuous;
        return true;
    }
    void clear() {
        running = false;
        if (simulation.reset(error)) {
            ++recordGeneration;
            recordView.invalidate();
        }
    }
    void importSegy() {
        if (SeisWave::readSegy(
                std::filesystem::path(std::u8string(reinterpret_cast<const char8_t *>(importPath.data()))),
                imported, error)) {
            useImported = true;
            ++importedRevision;
            recordView.invalidate();
            selectedReceiver = 0;
            recordWindow = true;
            status = l("SEG-Y loaded; simulation unchanged.", "SEG-Y 已载入，当前模拟保持不变。");
        }
    }
    void exportSegy() {
        if (!segy[0]) {
            error = l("Choose a SEG-Y file name.", "请选择 SEG-Y 文件名。");
            return;
        }
        const auto data = displayedRecords();
        int selected = 0;
        for (std::size_t c = 0; c < data.components.size(); ++c)
            selected += exportComponents[c];
        if (!selected) {
            error = l("Select at least one export component.", "请至少选择一个导出分量。");
            return;
        }
        auto path = std::filesystem::path(std::u8string(reinterpret_cast<const char8_t *>(segy.data())));
        if (path.extension().empty())
            path += ".sgy";
        for (std::size_t c = 0; c < data.components.size(); ++c)
            if (exportComponents[c]) {
                auto target = path;
                if (selected > 1)
                    target = path.parent_path() / (path.stem().native() +
                                                   std::filesystem::path("_" + data.components[c]).native() +
                                                   path.extension().native());
                SeisWave::SegyExportOptions options;
                options.profile = static_cast<SeisWave::SegyExportProfile>(segyProfile);
                options.sampleGain = segyGain;
                options.sampleIntervalSeconds = segyProfile >= 2 ? segyIntervalMs * .001 : 0;
                if (!SeisWave::writeSegy(data, static_cast<int>(c), target, error, options))
                    return;
            }
        status = segyGain == 1 ? l("SEG-Y exported without amplitude scaling; components use separate files.", "SEG-Y 已按原始振幅导出，多分量分别保存。")
                              : l("SEG-Y exported with the specified constant multiplier, recorded in the textual header.", "SEG-Y 已按指定固定倍率导出，倍率已写入文本头。" );
    }
    SnapshotExportOptions snapshotOptions() const {
        auto options = displayedSnapshotOptions(component);
        if (!snapshotFollowDisplay)
            options.components = snapshotComponents;
        options.includeMaterial = snapshotMaterial;
        return options;
    }
    bool exportWavefield() {
        auto path = std::filesystem::path(std::u8string(reinterpret_cast<const char8_t *>(vts.data())));
        if (path.extension().empty()) path += ".vts";
        if (!writeSnapshot(simulation, path, error, snapshotOptions()))
            return false;
        status = l("Selected raw Float64 wavefield components exported.", "所选波场分量已按原始 Float64 物理量导出。");
        return true;
    }
    void snapshotControls() {
        const bool changed = ImGui::Checkbox(l("Follow displayed component", "跟随当前显示分量"), &snapshotFollowDisplay);
        snapshotToggleRect = {ImGui::GetItemRectMin(), ImGui::GetItemRectMax()};
        if (changed && !snapshotFollowDisplay)
            snapshotComponents = displayedSnapshotOptions(component).components;
        if (!snapshotFollowDisplay) {
            const char *names[]{"Vx (m/s)", "Vz (m/s)", "|V| (m/s)", l("Pressure (Pa)", "压力（Pa）"),
                                "Sxx (Pa)", "Szz (Pa)", "Sxz (Pa)"};
            if (ImGui::BeginTable("##snapshotComponents", 2, ImGuiTableFlags_SizingStretchSame)) {
                for (int i = 0; i < 7; ++i) {
                    ImGui::TableNextColumn();
                    ImGui::Checkbox(names[i], &snapshotComponents[i]);
                    snapshotComponentRects[i] = {ImGui::GetItemRectMin(), ImGui::GetItemRectMax()};
                }
                ImGui::EndTable();
            }
            if (button("All wavefield components", "全选波场分量", FunctionRole3D::Display, {-1, 0}))
                snapshotComponents.fill(true);
        }
        ImGui::Checkbox(l("Include Vp / Vs / density", "附带 Vp / Vs / 密度"), &snapshotMaterial);
        const auto options = snapshotOptions();
        const char *names[]{"Vx", "Vz", "|V|", l("Pressure", "压力"), "Sxx", "Szz", "Sxz"};
        std::string selected;
        for (int i = 0; i < 7; ++i)
            if (options.components[i]) {
                if (!selected.empty()) selected += ", ";
                selected += names[i];
            }
        ImGui::TextWrapped("%s: %s", l("Export components", "将导出分量"),
                           selected.empty() ? l("None selected", "尚未选择") : selected.c_str());
        ImGui::TextWrapped("%s", l("Physical Float64 values; display gain, palette and normalization are excluded.",
                                   "保存原始 Float64 物理值，不应用显示增益、色带或归一化。"));
        ImGui::SetNextItemWidth(-1);
        ImGui::InputText("##2Dvts", vts.data(), vts.size());
        if (button("Snapshot save location", "选择快照保存位置", FunctionRole3D::Export, {-1, 0}))
            browse(vts, true, 2);
        ImGui::BeginDisabled(selected.empty());
        if (button("Export VTS snapshot", "导出 VTS 快照", FunctionRole3D::Export, {-1, 0}))
            exportWavefield();
        snapshotSaveRect = {ImGui::GetItemRectMin(), ImGui::GetItemRectMax()};
        ImGui::EndDisabled();
    }
    bool loadModel() {
        ModelDocument document;
        const auto path = std::filesystem::path(std::u8string(reinterpret_cast<const char8_t *>(modelPath.data())));
        if (!readModelVts(path, document, error))
            return false;
        const auto oldParameters = staged;
        const auto oldSettings = settings;
        const int oldBackend = selectedBackend;
        const int oldTerrainPreset = terrainPreset;
        terrainPreset = -1; // A saved model owns its explicit surface samples.
        staged = document.parameters;
        settings = document.compute;
        // GPU ordinal is machine-local. Resolve an available device on this machine again.
        settings.cudaDevice = -1;
        const bool fallback = document.backend == BackendKind3D::Cuda &&
                              !Seis3D::Curvilinear::queryBackend3D(BackendKind3D::Cuda).available;
        selectedBackend = fallback ? 0 : static_cast<int>(document.backend);
        if (!apply()) {
            staged = oldParameters;
            settings = oldSettings;
            selectedBackend = oldBackend;
            terrainPreset = oldTerrainPreset;
            return false;
        }
        terrainPresetSource.clear();
        updateTerrainPreview();
        status = fallback ? l("Model rebuilt at t=0 on CPU (CUDA unavailable here).", "模型已从 t=0 重建；本机 CUDA 不可用，采用 CPU。")
                          : l("Model and all simulation parameters restored at t=0.", "模型及全部模拟参数已恢复，从 t=0 开始。" );
        if (std::abs(simulation.dt() - document.referenceDt) > 1e-10 * document.referenceDt)
            status += l(" Recomputed dt differs from the saved reference; check the active panel.",
                        " 重新计算的 dt 与保存参考值不同，请核对生效参数面板。" );
        return true;
    }
    void modelFiles() {
        if (!parameterGroup(l("Model save / restore (.vts)", "模型保存与复现（.vts）"),
                                    ImGuiTreeNodeFlags_DefaultOpen))
            return;
        ImGui::TextWrapped("%s", l("Self-contained geometry, materials and parameters; load starts at t=0.",
                                   "保存曲网格、物性及全部参数；载入后从零时刻重建。"));
        ImGui::SetNextItemWidth(-1);
        ImGui::InputText("##2DmodelPath", modelPath.data(), modelPath.size());
        if (button("Browse model", "选择模型文件", FunctionRole3D::Model))
            browse(modelPath, false, 2);
        ImGui::SameLine();
        if (button("Load model", "载入模型", FunctionRole3D::Model))
            loadModel();
        if (button("Model save location", "选择保存位置", FunctionRole3D::Export, {-1, 0}))
            browse(modelPath, true, 2);
        ImGui::BeginDisabled(dirty());
        if (button("Save active model VTS", "保存当前生效模型 VTS", FunctionRole3D::Export, {-1, 0})) {
            auto path = std::filesystem::path(std::u8string(reinterpret_cast<const char8_t *>(modelPath.data())));
            if (path.extension().empty()) path += ".vts";
            if (writeModelVts(simulation, path, activeSettings, error))
                status = l("Reproducible model VTS saved.", "可复现模型 VTS 已保存。" );
        }
        ImGui::EndDisabled();
        if (dirty())
            ImGui::TextWrapped("%s", l("Apply pending edits before saving a model.", "存在待应用修改，请先重置并应用后保存模型。"));
    }
    void samplingControls() {
        ImGui::TextUnformatted(l("Record sampling interval", "记录采样间隔"));
        const double intervals[]{0, .5, 1, 2};
        int selected = 4;
        for (int i = 0; i < 4; ++i)
            if (staged.sampleIntervalMs == intervals[i]) selected = i;
        ImGui::SetNextItemWidth(-1);
        if (ImGui::Combo("##2DSamplePreset", &selected,
                         l("Each solver step\0" "0.5 ms\0" "1 ms\0" "2 ms\0Custom\0",
                           "每个计算步\0" "0.5 ms\0" "1 ms\0" "2 ms\0自定义\0")))
            staged.sampleIntervalMs = selected < 4 ? intervals[selected] : 1.5;
        if (selected == 4) {
            ImGui::SetNextItemWidth(-1);
            ImGui::InputDouble(l("ms###2DSampleMs", "毫秒###2DSampleMs"), &staged.sampleIntervalMs, .1, 1, "%.5f");
        }
        ImGui::TextWrapped("%s", l("Uniform output times; linear interpolation between accepted steps. The solver dt is unchanged.",
                                   "按均匀时刻记录，步间线性插值；不改变求解 dt。"));
    }
    void browse(std::array<char, 2048> &path, bool save, int format = 0) {
#ifdef _WIN32
        std::array<wchar_t, 32768> name{};
        OPENFILENAMEW dialog{};
        dialog.lStructSize = sizeof(dialog);
        dialog.lpstrFile = name.data();
        dialog.nMaxFile = static_cast<DWORD>(name.size());
        dialog.lpstrFilter = format == 2 ? L"SeisWave model (*.vts)\0*.vts\0All files\0*.*\0"
                            : format == 1 ? L"Surface profile (*.csv;*.txt)\0*.csv;*.txt\0All files\0*.*\0"
                                          : L"SEG-Y (*.sgy;*.segy)\0*.sgy;*.segy\0All files\0*.*\0";
        dialog.lpstrDefExt = format == 2 ? L"vts" : format == 1 ? L"csv" : L"sgy";
        dialog.Flags = OFN_EXPLORER | OFN_PATHMUSTEXIST | (save ? OFN_OVERWRITEPROMPT : OFN_FILEMUSTEXIST);
        if (save ? GetSaveFileNameW(&dialog) : GetOpenFileNameW(&dialog)) {
            const auto utf8 = std::filesystem::path(name.data()).u8string();
            if (utf8.size() >= path.size()) {
                error = "Selected path is too long.";
                return;
            }
            std::snprintf(path.data(), path.size(), "%s", reinterpret_cast<const char *>(utf8.c_str()));
        }
#else
        (void)path;
        (void)save;
        (void)format;
#endif
    }
    void toolbar() {
        if (ImGui::BeginMainMenuBar()) {
            if (button("Home", "主页", FunctionRole3D::Model))
                home = true;
            homeRect = {ImGui::GetItemRectMin(), ImGui::GetItemRectMax()};
            if (ImGui::BeginMenu(l("Workspace", "工作区"))) {
                ImGui::MenuItem(l("Parameters", "参数面板"), nullptr, &parameterWindow);
                ImGui::MenuItem(l("Active parameters", "当前生效参数"), nullptr, &summaryWindow);
                ImGui::MenuItem(l("Wavefield", "波场窗口"), nullptr, &waveWindow);
                ImGui::MenuItem(l("Receivers", "接收记录"), nullptr, &recordWindow);
                if (ImGui::MenuItem(l("Restore layout [F]", "恢复布局 [F]"))) {
                    SeisWave::Ui::setParameterDisplayMode(2,0);
                    parameterWindow = summaryWindow = waveWindow = recordWindow = layout = true;
                }
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu(l("Appearance", "界面外观"))) {
                int language = language3D();
                ImGui::SetNextItemWidth(190);
                if (ImGui::Combo("##language2D", &language, "English\0简体中文\0"))
                    setLanguage3D(language);
                ImGui::SetNextItemWidth(230);
                if (ImGui::BeginCombo("##theme2D", themeName3D(themeIndex3D()))) {
                    for (int i = 0; i < themeCount3D; ++i)
                        if (ImGui::Selectable(themeName3D(i), themeIndex3D() == i))
                            applyTheme3D(i);
                    ImGui::EndCombo();
                }
                float font = SeisWave::Ui::interfaceFontSize();
                ImGui::SetNextItemWidth(230);
                if (ImGui::SliderFloat(l("Font size (px)", "字体大小（px）"), &font, 16, 28, "%.0f")) {
                    SeisWave::Ui::setInterfaceFontSize(font);
                    layout = true;
                }
                glassAppearanceControls3D();
                ImGui::EndMenu();
            }
            if(ImGui::BeginMenu(l("Parameter panels","参数面板"))){if(parameterHost.menu()){parameterWindow=true;layout=true;}ImGui::EndMenu();}
            if (button("Help [F1]", "帮助 [F1]", FunctionRole3D::Display))
                help = !help;
            ImGui::EndMainMenuBar();
        }
        const auto *vp = ImGui::GetMainViewport();
        const float height = ImGui::GetFontSize() * (vp->WorkSize.x < 1250 ? 2 : 1) + 18;
        ImGui::SetNextWindowPos(vp->WorkPos);
        ImGui::SetNextWindowSize({vp->WorkSize.x, height});
        ImGui::Begin("##2DCommands", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoMove |
                         ImGuiWindowFlags_NoSavedSettings);
        if (button(running ? "Pause [Space]" : "Run [Space]", running ? "暂停 [Space]" : "运行 [Space]",
                   FunctionRole3D::Run)) {
            if (running)
                running = false;
            else
                prepareRun(true);
        }
        ImGui::SameLine();
        if (button("Step [N]", "单步 [N]", FunctionRole3D::Run) && prepareRun(false))
            simulation.advance(1, error);
        ImGui::SameLine();
        if (button("Clear [C]", "清空 [C]", FunctionRole3D::Danger))
            clear();
        ImGui::SameLine();
        if (button("Fit [R]", "适配 [R]", FunctionRole3D::Display))
            fit = true;
        if (ImGui::GetContentRegionAvail().x < 180)
            ImGui::NewLine();
        else
            ImGui::SameLine();
        char progress[160];
        std::snprintf(progress, sizeof(progress), "%d / %d (%.1f%%) | t %.4f s | dt %.5f ms", simulation.step(),
                      simulation.totalSteps(), 100.0 * simulation.step() / std::max(1, simulation.totalSteps()),
                      simulation.step() * simulation.dt(), simulation.dt() * 1000);
        ImGui::ProgressBar(static_cast<float>(simulation.step()) / std::max(1, simulation.totalSteps()),
                           {-1, ImGui::GetFrameHeight()}, progress);
        ImGui::End();
        const ImVec2 pos{vp->WorkPos.x, vp->WorkPos.y + height};
        ImGui::SetNextWindowPos(pos);
        ImGui::SetNextWindowSize({vp->WorkSize.x, vp->WorkSize.y - height});
        ImGui::Begin("##2DDockHost", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoDocking |
                         ImGuiWindowFlags_NoSavedSettings);
        const auto id = ImGui::GetID("2DDock");
        ImGui::DockSpace(id, {0, 0});
        if(lastParameterMode!=parameterHost.mode()){lastParameterMode=parameterHost.mode();layout=true;}
        if (layout || lastWidth != static_cast<int>(vp->Size.x) ||
            lastHeight != static_cast<int>(vp->Size.y)) {
            fit = true;
            ImGui::DockBuilderRemoveNode(id);
            ImGui::DockBuilderAddNode(id, ImGuiDockNodeFlags_DockSpace);
            ImGui::DockBuilderSetNodeSize(id, {vp->WorkSize.x - 24, vp->WorkSize.y - height - 20});
            ImGuiID main = id, left{}, records{};
            if(parameterHost.mode()!=1)ImGui::DockBuilderSplitNode(main, ImGuiDir_Left,
                                        std::clamp(ImGui::GetFontSize() * 20.f / vp->Size.x, .25f, .40f),
                                        &left, &main);
            focusParameters = true;
            const float recordRatio =
                std::clamp((ImGui::GetFontSize() * 7 + 110) / vp->WorkSize.y, .31f, .50f);
            ImGui::DockBuilderSplitNode(main, ImGuiDir_Down, recordRatio, &records, &main);
            if(parameterHost.mode()!=1)ImGui::DockBuilderDockWindow(label("Parameters and run", "参数与运行").c_str(), left);
            if(parameterHost.mode()!=1)ImGui::DockBuilderDockWindow(label("Active simulation parameters", "当前模拟参数").c_str(), left);
            ImGui::DockBuilderDockWindow(label("Curved X-Z wavefield", "曲面 X–Z 波场").c_str(), main);
            ImGui::DockBuilderDockWindow(label("Receiver records", "接收记录").c_str(), records);
            ImGui::DockBuilderFinish(id);
            layout = false;
            lastWidth = static_cast<int>(vp->Size.x);
            lastHeight = static_cast<int>(vp->Size.y);
        }
        ImGui::End();
    }
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
            if (staged.nx < 8 || staged.nx > maxAxisNodes || !std::isfinite(staged.spacing) || staged.spacing <= 0)
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
            if (staged.nx < 8 || staged.nx > maxAxisNodes || !std::isfinite(staged.spacing) || staged.spacing <= 0)
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
            status = l("Terrain preset staged; X follows model width, elevation is unchanged. Reset / apply.",
                       "地表预设已选择，水平范围随模型宽度适配，高程保持不变；重置并应用后生效。");
            return true;
        } catch (const std::exception &e) {
            error = e.what();
            return false;
        }
    }
    bool setTerrainPresetWidth(double width) {
        const double intervals = width / staged.spacing;
        if (terrainPreset < 0 || !std::isfinite(width) || width <= 0 ||
            !std::isfinite(staged.spacing) || staged.spacing <= 0 ||
            !std::isfinite(intervals) || intervals < 7 || intervals > maxAxisNodes - 1) {
            error = l("This width requires X nodes outside 8..16384 or invalid spacing.",
                      "当前宽度所需 X 节点超出 8..16384，或网格间距无效。");
            return false;
        }
        staged.nx = static_cast<int>(std::round(intervals)) + 1;
        fitAcquisition(staged);
        if (!syncTerrainPreset())
            return false;
        error.clear();
        return true;
    }
    void terrainPresetControls() {
        ImGui::TextUnformatted(l("Surface presets (fit model width)", "地表预设（适配模型宽度）"));
        ImGui::SetNextItemWidth(-1);
        if (ImGui::BeginCombo("##surfacePreset", terrainPreset < 0 ? l("Custom / file / analytic", "自定义 / 文件 / 解析地表")
                                                                       : l(surfacePresets[terrainPreset].en, surfacePresets[terrainPreset].zh))) {
            if (ImGui::Selectable(l("Custom / file / analytic", "自定义 / 文件 / 解析地表"), terrainPreset < 0)) {
                terrainPreset = -1;
                terrainPresetSource.clear(); // Freeze current samples; do not stretch imported measurements.
            }
            for (int i = 0; i < static_cast<int>(surfacePresets.size()); ++i)
                if (ImGui::Selectable(l(surfacePresets[i].en, surfacePresets[i].zh), terrainPreset == i))
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
        ImGui::TextWrapped("%s", l("Original 1 km shape is stretched horizontally and PCHIP-sampled at grid nodes. "
                                   "Elevation and datum stay unchanged; slopes change. Width buttons keep spacing and round X nodes.",
                                   "由原始 1 km 数据水平拉伸，并按网格节点保形插值。高程、基准保持不变，坡度会变化；宽度按钮保持间距，X 节点取整，以待应用尺寸为准。"));
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
        status = l("Profile loaded; review span and datum, then reset / apply.",
                   "高程剖面已载入；检查覆盖范围和高程基准后，重置并应用。");
        return true;
    }
    void modelDialogs() {
        const char *layersTitle =
            l("Layered material editor###2DLayerEditor", "分层介质编辑###2DLayerEditor");
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
                                  layerPreset < 0 ? l("Choose a material preset", "选择介质预设")
                                                  : l(presets[layerPreset].en, presets[layerPreset].zh))) {
                for (int i = 0; i < static_cast<int>(presets.size()); ++i)
                    if (ImGui::Selectable(l(presets[i].en, presets[i].zh), i == layerPreset)) {
                        layerPreset = i;
                        layerDraft = presets[i].layers;
                    }
                ImGui::EndCombo();
            }
            ImGui::TextWrapped(
                "%s", l("Thickness uses computational depth. The final layer fills all remaining depth.",
                        "厚度按计算深度定义，末层自动填充剩余深度；相同深度索引保持相同物性。"));
            const float tableHeight =
                std::max(100.f, ImGui::GetContentRegionAvail().y - ImGui::GetFontSize() * 6 - 48);
            if (ImGui::BeginTable("##layerTable", 5,
                                  ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
                                      ImGuiTableFlags_SizingStretchSame,
                                  {0, tableHeight})) {
                for (const char *name : {l("Layer", "地层"), l("Thickness (m)", "厚度（m）"), "Vp (m/s)",
                                         "Vs (m/s)", l("Density (kg/m3)", "密度（kg/m³）")})
                    ImGui::TableSetupColumn(name);
                ImGui::TableSetupScrollFreeze(0, 1);
                ImGui::TableHeadersRow();
                for (std::size_t i = 0; i < layerDraft.size(); ++i) {
                    ImGui::PushID(static_cast<int>(i));
                    auto &v = layerDraft[i];
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::Text("%zu", i + 1);
                    for (int c = 0; c < 4; ++c) {
                        ImGui::TableNextColumn();
                        ImGui::SetNextItemWidth(-1);
                        if (c == 0 && i + 1 == layerDraft.size())
                            ImGui::TextUnformatted(l("Auto", "自动"));
                        else {
                            double *value = c == 0   ? &v.thickness
                                            : c == 1 ? &v.vp
                                            : c == 2 ? &v.vs
                                                     : &v.density;
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
            if (button("Add layer", "增加地层", FunctionRole3D::Model)) {
                if (!layerDraft.empty())
                    layerDraft.back().thickness = 20;
                layerDraft.push_back({0, 4000, 2300, 2650});
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::BeginDisabled(layerDraft.size() <= 1);
            if (button("Remove last", "删除末层", FunctionRole3D::Display)) {
                layerDraft.pop_back();
                layerDraft.back().thickness = 0;
            }
            ImGui::EndDisabled();
            double vs = 1e100;
            for (const auto &v : layerDraft)
                vs = std::min(vs, v.vs);
            ImGui::TextWrapped("%s %.2f | h <= %.3f m (8 PPW, 2.5 f0)", l("Minimum Vs", "最低 Vs"), vs,
                               vs / (20 * std::max(1.0, staged.frequency)));
            if (!layerError.empty())
                ImGui::TextWrapped("%s", layerError.c_str());
            if (button("Use layers", "采用层表", FunctionRole3D::Run)) {
                bool valid = !layerDraft.empty();
                for (std::size_t i = 0; i < layerDraft.size(); ++i) {
                    const auto &v = layerDraft[i];
                    valid &= std::isfinite(v.thickness) && std::isfinite(v.vp) && std::isfinite(v.vs) &&
                             std::isfinite(v.density) && v.vs > 0 && v.vp > 0 && v.density > 0 &&
                             std::isfinite(v.density * v.vp * v.vp) && v.vp * v.vp > 4 * v.vs * v.vs / 3 &&
                             (i + 1 == layerDraft.size() || v.thickness > 0);
                }
                if (valid) {
                    layerDraft.back().thickness = 0;
                    staged.layers = layerDraft;
                    staged.layered = true;
                    ImGui::CloseCurrentPopup();
                } else
                    layerError = l("Require finite positive material values, positive bulk modulus and "
                                   "positive finite-layer thickness.",
                                   "物性必须为有限正值、体积模量必须为正，非末层厚度必须大于零。");
            }
            layerUseRect = {ImGui::GetItemRectMin(), ImGui::GetItemRectMax()};
            ImGui::SameLine();
            if (button("Cancel layers", "取消", FunctionRole3D::Display))
                ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
        const char *terrainTitle = l("External surface profile###2DTerrain", "外部地表高程###2DTerrain");
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
            ImGui::TextWrapped("%s", l("CSV/TXT: x_m, elevation_m (positive up), optional header. "
                                       "Coordinates must increase strictly. PCHIP interpolation is used.",
                                       "CSV/TXT 两列：x_m, elevation_m（高程向上为正），可有表头；X "
                                       "必须严格递增。采用保形三次 Hermite 插值。"));
            terrainPresetControls();
            ImGui::SetNextItemWidth(-1);
            if (ImGui::BeginCombo("##terrainExample", l("Choose an elevation example…", "选择示例高程…"))) {
                for (const auto &example : elevationExamples)
                    if (ImGui::Selectable(l(example.en, example.zh))) {
                        const auto path = (SeisWave::Ui::elevationExampleRoot() / example.file).u8string();
                        if (path.size() < surfacePath.size()) {
                            std::snprintf(surfacePath.data(), surfacePath.size(), "%s",
                                          reinterpret_cast<const char *>(path.c_str()));
                            loadTerrain();
                        } else
                            error = l("Example path is too long.", "示例路径过长。");
                    }
                ImGui::EndCombo();
            }
            ImGui::SetNextItemWidth(-1);
            ImGui::InputText("##surfacePath", surfacePath.data(), surfacePath.size());
            if (button("Browse terrain", "选择高程文件", FunctionRole3D::Model))
                browse(surfacePath, false, true);
            ImGui::SameLine();
            if (button("Load terrain", "载入高程", FunctionRole3D::Model))
                loadTerrain();
            ImGui::Checkbox(l("Shift first X to zero on import", "导入时将首点 X 平移至零"), &zeroSurfaceX);
            if (!staged.surfaceSamples.empty()) {
                ImGui::Checkbox(l("Use imported surface", "使用外部高程"), &staged.importedSurface);
                ImGui::TextUnformatted(l("Elevation datum (m)", "高程基准（m）"));
                ImGui::SetNextItemWidth(-1);
                ImGui::InputDouble("##elevationDatum", &staged.surfaceDatum, 1, 10, "%.3f");
                const auto &v = staged.surfaceSamples;
                ImGui::TextWrapped("%zu %s | X %.3f .. %.3f m | %s %.3f m", v.size(), l("samples", "个样点"),
                                   v.front()[0], v.back()[0], l("required span", "所需覆盖"),
                                   (staged.nx - 1) * staged.spacing);
                if (terrainPreset >= 0)
                    ImGui::TextWrapped("%s", l("Preset datum starts at 0 m; width adaptation preserves your datum. "
                                               "Model depth z = datum - elevation. Reset / apply rebuilds geometry and dt.",
                                               "预设基准初始为 0 m；宽度适配保留已设基准。模型深度 z=基准−高程，重置并应用会重建几何与 dt。"));
                else
                    ImGui::TextWrapped("%s", l("Initial datum is mean elevation. Model depth z = datum - "
                                           "elevation. Reset / apply rebuilds geometry, normals and dt.",
                                           "初始基准为样点高程平均值，模型深度 "
                                           "z=基准−高程。重置并应用会重新生成网格、法向和 dt。"));
                const auto &x = terrainPreviewX, &z = terrainPreviewZ;
                if (ImPlot::BeginPlot("##surfacePreview",
                                      {-1, std::max(180.f, ImGui::GetContentRegionAvail().y - 12)},
                                      ImPlotFlags_NoLegend)) {
                    ImPlot::SetupAxes("X (m)", l("Elevation (m)", "高程（m）"));
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
            if (button("Done terrain", "完成", FunctionRole3D::Run))
                ImGui::CloseCurrentPopup();
            terrainDoneRect = {ImGui::GetItemRectMin(), ImGui::GetItemRectMax()};
            ImGui::EndPopup();
        }
    }
    void controls(){if(parameterHost.mode()!=1)controlsPanel(false);}
    void floatingControls(){
        if(parameterHost.mode()==0)return;
        std::swap(category,floatingCategory);controlsPanel(true);std::swap(category,floatingCategory);
    }
    void controlsPanel(bool floating) {
        if(!floating&&!parameterWindow)return;
        if(!floating&&focusParameters){ImGui::SetNextWindowFocus();focusParameters=false;}
        const auto title=floating?label("Floating parameters 2D","悬浮参数 · 二维"):label("Parameters and run","参数与运行");
        if(floating)parameterHost.prepare(title.c_str());
        bool open=true;const bool visible=ImGui::Begin(title.c_str(),floating?&open:&parameterWindow,floating?parameterHost.flags():0);
        if(floating&&!open)SeisWave::Ui::setParameterDisplayMode(2,0);
        if(!visible){ImGui::End();return;}
        if(floating)parameterHost.tools();
        const char *en[]{"Model", "Survey", "Run", "Display", "Export"};
        const char *zh[]{"建模", "采集", "运行", "显示", "导出"};
        const float width =
            std::max(22.f, (ImGui::GetContentRegionAvail().x - 4 * ImGui::GetStyle().ItemSpacing.x) / 5);
        float longest = 1;
        for (int i = 0; i < 5; ++i)
            longest = std::max(longest, ImGui::CalcTextSize(l(en[i], zh[i])).x);
        ImGui::PushFont(nullptr, std::clamp(ImGui::GetFontSize() *
                                                (width - 2 * ImGui::GetStyle().FramePadding.x - 4) / longest,
                                            12.f, ImGui::GetFontSize()));
        for (int i = 0; i < 5; ++i) {
            if (i)
                ImGui::SameLine();
            if (button(en[i], zh[i],
                       i == 2   ? FunctionRole3D::Run
                       : i == 4 ? FunctionRole3D::Export
                                : FunctionRole3D::Model,
                       {width, 0}))
                category = i;
        }
        ImGui::PopFont();
        if (button("Expand all", "全部展开", FunctionRole3D::Display)) { ++groupEpoch; groupsExpanded = true; }
        ImGui::SameLine();
        if (button("Collapse all", "全部折叠", FunctionRole3D::Display)) { ++groupEpoch; groupsExpanded = false; }
        const float footer = ImGui::GetFontSize() * 8 + 82;
        ImGui::BeginChild("##2DParameterFields", {0, -footer}, ImGuiChildFlags_None,
                          ImGuiWindowFlags_AlwaysVerticalScrollbar);
        const auto input = [&](const char *name, const char *chinese, double &value, double increment = 1) {
            ImGui::TextUnformatted(l(name, chinese));
            ImGui::SetNextItemWidth(-1);
            const auto id = std::string("##") + name;
            ImGui::InputDouble(id.c_str(), &value, increment, increment * 10, "%.3f");
            if (std::string(name) == "Dominant frequency (Hz)")
                frequencyRect = {ImGui::GetItemRectMin(), ImGui::GetItemRectMax()};
        };
        const auto integer = [&](const char *en, const char *zh, int &value, int step = 1, int fast = 100) {
            ImGui::TextUnformatted(l(en, zh));
            ImGui::SetNextItemWidth(-1);
            const auto id = std::string("##int") + en;
            ImGui::InputInt(id.c_str(), &value, step, fast);
        };
        if (category == 0) {
            if (parameterGroup(l("Curved grid", "曲网格与尺寸"), ImGuiTreeNodeFlags_DefaultOpen)) {

                ImGui::TextUnformatted(l("Geometry presets (X x Z)", "几何预设（X × Z）"));
                ImGui::SetNextItemWidth(-1);
                int preset = -1;
                const int sizes[7][2]{{200, 200},   {400, 200},   {600, 300},  {1000, 1000},
                                      {2000, 1000}, {2000, 2000}, {4000, 2000}};
                for (int i = 0; i < 7; ++i)
                    if (staged.nx == sizes[i][0] && staged.nz == sizes[i][1])
                        preset = i;
                const char *names[]{"200 x 200",   "400 x 200",   "600 x 300",  "1000 x 1000",
                                    "2000 x 1000", "2000 x 2000", "4000 x 2000"};
                if (ImGui::BeginCombo("##2Dpreset", preset < 0 ? l("Custom", "自定义") : names[preset])) {
                    for (int i = 0; i < 7; ++i)
                        if (ImGui::Selectable(names[i], preset == i)) {
                            staged.nx = sizes[i][0];
                            staged.nz = sizes[i][1];
                            fitAcquisition(staged);
                        }
                    ImGui::EndCombo();
                }
                int dims[]{staged.nx, staged.nz};
                ImGui::TextUnformatted(l("Physical nodes X / Z (8..16384)", "物理节点 X / Z（8..16384）"));
                ImGui::SetNextItemWidth(-1);
                if (ImGui::InputInt2("##2Ddims", dims)) {
                    staged.nx = dims[0];
                    staged.nz = dims[1];
                    if (staged.nx >= 8 && staged.nz >= 8 && staged.nx <= maxAxisNodes &&
                        staged.nz <= maxAxisNodes)
                        fitAcquisition(staged);
                }
                input("Spacing (m)", "网格间距（m）", staged.spacing, .5);
                ImGui::TextWrapped("%s: %.2f x %.2f m", l("Staged span", "待应用尺寸"),
                                   (staged.nx - 1) * staged.spacing, (staged.nz - 1) * staged.spacing);
                terrainPresetControls();
                const double nodes = (static_cast<double>(std::max(0, staged.nx)) + 6) *
                                     (static_cast<double>(std::max(0, staged.nz)) + 6);
                ImGui::TextWrapped(
                    "%s %.0f MiB | GPU %.0f MiB", l("Grid estimate: RAM", "网格预算：内存"),
                    nodes * (480 + (staged.source.kind == SeisWave::SourceKind::IsotropicMoment ? 16 : 0)) /
                        (1024 * 1024),
                    nodes * (384 + (staged.source.kind == SeisWave::SourceKind::IsotropicMoment ? 16 : 0)) /
                        (1024 * 1024));
                ImGui::TextWrapped("%s", l("Limits also depend on available RAM/VRAM; rebuilding keeps the "
                                           "old session until success.",
                                           "实际容量取决于可用内存/显存，重建成功前保留原会话。"));
                if (button("External elevation profile", "外部地表高程…", FunctionRole3D::Model, {-1, 0}))
                    openTerrain = true;
                if (!staged.surfaceSamples.empty())
                    ImGui::Checkbox(l("Imported surface enabled", "启用外部高程"), &staged.importedSurface);
                ImGui::BeginDisabled(staged.importedSurface);
                input("Relief amplitude (m)", "地表起伏振幅（m）", staged.relief);
                input("Terrain wavelength (m)", "地形波长（m）", staged.wavelength, 10);
                ImGui::EndDisabled();
                input("Flatten depth (0=auto)", "展平深度（0=自动）", staged.flattenDepth, 10);
                if (button("Flat", "平地", FunctionRole3D::Model)) {
                    staged.importedSurface = false;
                    staged.relief = 0;
                }
                ImGui::SameLine();
                if (button("Curved", "曲面", FunctionRole3D::Model)) {
                    staged.importedSurface = false;
                    staged.relief = 10;
                    staged.flattenDepth = 0;
                }
            }
            if (parameterGroup(l("Elastic material", "弹性介质"), 0)) {

                ImGui::Checkbox(l("Layered material", "分层介质"), &staged.layered);
                if (!staged.layered) {
                    input("Vp (m/s)", "Vp（m/s）", staged.vp, 100);
                    input("Vs (m/s)", "Vs（m/s）", staged.vs, 100);
                    input("Density (kg/m3)", "密度（kg/m³）", staged.density, 50);
                    if (button("Material presets / layers", "介质预设与层表…", FunctionRole3D::Model,
                               {-1, 0}))
                        openLayers = true;
                } else {
                    ImGui::TextWrapped(
                        "%s", l("Layers follow computational depth; final layer fills the remainder.",
                                "地层按计算深度对齐；末层填充剩余深度。"));
                    double vs = 1e100;
                    for (const auto &v : staged.layers)
                        vs = std::min(vs, v.vs);
                    ImGui::Text("%zu %s | Vs min %.0f m/s", staged.layers.size(), l("layers", "层"), vs);
                    if (button("Edit layers / presets", "编辑层表与预设…", FunctionRole3D::Model, {-1, 0}))
                        openLayers = true;
                    layerEditRect = {ImGui::GetItemRectMin(), ImGui::GetItemRectMax()};
                }
            }
            modelFiles();
        } else if (category == 1) {
            if (parameterGroup(l("Source / dominant frequency", "震源与激发主频"),
                                        ImGuiTreeNodeFlags_DefaultOpen)) {

                ImGui::TextUnformatted(l("Source index X / Z (-1 centers X)", "震源索引 X / Z（X=-1 居中）"));
                int source[]{staged.sourceX, staged.sourceZ};
                ImGui::SetNextItemWidth(-1);
                if (ImGui::InputInt2("##source2D", source)) {
                    staged.sourceX = source[0];
                    staged.sourceZ = source[1];
                }
                input("Dominant frequency (Hz)", "激发主频（Hz）", staged.frequency);
                if (SeisWave::Ui::sourceControls(staged.source, staged.forcePerLength, staged.momentPerLength,
                                                 true, &sourceKindRect, &sourceMomentRect))
                    fitAcquisition(staged);
            }
            if (parameterGroup(l("Receiver line", "接收测线"), 0)) {

                integer("Start X", "起点 X", staged.receiverStart);
                integer("Count (max 1024)", "接收数（最多 1024）", staged.receiverCount);
                integer("Index stride", "节点间隔", staged.receiverStride);
                integer("Depth index", "深度索引", staged.receiverDepth);
                if (button("Fit source / receivers", "适配震源/接收位置", FunctionRole3D::Model, {-1, 0}))
                    fitAcquisition(staged);
                samplingControls();
                ImGui::TextWrapped("%s", l("Vx and Vz share the same output axis; time zero is included.",
                                           "Vx、Vz 共用输出时间轴，记录包含零时刻。"));
            }
        } else if (category == 2) {
            if (parameterGroup(l("Compute backend", "计算核心"), ImGuiTreeNodeFlags_DefaultOpen)) {

                ImGui::SetNextItemWidth(-1);
                const auto availability = Seis3D::Curvilinear::selectCudaDevice3D(inventory);
                if (ImGui::BeginCombo("##2Dbackend",
                                      selectedBackend ? "CUDA Float64" : "CPU / OpenMP Float64")) {
                    if (ImGui::Selectable("CPU / OpenMP Float64", selectedBackend == 0))
                        selectedBackend = 0;
                    ImGui::BeginDisabled(!availability.available);
                    if (ImGui::Selectable("CUDA Float64", selectedBackend == 1))
                        selectedBackend = 1;
                    ImGui::EndDisabled();
                    ImGui::EndCombo();
                }
                const auto selected = Seis3D::Curvilinear::selectCudaDevice3D(inventory, settings.cudaDevice);
                ImGui::SetNextItemWidth(-1);
                if (ImGui::BeginCombo("##2Ddevice", settings.cudaDevice < 0 ? l("Auto (largest usable VRAM)",
                                                                                "自动（可用总显存最大）")
                                                                            : selected.description.c_str())) {
                    if (ImGui::Selectable(l("Auto", "自动"), settings.cudaDevice < 0))
                        settings.cudaDevice = -1;
                    for (const auto &device : inventory.devices) {
                        ImGui::BeginDisabled(!device.available);
                        const auto name = "GPU " + std::to_string(device.index) + " | " + device.name;
                        if (ImGui::Selectable(name.c_str(), settings.cudaDevice == device.index))
                            settings.cudaDevice = device.index;
                        ImGui::EndDisabled();
                    }
                    ImGui::EndCombo();
                }
                if (button("Refresh devices", "重新检测显卡", FunctionRole3D::Model))
                    inventory = Seis3D::Curvilinear::queryCudaDevices3D();
                ImGui::TextWrapped("%s", selected.description.c_str());
                integer("CPU workers (0=auto)", "CPU 线程（0=自动）", settings.cpuThreads);
            }
            if (parameterGroup(l("Time / absorption", "时间与吸收层"), 0)) {
                input("Duration (s)", "模拟时长（s）", staged.duration, .1);
                integer("Maximum steps (0=duration)", "最大步数（0=按时长）", staged.maxSteps, 100, 1000);
                ImGui::TextDisabled("%s: %d", l("2D accepted-step limit", "二维单次推进上限"), maxTimeSteps);
                input("DRP CFL (0.02..0.8)", "DRP CFL（0.02..0.8）", staged.cfl, .02);
                ImGui::Checkbox(l("Enable sponge", "启用吸收海绵层"), &staged.absorbing);
                integer("X sponge cells", "X 方向吸收格数", staged.spongeX);
                integer("Bottom sponge cells", "底部吸收格数", staged.spongeZ);
                ImGui::TextDisabled("%s: 1..%d", l("Input cells", "可输入格数"), maxSpongeCells);
                ImGui::TextWrapped("%s: X %.3f m / Z %.3f m (%s)",
                                   l("Effective sponge width", "实际吸收宽度"),
                                   std::clamp(staged.spongeX, 1, std::max(1, staged.nx / 3)) * staged.spacing,
                                   std::clamp(staged.spongeZ, 1, std::max(1, staged.nz / 3)) * staged.spacing,
                                   l("each axis <= nodes / 3; applied after rebuild", "每方向不超过节点数1/3，重建后生效"));
                input("Sponge strength", "吸收强度", staged.spongeStrength, .1);
                samplingControls();
            }
        } else if (category == 3) {
            if (parameterGroup(l("Layers / palettes", "图层与独立色标"),
                                        ImGuiTreeNodeFlags_DefaultOpen)) {
                const char *components[]{"Vz", "Vx", "|V|", l("Pressure (Pa)", "压力（Pa）")};
                ImGui::SetNextItemWidth(-1);
                ImGui::Combo("##2Dcomponent", &component, components, 4);
                ImGui::Checkbox(l("Wavefield", "波场图层"), &showWave);
                ImGui::Checkbox(l("Material model", "介质图层"), &showModel);
                ImGui::Checkbox(l("Normalized wave colorbar", "归一化波场色标"), &normalized);
                ImGui::Checkbox(l("Actual grid [G]", "真实节点网格 [G]"), &grid);
                ImGui::Checkbox(l("Sponge overlay [B]", "吸收区显示 [B]"), &sponge);
                ImGui::TextUnformatted(l("Wave palette [Q]", "波场色带 [Q]"));
                ImGui::SetNextItemWidth(-1);
                ImGui::Combo("##2DwavePalette", &wavePalette,
                             Seis3D::Curvilinear::RecordPalettes::wavePaletteNames);
                ImGui::TextUnformatted(l("Gain", "显示增益"));
                ImGui::SetNextItemWidth(-1);
                ImGui::SliderFloat("##2DGain", &gain, .05f, 50, "%.2fx", ImGuiSliderFlags_Logarithmic);
                ImGui::TextUnformatted(l("Model palette [E]", "模型配色 [E]"));
                ImGui::SetNextItemWidth(-1);
                ImGui::Combo("##2DmodelPalette", &modelPalette,
                             Seis3D::Curvilinear::RecordPalettes::wavePaletteNames);
                ImGui::TextUnformatted(l("Model property", "模型物性"));
                ImGui::SetNextItemWidth(-1);
                ImGui::Combo("##2DModelProperty", &modelComponent, "Vp\0Vs\0Density\0");
            }
        } else {
            modelFiles();
            if (parameterGroup(l("SEG-Y import", "SEG-Y 导入"), ImGuiTreeNodeFlags_DefaultOpen)) {
                ImGui::SetNextItemWidth(-1);
                ImGui::InputText("##2DimportPath", importPath.data(), importPath.size());
                if (button("Browse input", "选择文件", FunctionRole3D::Model))
                    browse(importPath, false);
                ImGui::SameLine();
                if (button("Import SEG-Y", "载入 SEG-Y", FunctionRole3D::Model))
                    importSegy();
                if (!imported.traces.empty()) {
                    ImGui::Checkbox(l("Show imported records", "显示导入记录"), &useImported);
                    ImGui::TextWrapped("%s", imported.description.c_str());
                }
            }
            if (parameterGroup(l("SEG-Y export", "SEG-Y 导出"),
                                        ImGuiTreeNodeFlags_DefaultOpen)) {
                ImGui::TextWrapped(
                    "%s", l("Export the selected record source. Multiple components produce separate files.",
                            "导出当前记录来源；多分量分别保存为独立文件。"));
                const auto data = displayedRecords();
                for (std::size_t c = 0; c < data.components.size(); ++c) {
                    if (c)
                        ImGui::SameLine();
                    ImGui::Checkbox(data.components[c].c_str(), &exportComponents[c]);
                }
                ImGui::TextUnformatted(l("SEG-Y compatibility profile", "SEG-Y 兼容格式"));
                ImGui::SetNextItemWidth(-1);
                ImGui::Combo("##2DSegyProfile", &segyProfile,
                             l("Exact dt / rev2 IEEE32\0Exact dt / rev2 IEEE64\0Legacy / rev1 IBM32 (EBCDIC)\0Compatible / rev1 IEEE32 (ASCII)\0",
                               "精确时间步 / rev2 IEEE32\0精确时间步 / rev2 IEEE64\0旧软件兼容 / rev1 IBM32（EBCDIC）\0兼容 / rev1 IEEE32（ASCII）\0"));
                if (segyProfile >= 2) {
                    ImGui::TextWrapped("%s", l("Big endian, 240-byte headers, live trace ID=1, channel/CDP 1..N. Choose the same data format in the target importer.",
                                               "大端、240 字节道头、有效道标识 1，通道/CDP 按 1..N 编号；导入时匹配 IBM 或 IEEE 格式。"));
                    ImGui::TextUnformatted(l("Export sampling interval (ms)", "导出采样间隔（ms）"));
                    ImGui::SetNextItemWidth(-1);
                    ImGui::InputDouble("##2DSegyInterval", &segyIntervalMs, .5, 1, "%.4f");
                    ImGui::TextWrapped("%s", l("0 preserves the source interval (whole microseconds required); otherwise explicitly resamples linearly. 0.5 / 1 ms recommended.",
                                               "0 保留原间隔（须为整微秒），其他值显式线性重采样；建议 0.5 或 1 ms。"));
                }
                ImGui::TextUnformatted(l("Export amplitude multiplier", "导出振幅倍率"));
                ImGui::SetNextItemWidth(-1);
                ImGui::InputDouble("##2DSegyGain", &segyGain, 0, 0, "%.8g");
                if (button("Physical amplitude x1", "原始振幅 ×1", FunctionRole3D::Display)) segyGain = 1;
                ImGui::SameLine();
                if (button("Display copy x1e6", "显示副本 ×1e6", FunctionRole3D::Display)) segyGain = 1e6;
                ImGui::TextWrapped("%s", l("One constant for every trace/component; written to the textual header. Scaling changes exported units, not the simulation. Display AGC is excluded.",
                                           "所有道和分量共用一个固定倍率，文本头记录倍率。放大只影响导出副本并标记 scaled 单位，不改变模拟；不导出显示 AGC。"));
                ImGui::SetNextItemWidth(-1);
                ImGui::InputText("##2Dsegy", segy.data(), segy.size());
                if (button("Save as", "选择位置", FunctionRole3D::Export))
                    browse(segy, true);
                ImGui::SameLine();
                if (button("Export SEG-Y", "导出 SEG-Y", FunctionRole3D::Export))
                    exportSegy();
            }
            if (parameterGroup(l("Wavefield snapshot / CSV", "波场快照与 CSV"))) {
                ImGui::TextWrapped("%s", l("Wavefield VTS is for analysis, not model restore. Use the model module to reproduce a run.",
                                           "波场快照用于分析；复现模拟请使用上方的模型保存模块。"));
                snapshotControls();
                ImGui::SetNextItemWidth(-1);
                ImGui::InputText("##2Dcsv", csv.data(), csv.size());
                ImGui::TextDisabled("%s", l("CSV exports the active simulation (Vx/Vz).", "CSV 导出当前模拟的 Vx/Vz 记录。"));
                ImGui::BeginDisabled(useImported);
                if (button("Export Vx / Vz CSV", "导出 Vx/Vz 记录 CSV", FunctionRole3D::Export, {-1, 0}) &&
                    writeReceivers(
                        simulation,
                        std::filesystem::path(std::u8string(reinterpret_cast<const char8_t *>(csv.data()))),
                        error))
                    status = l("Active simulation Vx/Vz records exported to CSV.", "当前模拟的 Vx/Vz 记录已导出为 CSV。");
                ImGui::EndDisabled();
                if (useImported)
                    ImGui::TextWrapped("%s", l("Switch records to the simulation before exporting CSV; imported records can be exported as SEG-Y.",
                                               "导出 CSV 前请将记录源切回当前模拟；导入记录可导出为 SEG-Y。"));
            }
        }
        if (parameterGroup(l("Diagnostics", "数值诊断"))) {
            const auto &d = simulation.diagnostics();
            ImGui::TextWrapped("%s", simulation.backend().deviceName.c_str());
            ImGui::Text("PPW %.2f | Energy %.4g J/m", d.ppw, d.energy);
            ImGui::Text("%.3f s | %.4f ms/step", d.totalStepMs / 1000,
                        simulation.step() ? d.totalStepMs / simulation.step() : 0);
            ImGui::Text("traction %.3g", d.tractionRelative);
            if (d.ppw < 8)
                ImGui::TextWrapped(
                    "%s", l("Sampling is below the 8 PPW reference.", "空间采样低于参考每波长 8 点。"));
        }
        if (!error.empty())
            ImGui::TextWrapped("%s", error.c_str());
        else
            ImGui::TextWrapped("%s", status.c_str());
        ImGui::EndChild();
        ImGui::SeparatorText(l("Session / rebuild", "会话与重置"));
        ImGui::Text("%s f0 %.2f Hz | dt %.5f ms", l("Active", "生效"), simulation.parameters().frequency,
                    simulation.dt() * 1000);
        ImGui::TextColored(
            functionColor3D(dirty() ? FunctionRole3D::Warning : FunctionRole3D::Run), "%s",
            dirty() ? l("Pending changes: reset / apply or Run.", "参数待应用：重置并应用，或运行。")
                    : l("Parameters applied", "参数已生效"));
        if (button("Reset / apply parameters", "重置并应用参数",
                   dirty() ? FunctionRole3D::Warning : FunctionRole3D::Model,
                   {-1, ImGui::GetFrameHeight() * 1.4f}))
            apply();
        rebuildRect = {ImGui::GetItemRectMin(), ImGui::GetItemRectMax()};
        const float half = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) / 2;
        if (button("Undo edits", "撤销修改", FunctionRole3D::Display, {half, 0})) {
            staged = simulation.parameters();
            terrainPreset = activeTerrainPreset;
            terrainPresetSource = activeTerrainPresetSource;
            terrainResampledNx = staged.nx;
            terrainResampledSpacing = staged.spacing;
            updateTerrainPreview();
            settings = activeSettings;
            selectedBackend = static_cast<int>(simulation.backend().kind);
            error.clear();
        }
        ImGui::SameLine();
        if (button("Defaults", "恢复默认", FunctionRole3D::Display, {half, 0})) {
            staged = Parameters{};
            terrainPreset = -1;
            terrainPresetSource.clear();
            error.clear();
        }
        if (!error.empty()) {
            ImGui::TextColored(functionColor3D(FunctionRole3D::Danger), "%s",
                               l("Rebuild failed (hover for details)", "重建失败（悬停查看原因）"));
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", error.c_str());
        }
        ImGui::End();
    }
    void activeParameters() {
        if (!summaryWindow||parameterHost.mode()==1)
            return;
        if (!ImGui::Begin(label("Active simulation parameters", "当前模拟参数").c_str(), &summaryWindow)) {
            ImGui::End();
            return;
        }
        const auto &p = simulation.parameters();
        const auto &g = simulation.grid();
        const auto &d = simulation.diagnostics();
        const bool pending = dirty();
        ImGui::PushTextWrapPos(0);
        ImGui::TextColored(functionColor3D(pending ? FunctionRole3D::Warning : FunctionRole3D::Run), "%s",
                           pending ? l("Edits pending / active values below", "存在待应用修改 / 下列为生效值")
                                   : l("All parameters applied", "参数已全部生效"));
        if (pending)
            ImGui::TextWrapped("%s", l("Arrows show staged values. dt is recalculated only after a successful rebuild.",
                                       "箭头后为待应用值；仅在重建成功后更新 dt。"));
        ImGui::TextWrapped("%s", simulation.backend().deviceName.c_str());
        ImGui::TextUnformatted("MacCormack DRP + RK4 / Float64 / P-SV");
        ImGui::SeparatorText(l("Effective time / geometry", "生效时间与几何"));
        ImGui::Text("dt = %.8f ms", simulation.dt() * 1000);
        ImGui::Text("%s %.5f ms", l("Record dt", "记录间隔"), simulation.recordDt() * 1000);
        ImGui::Text("f0 %.3f Hz | CFL %.3f", p.frequency, p.cfl);
        ImGui::Text("%d x %d | h %.4f m", p.nx, p.nz, p.spacing);
        ImGui::TextUnformatted(p.source.kind == SeisWave::SourceKind::IsotropicMoment
                                   ? l("Isotropic moment source", "各向同性爆炸矩源")
                                   : l("Directional body force", "定向体力源"));
        ImGui::Text("t = %.5f s | %d / %d", simulation.step() * simulation.dt(), simulation.step(), simulation.totalSteps());
        ImGui::Text("%s %.5f s", l("Effective stop", "实际停止时刻"), simulation.totalSteps() * simulation.dt());
        ImGui::Text("X / Z = %.2f x %.2f m", (p.nx - 1) * p.spacing, (p.nz - 1) * p.spacing);
        ImGui::Text("%s %.3f m", l("Flatten", "展平深度"), g.flattenDepth);
        const auto src = g.coordinates[g.index((p.sourceX < 0 ? p.nx / 2 : p.sourceX) + ghost, p.sourceZ + ghost)];
        ImGui::Text("%s (%.2f, %.2f) m", l("Source", "震源"), src[0], src[1]);
        const auto &traces = simulation.receivers();
        ImGui::Text("%zu %s | %zu %s", traces.size(), l("traces", "道"), traces.front().vx.size(), l("samples", "样点"));
        ImGui::Text("%s %.5f s", l("Last record", "记录末时刻"), (traces.front().vx.size() - 1) * simulation.recordDt());
        if (p.sampleIntervalMs > 0 && simulation.recordDt() < simulation.dt())
            ImGui::TextWrapped("%s", l("Output is denser than solver steps: interpolated samples add no temporal resolution.",
                                       "输出比计算步更密：插值不会提高原计算时间分辨率。"));
        if (simulation.recordDt() * 2 * 2.5 * p.frequency >= 1)
            ImGui::TextWrapped("%s", l("Record Nyquist is below the 2.5 f0 reference bandwidth; choose a shorter interval.",
                                       "记录奈奎斯特频率低于 2.5f0 参考频带，请缩短采样间隔。"));
        ImGui::SeparatorText(l("Parameter audit", "参数核对"));
        const auto number = [](double v) {
            char text[64];
            std::snprintf(text, sizeof(text), "%.8g", v);
            return std::string(text);
        };
        if (ImGui::BeginTable("##2DActiveTable", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH |
                                                       ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableSetupColumn(l("Parameter", "参数"), ImGuiTableColumnFlags_WidthStretch, .48f);
            ImGui::TableSetupColumn(l("Active / pending", "生效 / 待应用"), ImGuiTableColumnFlags_WidthStretch, .52f);
            ImGui::TableHeadersRow();
            const auto row = [&](const char *en, const char *zh, const std::string &value,
                                 const std::string &edit, bool changed) {
                ImGui::TableNextRow(); ImGui::TableNextColumn();
                ImGui::TextWrapped("%s", l(en, zh)); ImGui::TableNextColumn();
                ImGui::TextWrapped("%s", value.c_str());
                if (changed) {
                    ImGui::PushStyleColor(ImGuiCol_Text, functionColor3D(FunctionRole3D::Warning));
                    ImGui::TextWrapped("-> %s", edit.c_str());
                    ImGui::PopStyleColor();
                }
            };
            const auto scalar = [&](const char *en, const char *zh, double active, double edit) {
                row(en, zh, number(active), number(edit), active != edit);
            };
#define ROW(en, zh, member) scalar(en, zh, p.member, staged.member)
            ROW("Nx", "X 节点", nx); ROW("Nz", "Z 节点", nz);
            ROW("Spacing (m)", "间距（m）", spacing);
            ROW("Uniform Vp (m/s)", "均匀 Vp（m/s）", vp); ROW("Uniform Vs (m/s)", "均匀 Vs（m/s）", vs);
            ROW("Uniform density (kg/m3)", "均匀密度（kg/m³）", density);
            ROW("Layered (0/1)", "分层（0/1）", layered);
            row("Layer table", "层表", std::to_string(p.layers.size()), std::to_string(staged.layers.size()) + l(" (edited)", "（内容已修改）"), p.layers != staged.layers);
            ROW("External terrain (0/1)", "外部地形（0/1）", importedSurface);
            ROW("Datum (m)", "高程基准（m）", surfaceDatum);
            row("Terrain samples", "高程样点", std::to_string(p.surfaceSamples.size()), std::to_string(staged.surfaceSamples.size()) + l(" (edited)", "（内容已修改）"), p.surfaceSamples != staged.surfaceSamples);
            ROW("Relief (m)", "起伏振幅（m）", relief); ROW("Wavelength (m)", "地形波长（m）", wavelength);
            ROW("Flatten (0=auto)", "展平（0=自动）", flattenDepth);
            ROW("Frequency (Hz)", "主频（Hz）", frequency);
            scalar("Source kind (0=force, 1=moment)", "震源（0=力，1=矩）", static_cast<int>(p.source.kind), static_cast<int>(staged.source.kind));
            const auto direction = [&](const auto &v) { return number(v[0]) + ", " + number(v[1]) + ", " + number(v[2]); };
            row("Force direction XYZ (raw)", "力方向 XYZ（原值）", direction(p.source.direction), direction(staged.source.direction), p.source.direction != staged.source.direction);
            ROW("Force (N/m)", "体力（N/m）", forcePerLength); ROW("Moment (N)", "各向同性矩（N）", momentPerLength);
            ROW("Source X (-1=center)", "震源 X（-1=居中）", sourceX); ROW("Source Z", "震源 Z", sourceZ);
            ROW("Duration (s)", "时长（s）", duration); ROW("Step cap (0=duration)", "步数上限（0=按时长）", maxSteps);
            ROW("CFL", "CFL", cfl); ROW("Absorption (0/1)", "吸收层（0/1）", absorbing);
            ROW("X sponge cells", "X 吸收格数", spongeX); ROW("Z sponge cells", "底部吸收格数", spongeZ);
            ROW("Sponge strength", "吸收强度", spongeStrength);
            ROW("Receiver start", "接收起点", receiverStart); ROW("Receiver count", "接收道数", receiverCount);
            ROW("Receiver index stride", "接收节点间隔", receiverStride); ROW("Receiver depth index", "接收深度索引", receiverDepth);
            ROW("Sample interval ms (0=dt)", "采样毫秒（0=dt）", sampleIntervalMs);
#undef ROW
            scalar("Backend (0=CPU, 1=CUDA)", "核心（0=CPU，1=CUDA）", static_cast<int>(simulation.backend().kind), selectedBackend);
            scalar("CPU threads requested", "请求 CPU 线程", activeSettings.cpuThreads, settings.cpuThreads);
            scalar("CUDA device requested (-1=auto)", "请求 CUDA 卡（-1=自动）", activeSettings.cudaDevice, settings.cudaDevice);
            ImGui::EndTable();
        }
        if (p.layered && ImGui::CollapsingHeader(l("Active layers", "生效层表"), ImGuiTreeNodeFlags_DefaultOpen)) {
            for (std::size_t i = 0; i < p.layers.size(); ++i) {
                const auto &v = p.layers[i];
                ImGui::TextWrapped("%zu: h %.2f m | Vp %.1f | Vs %.1f | rho %.1f", i + 1, v.thickness, v.vp, v.vs, v.density);
            }
        }
        ImGui::SeparatorText(l("Numerical diagnostics", "数值诊断"));
        ImGui::Text("PPW %.3f | J min %.6f", d.ppw, g.minJacobian);
        ImGui::Text("dt wave %.6f ms", d.waveDt * 1000);
        ImGui::Text("dt damping %.6f ms", d.dampingDt * 1000);
        ImGui::Text("dt source %.6f ms", d.sourceDt * 1000);
        ImGui::Text("%s %d", l("CPU workers", "生效 CPU 线程"), simulation.backend().workerThreads);
        ImGui::Text("%s %d", l("CUDA device", "生效 CUDA 卡"), simulation.backend().cudaDevice);
        ImGui::Text("%.3f s | %.4f ms/step", d.totalStepMs / 1000, simulation.step() ? d.totalStepMs / simulation.step() : 0);
        ImGui::PopTextWrapPos();
        ImGui::End();
    }
    void wavefield() {
        if (!waveWindow)
            return;
        if (!ImGui::Begin(label("Curved X-Z wavefield", "曲面 X–Z 波场").c_str(), &waveWindow)) {
            ImGui::End();
            return;
        }
        std::string frameError;
        if (!simulation.readFrame(component, frame, frameError)) {
            error = frameError;
            ImGui::End();
            return;
        }
        const auto &p = simulation.parameters();
        const auto &g = simulation.grid();
        const auto &material = modelComponent == 0 ? g.vp : modelComponent == 1 ? g.vs : g.density;
        const auto mm = std::minmax_element(material.begin(), material.end());
        const double minMaterial = *mm.first, maxMaterial = *mm.second;
        double peak = 1e-30;
        for (float v : frame)
            peak = std::max(peak, std::abs(static_cast<double>(v)));
        const double range = peak / gain;
        const bool compactPlot = ImGui::GetContentRegionAvail().y < ImGui::GetFontSize() * 13;
        if (ImGui::GetContentRegionAvail().x < 700)
            ImGui::Text("%d x %d | %.1f x %.1f m", p.nx, p.nz, (p.nx - 1) * p.spacing,
                        (p.nz - 1) * p.spacing);
        else
            ImGui::Text("X / Z: %.2f x %.2f m | %d x %d | f0 %.1f Hz", (p.nx - 1) * p.spacing,
                        (p.nz - 1) * p.spacing, p.nx, p.nz, p.frequency);
        const float plotHeight =
            std::max(80.f, ImGui::GetContentRegionAvail().y -
                               (compactPlot ? ImGui::GetFontSize() + 28 : ImGui::GetFontSize() * 2 + 65) -
                               (grid ? ImGui::GetFontSize() + ImGui::GetStyle().ItemSpacing.y : 0));
        if (fit)
            ImPlot::SetNextAxesToFit();
        if (ImPlot::BeginPlot("##curved2D", {-1, plotHeight}, ImPlotFlags_NoLegend | ImPlotFlags_Equal)) {
            const auto flags = plotHeight < 190 ? ImPlotAxisFlags_NoLabel : ImPlotAxisFlags_None;
            ImPlot::SetupAxes("X (m)", l("Depth (m)", "深度 (m)"),
                              flags | (plotHeight < 160 ? ImPlotAxisFlags_NoTickLabels : 0),
                              ImPlotAxisFlags_Invert | flags);
            // Fitting both bounds lets Equal expand the shorter axis, keeping the
            // complete model in view without distorting physical aspect ratios.
            const double xmax = (p.nx - 1) * p.spacing, zmax = (p.nz - 1) * p.spacing;
            double zmin = 0;
            for (int i = 0; i < p.nx; ++i)
                zmin = std::min(zmin, g.coordinates[g.index(i + ghost, ghost)][1]);
            if (plotHeight < ImGui::GetFontSize() * 10) {
                ImPlot::SetupAxisTicks(ImAxis_X1, 0, xmax, 3);
                ImPlot::SetupAxisTicks(ImAxis_Y1, 0, zmax, 2);
                ImPlot::SetupAxisFormat(ImAxis_X1, "%.3g");
                ImPlot::SetupAxisFormat(ImAxis_Y1, "%.3g");
            }
            const double boundsX[]{-.02 * xmax, 1.02 * xmax},
                boundsZ[]{zmin - .02 * (zmax - zmin), zmax + .02 * (zmax - zmin)};
            ImPlot::SetNextLineStyle({0, 0, 0, 0});
            ImPlot::PlotLine("##modelBounds", boundsX, boundsZ, 2);
            const auto limits = ImPlot::GetPlotLimits();
            domainVisible =
                limits.X.Min <= 0 && limits.X.Max >= xmax && limits.Y.Min <= zmin && limits.Y.Max >= zmax;
            auto *draw = ImPlot::GetPlotDrawList();
            ImPlot::PushPlotClipRect();
            const auto toPixel = [&](int i, int k) {
                const auto x = g.coordinates[g.index(i + ghost, k + ghost)];
                return ImPlot::PlotToPixels(x[0], x[1]);
            };
            const auto plotSize = ImPlot::GetPlotSize();
            renderStrideX =
                std::max(1, static_cast<int>(std::ceil((p.nx - 1) / std::max(64.f, plotSize.x / 2))));
            renderStrideZ =
                std::max(1, static_cast<int>(std::ceil((p.nz - 1) / std::max(64.f, plotSize.y / 2))));
            for (int k = 0; k < p.nz - 1; k += renderStrideZ)
                for (int i = 0; i < p.nx - 1; i += renderStrideX) {
                    const int i1 = std::min(p.nx - 1, i + renderStrideX),
                              k1 = std::min(p.nz - 1, k + renderStrideZ);
                    ImVec2 points[4]{toPixel(i, k), toPixel(i1, k), toPixel(i1, k1), toPixel(i, k1)};
                    ImU32 colors[4];
                    const int xx[]{i, i1, i1, i}, zz[]{k, k, k1, k1};
                    for (int v = 0; v < 4; ++v) {
                        const auto q = g.index(xx[v] + ghost, zz[v] + ghost);
                        const double model = maxMaterial > minMaterial
                                                 ? (material[q] - minMaterial) / (maxMaterial - minMaterial)
                                                 : .45;
                        auto base = showModel
                                        ? ImGui::ColorConvertU32ToFloat4(paletteColor(model, modelPalette))
                                        : palette3D().background;
                        const double norm =
                            std::clamp(frame[xx[v] + p.nx * zz[v]] / range, component == 2 ? 0.0 : -1.0, 1.0);
                        if (showWave) {
                            const auto wave = ImGui::ColorConvertU32ToFloat4(
                                paletteColor(component == 2 ? norm : .5 + .5 * norm, wavePalette));
                            const float alpha =
                                showModel ? static_cast<float>(std::min(1.0, std::abs(norm) * 4)) : .95f;
                            base = {base.x * (1 - alpha) + wave.x * alpha,
                                    base.y * (1 - alpha) + wave.y * alpha,
                                    base.z * (1 - alpha) + wave.z * alpha, 1};
                        }
                        if (sponge && p.absorbing &&
                            (xx[v] < std::min(p.spongeX, p.nx / 3) ||
                             xx[v] >= p.nx - std::min(p.spongeX, p.nx / 3) ||
                             zz[v] >= p.nz - std::min(p.spongeZ, p.nz / 3))) {
                            base.x = base.x * .8f + .2f;
                            base.y = base.y * .8f + .1f;
                        }
                        colors[v] = ImGui::ColorConvertFloat4ToU32(base);
                    }
                    quad(draw, points, colors);
                }
            if (grid) {
                for (int k = 0;; k = std::min(p.nz - 1, k + renderStrideZ)) {
                    for (int i = 0; i < p.nx - 1; i += renderStrideX)
                        draw->AddLine(toPixel(i, k), toPixel(std::min(p.nx - 1, i + renderStrideX), k),
                                      color3D(palette3D().text, .25f));
                    if (k == p.nz - 1)
                        break;
                }
                for (int i = 0;; i = std::min(p.nx - 1, i + renderStrideX)) {
                    for (int k = 0; k < p.nz - 1; k += renderStrideZ)
                        draw->AddLine(toPixel(i, k), toPixel(i, std::min(p.nz - 1, k + renderStrideZ)),
                                      color3D(palette3D().text, .25f));
                    if (i == p.nx - 1)
                        break;
                }
            }
            for (const auto &r : simulation.receivers())
                draw->AddCircleFilled(toPixel(r.x, r.z), 2.4f, IM_COL32(43, 194, 165, 240));
            draw->AddCircleFilled(toPixel(p.sourceX < 0 ? p.nx / 2 : p.sourceX, p.sourceZ), 4.5f,
                                  IM_COL32(246, 147, 39, 255));
            ImPlot::PopPlotClipRect();
            ImPlot::EndPlot();
            fit = false;
        }
        if (grid && (renderStrideX > 1 || renderStrideZ > 1))
            ImGui::Text("%s %d x %d", l("View stride (solver unchanged)", "显示抽样（计算网格不变）"),
                        renderStrideX, renderStrideZ);
        if (compactPlot) {
            if (ImGui::BeginTable("##2DCompactLegends", 2, ImGuiTableFlags_SizingStretchSame)) {
                for (int side = 0; side < 2; ++side) {
                    ImGui::TableNextColumn();
                    const auto at = ImGui::GetCursorScreenPos();
                    const float width = ImGui::GetContentRegionAvail().x;
                    for (int i = 0; i < 64; ++i)
                        ImGui::GetWindowDrawList()->AddRectFilled(
                            {at.x + width * i / 64, at.y}, {at.x + width * (i + 1) / 64, at.y + 6},
                            paletteColor(i / 63., side ? modelPalette : wavePalette));
                    ImGui::Dummy({width, 8});
                    if (side)
                        ImGui::Text("%s %.0f",
                                    modelComponent == 0   ? "Vp"
                                    : modelComponent == 1 ? "Vs"
                                                          : "rho",
                                    maxMaterial);
                    else
                        ImGui::Text("%s %s",
                                    component == 0   ? "Vz"
                                    : component == 1 ? "Vx"
                                    : component == 2 ? "|V|"
                                                     : "P",
                                    normalized ? (component == 2 ? "[0,1]" : "[-1,1]")
                                               : l("physical", "物理值"));
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip(
                            "%s %.4g .. %.4g %s", side ? "Model" : "Wave",
                            side ? minMaterial : (component == 2 ? 0 : -range), side ? maxMaterial : range,
                            side ? (modelComponent == 2 ? "kg/m3" : "m/s") : (component == 3 ? "Pa" : "m/s"));
                }
                ImGui::EndTable();
            }
            ImGui::End();
            return;
        }
        const float width = ImGui::GetContentRegionAvail().x;
        const auto bar = [&](int palette) {
            const auto at = ImGui::GetCursorScreenPos();
            for (int i = 0; i < 128; ++i)
                ImGui::GetWindowDrawList()->AddRectFilled({at.x + width * i / 128, at.y},
                                                          {at.x + width * (i + 1) / 128, at.y + 10},
                                                          paletteColor(i / 127., palette));
            ImGui::Dummy({width, 12});
        };
        bar(wavePalette);
        ImGui::Text("%s | %s | %s %.4g %s",
                    component == 0   ? "Vz"
                    : component == 1 ? "Vx"
                    : component == 2 ? "|V|"
                                     : "Pressure",
                    normalized ? (component == 2 ? "0 .. 1" : "-1 .. 1") : l("Physical scale", "物理刻度"),
                    normalized ? "1 =" : "+/-", range, component == 3 ? "Pa" : "m/s");
        bar(modelPalette);
        ImGui::Text("%s: %.2f .. %.2f %s",
                    modelComponent == 0   ? "Vp"
                    : modelComponent == 1 ? "Vs"
                                          : "Density",
                    minMaterial, maxMaterial, modelComponent == 2 ? "kg/m3" : "m/s");
        ImGui::End();
    }
    void records() {
        if (!recordWindow)
            return;
        if (!ImGui::Begin(label("Receiver records", "接收记录").c_str(), &recordWindow)) {
            ImGui::End();
            return;
        }
        recordHeight = ImGui::GetWindowSize().y;
        if (!imported.traces.empty()) {
            bool source = useImported;
            if (ImGui::Checkbox(l("Imported SEG-Y (off: simulation)", "导入 SEG-Y（取消：当前模拟）"),
                                &useImported) &&
                source != useImported) {
                recordView.invalidate();
                selectedReceiver = 0;
            }
        }
        recordView.drawContent(displayedRecords(), selectedReceiver);
        ImGui::End();
    }
    void shortcuts() {
        const auto &io = ImGui::GetIO();
        if (!io.WantTextInput && !ImGui::IsAnyItemActive() &&
            !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel)) {
            if (ImGui::IsKeyPressed(ImGuiKey_F1))
                help = !help;
            if (!help) {
                if (ImGui::IsKeyPressed(ImGuiKey_Space)) {
                    if (running)
                        running = false;
                    else
                        prepareRun(true);
                }
                if (ImGui::IsKeyPressed(ImGuiKey_N)) {
                    if (prepareRun(false))
                        simulation.advance(1, error);
                }
                if (ImGui::IsKeyPressed(ImGuiKey_C)) {
                    clear();
                }
                if (ImGui::IsKeyPressed(ImGuiKey_R))
                    fit = true;
                if (ImGui::IsKeyPressed(ImGuiKey_F)) {
                    SeisWave::Ui::setParameterDisplayMode(2,0);
                    parameterWindow = summaryWindow = waveWindow = recordWindow = layout = true;
                }
                if (ImGui::IsKeyPressed(ImGuiKey_G))
                    grid = !grid;
                if (ImGui::IsKeyPressed(ImGuiKey_B))
                    sponge = !sponge;
                const int direction = io.KeyShift ? -1 : 1;
                if (ImGui::IsKeyPressed(ImGuiKey_Q))
                    wavePalette = (wavePalette + direction + 14) % 14;
                if (ImGui::IsKeyPressed(ImGuiKey_W))
                    component = (component + direction + 4) % 4;
                if (ImGui::IsKeyPressed(ImGuiKey_E))
                    modelPalette = (modelPalette + direction + 14) % 14;
            }
        }
        if (help) {
            const auto *vp = ImGui::GetMainViewport();
            ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Appearing, {.5f, .5f});
            ImGui::SetNextWindowSize(
                {std::min(1100.f, vp->WorkSize.x * .94f), std::min(350.f, vp->WorkSize.y * .72f)},
                ImGuiCond_Appearing);
            if (ImGui::Begin(l("Keyboard shortcuts###2DHelp", "快捷键说明###2DHelp"), &help,
                             ImGuiWindowFlags_NoDocking)) {
                if (ImGui::BeginTable("##2DHelpHorizontal", 3,
                                      ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_BordersInnerV)) {
                    ImGui::TableNextColumn();
                    ImGui::SeparatorText(l("Run", "计算"));
                    ImGui::TextWrapped(
                        "%s", l("Space: run / pause\nN: one step\nC: clear current fields\nEdited parameters "
                                "apply on Run or Reset / apply.",
                                "Space：运行/"
                                "暂停\nN：单步\nC：清空当前波场\n修改参数后，运行或重置并应用使之生效。"));
                    ImGui::TableNextColumn();
                    ImGui::SeparatorText(l("Display", "显示"));
                    ImGui::TextWrapped(
                        "%s", l("Q: wave palette\nW: component\nE: material palette\nShift: reverse\nG: grid "
                                "/ B: sponge",
                                "Q：波场色带\nW：显示分量\nE：模型配色\nShift：反向\nG：网格 / B：吸收区"));
                    ImGui::TableNextColumn();
                    ImGui::SeparatorText(l("Workspace", "工作区"));
                    ImGui::TextWrapped("%s",
                                       l("R: fit model\nF: restore layout\nF1 / Esc: close help\nAppearance: "
                                         "adjust font size\nExport: import / export SEG-Y",
                                         "R：适配模型\nF：恢复布局\nF1 / "
                                         "Esc：关闭帮助\n界面外观：调整字体\n导出页：SEG-Y 导入/导出"));
                    ImGui::EndTable();
                }
            }
            ImGui::End();
            if (ImGui::IsKeyPressed(ImGuiKey_Escape))
                help = false;
        }
    }
};
} // namespace
#include "WorkflowCapture2D.inc"

int validateModelWorkflow2D() {
    std::string error;
    SeisWave::Ui::AppRuntime runtime;
    if (!runtime.open("2D model workflow validation", true, error))
        throw std::runtime_error(error);
    Workbench app;
    app.staged.nx = 160; app.staged.nz = 144;
    app.staged.relief = 5; app.staged.duration = .01; app.staged.maxSteps = 13;
    app.staged.sampleIntervalMs = .5;
    fitAcquisition(app.staged);
    if (!app.apply() || !app.simulation.advance(13, error))
        throw std::runtime_error(app.error + error);
    const auto saved = app.simulation.parameters();
    const double dt = app.simulation.dt();
    const auto out = SeisWave::Ui::outputRoot() / "validation/model-2d/ui";
    std::filesystem::create_directories(out);
    const auto path = out / std::filesystem::path(u8"可复现曲面模型.vts");
    const auto utf8 = path.u8string();
    std::snprintf(app.modelPath.data(), app.modelPath.size(), "%s", reinterpret_cast<const char *>(utf8.c_str()));
    if (!writeModelVts(app.simulation, path, app.activeSettings, error))
        throw std::runtime_error(error);
    app.staged.frequency = 45; app.staged.spacing = 2; app.staged.sampleIntervalMs = 1;
    if (!app.dirty() || !app.loadModel() || app.dirty() || app.simulation.step() != 0 ||
        app.simulation.parameters() != saved || app.simulation.dt() != dt || app.simulation.recordDt() != .0005)
        throw std::runtime_error("2D model load did not synchronize staged/active/dt/sample interval: " + app.error);
    app.staged.frequency = 50; app.staged.spacing = 2; app.staged.sampleIntervalMs = 1;
    if (!app.apply() || app.dirty() || app.simulation.dt() == dt || app.liveRecords().dt != .001)
        throw std::runtime_error("2D rebuild failed to synchronize dt / record interval / parameters");
    for (int i = 0; i < 6; ++i) {
        const int sizes[3][2]{{1600,1000},{1280,800},{960,640}};
        const int *size = sizes[i % 3];
        glfwSetWindowSize(runtime.window(), size[0], size[1]);
        setLanguage3D(i < 3 ? 1 : 0);
        applyTheme3D(i < 3 ? 9 : 4);
        SeisWave::Ui::setInterfaceFontSize(i < 3 ? 20.f : 26.f);
        app.category = i % 3 == 0 ? 0 : i % 3 == 1 ? 1 : 4;
        if (i >= 3) app.staged.frequency = 60; // Visible pending marker without modifying active dt.
        for (int frame = 0; frame < 4; ++frame) {
            runtime.begin();
            app.toolbar(); app.controls(); app.activeParameters(); app.wavefield(); app.records(); app.floatingControls();
            runtime.present();
        }
        const auto *parameters = ImGui::FindWindowByName(label("Parameters and run", "参数与运行").c_str());
        const auto *summary = ImGui::FindWindowByName(label("Active simulation parameters", "当前模拟参数").c_str());
        if (!parameters || !summary || !summary->Active ||
            !parameters->DockId || parameters->DockId != summary->DockId ||
            summary->Size.x < 150 || !app.domainVisible || app.liveRecords().dt != .001)
            throw std::runtime_error("2D active-parameter panel layout/data synchronization failed");
        int w, h;
        glfwGetFramebufferSize(runtime.window(), &w, &h);
        if (!SeisWave::Ui::capture(out / ("parameters-" + std::to_string(i) + ".ppm"), w, h))
            throw std::runtime_error("2D model workflow capture failed");
    }
    // Exercise the actual export controls and button, independently of parameter-pane scrolling.
    glfwSetWindowSize(runtime.window(), 1280, 800);
    setLanguage3D(1);
    SeisWave::Ui::setInterfaceFontSize(20);
    app.component = 3;
    const auto snapshotPath = out / "selected-wavefield.vts";
    const auto snapshotUtf8 = snapshotPath.u8string();
    std::snprintf(app.vts.data(), app.vts.size(), "%s", reinterpret_cast<const char *>(snapshotUtf8.c_str()));
    const auto drawExport = [&]() {
        runtime.begin();
        ImGui::SetNextWindowPos({20, 20});
        ImGui::SetNextWindowSize({460, 680});
        ImGui::Begin("Wavefield export controls");
        app.snapshotControls();
        ImGui::End();
        runtime.present();
    };
    const auto clickExport = [&](const ImRect &rect) {
        const auto point = rect.GetCenter();
        auto &io = ImGui::GetIO();
        io.AddMousePosEvent(point.x, point.y);
        io.AddMouseButtonEvent(0, true); drawExport();
        io.AddMouseButtonEvent(0, false); drawExport();
    };
    drawExport(); drawExport();
    clickExport(app.snapshotSaveRect);
    const auto snapshotText = [&]() {
        std::ifstream in(snapshotPath);
        return std::string(std::istreambuf_iterator<char>(in), {});
    };
    if (!app.error.empty() || snapshotText().find("<PointData Scalars=\"Pressure\">") == std::string::npos ||
        snapshotText().find("Name=\"Vz\"") != std::string::npos)
        throw std::runtime_error("2D snapshot button did not follow displayed pressure: " + app.error);
    clickExport(app.snapshotToggleRect);
    if (app.snapshotFollowDisplay || !app.snapshotComponents[3])
        throw std::runtime_error("2D snapshot manual toggle lost the displayed selection");
    clickExport(app.snapshotComponentRects[1]);
    clickExport(app.snapshotSaveRect);
    if (!app.snapshotComponents[1] || snapshotText().find("Name=\"Vz\"") == std::string::npos ||
        snapshotText().find("Name=\"Pressure\"") == std::string::npos)
        throw std::runtime_error("2D snapshot manual multi-component export failed");
    int captureWidth, captureHeight;
    glfwGetFramebufferSize(runtime.window(), &captureWidth, &captureHeight);
    SeisWave::Ui::capture(out / "snapshot-components.ppm", captureWidth, captureHeight);
    // UI export routing must pass compatibility, sampling and gain options together.
    const auto segyFile = out / "legacy-receivers.sgy";
    const auto segyUtf8 = segyFile.u8string();
    std::snprintf(app.segy.data(), app.segy.size(), "%s", reinterpret_cast<const char *>(segyUtf8.c_str()));
    app.useImported = false;
    app.segyProfile = 2; app.segyGain = 1e6; app.segyIntervalMs = .5;
    if (!app.simulation.advance(13, error)) throw std::runtime_error(error);
    app.exportSegy();
    SeisWave::ImportedRecords compatibleRecords;
    if (!app.error.empty() || !SeisWave::readSegy(segyFile, compatibleRecords, error) ||
        compatibleRecords.dt != .0005 || compatibleRecords.component != "Vz" || compatibleRecords.units != "scaled(m/s)")
        throw std::runtime_error("2D SEG-Y compatibility/gain/sampling controls were not applied: " + app.error + error);
    // A rejected edit must stop a running session without replacing its model
    // or discarding the receiver samples needed for export and analysis.
    const auto previousParameters = app.simulation.parameters();
    const auto previousSamples = app.simulation.receivers().front().vz;
    app.running = true;
    app.staged.receiverStart = app.staged.nx;
    if (app.apply() || app.running || app.simulation.parameters() != previousParameters ||
        app.simulation.receivers().front().vz != previousSamples)
        throw std::runtime_error("Rejected acquisition edit changed active model/records or left the session running");
    app.staged = previousParameters;
    app.error.clear();
    app.segy[0] = '\0';
    app.exportSegy();
    if (app.error.empty()) throw std::runtime_error("Blank SEG-Y destination was accepted");
    // Exercise the shared analysis viewer with empty, mismatched and invalid-dt
    // data; these previously reached unchecked sample/time array access.
    for (int invalid = 0; invalid < 3; ++invalid) {
        auto data = app.liveRecords();
        if (invalid == 0) data.traces.front().channels[0] = {};
        if (invalid == 1) data.traces.front().channels[1] = data.traces.front().channels[1].first(1);
        if (invalid == 2) data.dt = 0;
        runtime.begin();
        ImGui::Begin("Invalid record validation");
        app.recordView.drawContent(data, app.selectedReceiver);
        ImGui::End();
        runtime.present();
    }
    std::cout << "PASS workflow guards: rejected survey edit pauses and preserves model/records; blank SEG-Y path rejected; empty/mismatched/invalid-dt analysis is safe\n";
    std::cout << "PASS 2D model UI: UTF-8 VTS restore, staged/active synchronization, recomputed dt, 0.5/1 ms records, "
                 "responsive summary in 6 bilingual/theme/size/font layouts; actual snapshot controls follow pressure / manual Vz+pressure export; legacy SEG-Y gain/sampling\n";
    return 0;
}
#include "ThemePanelValidation2D.inc"
int runWorkbench2D(bool smoke, bool homeTest) {
    if(smoke)SeisWave::Ui::setParameterDisplayMode(2,0);
    std::string error;
    SeisWave::Ui::AppRuntime runtime;
    if (!runtime.open("SeisWave Lab · Curvilinear | 2D P-SV | CPU / CUDA", smoke, error)) {
        std::cerr << error << '\n';
        return 1;
    }
    if (smoke)
        std::filesystem::create_directories(SeisWave::Ui::outputRoot() / "screenshots/two-d");
    std::filesystem::create_directories(SeisWave::Ui::outputRoot() / "validation/two-d");
    Workbench app;
    if (smoke) {
        app.staged.nx = app.staged.nz = 160;
        app.staged.spacing = 2.5;
        app.staged.frequency = 30;
        app.staged.relief = 5;
        app.staged.sourceZ = 32;
        app.staged.duration = .1;
        app.staged.maxSteps = 0;
        fitAcquisition(app.staged);
    }
    if (!app.apply()) {
        std::cerr << app.error << '\n';
        return 2;
    }
    if (smoke) {
        if (!app.simulation.advance(app.simulation.totalSteps(), app.error)) {
            std::cerr << app.error << '\n';
            return 3;
        }
        const auto step = app.simulation.step();
        auto invalid = app.staged;
        invalid.nx = 2;
        if (app.simulation.initialize(invalid, app.error) || app.simulation.step() != step)
            throw std::runtime_error("2D failed initialization changed the running session");
        app.error.clear();
        if (!writeSnapshot(app.simulation, SeisWave::Ui::outputRoot() / "validation/two-d/smoke-2d.vts",
                           app.error) ||
            !writeReceivers(app.simulation, SeisWave::Ui::outputRoot() / "validation/two-d/smoke-2d.csv",
                            app.error))
            throw std::runtime_error(app.error);
    }
    const auto stepBefore = app.simulation.step();
    const double dtBefore = app.simulation.dt();
    double receiverFingerprint = 0;
    for (const auto &r : app.simulation.receivers())
        receiverFingerprint += std::accumulate(r.vz.begin(), r.vz.end(), 0.0);
    for (int frame = 0; !glfwWindowShouldClose(runtime.window()); ++frame) {
        if (smoke && !homeTest && frame % 10 == 0 && frame <= 50) {
            app.category = (frame / 10) % 5;
            const int sizes[3][2]{{1600, 1000}, {1280, 800}, {960, 640}};
            const int index = (frame / 10) % 3;
            glfwSetWindowSize(runtime.window(), sizes[index][0], sizes[index][1]);
            setLanguage3D(frame >= 30 ? 0 : 1);
            applyTheme3D(frame >= 30 ? 4 : 9);
            SeisWave::Ui::setInterfaceFontSize(frame == 0         ? 20.f
                                               : frame % 30 == 10 ? 22.f
                                               : frame % 30 == 20 ? 26.f
                                                                  : 18.f);
            app.layout = true;
            app.fit = true;
        }
        auto &io = ImGui::GetIO();
        if (smoke && !homeTest) {
            if (frame == 53) {
                const auto at = app.recordView.optionsBounds;
                io.AddFocusEvent(true);
                io.AddMousePosEvent((at[0] + at[2]) * .5f, (at[1] + at[3]) * .5f);
            }
            if (frame == 54)
                io.AddMouseButtonEvent(0, true);
            if (frame == 55)
                io.AddMouseButtonEvent(0, false);
            if (frame == 58)
                io.AddKeyEvent(ImGuiKey_Escape, true);
            if (frame == 59)
                io.AddKeyEvent(ImGuiKey_Escape, false);
            if (frame == 60) {
                app.category = 1;
                app.layout = true;
                glfwSetWindowSize(runtime.window(), 1600, 1000);
                setLanguage3D(1);
                applyTheme3D(9);
                SeisWave::Ui::setInterfaceFontSize(24);
            }
            if (frame == 61)
                io.AddFocusEvent(true);
            if (frame == 62) {
                const auto at = app.frequencyRect;
                io.AddMousePosEvent(at.Min.x + 45, at.GetCenter().y);
                io.AddMouseButtonEvent(0, true);
            }
            if (frame == 63)
                io.AddMouseButtonEvent(0, false);
            if (frame == 64) {
                io.AddKeyEvent(ImGuiMod_Ctrl, true);
                io.AddKeyEvent(ImGuiKey_A, true);
            }
            if (frame == 65) {
                io.AddKeyEvent(ImGuiKey_A, false);
                io.AddKeyEvent(ImGuiMod_Ctrl, false);
                io.AddInputCharactersUTF8("45");
            }
            if (frame == 66)
                io.AddKeyEvent(ImGuiKey_Enter, true);
            if (frame == 67) {
                io.AddKeyEvent(ImGuiKey_Enter, false);
                const auto at = app.rebuildRect.GetCenter();
                io.AddMousePosEvent(at.x, at.y);
            }
            if (frame == 68) {
                const auto at = app.rebuildRect.GetCenter();
                io.AddMousePosEvent(at.x, at.y);
                io.AddMouseButtonEvent(0, true);
            }
            if (frame == 69)
                io.AddMouseButtonEvent(0, false);
            if (frame == 71) {
                if (app.simulation.parameters().frequency != 45 || app.simulation.step() != 0 ||
                    app.simulation.dt() >= dtBefore) {
                    std::cerr << "staged=" << app.staged.frequency
                              << " active=" << app.simulation.parameters().frequency
                              << " step=" << app.simulation.step() << " dt=" << app.simulation.dt()
                              << " error=" << app.error << " rect=" << app.frequencyRect.Min.x << ","
                              << app.frequencyRect.Min.y << " reset=" << app.rebuildRect.Min.x << ","
                              << app.rebuildRect.Min.y << '\n';
                    int w, h;
                    glfwGetFramebufferSize(runtime.window(), &w, &h);
                    SeisWave::Ui::capture(SeisWave::Ui::outputRoot() / "screenshots/two-d/edit-failure.ppm",
                                          w, h);
                    throw std::runtime_error("Edited frequency / bottom reset button did not rebuild dt");
                }
            }
            if (frame == 71)
                io.AddKeyEvent(ImGuiKey_N, true);
            if (frame == 72)
                io.AddKeyEvent(ImGuiKey_N, false);
            if (frame == 73 && app.simulation.step() != 1)
                throw std::runtime_error("N failed after parameter reset");
            if (frame == 74) {
                const auto path = SeisWave::Ui::outputRoot() / "validation/two-d/ui-vz.sgy";
                std::snprintf(app.segy.data(), app.segy.size(), "%s", path.string().c_str());
                app.exportSegy();
                std::snprintf(app.importPath.data(), app.importPath.size(), "%s", path.string().c_str());
                app.importSegy();
                if (!app.useImported || app.imported.traces.size() != app.simulation.receivers().size())
                    throw std::runtime_error("2D SEG-Y UI round trip failed");
                app.category = 4;
                app.recordView.settings.style = 2;
            }
            if (frame == 76)
                io.AddKeyEvent(ImGuiKey_C, true);
            if (frame == 77)
                io.AddKeyEvent(ImGuiKey_C, false);
            if (frame == 78 && (app.simulation.step() != 0 || app.simulation.parameters().frequency != 45 ||
                                app.imported.traces.empty()))
                throw std::runtime_error("Clear altered parameters or imported records");
            if (frame == 80)
                io.AddKeyEvent(ImGuiKey_F1, true);
            if (frame == 81)
                io.AddKeyEvent(ImGuiKey_F1, false);
            if (frame == 85)
                io.AddKeyEvent(ImGuiKey_Escape, true);
            if (frame == 86)
                io.AddKeyEvent(ImGuiKey_Escape, false);
            if (frame == 88) {
                app.staged.frequency = 50;
                if (!app.prepareRun(true) || app.simulation.parameters().frequency != 50)
                    throw std::runtime_error("Run failed to apply pending frequency");
                app.running = false;
            }
            if (frame == 91) {
                app.category = 0;
                app.openLayers = true;
            }
            if (frame == 94) {
                app.layerPreset = 2;
                app.layerDraft = materialPresets()[2].layers;
            }
            if (frame == 96) {
                const auto at = app.layerUseRect.GetCenter();
                io.AddMousePosEvent(at.x, at.y);
                io.AddMouseButtonEvent(0, true);
            }
            if (frame == 97)
                io.AddMouseButtonEvent(0, false);
            if (frame == 99) {
                if (!app.staged.layered || app.staged.layers.size() != 5 ||
                    app.simulation.parameters().layered)
                    throw std::runtime_error("Layer dialog failed to apply staged preset transactionally");
                if (!app.apply())
                    throw std::runtime_error(app.error);
            }
            if (frame == 101) {
                const auto path = SeisWave::Ui::outputRoot() / "validation/terrain-2d/ui-profile.csv";
                std::filesystem::create_directories(path.parent_path());
                {
                    std::ofstream out(path);
                    out << "x_m,elevation_m\n1000,200\n1200,203\n1400,200\n";
                }
                std::snprintf(app.surfacePath.data(), app.surfacePath.size(), "%s", path.string().c_str());
                if (!app.loadTerrain() || !app.apply())
                    throw std::runtime_error(app.error);
                app.openTerrain = true;
            }
            if (frame == 106) {
                const auto at = app.terrainDoneRect.GetCenter();
                io.AddMousePosEvent(at.x, at.y);
                io.AddMouseButtonEvent(0, true);
            }
            if (frame == 107)
                io.AddMouseButtonEvent(0, false);
            if (frame == 105) {
                glfwSetWindowSize(runtime.window(), 960, 640);
                SeisWave::Ui::setInterfaceFontSize(26);
            }
            if (frame == 109) {
                glfwSetWindowSize(runtime.window(), 1600, 1000);
                SeisWave::Ui::setInterfaceFontSize(24);
            }
            if (frame == 118) {
                app.category = 1;
                app.staged.nx = app.staged.nz = 200;
                app.staged.spacing = 2.5;
                app.staged.frequency = 30;
                app.staged.relief = 5;
                app.staged.duration = .1;
                app.staged.maxSteps = 0;
                fitAcquisition(app.staged);
                if (!app.apply())
                    throw std::runtime_error(app.error);
            }
            if (frame == 120) {
                const auto at = app.sourceKindRect.GetCenter();
                io.AddMousePosEvent(at.x, at.y);
                io.AddMouseButtonEvent(0, true);
            }
            if (frame == 121)
                io.AddMouseButtonEvent(0, false);
            if (frame == 123) {
                if (app.sourceMomentRect.GetWidth() <= 0)
                    throw std::runtime_error("Source mechanism popup did not open");
                const auto at = app.sourceMomentRect.GetCenter();
                io.AddMousePosEvent(at.x, at.y);
                io.AddMouseButtonEvent(0, true);
            }
            if (frame == 124)
                io.AddMouseButtonEvent(0, false);
            if (frame == 128 && app.staged.source.kind != SeisWave::SourceKind::IsotropicMoment)
                throw std::runtime_error("Source combo did not switch to isotropic moment");
            if (frame == 129) {
                const auto at = app.rebuildRect.GetCenter();
                io.AddMousePosEvent(at.x, at.y);
                io.AddMouseButtonEvent(0, true);
            }
            if (frame == 130)
                io.AddMouseButtonEvent(0, false);
            if (frame == 132) {
                if (app.simulation.parameters().source.kind != SeisWave::SourceKind::IsotropicMoment ||
                    !app.simulation.advance(app.simulation.totalSteps(), app.error))
                    throw std::runtime_error("Source type did not apply / propagate");
            }
            if (frame == 136) {
                for (const auto &example : elevationExamples) {
                    if (std::string_view(example.file) == "hill_valley_1km.csv") {
                        app.staged.nx = app.staged.nz = 401;
                        app.staged.spacing = 2.5;
                        app.staged.flattenDepth = 0;
                    }
                    const auto path = (SeisWave::Ui::elevationExampleRoot() / example.file).u8string();
                    std::snprintf(app.surfacePath.data(), app.surfacePath.size(), "%s",
                                  reinterpret_cast<const char *>(path.c_str()));
                    if (!app.loadTerrain() || !app.apply() || !app.simulation.advance(1, app.error))
                        throw std::runtime_error(app.error);
                }
                app.staged.nz = 96;
                app.staged.spacing = 10;
                app.staged.frequency = 5;
                app.staged.sourceZ = 20;
                app.staged.duration = .01;
                app.staged.maxSteps = 2;
                for (int i = 0; i < static_cast<int>(surfacePresets.size()); ++i) {
                    if (!app.chooseTerrainPreset(i))
                        throw std::runtime_error(app.error);
                    for (int km : {1, 2, 4}) {
                        if (!app.setTerrainPresetWidth(km * 1000.) || !app.apply() ||
                            !app.simulation.advance(1, app.error) ||
                            app.simulation.parameters().surfaceSamples.back()[0] != km * 1000.0 ||
                            app.simulation.parameters().surfaceDatum != 0 || app.staged.spacing != 10 ||
                            app.staged.nz != 96)
                            throw std::runtime_error("Terrain preset did not follow edited model width: " + app.error);
                    }
                    app.staged.nx = 301; // Also exercise direct geometry edits, without the width helper.
                    app.staged.surfaceDatum = 17;
                    if (!app.apply() || app.staged.surfaceSamples.back()[0] != 3000 ||
                        app.staged.surfaceDatum != 17)
                        throw std::runtime_error("Direct X edit lost preset coverage or user datum");
                }
                if (!app.setTerrainPresetWidth(4000) || !app.apply())
                    throw std::runtime_error(app.error);
                std::cout << "2D terrain presets UI PASS: all eight presets adapt from original data to 1/2/4 km\n";
                app.openTerrain = true;
            }
            if (frame == 112) {
                app.staged.nx = 1200;
                app.staged.nz = 1100;
                app.staged.spacing = 10;
                app.staged.relief = 0;
                app.staged.layered = false;
                app.staged.importedSurface = false;
                app.staged.maxSteps = 2;
                app.staged.duration = .01;
                app.grid = true;
                if (!app.apply() || !app.simulation.advance(2, app.error))
                    throw std::runtime_error(app.error);
            }
        }
        if (homeTest && frame == 2) {
            const auto at = app.homeRect.GetCenter();
            io.AddMousePosEvent(at.x, at.y);
            io.AddMouseButtonEvent(0, true);
        }
        if (homeTest && frame == 3)
            io.AddMouseButtonEvent(0, false);
        if (app.running) {
            const auto begin = std::chrono::steady_clock::now();
            for (int i = 0; i < 8 && !app.simulation.completed(); ++i) {
                if (!app.simulation.advance(1, app.error)) {
                    app.running = false;
                    break;
                }
                if (std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin)
                        .count() > 16)
                    break;
            }
            if (app.simulation.completed())
                app.running = false;
        }
        runtime.begin();
        app.toolbar();
        app.controls();
        app.modelDialogs();
        app.activeParameters();
        if (app.dirty())
            app.running = false;
        app.wavefield();
        app.records();
        app.floatingControls();
        if(app.dirty())app.running=false;
        app.shortcuts();
        runtime.present();
        if (app.home) {
            std::cout << "2D Home route PASS\n";
            return SeisWave::Ui::returnHome;
        }
        if (smoke && !homeTest && frame == 83) {
            const auto *help =
                ImGui::FindWindowByName(l("Keyboard shortcuts###2DHelp", "快捷键说明###2DHelp"));
            if (!app.help || !help || help->Size.x <= help->Size.y)
                throw std::runtime_error("F1 help is not horizontal");
            int w, h;
            glfwGetFramebufferSize(runtime.window(), &w, &h);
            SeisWave::Ui::capture(SeisWave::Ui::outputRoot() / "screenshots/two-d/help-2d.ppm", w, h);
        }
        if (smoke && !homeTest && frame == 90) {
            if (!app.error.empty())
                throw std::runtime_error(app.error);
            std::cout << "2D UI PASS: edited 45 Hz by keyboard, bottom reset recomputed dt, N/C, "
                         "live/imported SEG-Y, automatic apply on Run, horizontal F1, fonts 18..26\n";
        }
        if (smoke && !homeTest && (frame == 95 || frame == 104)) {
            int w, h;
            glfwGetFramebufferSize(runtime.window(), &w, &h);
            SeisWave::Ui::capture(SeisWave::Ui::outputRoot() / (frame == 95
                                                                    ? "screenshots/two-d/layers-2d.ppm"
                                                                    : "screenshots/two-d/terrain-2d.ppm"),
                                  w, h);
            if (ImGui::GetCurrentContext()->OpenPopupStack.empty())
                throw std::runtime_error("Model editor popup did not open");
        }
        if (smoke && !homeTest && frame == 110) {
            if (!ImGui::GetCurrentContext()->OpenPopupStack.empty() ||
                !app.simulation.parameters().importedSurface ||
                app.simulation.parameters().layers.size() != 5)
                throw std::runtime_error("Terrain / layer dialog workflow failed");
            std::cout << "2D model UI PASS: five-layer preset popup, external CSV terrain, datum, geometry / "
                         "dt rebuild\n";
        }
        if (smoke && !homeTest && frame == 116) {
            if (app.renderStrideX <= 1 || app.renderStrideZ <= 1 || !app.domainVisible ||
                app.simulation.parameters().nx != 1200 || app.simulation.parameters().nz != 1100)
                throw std::runtime_error("Large 2D view sampling / bounds failed");
            std::cout << "2D large model UI PASS: 1200x1100, full simulation / adaptive view stride\n";
        }
        if (smoke && !homeTest && (frame == 134 || frame == 140)) {
            int w, h;
            glfwGetFramebufferSize(runtime.window(), &w, &h);
            SeisWave::Ui::capture(SeisWave::Ui::outputRoot() /
                                      (frame == 134 ? "screenshots/two-d/source-explosion.ppm"
                                                    : "screenshots/two-d/elevation-examples.ppm"),
                                  w, h);
            if (frame == 140) {
                std::cout << "2D source / examples UI PASS: actual mechanism combo and rebuild, explosion "
                             "propagation, all deployed elevation files\n";
                return 0;
            }
        }
        if (smoke && !homeTest && frame % 10 == 7 && frame <= 57) {
            int width, height;
            glfwGetFramebufferSize(runtime.window(), &width, &height);
            if (!SeisWave::Ui::capture(SeisWave::Ui::outputRoot() /
                                           ("screenshots/two-d/2d-" + std::to_string(frame / 10) + ".ppm"),
                                       width, height))
                return 4;
            double check = 0;
            for (const auto &r : app.simulation.receivers())
                check += std::accumulate(r.vz.begin(), r.vz.end(), 0.0);
            if (!app.domainVisible || app.recordHeight < 140)
                throw std::runtime_error("2D layout cropped the physical model or receiver panel");
            if (app.simulation.step() != stepBefore || app.simulation.dt() != dtBefore ||
                check != receiverFingerprint || !app.error.empty())
                throw std::runtime_error("2D presentation modified the numerical session");
            if (frame == 57) {
                const auto &popups = ImGui::GetCurrentContext()->OpenPopupStack;
                if (popups.empty() || !popups.back().Window || !popups.back().Window->Active)
                    throw std::runtime_error("Record display parameters did not open");
                std::cout << "2D workspace PASS: CPU/CUDA session, failed rebuild, VTS/CSV, 6 layouts, "
                             "bilingual themes, raw records retained\n";
                // Continue with real widget edits, reset, SEG-Y and keyboard checks.
            }
        }
        if (homeTest && frame > 8)
            throw std::runtime_error("2D Home button did not route back");
    }
    return 0;
}
} // namespace Seis2D::Curvilinear::Ui
