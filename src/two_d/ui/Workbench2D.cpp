#include "GridDisplay2D.hpp"
#include "InitialModel2D.hpp"
#include "ScalarLegendStrip.hpp"
#include "ParameterPanel2D.hpp"
#include "ModelEditor2D.hpp"
#include "CommandStrip2D.hpp"
#include "AbsorberDisplay2D.hpp"
#include "ModuleNavigation2D.hpp"
#include "MenuWidgets.hpp"
#include "Workbench2D.hpp"
#include "../MemoryBudget2D.hpp"
#include "AppRuntime.hpp"
#include "GlassMaterial.hpp"
#include "ParameterPanelHost.hpp"
#include "ThemeValidation.hpp"
#include "Seis2D/AsyncSimulation2D.hpp"
#include "WorkspaceTools.hpp"
#include "SimulationTiming.hpp"
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
#include "Seis2D/SurfaceSurvey2D.hpp"
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
#include <limits>
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
    p.sourceZ = std::clamp(p.sourceZ, p.source.kind==SeisWave::SourceKind::IsotropicMoment?margin:0, std::max(margin, p.nz - 1 - margin));
    p.receiverStart = std::clamp(p.receiverStart, 0, std::max(0, p.nx - 1));
    p.receiverDepth = std::clamp(p.receiverDepth, 0, std::max(0, p.nz - 1));
    p.receiverStride = std::max(1, p.receiverStride);
    p.receiverCount =
        std::max(1, std::min({p.receiverCount, 1024, 1 + (p.nx - 1 - p.receiverStart) / p.receiverStride}));
}
struct ElasticDraft2D {Parameters parameters;ComputeSettings3D compute;int backend{};};
std::array<std::optional<ElasticDraft2D>,2> elasticDrafts2D;
struct Workbench {
    SeisWave::Ui::ModuleNavigation2D navigation;
    bool rememberDraft{};
    AsyncSimulation simulation;
    Parameters staged;
    ComputeSettings3D settings, activeSettings;
    int selectedBackend{}, category{}, component{}, wavePalette{1}, modelPalette{1}, modelComponent{},
        selectedReceiver{};
    bool running{}, home{}, help{}, fit{true}, grid{true}, sponge{}, lastExternalOverlay{},normalized{true}, showWave{true},
        showModel{true};
    float gain{1}, absorberOpacity{SeisWave::Ui::defaultAbsorberOpacity2D};
    int renderedAbsorberCells{};
    double displayedWaveRange{};
    std::string error, status;
    std::vector<float> frame;
    std::array<char, 2048> vts{}, csv{}, segy{}, importPath{}, surfacePath{}, modelPath{};
    int activeTerrainPreset{-1};
    SurfaceSamples activeTerrainPresetSource;
    ImRect layerEditRect;
    int renderStrideX{1}, renderStrideZ{1}, gridDisplayStride{1};
    SeisWave::Ui::RecordView recordView;
    SeisWave::ImportedRecords imported;
    bool useImported{};
    int segyProfile{};
    double segyGain{1}, segyIntervalMs{.5};
    std::array<bool, 3> exportComponents{true, false, false};
    bool snapshotFollowDisplay{true}, snapshotMaterial{};
    std::array<bool, 7> snapshotComponents{false, true, false, false, false, false, false};
    std::uint64_t recordGeneration{1}, importedRevision{1};
    ImRect rebuildRect, frequencyRect, sourceKindRect, sourceMomentRect,absorptionRect,computeGroupRect,externalAbsorptionRect,precisionRect;
    std::array<ImRect,2> precisionChoiceRects;
    std::array<ImRect,3> absorptionChoiceRects;
    ImRect snapshotToggleRect, snapshotSaveRect,attenuationToggleRect,attenuationQpRect;
    std::array<ImRect, 7> snapshotComponentRects;
    int lastHeight{};
    Seis3D::Curvilinear::CudaDeviceInventory3D inventory;
    ImRect homeRect,elapsedRect,imagingRect;
    ImRect surfaceReceiverRect,autoReceiverRect;
    std::array<ImVec4,2> runtimeOptionRects;
    bool layout{true}, domainVisible{}, focusParameters{};
    int lastWidth{};
    float recordHeight{};
    SeisWave::Ui::ParameterPanelHost parameterHost{2};
    SeisWave::Ui::WorkspaceTools workspace;
    int floatingCategory{},lastParameterMode{};
    bool parameterWindow{true}, waveWindow{true}, recordWindow{true}, summaryWindow{true};
    int groupEpoch{};
    bool groupsExpanded{true};
    std::unordered_map<ImGuiID,int> groupApplied;
    SeisWave::Ui::ModelEditor2D modelEditor{staged,surfacePath,error,status};
    auto& editorState(){return modelEditor;}
    std::vector<double>& terrainPreviewX=modelEditor.terrainPreviewX;
    std::vector<double>& terrainPreviewZ=modelEditor.terrainPreviewZ;
    std::vector<Layer>& layerDraft=modelEditor.layerDraft;
    bool& openLayers=modelEditor.openLayers;
    bool& openTerrain=modelEditor.openTerrain;
    bool& zeroSurfaceX=modelEditor.zeroSurfaceX;
    int& layerPreset=modelEditor.layerPreset;
    int& terrainPreset=modelEditor.terrainPreset;
    int& terrainResampledNx=modelEditor.terrainResampledNx;
    double& terrainResampledSpacing=modelEditor.terrainResampledSpacing;
    SurfaceSamples& terrainPresetSource=modelEditor.terrainPresetSource;
    std::string& layerError=modelEditor.layerError;
    ImVec2& dialogViewport=modelEditor.dialogViewport;
    ImRect& layerUseRect=modelEditor.layerUseRect;
    ImRect& terrainDoneRect=modelEditor.terrainDoneRect;
    bool parameterGroup(const char *title, ImGuiTreeNodeFlags flags = 0) {
        return SeisWave::Ui::parameterGroup2D(title, flags, groupEpoch, groupsExpanded, groupApplied);
    }
    Workbench(bool viscoelastic = false, bool remember = false) {
        modelEditor.browseTerrain=[this]{browse(surfacePath,false,true);};
        modelEditor.fitAcquisition=[](Parameters& p){fitAcquisition(p);};
        rememberDraft = remember;
        navigation.current =
            viscoelastic ? SeisWave::Ui::Module2D::Viscoelastic : SeisWave::Ui::Module2D::Elastic;
        SeisWave::Ui::applyInitialModel2D(staged);
        staged.externalAbsorption = true;
        staged.autoSurfaceReceivers=true;
        inventory = Seis3D::Curvilinear::queryCudaDevices3D();
        selectedBackend = Seis3D::Curvilinear::selectCudaDevice3D(inventory).available ? 1 : 0;
        if (remember && elasticDrafts2D[viscoelastic ? 1 : 0]) {
            const auto &draft = *elasticDrafts2D[viscoelastic ? 1 : 0];
            staged = draft.parameters;
            settings = draft.compute;
            selectedBackend = draft.backend;
        }
        if (auto model = SeisWave::Ui::modelMailbox2D().take(navigation.current)) {
            staged = std::move(model->model);
            settings = model->compute;
            selectedBackend = static_cast<int>(model->backend);
        }
        if (remember || viscoelastic)
            staged.attenuation.enabled = viscoelastic;
        const auto output = SeisWave::Ui::outputRoot() / "2d";
        std::filesystem::create_directories(output);
        const auto setPath = [&](auto &field, const char *file) {
            const auto name = (output / file).u8string();
            std::snprintf(field.data(), field.size(), "%s", reinterpret_cast<const char *>(name.c_str()));
        };
        setPath(vts, "wavefield.vts");
        setPath(modelPath, "model.vts");
        setPath(csv, "receivers.csv");
        setPath(segy, "receivers.sgy");
        syncSurfaceReceivers();
    }
    void syncSurfaceReceivers(){
        if(staged.nx<8||staged.nz<8||staged.nx>maxAxisNodes||staged.nz>maxAxisNodes)return;
        const int margin=staged.absorbing&&!staged.externalAbsorption?Detail::domainLayout(staged).widthX:0;
        synchronizeSurfaceReceivers(staged,false,1024,margin);
    }
    ~Workbench() {
        if (rememberDraft) {
            auto p = staged;
            p.attenuation.enabled = navigation.current == SeisWave::Ui::Module2D::Viscoelastic;
            elasticDrafts2D[p.attenuation.enabled ? 1 : 0] =
                ElasticDraft2D{std::move(p), settings, selectedBackend};
        }
    }
    bool apply() {
        running = false;
        if (rememberDraft &&
            staged.attenuation.enabled != (navigation.current == SeisWave::Ui::Module2D::Viscoelastic)) {
            error = l("Switch to the matching physics module before rebuilding.",
                      "请先切换到对应的物理模块，再重建模型。");
            return false;
        }
        if (!syncTerrainPreset())
            return false;
        syncSurfaceReceivers();
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
        const auto source = simulation.grid().coordinates[simulation.grid().modelIndex(
            (p.sourceX < 0 ? p.nx / 2 : p.sourceX), p.sourceZ)];
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
        status = std::string(l("Raw wavefield exported: ", "原始物理波场已导出："))+precisionName(simulation.parameters().precision);
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
        ImGui::TextWrapped("%s", l("Physical values in the active calculation precision; display gain, palette and normalization are excluded.",
                                   "按当前计算精度保存原始物理值，不应用显示增益、色带或归一化。"));
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
        if (rememberDraft && document.parameters.attenuation.enabled !=
                                 (navigation.current == SeisWave::Ui::Module2D::Viscoelastic)) {
            const auto target = document.parameters.attenuation.enabled
                                    ? SeisWave::Ui::Module2D::Viscoelastic
                                    : SeisWave::Ui::Module2D::Elastic;
            document.compute.cudaDevice = -1;
            if (document.backend == BackendKind3D::Cuda &&
                !Seis3D::Curvilinear::queryBackend3D(BackendKind3D::Cuda).available)
                document.backend = BackendKind3D::Cpu;
            SeisWave::Ui::modelMailbox2D().post(
                target, {navigation.current, std::move(document.parameters), document.compute, document.backend});
            navigation.requested = SeisWave::Ui::moduleRoute(target);
            return true;
        }
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
        SeisWave::Ui::setGlassSceneActive(running);
        if(workspace.beginFrame()){layout=false;lastParameterMode=parameterHost.mode();}
        if(!workspace.message.empty())status=std::exchange(workspace.message,{});
        if (SeisWave::Ui::beginWorkbenchMenuBar()) {
            if (button("Home", "主页", FunctionRole3D::Model))
                home = true;
            homeRect = {ImGui::GetItemRectMin(), ImGui::GetItemRectMax()};
            navigation.draw(staged,settings,static_cast<BackendKind3D>(selectedBackend),error);
            imagingRect=navigation.rootRect;
            if(SeisWave::Ui::MenuBarOverflow more({l("Workspace","工作区"),l("Appearance","界面外观"),l("Parameter panels","参数面板"),l("Help [F1]","帮助 [F1]")},l("More","更多"));more.open){
            if (SeisWave::Ui::beginDropdownMenu(l("Workspace", "工作区"))) {
                std::array<bool,4> visibility{parameterWindow,summaryWindow,waveWindow,recordWindow};
                if(workspace.menu(2,visibility)){parameterWindow=visibility[0];summaryWindow=visibility[1];waveWindow=visibility[2];recordWindow=visibility[3];lastParameterMode=parameterHost.mode();layout=false;}
                ImGui::MenuItem(l("Parameters", "参数面板"), nullptr, &parameterWindow);
                ImGui::MenuItem(l("Active parameters", "当前生效参数"), nullptr, &summaryWindow);
                ImGui::MenuItem(l("Wavefield", "波场窗口"), nullptr, &waveWindow);
                ImGui::MenuItem(l("Receivers", "接收记录"), nullptr, &recordWindow);
                if (ImGui::MenuItem(l("Restore layout [F]", "恢复布局 [F]"))) {
                    SeisWave::Ui::setParameterDisplayMode(2,0);
                    workspace.forgetFocus();
                    parameterWindow = summaryWindow = waveWindow = recordWindow = layout = true;
                }
                ImGui::EndMenu();
            }
            if (SeisWave::Ui::beginDropdownMenu(l("Appearance", "界面外观"))) {
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
                }
                glassAppearanceControls3D();
                ImGui::EndMenu();
            }
            if(SeisWave::Ui::beginDropdownMenu(l("Parameter panels","参数面板"))){if(parameterHost.menu())parameterWindow=true;ImGui::EndMenu();}
            if (button("Help [F1]", "帮助 [F1]", FunctionRole3D::Display))
                help = !help;
            }
            SeisWave::Ui::endWorkbenchMenuBar();
        }
        const auto *vp = ImGui::GetMainViewport();
        const auto elapsed=std::string(l("Compute time: ","计算耗时："))+SeisWave::Ui::computeElapsedText(simulation.diagnostics().totalStepMs);
        char progress[192];
        std::snprintf(progress,sizeof(progress),"%d / %d (%.1f%%) | t %.4f s | dt %.5f ms",simulation.step(),simulation.totalSteps(),
                      100.0*simulation.step()/std::max(1,simulation.totalSteps()),simulation.step()*simulation.dt(),simulation.dt()*1000);
        const char* commands[]{l(running?"Pause [Space]":"Run [Space]",running?"暂停 [Space]":"运行 [Space]"),
            l("Step [N]","单步 [N]"),l("Clear [C]","清空 [C]"),l("Apply / rebuild","应用 / 重建"),l("Fit [R]","适配 [R]")};
        const auto& style=ImGui::GetStyle();std::array<float,7> widths{};
        for(int i=0;i<5;++i)widths[i]=ImGui::CalcTextSize(commands[i]).x+2*style.FramePadding.x;
        widths[5]=ImGui::CalcTextSize(elapsed.c_str()).x;
        widths[6]=ImGui::CalcTextSize(progress).x+2*style.FramePadding.x;
        SeisWave::Ui::CommandStrip2D strip(widths,{0,0,0,1,2,3,3},
            {l("Compute","计算"),l("Model","模型"),l("View","视图"),l("Status","状态")});
        const int rows=strip.rows(vp->WorkSize.x-2*style.WindowPadding.x);
        const float height=rows*ImGui::GetFrameHeight()+(rows-1)*style.ItemSpacing.y+2*style.WindowPadding.y;
        ImGui::SetNextWindowPos(vp->WorkPos);
        ImGui::SetNextWindowSize({vp->WorkSize.x, height});
        ImGui::Begin("##2DCommands", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoMove |
                         ImGuiWindowFlags_NoSavedSettings);
        const auto next=[&](int i){strip.next(i);};
        next(0);
        if (button(running ? "Pause [Space]" : "Run [Space]", running ? "暂停 [Space]" : "运行 [Space]",
                   FunctionRole3D::Run)) {
            if (running)
                running = false;
            else
                prepareRun(true);
        }
        next(1);
        if (button("Step [N]", "单步 [N]", FunctionRole3D::Run) && prepareRun(false))
            simulation.advance(1, error);
        next(2);
        if (button("Clear [C]", "清空 [C]", FunctionRole3D::Danger))
            clear();
        next(3);
        if(button("Apply / rebuild","应用 / 重建",FunctionRole3D::Model))apply();
        next(4);
        if (button("Fit [R]", "适配 [R]", FunctionRole3D::Display))
            fit = true;
        next(5);ImGui::AlignTextToFramePadding();
        // Read after buttons so Clear/Step is reflected in the same UI frame.
        const auto currentElapsed=std::string(l("Compute time: ","计算耗时："))+SeisWave::Ui::computeElapsedText(simulation.diagnostics().totalStepMs);
        ImGui::TextColored(functionColor3D(FunctionRole3D::Run),"%s",currentElapsed.c_str());
        elapsedRect={ImGui::GetItemRectMin(),ImGui::GetItemRectMax()};
        if(ImGui::IsItemHovered())ImGui::SetTooltip("%s",SeisWave::Ui::computeElapsedHelp(language3D()!=0));
        next(6);
        std::snprintf(progress,sizeof(progress),"%d / %d (%.1f%%) | t %.4f s | dt %.5f ms",simulation.step(),simulation.totalSteps(),
                      100.0*simulation.step()/std::max(1,simulation.totalSteps()),simulation.step()*simulation.dt(),simulation.dt()*1000);
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
        if(lastParameterMode!=parameterHost.mode()){layout=!workspace.changeMode(lastParameterMode,parameterHost.mode());lastParameterMode=parameterHost.mode();}
        if(lastWidth!=static_cast<int>(vp->Size.x)||lastHeight!=static_cast<int>(vp->Size.y))fit=true;
        if (layout || !ImGui::DockBuilderGetNode(id)) {
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
        lastWidth=static_cast<int>(vp->Size.x);lastHeight=static_cast<int>(vp->Size.y);
        ImGui::End();
    }
    void updateTerrainPreview(){modelEditor.updateTerrainPreview();}
    bool syncTerrainPreset(){return modelEditor.syncTerrainPreset();}
    bool chooseTerrainPreset(int i){return modelEditor.chooseTerrainPreset(i);}
    bool setTerrainPresetWidth(double w){return modelEditor.setTerrainPresetWidth(w);}
    void terrainPresetControls(){modelEditor.terrainPresetControls();}
    bool loadTerrain(){return modelEditor.loadTerrain();}
    void modelDialogs(){modelEditor.modelDialogs();}
    void attenuationControls() {
        const auto input=[&](const char* en,const char* zh,double& value,double increment){ImGui::TextUnformatted(l(en,zh));ImGui::SetNextItemWidth(-1);const auto id=std::string("##")+en;ImGui::InputDouble(id.c_str(),&value,increment,increment*10,"%.3f");};
        if(parameterGroup(l("Qp / Qs attenuation (GSLS)","Qp / Qs 黏弹性（GSLS）"),0)) {
            auto& q=staged.attenuation;ImGui::Checkbox(l("Enable material attenuation","启用介质衰减"),&q.enabled);attenuationToggleRect={ImGui::GetItemRectMin(),ImGui::GetItemRectMax()};
            if(q.enabled){
                if(!staged.layered){input("Qp","Qp",q.qp,5);attenuationQpRect={ImGui::GetItemRectMin(),ImGui::GetItemRectMax()};input("Qs","Qs",q.qs,5);}
                else ImGui::TextWrapped("%s",l("Qp/Qs are set per layer in the layer editor.","Qp/Qs 在层表编辑器中逐层设置。"));
                ImGui::SliderInt(l("Relaxation mechanisms","松弛机制数"),&q.mechanisms,1,3);
                input("Band minimum (Hz)","拟合频带下限（Hz）",q.minFrequency,1);input("Band maximum (Hz)","拟合频带上限（Hz）",q.maxFrequency,5);input("Reference frequency (Hz)","参考频率（Hz）",q.referenceFrequency,1);
                ImGui::TextWrapped("%s",l("Vp/Vs are phase velocities at the reference frequency. Q is fitted over this band; more mechanisms cost more memory. Passive bulk/shear checks may reject incompatible Qp/Qs.","Vp/Vs 为参考频率处相速度；Q 在所设频带内近似拟合。机制数越多，内存越大。不满足体积/剪切被动性的 Qp/Qs 将被拒绝。"));
                if(staged.frequency<q.minFrequency||2.5*staged.frequency>q.maxFrequency)ImGui::TextWrapped("%s",l("The source effective band may exceed the Q fit band.","源的有效频带可能超出 Q 拟合频带，请核对。"));
            }
        }
    }
    void controls(){recordView.workspace=&workspace;if(parameterHost.mode()!=1)controlsPanel(false);}
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
        SeisWave::Ui::parameterTabs2D(category);
        SeisWave::Ui::parameterGroupButtons2D(groupEpoch, groupsExpanded);
        const SeisWave::Ui::ParameterSession2D session{simulation.initialized(), dirty(),
            simulation.parameters().frequency, simulation.dt(), error};
        const float footer = session.height();
        ImGui::BeginChild("##2DParameterFields", {0, -footer}, ImGuiChildFlags_None,
                          ImGuiWindowFlags_AlwaysVerticalScrollbar);
        const auto input = [&](const char *name, const char *chinese, double &value, double increment = 1, const char* format="%.3f") {
            ImGui::TextUnformatted(l(name, chinese));
            ImGui::SetNextItemWidth(-1);
            const auto id = std::string("##") + name;
            ImGui::InputDouble(id.c_str(), &value, increment, increment * 10, format);
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
                const auto cpuBudget=Detail::memoryBudget(staged,BackendKind3D::Cpu);
                const auto gpuBudget=Detail::memoryBudget(staged,BackendKind3D::Cuda);
                const auto domain=Detail::domainLayout(staged);
                ImGui::Text("%s %d x %d",l("Computational nodes (incl. absorber)","实际计算节点（含吸收带）"),domain.nx,domain.nz);
                ImGui::TextWrapped(
                    "%s %.0f MiB | GPU %.0f MiB", l("Grid estimate: RAM", "网格预算：内存"),
                    cpuBudget.hostBytes/(1024*1024),gpuBudget.deviceBytes/(1024*1024));
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
            attenuationControls();
            modelFiles();
        } else if (category == 1) {
            if(button("Uniform surface receivers","地表均匀布设",FunctionRole3D::Model,{-1,0})){
                const int margin=staged.absorbing&&!staged.externalAbsorption?Detail::domainLayout(staged).widthX:0;
                if(!staged.autoSurfaceReceivers)staged.receiverTargetCount=staged.receiverCount;
                staged.autoSurfaceReceivers=true;
                if(synchronizeSurfaceReceivers(staged,false,1024,margin))
                    status=l("Surface receiver line staged; count adjusted for uniform node spacing. Reset / apply.","地表均匀检波器已设置；道数按整数节点间距就近调整，重置并应用后生效。");
                else error=l("Set valid model dimensions first.","请先设置有效的模型尺寸。");
            }
            surfaceReceiverRect={ImGui::GetItemRectMin(),ImGui::GetItemRectMax()};
            if(ImGui::IsItemHovered())ImGui::SetTooltip("%s",l("Uniform X spacing along the surface; uses current count as a target. Internal absorbers are excluded. Does not change the source.",
                "按当前道数沿X等间距铺设，贴合地表；内部吸收模式自动避开侧吸收带，不改变震源。"));
            if(ImGui::Checkbox(l("Auto surface receivers","自动沿地表均匀布设"),&staged.autoSurfaceReceivers)&&staged.autoSurfaceReceivers)
                staged.receiverTargetCount=staged.receiverCount;
            autoReceiverRect={ImGui::GetItemRectMin(),ImGui::GetItemRectMax()};
            syncSurfaceReceivers();
            if(staged.autoSurfaceReceivers){
                integer("Requested count (max 1024)","期望道数（最多1024）",staged.receiverTargetCount);
                ImGui::Text("%s %d",l("Actual receivers","实际道数"),staged.receiverCount);
            }
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
                ImGui::BeginDisabled(staged.autoSurfaceReceivers);
                integer("Start X", "起点 X", staged.receiverStart);
                integer("Count (max 1024)", "接收数（最多 1024）", staged.receiverCount);
                integer("Index stride", "节点间隔", staged.receiverStride);
                integer("Depth index", "深度索引", staged.receiverDepth);
                ImGui::EndDisabled();
                if (button("Fit source / receivers", "适配震源/接收位置", FunctionRole3D::Model, {-1, 0}))
                    fitAcquisition(staged);
                samplingControls();
                ImGui::TextWrapped("%s", l("Vx and Vz share the same output axis; time zero is included.",
                                           "Vx、Vz 共用输出时间轴，记录包含零时刻。"));
            }
        } else if (category == 2) {
            const bool computeOpen=parameterGroup(l("Compute backend", "计算核心"), ImGuiTreeNodeFlags_DefaultOpen);
            computeGroupRect={ImGui::GetItemRectMin(),ImGui::GetItemRectMax()};
            if (computeOpen) {
                const char* precisionChoices[]{l("FP64 (validation / default)","FP64（验证／默认）"),l("FP32 (fields and model)","FP32（波场与模型单精度）")};
                ImGui::TextUnformatted(l("Calculation precision","计算精度"));ImGui::SetNextItemWidth(-1);
                const bool precisionOpen=ImGui::BeginCombo("##2Dprecision",precisionChoices[static_cast<int>(staged.precision)]);
                precisionRect={ImGui::GetItemRectMin(),ImGui::GetItemRectMax()};
                if(precisionOpen){for(int option=0;option<2;++option){if(ImGui::Selectable(precisionChoices[option],static_cast<int>(staged.precision)==option))staged.precision=static_cast<Precision>(option);
                    precisionChoiceRects[option]={ImGui::GetItemRectMin(),ImGui::GetItemRectMax()};}ImGui::EndCombo();}
                ImGui::TextWrapped("%s",l("Precision applies to fields, model coefficients and boundary memories. Changing it rebuilds from t=0.","精度同时作用于波场、模型系数及边界记忆；修改后从零时刻重建。"));
                ImGui::SetNextItemWidth(-1);
                const auto availability = Seis3D::Curvilinear::selectCudaDevice3D(inventory);
                if (ImGui::BeginCombo("##2Dbackend",
                                      selectedBackend ? "CUDA" : "CPU / OpenMP")) {
                    if (ImGui::Selectable("CPU / OpenMP", selectedBackend == 0))
                        selectedBackend = 0;
                    ImGui::BeginDisabled(!availability.available);
                    if (ImGui::Selectable("CUDA", selectedBackend == 1))
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
                int absorption=staged.absorbing?(staged.absorber==Absorber::CfsPml?2:1):0;
                ImGui::TextUnformatted(l("Absorbing boundary","吸收边界"));ImGui::SetNextItemWidth(-1);
                const char* choices[]{l("Off","关闭"),l("Sponge (legacy)","海绵（原有）"),"CFS-PML (ADE)"};
                const bool opened=ImGui::BeginCombo("##2Dabsorption",choices[absorption]);
                absorptionRect={ImGui::GetItemRectMin(),ImGui::GetItemRectMax()};
                if(opened){for(int option=0;option<3;++option){
                    if(ImGui::Selectable(choices[option],absorption==option)){staged.absorbing=option!=0;if(option)staged.absorber=option==2?Absorber::CfsPml:Absorber::Sponge;}
                    absorptionChoiceRects[option]={ImGui::GetItemRectMin(),ImGui::GetItemRectMax()};
                }ImGui::EndCombo();}
                ImGui::Checkbox(l("Extend absorbing strips outside model","吸收带向模型外扩展（推荐）"),&staged.externalAbsorption);
                externalAbsorptionRect={ImGui::GetItemRectMin(),ImGui::GetItemRectMax()};
                if(ImGui::IsItemHovered())ImGui::SetTooltip("%s",l("Adds left/right and bottom strips. Target model, terrain and acquisition coordinates stay unchanged. Costs additional RAM/VRAM; rebuild required.","左右、底部额外增加吸收网格；目标模型、地形及采集坐标不变。会增加内存/显存，重建后生效。"));
                const auto domain=Detail::domainLayout(staged);
                ImGui::TextWrapped("%s %d x %d | %s %d x %d",l("Target model","目标模型"),staged.nx,staged.nz,l("Actual computation","实际计算"),domain.nx,domain.nz);
                if(Detail::acquisitionInAbsorber(staged)){ImGui::PushStyleColor(ImGuiCol_Text,{.8f,.15f,.05f,1});ImGui::TextWrapped("%s",l("Source support / receivers enter the absorber. Enable external strips or adjust acquisition.","震源支撑或检波器进入吸收区：建议启用外扩或调整布设。"));ImGui::PopStyleColor();}
                integer("X sponge cells", "X 方向吸收格数", staged.spongeX);
                integer("Bottom sponge cells", "底部吸收格数", staged.spongeZ);
                ImGui::TextDisabled("%s: 1..%d", l("Input cells", "可输入格数"), maxSpongeCells);
                ImGui::TextWrapped("%s: X %.3f m / Z %.3f m (%s)",
                                   l("Effective sponge width", "实际吸收宽度"),
                                   std::clamp(staged.spongeX, 1, std::max(1, staged.nx / 3)) * staged.spacing,
                                   std::clamp(staged.spongeZ, 1, std::max(1, staged.nz / 3)) * staged.spacing,
                                   l("each axis <= nodes / 3; applied after rebuild", "每方向不超过节点数1/3，重建后生效"));
                if(staged.absorber==Absorber::Sponge)input("Sponge strength", "海绵强度", staged.spongeStrength, .1);
                else {input("Target R (1e-8..0.1)","PML目标R（1e-8..0.1）",staged.cpml.targetReflection,0,"%.3e");
                    input("Kappa max (1..20)","PML κ最大值（1..20）",staged.cpml.kappaMax,.5);
                    input("PML reference Hz (0=source)","PML参考频率Hz（0=随震源）",staged.cpml.frequency,1);
                    ImGui::TextWrapped("%s",l("Target R specifies the profile, not a measured reflection guarantee. Top remains traction-free; changes require rebuild.","目标R仅用于生成轮廓，不是实测反射率保证。顶面保持自由表面，修改后需重建。"));}

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
                runtimeOptionRects=SeisWave::Ui::runtimeDisplayOptions();
                ImGui::Checkbox(l("Actual grid [G]", "真实节点网格 [G]"), &grid);
                ImGui::TextDisabled("%s %.3g m",l("Grid line spacing (auto):","网格线间隔（自动）："),gridDisplayStride*simulation.parameters().spacing);
                if(ImGui::Checkbox(l("Absorber overlay [B]", "吸收区显示 [B]"), &sponge))fit=true;
                if(sponge){
                    ImGui::TextUnformatted(l("Boundary tint opacity", "边界着色不透明度"));ImGui::SetNextItemWidth(-1);
                    ImGui::SliderFloat("##elasticAbsorberOpacity",&absorberOpacity,0.f,.5f,"%.2f");
                }
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
            ImGui::Text(simulation.parameters().attenuation.enabled?"PPW %.2f | elastic-equivalent energy %.4g J/m":"PPW %.2f | Energy %.4g J/m", d.ppw, d.energy);
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
        syncSurfaceReceivers();
        const auto sessionActions = SeisWave::Ui::parameterSession2D(session);
        rebuildRect = sessionActions.rebuildRect;
        if (sessionActions.rebuild) apply();
        if (sessionActions.undo) {
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
        if (sessionActions.defaults) {
            staged = Parameters{};
            wavePalette=modelPalette=1;
            SeisWave::Ui::applyInitialModel2D(staged);
            staged.externalAbsorption=true;
            staged.autoSurfaceReceivers=true;syncSurfaceReceivers();
            if (rememberDraft)
                staged.attenuation.enabled = navigation.current == SeisWave::Ui::Module2D::Viscoelastic;
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
        ImGui::TextUnformatted(p.attenuation.enabled?"MacCormack DRP + RK4 / Float64 / P-SV + GSLS":"MacCormack DRP + RK4 / Float64 / P-SV");
        ImGui::SeparatorText(l("Effective time / geometry", "生效时间与几何"));
        ImGui::Text("dt = %.8f ms", simulation.dt() * 1000);
        ImGui::Text("%s %.5f ms", l("Record dt", "记录间隔"), simulation.recordDt() * 1000);
        ImGui::Text("f0 %.3f Hz | CFL %.3f", p.frequency, p.cfl);
        if(p.absorbing&&p.absorber==Absorber::CfsPml)ImGui::Text("CFS-PML | %.2f MiB | dt limit %.6f ms",d.cpmlMemoryMiB,d.cpmlDt*1000);
        if(p.attenuation.enabled){ImGui::Text("GSLS %d | fref %.3f Hz | band %.3f..%.3f Hz",p.attenuation.mechanisms,p.attenuation.referenceFrequency,p.attenuation.minFrequency,p.attenuation.maxFrequency);if(!p.layered)ImGui::Text("Qp %.3f | Qs %.3f",p.attenuation.qp,p.attenuation.qs);}
        ImGui::Text("%d x %d | h %.4f m", p.nx, p.nz, p.spacing);
        ImGui::Text("%s: %s",l("Field/model precision","波场／模型精度"),precisionName(p.precision));
        ImGui::Text("%s %d x %d",l("Computation (with absorber)","计算节点（含吸收带）"),g.nx-2*ghost,g.nz-2*ghost);
        ImGui::TextUnformatted(p.source.kind == SeisWave::SourceKind::IsotropicMoment
                                   ? l("Isotropic moment source", "各向同性爆炸矩源")
                                   : l("Directional body force", "定向体力源"));
        ImGui::Text("t = %.5f s | %d / %d", simulation.step() * simulation.dt(), simulation.step(), simulation.totalSteps());
        ImGui::Text("%s %.5f s", l("Effective stop", "实际停止时刻"), simulation.totalSteps() * simulation.dt());
        ImGui::Text("X / Z = %.2f x %.2f m", (p.nx - 1) * p.spacing, (p.nz - 1) * p.spacing);
        ImGui::Text("%s %.3f m", l("Flatten", "展平深度"), g.flattenDepth);
        const auto src = g.coordinates[g.modelIndex((p.sourceX < 0 ? p.nx / 2 : p.sourceX),p.sourceZ)];
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
            scalar("Precision (0=FP64,1=FP32)","精度（0=FP64，1=FP32）",static_cast<int>(p.precision),static_cast<int>(staged.precision));
            ROW("External absorber (0/1)","吸收带外扩（0/1）",externalAbsorption);
            ROW("X sponge cells", "X 吸收格数", spongeX); ROW("Z sponge cells", "底部吸收格数", spongeZ);
            ROW("Sponge strength", "吸收强度", spongeStrength);
            scalar("Boundary kind (0=sponge,1=PML)","边界类型（0=海绵，1=PML）",static_cast<int>(p.absorber),static_cast<int>(staged.absorber));
            if(p.absorber==Absorber::CfsPml||staged.absorber==Absorber::CfsPml){
                scalar("PML target R","PML目标R",p.cpml.targetReflection,staged.cpml.targetReflection);
                scalar("PML kappa max","PML κ最大值",p.cpml.kappaMax,staged.cpml.kappaMax);
                scalar("PML Hz (0=source)","PML频率（0=随源）",p.cpml.frequency,staged.cpml.frequency);
            }
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
                if(p.attenuation.enabled)ImGui::Text("Qp %.3f | Qs %.3f",v.qp,v.qs);
            }
        }
        ImGui::SeparatorText(l("Numerical diagnostics", "数值诊断"));
        ImGui::Text("PPW %.3f | J min %.6f", d.ppw, g.minJacobian);
        ImGui::Text("%s | RAM %.1f MiB | CUDA %.1f MiB",precisionName(p.precision),d.residentHostArrayBytes/(1024.*1024),d.residentDeviceBytes/(1024.*1024));
        ImGui::Text("dt wave %.6f ms", d.waveDt * 1000);
        ImGui::Text("dt damping %.6f ms", d.dampingDt * 1000);
        ImGui::Text("dt source %.6f ms", d.sourceDt * 1000);
        if(p.absorbing&&p.absorber==Absorber::CfsPml)ImGui::Text("CFS-PML | %.2f MiB | dt limit %.6f ms",d.cpmlMemoryMiB,d.cpmlDt*1000);
        if(p.attenuation.enabled){ImGui::Text("GSLS %d | state fields %d | dt relax %.6f ms",p.attenuation.mechanisms,d.stateFields,d.relaxationDt*1000);ImGui::TextWrapped(l("Maximum fitted Q error %.2f%%; energy is an elastic-equivalent diagnostic, not total viscoelastic energy.","Q 拟合最大相对误差 %.2f%%；能量为弹性等效诊断，不是黏弹性总能量。"),d.maxRelativeQError*100);}
        ImGui::Text("%s %d", l("CPU workers", "生效 CPU 线程"), simulation.backend().workerThreads);
        ImGui::Text("%s %d", l("CUDA device", "生效 CUDA 卡"), simulation.backend().cudaDevice);
        ImGui::Text("%.3f s | %.4f ms/step", d.totalStepMs / 1000, simulation.step() ? d.totalStepMs / simulation.step() : 0);
        ImGui::PopTextWrapPos();
        ImGui::End();
    }
    void wavefield() {
        if (!waveWindow)
            return;
        const auto waveTitle=label("Curved X-Z wavefield", "曲面 X–Z 波场");
        const auto waveFlags=workspace.prepare(waveTitle.c_str());
        if(workspace.focused==waveTitle){
            if(const auto* commands=ImGui::FindWindowByName("##2DCommands")){
                const auto* viewport=ImGui::GetMainViewport();
                const float top=std::max(viewport->WorkPos.y,commands->Pos.y+commands->Size.y)+3;
                ImGui::SetNextWindowPos({viewport->WorkPos.x+3,top});
                ImGui::SetNextWindowSize({viewport->WorkSize.x-6,viewport->WorkPos.y+viewport->WorkSize.y-top-3});
            }
        }
        if (!ImGui::Begin(waveTitle.c_str(), &waveWindow,waveFlags)) {
            ImGui::End();
            return;
        }
        workspace.tools(waveTitle.c_str());
        std::string frameError;
        if (!simulation.readFrame(component, frame, frameError, true)) {
            error = frameError;
            ImGui::TextWrapped("%s",frameError.empty()?l("Refreshing wavefield in background...","后台正在刷新波场…"):frameError.c_str());
            ImGui::End();
            return;
        }
        const auto &p = simulation.parameters();
        const auto &g = simulation.grid();
        const auto &material = modelComponent == 0 ? g.vp : modelComponent == 1 ? g.vs : g.density;
        double minMaterial=std::numeric_limits<double>::infinity(),maxMaterial=-minMaterial;
        for(int k=0;k<p.nz;++k){const auto row=material.begin()+g.modelIndex(0,k);const auto mm=std::minmax_element(row,row+p.nx);
            minMaterial=std::min(minMaterial,*mm.first);maxMaterial=std::max(maxMaterial,*mm.second);}
        const auto domain=Detail::domainLayout(p);
        const auto frameIndex=[&](int i,int k){return std::size_t(k)*domain.nx+(i+domain.offsetX);};
        const bool externalOverlay=p.absorbing&&p.externalAbsorption&&sponge;
        if(externalOverlay!=lastExternalOverlay){fit=true;lastExternalOverlay=externalOverlay;}
        if(p.absorbing&&p.externalAbsorption)ImGui::TextWrapped("%s",l("B: real absorber wavefield with translucent amber tint; target and absorber share the wave scale. Exports retain the target domain.","B：半透明浅橙色吸收区叠加真实波场，内外共用波场色标；导出仍为目标模型范围。"));
        double peak = 1e-30;
        for(int k=0;k<p.nz;++k)for(int i=0;i<p.nx;++i)
            peak=std::max(peak,std::abs(double(frame[frameIndex(i,k)])));
        const double range = peak / gain;
        displayedWaveRange=range;
        renderedAbsorberCells=0;
        std::vector<SeisWave::Ui::ScalarLegend> legends;
        char legendTitle[192];
        if (showWave) {
            const char* name=component==0?"Vz":component==1?"Vx":component==2?"|V|":"p";
            const char* unit=component==3?"Pa":"m/s";
            if(normalized)std::snprintf(legendTitle,sizeof(legendTitle),"%s | 1 = %.3g %s",name,range,unit);
            else std::snprintf(legendTitle,sizeof(legendTitle),"%s (%s)",name,unit);
            legends.push_back({legendTitle,component==2?0.:(normalized?-1.:-range),normalized?1.:range,
                [this](double t){return paletteColor(t,wavePalette);}});
        }
        if(showModel){
            std::snprintf(legendTitle,sizeof(legendTitle),"%s (%s)",modelComponent==0?"Vp":modelComponent==1?"Vs":"Density",modelComponent==2?"kg/m3":"m/s");
            legends.push_back({legendTitle,minMaterial,maxMaterial,[this](double t){return paletteColor(t,modelPalette);}});
        }
        const float legendHeight=SeisWave::Ui::scalarLegendHeight(static_cast<int>(legends.size()),ImGui::GetContentRegionAvail().x);
        if (ImGui::GetContentRegionAvail().x < 700)
            ImGui::Text("%d x %d | %.1f x %.1f m", p.nx, p.nz, (p.nx - 1) * p.spacing,
                        (p.nz - 1) * p.spacing);
        else
            ImGui::Text("X / Z: %.2f x %.2f m | %d x %d | f0 %.1f Hz", (p.nx - 1) * p.spacing,
                        (p.nz - 1) * p.spacing, p.nx, p.nz, p.frequency);
        const float plotHeight =
            std::max(80.f, ImGui::GetContentRegionAvail().y -
                               legendHeight -
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
            const double fitLeft=externalOverlay?-domain.widthX*p.spacing:0,
                         fitRight=externalOverlay?xmax+domain.widthX*p.spacing:xmax,
                         fitBottom=externalOverlay?zmax+domain.widthZ*p.spacing:zmax;
            double zmin = 0;
            for (int i = externalOverlay?-domain.offsetX:0; i < p.nx+(externalOverlay?domain.offsetX:0); ++i)
                zmin = std::min(zmin, g.coordinates[g.modelIndex(i,0)][1]);
            if (plotHeight < ImGui::GetFontSize() * 10) {
                ImPlot::SetupAxisTicks(ImAxis_X1, 0, xmax, 3);
                ImPlot::SetupAxisTicks(ImAxis_Y1, 0, zmax, 2);
                ImPlot::SetupAxisFormat(ImAxis_X1, "%.3g");
                ImPlot::SetupAxisFormat(ImAxis_Y1, "%.3g");
            }
            const double boundsX[]{fitLeft-.02*(fitRight-fitLeft),fitRight+.02*(fitRight-fitLeft)},
                boundsZ[]{zmin-.02*(fitBottom-zmin),fitBottom+.02*(fitBottom-zmin)};
            ImPlot::SetNextLineStyle({0, 0, 0, 0});
            ImPlot::PlotLine("##modelBounds", boundsX, boundsZ, 2);
            const auto limits = ImPlot::GetPlotLimits();
            domainVisible =
                limits.X.Min <= fitLeft && limits.X.Max >= fitRight && limits.Y.Min <= zmin && limits.Y.Max >= fitBottom;
            auto *draw = ImPlot::GetPlotDrawList();
            ImPlot::PushPlotClipRect();
            const auto toPixel = [&](int i, int k) {
                const auto x = g.coordinates[g.modelIndex(i,k)];
                return ImPlot::PlotToPixels(x[0], x[1]);
            };
            const auto plotSize = ImPlot::GetPlotSize();
            renderStrideX =
                std::max(1, static_cast<int>(std::ceil((p.nx - 1) / std::max(64.f, plotSize.x / 2))));
            renderStrideZ =
                std::max(1, static_cast<int>(std::ceil((p.nz - 1) / std::max(64.f, plotSize.y / 2))));
            const auto renderCell=[&](int i,int k,int i1,int k1,bool absorberCell){
                const ImVec2 points[4]{toPixel(i,k),toPixel(i1,k),toPixel(i1,k1),toPixel(i,k1)};
                ImU32 colors[4];
                const int xx[]{i,i1,i1,i},zz[]{k,k,k1,k1};
                for(int v=0;v<4;++v){
                    const auto q=g.modelIndex(xx[v],zz[v]);
                    const double model=maxMaterial>minMaterial?(material[q]-minMaterial)/(maxMaterial-minMaterial):.45;
                    auto base=showModel?ImGui::ColorConvertU32ToFloat4(paletteColor(model,modelPalette)):palette3D().background;
                    const double norm=std::clamp(frame[frameIndex(xx[v],zz[v])]/range,component==2?0.:-1.,1.);
                    if(showWave){
                        const auto wave=ImGui::ColorConvertU32ToFloat4(paletteColor(component==2?norm:.5+.5*norm,wavePalette));
                        const float alpha=showModel?static_cast<float>(std::min(1.,std::abs(norm)*4)):.95f;
                        base={base.x*(1-alpha)+wave.x*alpha,base.y*(1-alpha)+wave.y*alpha,base.z*(1-alpha)+wave.z*alpha,1};
                    }
                    if(absorberCell)base=SeisWave::Ui::tintAbsorber2D(base,absorberOpacity);
                    // Preserve the legacy internal-strip hue, with adjustable opacity.
                    if(sponge&&p.absorbing&&!p.externalAbsorption&&
                        (xx[v]<domain.widthX||xx[v]>=p.nx-domain.widthX||zz[v]>=p.nz-domain.widthZ)){
                        base.x=base.x*(1-absorberOpacity)+absorberOpacity;
                        base.y=base.y*(1-absorberOpacity)+.5f*absorberOpacity;
                    }
                    colors[v]=ImGui::ColorConvertFloat4ToU32(base);
                }
                quad(draw,points,colors);
                if(absorberCell){
                    ++renderedAbsorberCells;
                    if(grid)draw->AddPolyline(points,4,IM_COL32(177,106,26,90),ImDrawFlags_Closed,.5f);
                }
            };
            if(externalOverlay){
                const auto strip=[&](int x0,int x1,int z0,int z1){
                    for(int k=z0;k<z1;k+=renderStrideZ)for(int i=x0;i<x1;i+=renderStrideX)
                        renderCell(i,k,std::min(x1,i+renderStrideX),std::min(z1,k+renderStrideZ),true);
                };
                strip(-domain.offsetX,0,0,p.nz-1+domain.widthZ);
                strip(p.nx-1,p.nx-1+domain.offsetX,0,p.nz-1+domain.widthZ);
                strip(0,p.nx-1,p.nz-1,p.nz-1+domain.widthZ);
            }
            for(int k=0;k<p.nz-1;k+=renderStrideZ)for(int i=0;i<p.nx-1;i+=renderStrideX)
                renderCell(i,k,std::min(p.nx-1,i+renderStrideX),std::min(p.nz-1,k+renderStrideZ),false);
            const auto o=ImPlot::PlotToPixels(0,0),px=ImPlot::PlotToPixels(p.spacing,0),pz=ImPlot::PlotToPixels(0,p.spacing);
            gridDisplayStride=SeisWave::Ui::displayGridStride2D(p.nx,p.nz,p.spacing,std::min(std::abs(px.x-o.x),std::abs(pz.y-o.y)));
            if (grid) {
                for (int k = 0;; k = std::min(p.nz - 1, k + gridDisplayStride)) {
                    for (int i = 0; i < p.nx - 1; i += gridDisplayStride)
                        draw->AddLine(toPixel(i, k), toPixel(std::min(p.nx - 1, i + gridDisplayStride), k),
                                      color3D(palette3D().text, .25f));
                    if (k == p.nz - 1)
                        break;
                }
                for (int i = 0;; i = std::min(p.nx - 1, i + gridDisplayStride)) {
                    for (int k = 0; k < p.nz - 1; k += gridDisplayStride)
                        draw->AddLine(toPixel(i, k), toPixel(i, std::min(p.nz - 1, k + gridDisplayStride)),
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
        SeisWave::Ui::drawScalarLegendStrip("##2DFieldLegends",legends);
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
                    workspace.forgetFocus();
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

int validatePrecisionWorkflow2D(){
    std::string error;SeisWave::Ui::AppRuntime runtime;
    if(!runtime.open("2D precision workflow",true,error))throw std::runtime_error(error);
    glfwSetWindowSize(runtime.window(),2560,1494);setLanguage3D(1);applyTheme3D(11);SeisWave::Ui::setParameterDisplayMode(2,0);
    Workbench app;app.staged.nx=96;app.staged.nz=80;app.staged.sourceZ=16;app.staged.receiverCount=20;app.staged.relief=2;app.staged.duration=.08;app.staged.maxSteps=0;app.staged.absorber=Absorber::CfsPml;app.staged.attenuation.enabled=true;
    if(app.staged.precision!=Precision::Float64||!app.apply())throw std::runtime_error("Default precision changed / build failed: "+app.error);
    const auto doubleBytes=app.simulation.diagnostics().modelArrayBytes;
    app.category=2;++app.groupEpoch;app.groupsExpanded=true;
    const auto draw=[&]{runtime.begin();app.toolbar();app.controls();app.activeParameters();app.wavefield();app.records();runtime.present();};
    auto& io=ImGui::GetIO();io.AddFocusEvent(true);for(int i=0;i<5;++i)draw();
    const auto click=[&](ImRect rect){const auto at=rect.GetCenter();io.AddMousePosEvent(at.x,at.y);draw();draw();io.AddMouseButtonEvent(0,true);draw();io.AddMouseButtonEvent(0,false);draw();draw();};
    click(app.precisionRect);click(app.precisionChoiceRects[1]);
    if(app.staged.precision!=Precision::Float32||app.simulation.parameters().precision!=Precision::Float64||!app.dirty())throw std::runtime_error("Precision combo failed to stage FP32");
    if(!app.apply()||app.simulation.step()!=0||app.simulation.grid().vp.precision()!=Precision::Float32||app.simulation.diagnostics().modelArrayBytes*2!=doubleBytes||!app.simulation.advance(60,error))throw std::runtime_error("FP32 GUI rebuild/model storage/advance failed: "+app.error+error);
    const auto folder=SeisWave::Ui::outputRoot()/"validation/precision-2d/ui";std::filesystem::create_directories(folder);
    const auto file=folder/"fp32_model.vts";if(!writeModelVts(app.simulation,file,app.activeSettings,error))throw std::runtime_error(error);
    const auto path=file.u8string();std::snprintf(app.modelPath.data(),app.modelPath.size(),"%s",reinterpret_cast<const char*>(path.c_str()));
    app.staged.precision=Precision::Float64;if(!app.loadModel()||app.dirty()||app.staged.precision!=Precision::Float32||app.simulation.step()!=0)throw std::runtime_error("GUI model load lost selected precision: "+app.error);
    if(!app.simulation.advance(60,error))throw std::runtime_error(error);
    io.AddMousePosEvent(-100,-100);app.fit=true;for(int i=0;i<5;++i)draw();int w{},h{};glfwGetFramebufferSize(runtime.window(),&w,&h);
    if(!SeisWave::Ui::capture(folder/"fp32-parameters.ppm",w,h)||!app.domainVisible||glGetError()!=GL_NO_ERROR)throw std::runtime_error("FP32 UI capture/render failed");
    click(app.precisionRect);click(app.precisionChoiceRects[0]);
    if(!app.dirty()||!app.apply()||app.simulation.step()!=0||app.simulation.grid().vp.precision()!=Precision::Float64||app.simulation.diagnostics().modelArrayBytes!=doubleBytes)throw std::runtime_error("FP64 switch back did not rebuild / clear old run: "+app.error);
    std::cout<<"Precision UI PASS: FP64 default, real FP32/FP64 combo, staged/apply, model storage halves, GSLS/PML advance, VTS reload, switch back clears run\n";return 0;
}

int validateCfsPmlWorkflow2D(){
    std::string error;SeisWave::Ui::AppRuntime runtime;
    if(!runtime.open("2D CFS-PML workflow",true,error))throw std::runtime_error(error);
    glfwSetWindowSize(runtime.window(),2560,1494);setLanguage3D(1);applyTheme3D(11);SeisWave::Ui::setParameterDisplayMode(2,0);
    Workbench app;
    {
        const auto saved=app.staged;
        const bool clock=SeisWave::Ui::systemClockVisible(),fps=SeisWave::Ui::frameRateVisible();
        SeisWave::Ui::systemClockVisible()=SeisWave::Ui::frameRateVisible()=true;
        glfwSetWindowSize(runtime.window(),1600,1000);SeisWave::Ui::setInterfaceFontSize(22);
        if(!app.apply()||!app.simulation.advance(250,error))throw std::runtime_error(app.error+error);
        app.category=3;app.layout=true;
        const auto draw=[&]{runtime.begin();app.toolbar();app.controls();app.activeParameters();app.wavefield();app.records();runtime.present();};
        auto& io=ImGui::GetIO();io.AddFocusEvent(true);io.AddMousePosEvent(-100,-100);
        for(int i=0;i<5;++i)draw();
        const auto folder=SeisWave::Ui::outputRoot()/"screenshots/defaults-status";
        std::filesystem::create_directories(folder);
        if(app.staged.nx!=500||app.staged.nz!=200||app.staged.spacing!=2||app.staged.frequency!=30||app.staged.sourceZ!=0||app.staged.sourceX!=-1||app.wavePalette!=1||app.modelPalette!=1||app.gridDisplayStride!=1)throw std::runtime_error("Initial elastic layout / source / grid / palette mismatch");
        int w{},h{};glfwGetFramebufferSize(runtime.window(),&w,&h);
        if(!SeisWave::Ui::capture(folder/"elastic.ppm",w,h))throw std::runtime_error("Default elastic capture failed");
        const auto clickStatus=[&](int field){const auto rect=app.runtimeOptionRects[field];io.AddMousePosEvent((rect.x+rect.z)*.5f,(rect.y+rect.w)*.5f);draw();io.AddMouseButtonEvent(0,true);draw();io.AddMouseButtonEvent(0,false);draw();};
        const auto step=app.simulation.step();
        clickStatus(0);if(SeisWave::Ui::systemClockVisible()||!SeisWave::Ui::frameRateVisible())throw std::runtime_error("Clock display checkbox failed");
        clickStatus(1);if(SeisWave::Ui::systemClockVisible()||SeisWave::Ui::frameRateVisible()||SeisWave::Ui::runtimeStatusWidth()!=0)throw std::runtime_error("FPS display checkbox failed");
        const auto file=SeisWave::Ui::outputRoot()/"validation/defaults-status-preferences.cfg";
        SeisWave::Ui::saveAppearancePreferences(file);
        SeisWave::Ui::systemClockVisible()=SeisWave::Ui::frameRateVisible()=true;
        if(!SeisWave::Ui::loadAppearancePreferences(file)||SeisWave::Ui::systemClockVisible()||SeisWave::Ui::frameRateVisible())throw std::runtime_error("Runtime display preferences roundtrip failed");
        clickStatus(0);clickStatus(1);
        if(!SeisWave::Ui::systemClockVisible()||!SeisWave::Ui::frameRateVisible()||app.simulation.step()!=step)throw std::runtime_error("Runtime display toggle changed simulation / failed to restore");
        std::ofstream(SeisWave::Ui::outputRoot()/"validation/defaults-status-ui.txt")<<"PASS: elastic default 500x200 / 2 m / 30 Hz / centered row-0 source / Coolwarm model+wave; native 2 m grid overlay; actual clock/FPS checkboxes, zero reserved width when hidden, preference roundtrip, unchanged simulation step\n";
        SeisWave::Ui::systemClockVisible()=clock;SeisWave::Ui::frameRateVisible()=fps;
        app.staged=saved;app.category=2;app.layout=true;
        glfwSetWindowSize(runtime.window(),2560,1494);
    }
    app.staged.nx=96;app.staged.nz=80;app.staged.sourceZ=16;app.staged.receiverCount=20;app.staged.receiverTargetCount=20;app.staged.relief=2;app.staged.duration=.08;app.staged.maxSteps=0;
    if(!app.apply())throw std::runtime_error(app.error);
    app.category=2;++app.groupEpoch;app.groupsExpanded=true;
    const auto draw=[&]{runtime.begin();app.toolbar();app.controls();app.activeParameters();app.wavefield();app.records();runtime.present();};
    auto& io=ImGui::GetIO();io.AddFocusEvent(true);for(int i=0;i<5;++i)draw();
    const auto click=[&](ImRect rect){const auto at=rect.GetCenter();io.AddMousePosEvent(at.x,at.y);draw();draw();io.AddMouseButtonEvent(0,true);draw();io.AddMouseButtonEvent(0,false);draw();draw();};
    click(app.computeGroupRect);click(app.absorptionRect);click(app.absorptionChoiceRects[2]);
    if(app.staged.absorber!=Absorber::CfsPml||app.simulation.parameters().absorber!=Absorber::Sponge||!app.dirty()){
        int w{},h{};glfwGetFramebufferSize(runtime.window(),&w,&h);SeisWave::Ui::capture(SeisWave::Ui::outputRoot()/"validation/cfs-pml-2d/ui_failure.ppm",w,h);
        std::cout<<"PML rect "<<app.absorptionRect.Min.x<<","<<app.absorptionRect.Min.y<<" choice "<<app.absorptionChoiceRects[2].Min.x<<","<<app.absorptionChoiceRects[2].Min.y<<" stage "<<static_cast<int>(app.staged.absorber)<<" active "<<static_cast<int>(app.simulation.parameters().absorber)<<std::endl;
        throw std::runtime_error("PML dropdown failed to stage the actual selection");}

    if(!app.apply()||!app.simulation.advance(30,error)||app.simulation.diagnostics().cpmlScalars==0)throw std::runtime_error("PML GUI rebuild/advance failed: "+app.error+error);
    if(!app.simulation.parameters().externalAbsorption||app.simulation.grid().modelOffsetX!=16)throw std::runtime_error("New workbench did not enable external strips");
    click(app.externalAbsorptionRect);
    if(app.staged.externalAbsorption||!app.simulation.parameters().externalAbsorption||!app.dirty())throw std::runtime_error("External strip toggle staging failed");
    if(!app.apply()||app.simulation.grid().modelOffsetX!=0)throw std::runtime_error("Legacy internal strip rebuild failed");
    for(int i=0;i<3;++i)draw();click(app.externalAbsorptionRect);
    if(!app.apply()||!app.simulation.advance(30,error)||app.simulation.grid().modelOffsetX!=16)throw std::runtime_error("External strip rebuild failed");
    io.AddMousePosEvent(-100,-100);app.sponge=true;app.fit=true;for(int i=0;i<5;++i)draw();
    int ew{},eh{};glfwGetFramebufferSize(runtime.window(),&ew,&eh);
    if(!SeisWave::Ui::capture(SeisWave::Ui::outputRoot()/"validation/cfs-pml-2d/external-absorption.ppm",ew,eh)||!app.domainVisible)throw std::runtime_error("External absorber view did not retain complete model");
    app.sponge=false;app.fit=true;
    for(int i=0;i<5;++i)draw();
    const auto path=SeisWave::Ui::outputRoot()/"validation/cfs-pml-2d/parameters.ppm";
    int width{},height{};glfwGetFramebufferSize(runtime.window(),&width,&height);if(!SeisWave::Ui::capture(path,width,height)||glGetError()!=GL_NO_ERROR)throw std::runtime_error("PML UI capture failed");
    app.category=1;app.groupsExpanded=true;++app.groupEpoch;for(int i=0;i<3;++i)draw();
    app.staged.autoSurfaceReceivers=false;app.staged.receiverDepth=2;
    if(!app.apply())throw std::runtime_error(app.error);
    for(int i=0;i<3;++i)draw();
    const auto oldSurvey=app.simulation.parameters();click(app.surfaceReceiverRect);
    if(app.staged.receiverDepth!=0||app.simulation.parameters()!=oldSurvey||!app.dirty())
        throw std::runtime_error("Surface survey shortcut changed active data or missed surface row");
    if(!app.apply()||app.simulation.receivers().size()!=std::size_t(app.staged.receiverCount))throw std::runtime_error(app.error);
    for(const auto& r:app.simulation.receivers()){
        const auto q=app.simulation.grid().modelIndex(r.x,0);
        if(r.z!=0||r.position!=app.simulation.grid().coordinates[q])throw std::runtime_error("Elastic surface receiver did not follow terrain");
    }
    const auto requestedCount=app.staged.receiverTargetCount;
    const auto activeSurface=app.simulation.parameters();app.staged.nx+=40;
    for(int i=0;i<3;++i)draw();const auto aligned=app.staged;
    for(int i=0;i<5;++i)draw();
    if(app.staged!=aligned||app.staged.receiverTargetCount!=requestedCount||app.simulation.parameters()!=activeSurface)
        throw std::runtime_error("Elastic automatic line drifted or changed active data during resize");
    if(!app.apply()||!app.simulation.advance(30,error))throw std::runtime_error(app.error+error);
    for(const auto&r:app.simulation.receivers())if(r.z!=0||r.position!=app.simulation.grid().coordinates[app.simulation.grid().modelIndex(r.x,0)])
        throw std::runtime_error("Elastic automatic resized line missed terrain surface");
    if(!app.simulation.advance(100,error))throw std::runtime_error(error);
    glfwSetWindowSize(runtime.window(),1600,1000);app.layout=true;io.AddMousePosEvent(-100,-100);for(int i=0;i<4;++i)draw();
    int aw{},ah{};glfwGetFramebufferSize(runtime.window(),&aw,&ah);
    if(!SeisWave::Ui::capture(SeisWave::Ui::outputRoot()/"screenshots/auto-surface/elastic.ppm",aw,ah))throw std::runtime_error("Elastic auto capture failed");
    click(app.autoReceiverRect);if(app.staged.autoSurfaceReceivers)throw std::runtime_error("Elastic auto toggle did not disable");
    app.staged.receiverStart=3;app.staged.receiverCount=5;app.staged.receiverStride=4;app.staged.receiverDepth=3;
    if(!app.apply()||app.simulation.receivers().size()!=5||app.simulation.receivers()[0].z!=3)
        throw std::runtime_error("Elastic manual line was overwritten after disabling auto");
    std::filesystem::create_directories(SeisWave::Ui::outputRoot()/"validation/auto-surface-2026-10-31");
    std::ofstream(SeisWave::Ui::outputRoot()/"validation/auto-surface-2026-10-31/elastic-ui.txt")<<"PASS: actual surface preset / automatic resize / stable desired count / active data isolation / applied terrain coordinates / manual override\n";
    const auto captureFolder=SeisWave::Ui::outputRoot()/"screenshots/absorber-display";
    std::filesystem::create_directories(captureFolder);
    glfwSetWindowSize(runtime.window(),1600,1000);
    for(bool q:{false,true}){
        app.staged=Parameters{};app.staged.nx=161;app.staged.nz=121;app.staged.spacing=2;
        app.staged.vp=550;app.staged.vs=250;app.staged.density=1550;app.staged.relief=2;
        app.staged.wavelength=160;app.staged.flattenDepth=100;app.staged.frequency=20;
        app.staged.sourceX=12;app.staged.sourceZ=30;app.staged.receiverStart=10;
        app.staged.receiverCount=40;app.staged.receiverStride=3;app.staged.receiverDepth=2;
        app.staged.duration=.22;app.staged.maxSteps=0;app.staged.absorber=Absorber::CfsPml;
        app.staged.externalAbsorption=true;app.staged.precision=Precision::Float32;
        app.staged.attenuation.enabled=q;app.staged.attenuation.referenceFrequency=20;
        app.selectedBackend=0;app.settings.cpuThreads=1;
        if(!app.apply()||!app.simulation.advance(static_cast<int>(.18/app.simulation.dt()),error))throw std::runtime_error(app.error+error);
        app.category=3;app.component=0;app.showModel=true;app.showWave=true;
        app.sponge=false;app.fit=true;for(int i=0;i<4;++i)draw();
        const double targetRange=app.displayedWaveRange;
        const auto physicalFrame=app.frame;
        app.sponge=true;app.fit=true;for(int i=0;i<4;++i)draw();
        if(app.renderedAbsorberCells==0||!app.domainVisible||app.displayedWaveRange!=targetRange||app.frame!=physicalFrame)
            throw std::runtime_error("Elastic absorber wavefield or shared normalization failed");
        app.absorberOpacity=0;draw();
        if(app.frame!=physicalFrame||app.displayedWaveRange!=targetRange)throw std::runtime_error("Tint changed elastic field");
        app.absorberOpacity=SeisWave::Ui::defaultAbsorberOpacity2D;
        // Publish the existing maximized wavefield view, without dense grid lines
        // obscuring the small pulse. This does not change normal workspace defaults.
        app.grid=false;app.workspace.focused=label("Curved X-Z wavefield", "曲面 X–Z 波场");
        app.workspace.focusNew=true;app.fit=true;for(int i=0;i<3;++i)draw();
        const auto* waveWindow=ImGui::FindWindowByName(app.workspace.focused.c_str());
        const auto* commands=ImGui::FindWindowByName("##2DCommands");
        if(!waveWindow||!commands||waveWindow->Pos.y<commands->Pos.y+commands->Size.y)
            throw std::runtime_error("Maximized wavefield covered run commands");
        int w{},h{};glfwGetFramebufferSize(runtime.window(),&w,&h);
        if(w!=1600||h!=1000||!SeisWave::Ui::capture(captureFolder/(q?"viscoelastic.ppm":"elastic.ppm"),w,h)||glGetError()!=GL_NO_ERROR)
            throw std::runtime_error("Tinted elastic absorber screenshot failed");
    }
    const auto folder=SeisWave::Ui::outputRoot()/"validation/absorber-display-2026-10-31";
    std::filesystem::create_directories(folder);
    std::ofstream(folder/"elastic-ui.txt")<<"PASS: elastic/GSLS external wavefield rendered; full-domain fit; target-only scale unchanged by B; original amber hue at 14% opacity; tint does not modify physical data\n";
    std::cout<<"CFS-PML UI PASS: real dropdown, staged selection, rebuild, advance, memory and dt diagnostics; external/internal toggle; elastic/GSLS real absorber wavefield with translucent amber\n";return 0;
}

int validateAttenuationWorkflow2D() {
    std::string error;SeisWave::Ui::AppRuntime runtime;
    if(!runtime.open("2D Qp/Qs workflow",true,error))throw std::runtime_error(error);
    setLanguage3D(1);SeisWave::Ui::setParameterDisplayMode(2,0);
    Workbench app;app.staged.nx=128;app.staged.nz=96;app.staged.sourceZ=18;app.staged.receiverCount=20;app.staged.relief=3;app.staged.duration=.08;app.staged.maxSteps=40;
    if(!app.apply())throw std::runtime_error(app.error);
    const auto draw=[&]{runtime.begin();ImGui::SetNextWindowPos({20,30},ImGuiCond_Always);ImGui::SetNextWindowSize({590,700},ImGuiCond_Always);ImGui::Begin("Qp/Qs workflow controls");ImGui::SetNextItemOpen(true,ImGuiCond_Always);app.attenuationControls();ImGui::End();runtime.present();};
    auto& io=ImGui::GetIO();io.AddFocusEvent(true);draw();draw();
    const auto click=[&](ImRect rect){const auto at=rect.GetCenter();io.AddMousePosEvent(at.x,at.y);io.AddMouseButtonEvent(0,true);draw();io.AddMouseButtonEvent(0,false);draw();};
    click(app.attenuationToggleRect);
    if(!app.staged.attenuation.enabled||app.simulation.parameters().attenuation.enabled)throw std::runtime_error("Q checkbox did not stage a change");
    click(app.attenuationQpRect);io.AddKeyEvent(ImGuiMod_Ctrl,true);io.AddKeyEvent(ImGuiKey_A,true);draw();io.AddKeyEvent(ImGuiKey_A,false);io.AddKeyEvent(ImGuiMod_Ctrl,false);io.AddInputCharactersUTF8("80");draw();io.AddKeyEvent(ImGuiKey_Enter,true);draw();io.AddKeyEvent(ImGuiKey_Enter,false);draw();
    if(app.staged.attenuation.qp!=80||!app.apply()||!app.simulation.advance(30,error))throw std::runtime_error("Q input/apply failed: "+app.error+error);
    const auto snapshot=app.simulation.snapshot(true);if(snapshot.memory.size()!=12||snapshot.syy.empty())throw std::runtime_error("GUI did not run GSLS state");
    const auto folder=SeisWave::Ui::outputRoot()/"validation/viscoelastic/ui";std::filesystem::create_directories(folder);draw();int width=0,height=0;glfwGetFramebufferSize(runtime.window(),&width,&height);if(!SeisWave::Ui::capture(folder/"parameters.ppm",width,height))throw std::runtime_error("Q screenshot failed");
    app.staged.layered=true;app.openLayers=true;
    for(int frame=0;frame<4;++frame){runtime.begin();ImGui::Begin("Q layer workflow");app.modelDialogs();ImGui::End();runtime.present();}
    const auto* popup=ImGui::FindWindowByName(l("Layered material editor###2DLayerEditor","分层介质编辑###2DLayerEditor"));if(!popup||!popup->Active)throw std::runtime_error("Q layer table did not open");
    if(!SeisWave::Ui::capture(folder/"layers.ppm",width,height))throw std::runtime_error("Q layer screenshot failed");
    if(glGetError()!=GL_NO_ERROR)throw std::runtime_error("Q UI OpenGL error");
    std::cout<<"Q UI PASS: real enable checkbox, keyboard Qp edit, staged/apply, CUDA/CPU advance with Syy/memories and per-layer Q table\n";return 0;
}

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
    if(!app.simulation.advance(3,error))throw std::runtime_error(error);
    const double elapsedBeforeLayouts=app.simulation.diagnostics().totalStepMs;
    if(!(elapsedBeforeLayouts>0))throw std::runtime_error("2D elapsed timer did not advance");
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
        const auto* commands=ImGui::FindWindowByName("##2DCommands");
        if(!commands||!commands->InnerRect.Contains(app.elapsedRect)||app.elapsedRect.GetWidth()<=0||app.simulation.diagnostics().totalStepMs!=elapsedBeforeLayouts)
            throw std::runtime_error("2D timer clipped or counted paused presentation time");
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
    app.clear();
    if(app.simulation.diagnostics().totalStepMs!=0||!app.prepareRun(false)||!app.simulation.advance(1,error)||!(app.simulation.diagnostics().totalStepMs>0)||!app.apply()||app.simulation.diagnostics().totalStepMs!=0)
        throw std::runtime_error("2D elapsed timer clear / step / rebuild failed: "+app.error+error);
    std::cout<<"PASS 2D elapsed timer: visible at 960/1280/1600 px in both languages; paused time excluded; clear/step/rebuild\n";
    std::cout << "PASS 2D model UI: UTF-8 VTS restore, staged/active synchronization, recomputed dt, 0.5/1 ms records, "
                 "responsive summary in 6 bilingual/theme/size/font layouts; actual snapshot controls follow pressure / manual Vz+pressure export; legacy SEG-Y gain/sampling\n";
    return 0;
}
#include "ThemePanelValidation2D.inc"
int runWorkbench2D(bool smoke, bool homeTest, bool imagingRouteTest, bool viscoelastic) {
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
    Workbench app(viscoelastic,!smoke);
    if (smoke && !homeTest) {
        const auto savedDrafts = elasticDrafts2D;
        for (bool q : {false, true}) {
            Workbench isolated(q, true);
            isolated.staged.frequency = q ? 25 : 20;
        }
        for (bool q : {false, true}) {
            Workbench restored(q, true);
            if (restored.staged.frequency != (q ? 25 : 20) || restored.staged.attenuation.enabled != q)
                throw std::runtime_error("Elastic/viscoelastic drafts are not independent");
            restored.staged.attenuation.enabled = !q;
            if (restored.apply() || restored.simulation.initialized())
                throw std::runtime_error("Rebuild allocated a different physics module before routing");
            restored.staged.attenuation.enabled = q;
        }
        elasticDrafts2D = savedDrafts;
        std::cout << "2D mode isolation PASS: separate elastic/viscoelastic drafts; no foreign rebuild\n";
    }
    if (smoke) {
        // Keep the established homogeneous CFL/source-limit keyboard fixture;
        // interactive startup now uses a separate three-layer example.
        app.staged.layered=false;
        app.staged.vp=3000;app.staged.vs=1700;app.staged.density=2200;
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
        if(smoke){std::cerr<<app.error<<'\n';return 2;}
        const auto pending=app.staged;const auto settings=app.settings;const int backend=app.selectedBackend;const auto reason=app.error;
        app.staged=Parameters{};app.staged.externalAbsorption=true;app.staged.attenuation.enabled=viscoelastic;app.settings={};app.selectedBackend=0;
        if(!app.apply()){std::cerr<<app.error<<'\n';return 2;}
        app.staged=pending;app.settings=settings;app.selectedBackend=backend;
        app.error=std::string(l("Copied model could not be initialized; edit the pending inputs: ","复制模型无法初始化，请修改待应用参数："))+reason;
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
            const auto at = imagingRouteTest?app.imagingRect.GetCenter():app.homeRect.GetCenter();
            io.AddMousePosEvent(at.x, at.y);
            io.AddMouseButtonEvent(0, true);
        }
        if (homeTest && frame == 3)
            io.AddMouseButtonEvent(0, false);
        if(homeTest&&imagingRouteTest){
            if(frame==4||frame==6){const auto point=(frame==4?app.navigation.acousticRect:app.navigation.rtmRect).GetCenter();io.AddMousePosEvent(point.x,point.y);io.AddMouseButtonEvent(0,true);}
            if(frame==5||frame==7)io.AddMouseButtonEvent(0,false);
        }
        if(!app.simulation.poll(app.error))app.running=false;
        if(app.running) {
            app.simulation.queue(8,16);
            if(app.simulation.completed())app.running=false;
        }else app.simulation.pause();
        runtime.begin();
        app.toolbar();
        app.controls();
        app.modelDialogs();
        app.syncSurfaceReceivers();
        app.activeParameters();
        if (app.dirty())
            app.running = false;
        app.wavefield();
        app.records();
        app.floatingControls();
        if(app.dirty())app.running=false;
        app.shortcuts();
        runtime.present();
        if(homeTest&&imagingRouteTest&&(frame==3||frame==5)){
            int w,h;glfwGetFramebufferSize(runtime.window(),&w,&h);
            SeisWave::Ui::capture(SeisWave::Ui::outputRoot()/(frame==3?"screenshots/module-ui/elastic-menu.ppm":"screenshots/module-ui/acoustic-submenu.ppm"),w,h);
        }
        if (app.home) {
            std::cout << "2D Home route PASS\n";
            return SeisWave::Ui::returnHome;
        }
        if(app.navigation.requested)return app.navigation.requested;
        if(!smoke&&app.staged.attenuation.enabled!=(app.navigation.current==SeisWave::Ui::Module2D::Viscoelastic)){
            const auto target=app.staged.attenuation.enabled?SeisWave::Ui::Module2D::Viscoelastic:SeisWave::Ui::Module2D::Elastic;
            SeisWave::Ui::modelMailbox2D().post(target,{app.navigation.current,app.staged,app.settings,static_cast<BackendKind3D>(app.selectedBackend)});
            return SeisWave::Ui::moduleRoute(target);
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
        if (homeTest && frame > (imagingRouteTest?12:8))
            throw std::runtime_error("2D Home button did not route back");
    }
    return 0;
}
} // namespace Seis2D::Curvilinear::Ui
