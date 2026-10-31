#include "AcousticWorkbench2D.hpp"
#include "AbsorberDisplay2D.hpp"
#include "AppRuntime.hpp"
#include "CommandStrip2D.hpp"
#include "GridDisplay2D.hpp"
#include "InitialModel2D.hpp"
#include "MenuWidgets.hpp"
#include "ModelEditor2D.hpp"
#include "ModuleNavigation2D.hpp"
#include "Presentation3D.hpp"
#include "RecordPalettes3D.hpp"
#include "RecordView.hpp"
#include "ScalarLegendStrip.hpp"
#include "Seis2D/AcousticSolver2D.hpp"
#include "Seis2D/ModelIo2D.hpp"
#include "Seis2D/SurfaceSurvey2D.hpp"
#include "SeisCommon/BackgroundRunner.hpp"
#include "SimulationTiming.hpp"
#include "WorkspaceSidebar2D.hpp"
#include "imgui.h"
#include "imgui_internal.h"
#include "implot.h"
#include <GLFW/glfw3.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <optional>
#ifdef _WIN32
// clang-format off
#include <windows.h>
#include <commdlg.h>
// clang-format on
#endif
namespace Seis2D::Acoustic::Ui {
using namespace Seis3D::Curvilinear::Ui;
namespace Palettes = Seis3D::Curvilinear::RecordPalettes;
namespace {
std::optional<Options> acousticDraft;
const char *l(const char *a, const char *b) { return language3D() ? b : a; }
std::filesystem::path path(const char *text) {
    return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t *>(text)));
}
void browse(std::array<char, 2048> &field, bool surface) {
#ifdef _WIN32
    std::array<wchar_t, 32768> file{};
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.lpstrFile = file.data();
    dialog.nMaxFile = static_cast<DWORD>(file.size());
    dialog.lpstrFilter =
        surface ? L"Elevation CSV\0*.csv;*.txt\0All\0*.*\0" : L"Model VTS\0*.vts\0All\0*.*\0";
    dialog.Flags = OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR | OFN_PATHMUSTEXIST;
    if (GetOpenFileNameW(&dialog)) {
        auto p = std::filesystem::path(file.data()).u8string();
        if (p.size() < field.size())
            std::snprintf(field.data(), field.size(), "%s", reinterpret_cast<const char *>(p.c_str()));
    }
#else
    (void)field;
    (void)surface;
#endif
}
struct Publication {
    int step{}, samples{};
    std::size_t firstSample{};
    double recordDt{};
    Diagnostics diagnostics;
    std::vector<float> frame;
    std::vector<std::vector<double>> records;
    std::string error;
};
struct Workspace {
    Options options = acousticDraft ? *acousticDraft : defaultOptions();
    Simulation simulation;
    SeisWave::BackgroundRunner<Publication> worker;
    Publication shown;
    SeisWave::Ui::ModuleNavigation2D navigation{SeisWave::Ui::Module2D::AcousticForward};
    SeisWave::Ui::WorkspaceSidebar2D chrome;
    bool showSummary{true};
    bool running{}, home{}, layout{true}, fit{true}, showVs{};
    bool help{}, equalAspect{true}, gridLines{true}, absorberOverlay{}, showSource{true}, showReceivers{true},
        segy64{};
    bool domainVisible{};
    int markerReceivers{}, renderedAbsorberCells{};
    double displayedPressurePeak{};
    ImVec2 sourcePixel{}, lastPlotSize{};
    double aspectError{}, modelMinimum{}, modelMaximum{1};
    int modelPalette{1}, gridStride{1};
    double gridCellAspectError{};
    int groupEpoch{};
    bool groupsExpanded{true};
    std::unordered_map<ImGuiID, int> groupApplied;
    int backend{}, precision{}, palette{1}, fieldMode{2}, selectedReceiver{};
    SeisWave::Ui::RecordView recordView;
    std::vector<std::vector<double>> recordSamples;
    std::size_t publishedSamples{};
    std::uint64_t recordGeneration{1};
    float clip{.5f}, absorberOpacity{SeisWave::Ui::defaultAbsorberOpacity2D};
    std::string error, status;
    std::array<char, 2048> modelPath{}, surfacePath{};
    std::array<bool, 2> fitted{};
    ImRect runRect, waveRect, recordsRect, recordPlotRect, frequencyRect;
    SeisWave::Ui::ParameterActions2D sessionActions;
    ImGuiID parameterDockNode{}, recordDockNode{}, fieldsWindowId{};
    ImRect fieldsRect;
    int previousCategory{-1};
    SeisWave::Ui::ModelEditor2D modelEditor{options.model, surfacePath, error, status, true};
    ImRect layersRect, terrainDialogRect, surfaceReceiverRect, propagatorRect, autoReceiverRect;
    Capacity capacity;
    int activeTerrainPreset{-1};
    SurfaceSamples activeTerrainPresetSource;
    double nextCapacityRefresh{};
    std::optional<std::array<double, 14>> capacityKey;
    double manualMemoryMiB{4096};
    ImRect backendRect, precisionRect, autoBudgetRect, capacityNoticeRect;
    void refreshCapacity() {
        const auto &p = options.model;
        const std::array<double, 14> key{double(p.nx),
                                         double(p.nz),
                                         double(p.maxSteps),
                                         double(p.receiverCount),
                                         double(options.absorberCells),
                                         double(backend),
                                         double(precision),
                                         double(options.damping),
                                         double(static_cast<int>(options.absorber)),
                                         options.memoryMiB,
                                         p.duration,
                                         p.sampleIntervalMs,
                                         double(options.compute.cudaDevice),
                                         double(static_cast<int>(options.propagator))};
        if (!capacityKey || *capacityKey != key || ImGui::GetTime() >= nextCapacityRefresh) {
            auto request = options;
            request.backend = static_cast<BackendKind3D>(backend);
            request.model.precision = precision ? Precision::Float32 : Precision::Float64;
            capacity = estimateCapacity(request, Seis3D::Curvilinear::queryBackendMemory3D(
                                                     request.backend, request.compute.cudaDevice));
            capacityKey = key;
            nextCapacityRefresh = ImGui::GetTime() + .75;
        }
    }
    void capacityControls() {
        refreshCapacity();
        const bool reuse = simulation.initialized() && !dirty();
        const auto currentNodes = std::size_t(std::max(0, options.model.nx)) * std::max(0, options.model.nz);
        const bool showCurrent = reuse && currentNodes > capacity.aspectNodes;
        ImGui::TextWrapped("%s | %s | %s %.0f MiB", backend ? "CUDA" : "CPU", precision ? "FP32" : "FP64",
                           options.memoryMiB == 0 ? l("Auto RAM", "自动RAM") : l("Manual RAM", "手动RAM"),
                           capacity.ramBudgetMiB);
        ImGui::TextWrapped("%s %d x %d = %zu",
                           showCurrent ? l("Reusable active grid", "当前可复用网格")
                                       : l("New-build maximum at this aspect ratio", "当前比例新建上限"),
                           showCurrent ? options.model.nx : capacity.aspectNx,
                           showCurrent ? options.model.nz : capacity.aspectNz,
                           showCurrent ? currentNodes : capacity.aspectNodes);
        const bool details = ImGui::TreeNode(l("Memory / grid capacity details", "内存与网格容量详情"));
        if (details) {
            if (backend == 1)
                ImGui::TextWrapped("%s %.0f MiB | %s %.0f MiB", l("Free RAM", "可用内存"),
                                   capacity.freeRamMiB, l("Free VRAM", "可用显存"), capacity.freeDeviceMiB);
            else
                ImGui::TextWrapped("%s %.0f MiB | CPU", l("Free RAM", "可用内存"), capacity.freeRamMiB);
            ImGui::TextWrapped("%s %.1f MiB | GPU %.1f MiB", l("Requested estimate", "当前估算"),
                               capacity.estimatedRamMiB, capacity.estimatedDeviceMiB);
            ImGui::TextWrapped("%s %d | %s %d", l("Max Nx at this Nz", "当前Nz下最大Nx"),
                               capacity.maxNxGivenNz, l("Max Nz at this Nx", "当前Nx下最大Nz"),
                               capacity.maxNzGivenNx);
            ImGui::TextWrapped("%s %d x %d = %zu", l("At current aspect ratio", "按当前长宽比上限"),
                               capacity.aspectNx, capacity.aspectNz, capacity.aspectNodes);
            ImGui::TextWrapped(
                "%s",
                l("Conservative free-memory estimate incl. CPML, rebuild and raw records; not a physical "
                  "sampling limit. Rechecked on apply; axis cap 16384.",
                  "包含CPML、重建及完整记录的保守预算，非物理采样上限；应用时再次检查。每轴上限16384。"));
            ImGui::TreePop();
        }
        if (reuse && !capacity.withinBudget)
            ImGui::TextWrapped("%s", l("Active grid can be reused. The estimate above reserves the active "
                                       "session while building another model.",
                                       "当前生效网格可直接复用；以上新建估算保留了当前会话占用。"));
        if (!capacity.withinBudget && !reuse) {
            const char *reason = l("Invalid dimensions or memory settings.", "网格尺寸或内存设置无效。");
            switch (capacity.limit) {
            case Capacity::Limit::Axis:
                reason = l("Grid axes must be 16..16384.", "网格每轴必须为16..16384。");
                break;
            case Capacity::Limit::ManualRam:
                reason = l("Manual RAM cap is too small; enable automatic budget or increase it.",
                           "手动RAM上限过低，请启用自动预算或提高上限。");
                break;
            case Capacity::Limit::SystemRam:
                reason = l("Insufficient free RAM for rebuild, including assembly and records.",
                           "可用RAM不足以重建，估算包含组装临时数组和记录。");
                break;
            case Capacity::Limit::DeviceMemory:
                reason =
                    l("Insufficient free VRAM on the selected CUDA device.", "所选CUDA设备空闲显存不足。");
                break;
            case Capacity::Limit::DeviceUnavailable:
                reason = l("CUDA device is unavailable or free VRAM could not be queried.",
                           "CUDA设备不可用，或无法查询空闲显存。");
                break;
            default:
                break;
            }
            ImGui::TextColored({.8f, .2f, .05f, 1}, "%s", reason);
            capacityNoticeRect = {ImGui::GetItemRectMin(), ImGui::GetItemRectMax()};
            ImGui::BeginDisabled(capacity.aspectNx < 16 || capacity.aspectNz < 16);
            if (ImGui::Button(l("Reduce to budget at same aspect ratio", "按当前比例缩减至预算"))) {
                options.model.nx = capacity.aspectNx;
                options.model.nz = capacity.aspectNz;
                modelEditor.fitAcquisition(options.model);
                modelEditor.syncTerrainPreset();
                nextCapacityRefresh = 0;
            }
            ImGui::EndDisabled();
        }
    }
    Workspace() {
        if (!acousticDraft) {
            SeisWave::Ui::applyInitialModel2D(options.model, true);
            options.model.autoSurfaceReceivers = true;
        }
        modelEditor.browseTerrain = [this] { browse(surfacePath, true); };
        modelEditor.fitAcquisition = [](Parameters &p) {
            p.sourceX = p.sourceX < 0 ? -1 : std::clamp(p.sourceX, 1, std::max(1, p.nx - 2));
            p.sourceZ = std::clamp(p.sourceZ, 1, std::max(1, p.nz - 2));
            p.receiverStart = std::clamp(p.receiverStart, 0, std::max(0, p.nx - 1));
            p.receiverDepth = std::clamp(p.receiverDepth, 1, std::max(1, p.nz - 2));
            p.receiverStride = std::max(1, p.receiverStride);
            p.receiverCount =
                std::clamp(p.receiverCount, 1,
                           std::max(1, std::min(4096, 1 + (p.nx - 1 - p.receiverStart) / p.receiverStride)));
        };
        if (auto input = SeisWave::Ui::modelMailbox2D().take(SeisWave::Ui::Module2D::AcousticForward)) {
            options.model = std::move(input->model);
            options.model.attenuation.enabled = false;
            options.compute = input->compute;
            options.backend = input->backend;
        }
        backend = static_cast<int>(options.backend);
        precision = options.model.precision == Precision::Float32 ? 1 : 0;
        syncSurfaceReceivers();
    }
    void syncSurfaceReceivers() { synchronizeSurfaceReceivers(options.model, true, 4096); }
    ~Workspace() {
        try {
            worker.cancel();
            worker.wait();
        } catch (...) {
        }
    }
    Publication publish() {
        Publication p;
        p.step = simulation.step();
        p.diagnostics = simulation.diagnostics();
        simulation.readPressure(p.frame, p.error, true);
        const auto &receivers = simulation.receivers();
        p.samples = static_cast<int>(receivers.front().pressure.size());
        p.firstSample = publishedSamples <= static_cast<std::size_t>(p.samples) ? publishedSamples : 0;
        p.recordDt = simulation.recordDt();
        p.records.resize(receivers.size());
        for (std::size_t r = 0; r < receivers.size(); ++r)
            p.records[r].assign(receivers[r].pressure.begin() + p.firstSample, receivers[r].pressure.end());
        publishedSamples = p.samples;
        return p;
    }
    void accept(Publication p) {
        if (!p.error.empty()) {
            error = p.error;
            running = false;
        }
        if (p.firstSample == 0) {
            recordSamples = std::move(p.records);
            ++recordGeneration;
            selectedReceiver =
                std::clamp(selectedReceiver, 0, std::max(0, static_cast<int>(recordSamples.size()) - 1));
            recordView.invalidate();
        } else {
            if (recordSamples.size() != p.records.size())
                throw std::runtime_error("Acoustic record publication shape mismatch.");
            for (std::size_t r = 0; r < recordSamples.size(); ++r) {
                if (recordSamples[r].size() != p.firstSample)
                    throw std::runtime_error("Acoustic record publication gap.");
                recordSamples[r].insert(recordSamples[r].end(), p.records[r].begin(), p.records[r].end());
            }
        }
        shown = std::move(p);
    }
    void poll() {
        try {
            if (auto result = worker.poll())
                accept(std::move(*result));
        } catch (const std::exception &e) {
            error = e.what();
            running = false;
        }
    }
    void wait() {
        try {
            if (auto result = worker.wait())
                accept(std::move(*result));
        } catch (const std::exception &e) {
            error = e.what();
            running = false;
        }
    }
    bool apply() {
        running = false;
        wait();
        options.backend = static_cast<BackendKind3D>(backend);
        options.model.precision = precision ? Precision::Float32 : Precision::Float64;
        if (!modelEditor.syncTerrainPreset())
            return false;
        syncSurfaceReceivers();
        if (!simulation.initialize(options, error))
            return false;
        options = simulation.options();
        activeTerrainPreset = modelEditor.terrainPreset;
        activeTerrainPresetSource = modelEditor.terrainPresetSource;
        nextCapacityRefresh = 0;
        const auto range = std::minmax_element(simulation.grid().vp.begin(), simulation.grid().vp.end());
        modelMinimum = *range.first;
        modelMaximum = std::max(*range.second, modelMinimum + 1);
        publishedSamples = 0;
        accept(publish());
        fit = true;
        status = l("Model rebuilt; parameters applied.", "模型已重建，参数已生效。");
        return true;
    }
    bool prepare() {
        syncSurfaceReceivers();
        options.backend = static_cast<BackendKind3D>(backend);
        options.model.precision = precision ? Precision::Float32 : Precision::Float64;
        return simulation.initialized() && simulation.options() == options ? true : apply();
    }
    void toggleRun() {
        if (running) {
            running = false;
            worker.cancel();
            return;
        }
        wait();
        if (!prepare())
            return;
        if (simulation.completed()) {
            if (!simulation.reset(error))
                return;
            publishedSamples = 0;
            accept(publish());
        }
        running = true;
    }
    void singleStep() {
        running = false;
        wait();
        if (!prepare())
            return;
        if (simulation.completed()) {
            if (!simulation.reset(error))
                return;
            publishedSamples = 0;
        }
        if (simulation.advance(1, error))
            accept(publish());
    }
    void clear() {
        running = false;
        wait();
        if (simulation.initialized() && simulation.reset(error)) {
            publishedSamples = 0;
            accept(publish());
        }
    }
    void restoreLayout() {
        chrome.floating = false;
        chrome.center = true;
        chrome.views.forgetFocus();
        showSummary = layout = fit = true;
    }
    void shortcuts() {
        const auto &io = ImGui::GetIO();
        const bool available =
            !io.WantTextInput && !ImGui::IsAnyItemActive() &&
            !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel) &&
            !io.KeyCtrl && !io.KeyAlt && !io.KeySuper;
        if (available) {
            if (ImGui::IsKeyPressed(ImGuiKey_F1, false))
                help = !help;
            if (help) {
                if (ImGui::IsKeyPressed(ImGuiKey_Escape, false))
                    help = false;
            } else {
                if (ImGui::IsKeyPressed(ImGuiKey_Space, false))
                    toggleRun();
                if (ImGui::IsKeyPressed(ImGuiKey_N, false))
                    singleStep();
                if (ImGui::IsKeyPressed(ImGuiKey_C, false))
                    clear();
                if (ImGui::IsKeyPressed(ImGuiKey_R, false))
                    fit = true;
                if (ImGui::IsKeyPressed(ImGuiKey_F, false))
                    restoreLayout();
                if (ImGui::IsKeyPressed(ImGuiKey_G, false))
                    gridLines = !gridLines;
                if (ImGui::IsKeyPressed(ImGuiKey_B, false)) {
                    absorberOverlay = !absorberOverlay;
                    fit = true;
                }
                const int direction = io.KeyShift ? -1 : 1;
                if (ImGui::IsKeyPressed(ImGuiKey_Q, false))
                    palette = (palette + direction + 14) % 14;
                if (ImGui::IsKeyPressed(ImGuiKey_W, false))
                    fieldMode = (fieldMode + 1) % 3;
                if (ImGui::IsKeyPressed(ImGuiKey_E, false))
                    modelPalette = (modelPalette + direction + 14) % 14;
            }
        }
        if (!help)
            return;
        const auto *vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Appearing, {.5f, .5f});
        ImGui::SetNextWindowSize(
            {std::min(1100.f, vp->WorkSize.x * .94f), std::min(350.f, vp->WorkSize.y * .72f)},
            ImGuiCond_Appearing);
        if (ImGui::Begin(l("Keyboard shortcuts###AcousticHelp", "声学快捷键说明###AcousticHelp"), &help,
                         ImGuiWindowFlags_NoDocking)) {
            if (ImGui::BeginTable("##AcousticHelp", 3,
                                  ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_BordersInnerV)) {
                ImGui::TableNextColumn();
                ImGui::SeparatorText(l("Run", "计算"));
                ImGui::TextWrapped("%s",
                                   l("Space: run / pause\nN: one step\nC: clear active fields\nRun / Step "
                                     "applies edited inputs. Completed simulations restart from t=0.",
                                     "Space：运行/暂停\nN：单步\nC：清空生效波场\n运行/"
                                     "单步先应用修改；已完成模拟再次运行从 t=0 开始。"));
                ImGui::TableNextColumn();
                ImGui::SeparatorText(l("Display", "显示"));
                ImGui::TextWrapped("%s",
                                   l("Q: pressure palette\nW: pressure / model / overlay\nE: Vp "
                                     "palette\nShift: reverse palettes\nG: grid / B: external "
                                     "wavefield\nAcoustics has one pressure field, not Vx/Vz components.",
                                     "Q：压力色带\nW：压力/模型/叠加\nE：Vp配色\nShift：色带反向\nG：网格 "
                                     "/ B：吸收区波场\n声学只有压力标量，不切换 Vx/Vz。"));
                ImGui::TableNextColumn();
                ImGui::SeparatorText(l("Workspace", "工作区"));
                ImGui::TextWrapped(
                    "%s", l("R: fit plots\nF: restore docked layout\nF1 / Esc: close help\nMaximize view / "
                            "Esc: restore\nText entry and open menus suppress global shortcuts.",
                            "R：适配图件\nF：恢复停靠布局\nF1 / Esc：关闭帮助\n放大视图 / "
                            "Esc：还原\n输入文字、打开菜单时不触发全局快捷键。"));
                ImGui::EndTable();
            }
        }
        ImGui::End();
    }
    void queue() {
        if (!running || worker.busy() || !simulation.initialized() || shown.step >= simulation.totalSteps())
            return;
        worker.submit([this](const std::atomic_bool &cancel) {
            std::string failure;
            auto begin = std::chrono::steady_clock::now();
            for (int n = 0; n < 8 && !cancel && !simulation.completed(); ++n) {
                if (!simulation.advance(1, failure))
                    break;
                if (std::chrono::steady_clock::now() - begin > std::chrono::milliseconds(16))
                    break;
            }
            auto out = publish();
            if (!failure.empty())
                out.error = failure;
            return out;
        });
    }
    void integer(const char *label, int *value) {
        if (label[0] == '#') {
            ImGui::InputInt(label, value, 0, 0);
            return;
        }
        ImGui::TextUnformatted(label);
        ImGui::SetNextItemWidth(-1);
        auto id = std::string("##") + label;
        ImGui::InputInt(id.c_str(), value, 0, 0);
    }
    void number(const char *label, double *value) {
        if (label[0] == '#') {
            ImGui::InputDouble(label, value, 0, 0, "%.5g");
            return;
        }
        ImGui::TextUnformatted(label);
        ImGui::SetNextItemWidth(-1);
        auto id = std::string("##") + label;
        ImGui::InputDouble(id.c_str(), value, 0, 0, "%.5g");
    }
    bool dirty() const {
        return !simulation.initialized() || options != simulation.options() ||
               backend != static_cast<int>(simulation.options().backend) ||
               precision != (simulation.options().model.precision == Precision::Float32 ? 1 : 0);
    }
    void undoEdits() {
        if (!simulation.initialized())
            return;
        options = simulation.options();
        backend = static_cast<int>(options.backend);
        precision = options.model.precision == Precision::Float32 ? 1 : 0;
        modelEditor.terrainPreset = activeTerrainPreset;
        modelEditor.terrainPresetSource = activeTerrainPresetSource;
        modelEditor.terrainResampledNx = options.model.nx;
        modelEditor.terrainResampledSpacing = options.model.spacing;
        modelEditor.updateTerrainPreview();
        nextCapacityRefresh = 0;
        error.clear();
        status.clear();
    }
    void defaults() {
        options = defaultOptions();
        palette = modelPalette = 1;
        SeisWave::Ui::applyInitialModel2D(options.model, true);
        options.model.autoSurfaceReceivers = true;
        syncSurfaceReceivers();
        modelEditor.terrainPreset = -1;
        modelEditor.terrainPresetSource.clear();
        nextCapacityRefresh = 0;
        backend = static_cast<int>(options.backend);
        precision = options.model.precision == Precision::Float32 ? 1 : 0;
        error.clear();
        status.clear();
    }
    bool parameterGroup(const char *title, ImGuiTreeNodeFlags flags = 0) {
        return SeisWave::Ui::parameterGroup2D(title, flags, groupEpoch, groupsExpanded, groupApplied);
    }
    void controls() {
        const char *title = l("Parameters and run###AcousticControls", "参数与运行###AcousticControls");
        chrome.prepare(title);
        const bool visible = ImGui::Begin(title, nullptr, chrome.floating ? ImGuiWindowFlags_NoDocking : 0);
        if (!visible) {
            ImGui::End();
            return;
        }
        chrome.navigation();
        SeisWave::Ui::parameterGroupButtons2D(groupEpoch, groupsExpanded);
        const SeisWave::Ui::ParameterSession2D session{
            simulation.initialized(), dirty(),
            simulation.initialized() ? simulation.options().model.frequency : 0,
            simulation.initialized() ? simulation.dt() : 0, error};
        ImGui::BeginChild("##AcousticParameterFields", {0, -session.height()}, ImGuiChildFlags_None,
                          ImGuiWindowFlags_AlwaysVerticalScrollbar);
        fieldsWindowId = ImGui::GetCurrentWindow()->ID;
        fieldsRect = {ImGui::GetWindowPos(),
                      {ImGui::GetWindowPos().x + ImGui::GetWindowSize().x,
                       ImGui::GetWindowPos().y + ImGui::GetWindowSize().y}};
        if (previousCategory != chrome.category) {
            ImGui::SetScrollY(0);
            previousCategory = chrome.category;
        }
        ImGui::PushItemWidth(-ImGui::GetFontSize() * 7);
        ImGui::TextDisabled("%s", l("Acoustic / P | pressure in Pa", "声学 / P · 声学压力 p（Pa）"));
        if (!error.empty())
            ImGui::TextWrapped("%s", error.c_str());
        if (!status.empty())
            ImGui::TextWrapped("%s", status.c_str());
        if (chrome.category == 0 &&
            parameterGroup(l("Model and terrain", "模型与地形"), ImGuiTreeNodeFlags_DefaultOpen)) {
            capacityControls();
            integer("Nx", &options.model.nx);
            integer("Nz", &options.model.nz);
            number(l("Spacing (m)", "间距 (m)"), &options.model.spacing);
            double width = (options.model.nx - 1) * options.model.spacing,
                   depth = (options.model.nz - 1) * options.model.spacing;
            ImGui::TextUnformatted(l("Model width / depth (m)", "模型宽度 / 深度（m）"));
            ImGui::SetNextItemWidth(-1);
            if (ImGui::InputDouble("##acousticWidth", &width, 0, 0, "%.2f") && std::isfinite(width) &&
                width > 0 && options.model.spacing > 0 && width / options.model.spacing <= 16383)
                options.model.nx = std::max(16, int(std::llround(width / options.model.spacing)) + 1);
            ImGui::SetNextItemWidth(-1);
            if (ImGui::InputDouble("##acousticDepth", &depth, 0, 0, "%.2f") && std::isfinite(depth) &&
                depth > 0 && options.model.spacing > 0 && depth / options.model.spacing <= 16383)
                options.model.nz = std::max(16, int(std::llround(depth / options.model.spacing)) + 1);
            ImGui::Checkbox(l("Layered", "分层模型"), &options.model.layered);
            if (options.model.layered) {
                ImGui::Text("%zu %s", options.model.layers.size(),
                            l("layers; final layer fills remaining depth", "层；末层自动填充剩余深度"));
                if (functionButton3D(l("Edit layers / presets", "分层设计 / 介质预设"), FunctionRole3D::Model,
                                     {-1, 0}))
                    modelEditor.openLayers = true;
                layersRect = {ImGui::GetItemRectMin(), ImGui::GetItemRectMax()};
            } else {
                number("Vp (m/s)", &options.model.vp);
                number("ρ (kg/m³)", &options.model.density);
            }
            if (functionButton3D(l("Terrain / elevation editor", "地形 / 高程设置"), FunctionRole3D::Model,
                                 {-1, 0}))
                modelEditor.openTerrain = true;
            terrainDialogRect = {ImGui::GetItemRectMin(), ImGui::GetItemRectMax()};
            modelEditor.syncTerrainPreset();
            ImGui::Checkbox(l("Imported elevation", "使用外部高程"), &options.model.importedSurface);
            number(l("Relief (m)", "起伏 (m)"), &options.model.relief);
            number(l("Terrain wavelength (m)", "地形波长 (m)"), &options.model.wavelength);
            number(l("Flatten depth; 0 auto", "展平深度；0 自动"), &options.model.flattenDepth);
            number(l("Datum (m)", "高程基准 (m)"), &options.model.surfaceDatum);
        }
        if (chrome.category == 1 &&
            parameterGroup(l("Source and receivers", "震源与接收"), ImGuiTreeNodeFlags_DefaultOpen)) {
            if (functionButton3D(l("Uniform surface receivers", "地表均匀布设"), FunctionRole3D::Model,
                                 {-1, 0})) {
                if (!options.model.autoSurfaceReceivers)
                    options.model.receiverTargetCount = options.model.receiverCount;
                options.model.autoSurfaceReceivers = true;
                if (synchronizeSurfaceReceivers(options.model, true, 4096)) {
                    nextCapacityRefresh = 0;
                    status = l("Uniform near-surface pressure receivers staged at row 1; reset / apply.",
                               "近地表检波器已沿X均匀设置在首层节点；重置并应用后生效。");
                } else
                    error = l("Set valid model dimensions first.", "请先设置有效的模型尺寸。");
            }
            surfaceReceiverRect = {ImGui::GetItemRectMin(), ImGui::GetItemRectMax()};
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip(
                    "%s",
                    l("Uses current count as a target; integer node spacing may adjust the count. Pressure "
                      "receivers use the first subsurface row because surface pressure is zero.",
                      "按当前道数推算整数节点间距，实际道数就近调整。声学自由面压力为零，因此布设在地表下首层"
                      "节点。"));
            if (ImGui::Checkbox(l("Auto surface receivers", "自动沿地表均匀布设"),
                                &options.model.autoSurfaceReceivers) &&
                options.model.autoSurfaceReceivers)
                options.model.receiverTargetCount = options.model.receiverCount;
            autoReceiverRect = {ImGui::GetItemRectMin(), ImGui::GetItemRectMax()};
            syncSurfaceReceivers();
            if (options.model.autoSurfaceReceivers) {
                integer(l("Requested receiver count", "期望接收点数"), &options.model.receiverTargetCount);
                ImGui::Text("%s %d", l("Actual receivers", "实际道数"), options.model.receiverCount);
            }
            integer(l("Source X index", "震源 X 索引"), &options.model.sourceX);
            integer(l("Source Z index", "震源 Z 索引"), &options.model.sourceZ);
            number(l("Frequency (Hz)", "主频 (Hz)"), &options.model.frequency);
            frequencyRect = {ImGui::GetItemRectMin(), ImGui::GetItemRectMax()};
            number(l("Duration (s)", "记录时长 (s)"), &options.model.duration);
            integer(l("Max steps; 0 full", "步数上限；0 完整"), &options.model.maxSteps);
            number(l("Strength Pa m2/s2", "源强 Pa m²/s²"), &options.sourceStrength);
            ImGui::TextWrapped("%s",
                               l("Area-normalized nodal pressure acceleration Ricker source, not an elastic "
                                 "force/moment source.",
                                 "节点面积归一化的压力加速度 Ricker 源，与弹性体力 / 矩源的单位不同。"));
            ImGui::BeginDisabled(options.model.autoSurfaceReceivers);
            integer(l("Receiver start X", "接收起点 X"), &options.model.receiverStart);
            integer(l("Receiver count", "接收点数"), &options.model.receiverCount);
            integer(l("Receiver stride", "接收间隔索引"), &options.model.receiverStride);
            integer(l("Receiver Z index", "接收 Z 索引"), &options.model.receiverDepth);
            ImGui::EndDisabled();
            number(l("Sampling (ms); 0 each step", "采样 ms；0 每步"), &options.model.sampleIntervalMs);
        }
        if (chrome.category == 2 &&
            parameterGroup(l("Compute and absorber", "计算与吸收边界"), ImGuiTreeNodeFlags_DefaultOpen)) {
            int method = static_cast<int>(options.propagator);
            if (SeisWave::Ui::choiceField2D(l("Acoustic propagator", "声学算法"), &method,
                                            l("P1 FEM (baseline)\0Curvilinear DRP7-RK4 (experimental)\0",
                                              "P1 有限元（基准）\0曲网格 DRP7-RK4（试验）\0")))
                options.propagator = static_cast<Propagator>(method);
            propagatorRect = {ImGui::GetItemRectMin(), ImGui::GetItemRectMax()};
            if (options.propagator == Propagator::CurvilinearDrp)
                ImGui::TextWrapped("%s", l("Independent p/vx/vz forward solver; RTM remains P1.",
                                           "独立 p/vx/vz 正演核；RTM仍使用P1。"));
            SeisWave::Ui::choiceField2D(l("Backend", "计算核心"), &backend, "CPU / OpenMP\0CUDA\0");
            backendRect = {ImGui::GetItemRectMin(), ImGui::GetItemRectMax()};
            SeisWave::Ui::choiceField2D(l("Precision", "计算精度"), &precision, "FP64\0FP32\0");
            precisionRect = {ImGui::GetItemRectMin(), ImGui::GetItemRectMax()};
            integer(l("CPU workers; 0 auto", "CPU 线程；0 自动"), &options.compute.cpuThreads);
            integer(l("GPU ordinal; -1 auto", "GPU 编号；-1 自动"), &options.compute.cudaDevice);
            bool automatic = options.memoryMiB == 0;
            if (ImGui::Checkbox(l("Automatic RAM budget (recommended)", "自动RAM预算（推荐）"), &automatic)) {
                if (automatic) {
                    if (options.memoryMiB >= 16)
                        manualMemoryMiB = options.memoryMiB;
                    options.memoryMiB = 0;
                } else
                    options.memoryMiB = manualMemoryMiB;
            }
            autoBudgetRect = {ImGui::GetItemRectMin(), ImGui::GetItemRectMax()};
            if (!automatic)
                number(l("Manual RAM cap (MiB)", "手动RAM上限 (MiB)"), &options.memoryMiB);
            capacityControls();
            integer(l("External strip cells", "外扩条带格数"), &options.absorberCells);
            ImGui::Checkbox(l("Absorption enabled", "启用边界吸收"), &options.damping);
            int boundary = static_cast<int>(options.absorber);
            if (SeisWave::Ui::choiceField2D(l("Absorbing boundary", "吸收边界"), &boundary,
                                            l("Legacy sponge\0CFS-CPML\0", "原阻尼层（对照）\0CFS-CPML\0")))
                options.absorber = static_cast<AbsorbingBoundary>(boundary);
            if (options.absorber == AbsorbingBoundary::Cpml) {
                number(l("CPML target reflection", "CPML 目标反射率"), &options.cpml.targetReflection);
                number(l("CPML kappa maximum", "CPML κ 上限"), &options.cpml.kappaMax);
                number(l("CPML frequency; 0 source", "CPML 参考频率；0 随源"), &options.cpml.frequency);
            }
            ImGui::TextWrapped(
                "%s",
                options.propagator == Propagator::P1
                    ? l("P1 FEM acoustic operator. Independent weak-form CFS-CPML outside the target; "
                        "legacy sponge remains available for comparison. No RTM history.",
                        "P1 有限元声学核；目标域外使用独立弱形式 CFS-CPML，保留旧阻尼层作对照。无 RTM 历史。")
                    : l("Central DRP7 / classical RK4; zero-pressure surface; external ADE CFS-CPML. "
                        "Experimental forward only.",
                        "中心 DRP7 / 固定算子 RK4；零压力自由面，外扩 ADE CFS-CPML。试验正演核。"));
            if (options.propagator == Propagator::CurvilinearDrp) {
                number(l("Selective numerical dissipation (0..1)", "选择性数值耗散（0..1，非Q）"),
                       &options.drpDissipation);
                ImGui::TextWrapped("%s", l("Area-normalized 5x5 binomial source; high-wave-number pressure "
                                           "filter is not material Q.",
                                           "5×5面积归一化震源；高波数压力滤波属于数值耗散，不代表介质Q。"));
            }
        }
        if (chrome.category == 0 &&
            parameterGroup(l("Model files", "模型文件"), ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::SetNextItemWidth(-1);
            ImGui::InputText("##acousticModel", modelPath.data(), modelPath.size());
            if (ImGui::Button(l("Browse model", "选择模型")))
                browse(modelPath, false);
            ImGui::SameLine();
            if (ImGui::Button(l("Load model", "载入模型"))) {
                Options next;
                std::string first;
                if (Acoustic::readModelVts(path(modelPath.data()), next, first)) {
                    options = next;
                    modelEditor.terrainPreset = -1;
                    modelEditor.terrainPresetSource.clear();
                    backend = static_cast<int>(next.backend);
                    precision = next.model.precision == Precision::Float32 ? 1 : 0;
                    error.clear();
                } else {
                    Curvilinear::ModelDocument doc;
                    if (Curvilinear::readModelVts(path(modelPath.data()), doc, error)) {
                        options.model = doc.parameters;
                        modelEditor.terrainPreset = -1;
                        modelEditor.terrainPresetSource.clear();
                        options.model.attenuation.enabled = false;
                        options.compute = doc.compute;
                        options.compute.cudaDevice = -1;
                        backend = static_cast<int>(doc.backend);
                        precision = doc.parameters.precision == Precision::Float32 ? 1 : 0;
                        status = l("Elastic model loaded as acoustic; Q disabled, Vs retained but not used. "
                                   "Review depths and receiver positions.",
                                   "弹性模型已作为声学模型载入；关闭 Q，保留 Vs "
                                   "但不参与计算，请核对深度及接收位置。");
                    } else
                        error = first + " / " + error;
                }
            }
            ImGui::SetNextItemWidth(-1);
            ImGui::InputText("##acousticSurface", surfacePath.data(), surfacePath.size());
            if (ImGui::Button(l("Browse DEM profile", "选择高程")))
                browse(surfacePath, true);
            ImGui::SameLine();
            if (ImGui::Button(l("Load profile", "载入高程"))) {
                modelEditor.loadTerrain();
            }
        }
        if (chrome.category == 3 &&
            parameterGroup(l("Pressure display", "声学压力 p 显示"), ImGuiTreeNodeFlags_DefaultOpen)) {
            SeisWave::Ui::choiceField2D(
                l("Field view [W]", "显示模式 [W]"), &fieldMode,
                l("Pressure only\0Vp model only\0Model + wavefield\0", "压力波场\0Vp 模型\0模型＋波场\0"));
            if (ImGui::Checkbox(l("Equal X/Z scale", "X/Z 等比例坐标"), &equalAspect))
                fit = true;
            SeisWave::Ui::runtimeDisplayOptions();
            ImGui::Checkbox(l("Grid [G]", "网格 [G]"), &gridLines);
            ImGui::TextDisabled("%s %.3g m", l("Grid line spacing (auto):", "网格线间隔（自动）："),
                                gridStride * (simulation.initialized() ? simulation.options().model.spacing
                                                                       : options.model.spacing));
            if (ImGui::Checkbox(l("Absorber wavefield [B]", "吸收区波场 [B]"), &absorberOverlay))
                fit = true;
            if (absorberOverlay) {
                ImGui::TextUnformatted(l("Boundary tint opacity", "边界着色不透明度"));
                ImGui::SetNextItemWidth(-1);
                ImGui::SliderFloat("##acousticAbsorberOpacity", &absorberOpacity, 0.f, .5f, "%.2f");
            }
            ImGui::Checkbox(l("Source marker", "显示震源"), &showSource);
            ImGui::Checkbox(l("Receiver markers", "显示检波点"), &showReceivers);
            SeisWave::Ui::choiceField2D(l("Vp palette [E]", "Vp 模型配色 [E]"), &modelPalette,
                                        Palettes::wavePaletteNames);
            ImGui::TextUnformatted(l("Pressure clip / peak", "压力限幅 / 峰值"));
            ImGui::SetNextItemWidth(-1);
            ImGui::SliderFloat("##acousticClip", &clip, .01f, 1.f);
            SeisWave::Ui::choiceField2D(l("Pressure palette", "压力配色"), &palette,
                                        Palettes::wavePaletteNames);
            if (ImGui::Button(l("Fit plots", "适配图件")))
                fit = true;
        }
        if (chrome.category == 4 &&
            parameterGroup(l("Export active data", "导出生效数据"), ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::Checkbox(l("SEG-Y IEEE64 (otherwise IEEE32)", "SEG-Y IEEE64（默认IEEE32）"), &segy64);
            ImGui::BeginDisabled(!simulation.initialized());
            if (functionButton3D(l("Export active simulation", "导出生效模拟"), FunctionRole3D::Export,
                                 {-1, 0})) {
                running = false;
                wait();
                auto destination =
                    SeisWave::Ui::outputRoot() / "acoustic-2d" /
                    ("experiment-" +
                     std::to_string(std::chrono::system_clock::now().time_since_epoch().count()));
                if (Acoustic::exportResult(simulation, destination, error, segy64)) {
                    auto p = destination.u8string();
                    status =
                        std::string(l("Exported: ", "已导出：")) + reinterpret_cast<const char *>(p.c_str());
                }
            }
            ImGui::EndDisabled();
            ImGui::TextWrapped(
                "%s",
                l("Pressure records: SEG-Y rev2 in Pa, gain=1, plus CSV. Models and fields: VTS. "
                  "IEEE32 is conventional; IEEE64 retains double samples. No display AGC or normalization.",
                  "压力记录输出 SEG-Y "
                  "rev2（Pa，增益1）及CSV；模型和波场使用VTS。IEEE32常用，IEEE64保留双精度；不使用显示AGC或归"
                  "一化。"));
        }
        ImGui::PopItemWidth();
        ImGui::EndChild();
        syncSurfaceReceivers();
        sessionActions = SeisWave::Ui::parameterSession2D(session);
        if (sessionActions.rebuild)
            apply();
        if (sessionActions.undo)
            undoEdits();
        if (sessionActions.defaults)
            defaults();
        ImGui::End();
    }
    ImPlotColormap cmap(int index) {
        auto name = "AcousticPalette" + std::to_string(index);
        int id = ImPlot::GetColormapIndex(name.c_str());
        if (id < 0) {
            ImVec4 p[5];
            const auto &colors = Palettes::wavePalette(index);
            for (int i = 0; i < 5; ++i)
                p[i] = {colors[i][0], colors[i][1], colors[i][2], 1};
            id = ImPlot::AddColormap(name.c_str(), p, 5, false);
        }
        return id;
    }
    void wavefield() {
        const char *title = l("Acoustic pressure###AcousticWave", "声学压力 p###AcousticWave");
        const bool visible = ImGui::Begin(title, nullptr, chrome.views.prepare(title));
        if (visible)
            chrome.views.tools(title);
        waveRect = {ImGui::GetWindowPos(),
                    {ImGui::GetWindowPos().x + ImGui::GetWindowSize().x,
                     ImGui::GetWindowPos().y + ImGui::GetWindowSize().y}};
        if (!visible || !simulation.initialized() || shown.frame.empty()) {
            ImGui::End();
            return;
        }
        const auto &g = simulation.grid();
        const auto &active = simulation.options();
        const auto &p = active.model;
        const auto size = ImGui::GetContentRegionAvail();
        const bool showModel = fieldMode != 0, showWave = fieldMode != 1;
        const int legendCount = int(showModel) + int(showWave);
        const float height =
            std::max(80.f, size.y - SeisWave::Ui::scalarLegendHeight(legendCount, size.x) -
                               ImGui::GetTextLineHeightWithSpacing() - 2 * ImGui::GetStyle().ItemSpacing.y);
        const bool compactScale = height < 160;
        const ImVec2 plotSize{std::max(120.f, size.x), height};
        if (plotSize.x != lastPlotSize.x || plotSize.y != lastPlotSize.y)
            fitted[0] = false;
        lastPlotSize = plotSize;
        const double modelLow = modelMinimum, modelHigh = modelMaximum;
        const int padding = active.absorberCells;
        const auto frameIndex = [&](int x, int z) { return std::size_t(z) * g.computeNx + (x + padding); };
        // Keep the same target-domain amplitude scale when B is toggled. No independent
        // normalization or artificial fade is applied to pressure inside the absorber.
        double high = 1e-30;
        for (int z = 0; z < g.nz; ++z)
            for (int x = 0; x < g.nx; ++x)
                high = std::max(high, std::abs(double(shown.frame[frameIndex(x, z)])));
        high *= clip;
        displayedPressurePeak = high;
        renderedAbsorberCells = 0;
        if (!showWave)
            high = modelHigh;
        const bool external = absorberOverlay && active.damping;
        const double pad = external ? active.absorberCells * p.spacing : 0;
        const double xmax = g.x[g.nx - 1], zmax = g.z[g.z.size() - 1];
        double zmin = 0;
        for (int x = 0; x < g.nx; ++x)
            zmin = std::min(zmin, g.z[x]);
        const double left = -pad, right = xmax + pad, bottom = zmax + pad;
        if (!fitted[0])
            ImPlot::SetNextAxesToFit();
        auto colors = cmap(showWave ? palette : modelPalette);
        const auto materialColors = cmap(modelPalette);
        ImPlot::PushColormap(colors);
        ImPlot::PushStyleVar(ImPlotStyleVar_PlotMinSize, ImVec2{120, 80});
        ImGui::PushFont(nullptr, compactScale ? std::min(14.f, ImGui::GetFontSize()) : ImGui::GetFontSize());
        if (ImPlot::BeginPlot("##acousticPressure", plotSize,
                              ImPlotFlags_NoLegend | (equalAspect ? ImPlotFlags_Equal : 0))) {
            fitted[0] = true;
            const auto compact = height < 160 ? ImPlotAxisFlags_NoLabel : ImPlotAxisFlags_None;
            ImPlot::SetupAxes("X (m)", "Z (m)", compact, compact | ImPlotAxisFlags_Invert);
            if (height < 190) {
                ImPlot::SetupAxisFormat(ImAxis_X1, "%.3g");
                ImPlot::SetupAxisFormat(ImAxis_Y1, "%.3g");
                ImPlot::SetupAxisTicks(ImAxis_X1, 0, xmax, compactScale ? 2 : 3);
                if (compactScale)
                    ImPlot::SetupAxisTicks(ImAxis_Y1, zmin, zmax, 2);
            }
            // Fit both extents and let Equal expand the short axis, never crop the model.
            const double bx[]{left - .02 * (right - left), right + .02 * (right - left)},
                bz[]{zmin - .02 * (bottom - zmin), bottom + .02 * (bottom - zmin)};
            ImPlot::SetNextLineStyle({0, 0, 0, 0});
            ImPlot::PlotLine("##acousticBounds", bx, bz, 2);
            const auto limits = ImPlot::GetPlotLimits();
            domainVisible = limits.X.Min <= left && limits.X.Max >= right && limits.Y.Min <= zmin &&
                            limits.Y.Max >= bottom;
            auto *draw = ImPlot::GetPlotDrawList();
            ImPlot::PushPlotClipRect();
            // The acoustic mesh extends side columns at their edge elevation, and is
            // horizontal below the target bottom. Reuse that geometry without retaining
            // a second full set of model coordinates for display.
            const auto point = [&](int x, int z) {
                const int edge = std::clamp(x, 0, g.nx - 1);
                const double px = x < 0 || x >= g.nx ? x * p.spacing : double(g.x[edge]);
                const double pz = z >= g.nz ? z * p.spacing : double(g.z[std::size_t(z) * g.nx + edge]);
                return ImPlotPoint(px, pz);
            };
            const auto toPixel = [&](int x, int z) {
                const auto at = point(x, z);
                return ImPlot::PlotToPixels(at.x, at.y);
            };
            const auto material = [&](int x, int z) -> double {
                if (z < g.nz)
                    return g.vp[std::size_t(z) * g.nx + std::clamp(x, 0, g.nx - 1)];
                double depth = 0;
                if (p.layered)
                    for (std::size_t layer = 0; layer < p.layers.size(); ++layer) {
                        depth += p.layers[layer].thickness;
                        if (layer + 1 == p.layers.size() || z * p.spacing < depth)
                            return p.layers[layer].vp;
                    }
                return p.vp;
            };
            const auto origin = ImPlot::PlotToPixels(0, 0);
            const auto px = ImPlot::PlotToPixels(p.spacing, 0), pz = ImPlot::PlotToPixels(0, p.spacing);
            aspectError = std::abs(std::abs((px.x - origin.x) / (pz.y - origin.y)) - 1);
            auto flags = draw->Flags;
            draw->Flags &= ~ImDrawListFlags_AntiAliasedFill;
            const auto pixels = ImPlot::GetPlotSize();
            const int dx = std::max(1, static_cast<int>(std::ceil(g.nx / std::max(32.f, pixels.x / 2)))),
                      dz = std::max(1, static_cast<int>(std::ceil(g.nz / std::max(32.f, pixels.y / 2))));
            const int firstX = external ? -padding : 0;
            const int lastX = external ? g.nx - 1 + padding : g.nx - 1;
            const int lastZ = external ? g.nz - 1 + padding : g.nz - 1;
            for (int z = 0; z < lastZ;) {
                const int zz = std::min({lastZ, z + dz, z < g.nz - 1 ? g.nz - 1 : lastZ});
                for (int x = firstX; x < lastX;) {
                    // Stop at the target interface so a sampled display cell never
                    // interpolates across an unshown strip or hides the interface.
                    const int xx = std::min({lastX, x + dx, x < 0 ? 0 : x < g.nx - 1 ? g.nx - 1 : lastX});
                    const int xs[4]{x, xx, xx, x}, zs[4]{z, z, zz, zz};
                    const bool absorberCell = x < 0 || xx >= g.nx || zz >= g.nz;
                    if (absorberCell)
                        ++renderedAbsorberCells;
                    ImVec2 points[4];
                    ImU32 vertexColors[4];
                    for (int j = 0; j < 4; ++j) {
                        const auto q = frameIndex(xs[j], zs[j]);
                        points[j] = toPixel(xs[j], zs[j]);
                        auto color = showModel ? ImPlot::SampleColormap(
                                                     static_cast<float>((material(xs[j], zs[j]) - modelLow) /
                                                                        (modelHigh - modelLow)),
                                                     materialColors)
                                               : palette3D().background;
                        if (showWave) {
                            const double normalized = std::clamp(shown.frame[q] / high, -1., 1.);
                            const auto wave =
                                ImPlot::SampleColormap(static_cast<float>(.5 + .5 * normalized), colors);
                            const float alpha =
                                showModel ? static_cast<float>(std::min(1., 4 * std::abs(normalized))) : .95f;
                            color = {color.x * (1 - alpha) + wave.x * alpha,
                                     color.y * (1 - alpha) + wave.y * alpha,
                                     color.z * (1 - alpha) + wave.z * alpha, 1};
                        }
                        if (absorberCell)
                            color = SeisWave::Ui::tintAbsorber2D(color, absorberOpacity);
                        vertexColors[j] = ImGui::ColorConvertFloat4ToU32(color);
                    }
                    draw->PrimReserve(6, 4);
                    const auto base = static_cast<ImDrawIdx>(draw->_VtxCurrentIdx);
                    for (int j : {0, 1, 2, 0, 2, 3})
                        draw->PrimWriteIdx(static_cast<ImDrawIdx>(base + j));
                    for (int j = 0; j < 4; ++j)
                        draw->PrimWriteVtx(points[j], ImGui::GetFontTexUvWhitePixel(), vertexColors[j]);
                    x = xx;
                }
                z = zz;
            }
            draw->Flags = flags;
            if (external) {
                // Mark the physical target boundary without tinting the wavefield.
                const auto interfaceColor = IM_COL32(215, 123, 30, 210);
                for (int edge : {0, g.nx - 1})
                    for (int z = 0; z < g.nz - 1; ++z)
                        draw->AddLine(toPixel(edge, z), toPixel(edge, z + 1), interfaceColor, 1);
                draw->AddLine(toPixel(0, g.nz - 1), toPixel(g.nx - 1, g.nz - 1), interfaceColor, 1);
                draw->AddLine(toPixel(firstX, 0), toPixel(0, 0), color3D(palette3D().text, .8f), 1);
                draw->AddLine(toPixel(g.nx - 1, 0), toPixel(lastX, 0), color3D(palette3D().text, .8f), 1);
            }
            if (gridLines) {
                // The same logical stride in X and eta preserves square cells in the flat domain.
                // Plot viewport width includes equal-aspect margins and cannot determine X stride.
                const double pixelsPerCell = std::min(std::abs(px.x - origin.x), std::abs(pz.y - origin.y));
                gridStride = SeisWave::Ui::displayGridStride2D(g.nx, g.nz, p.spacing, pixelsPerCell);
                const int gx = gridStride, gz = gridStride;
                const auto start = toPixel(0, 0), horizontal = toPixel(gx, 0), vertical = toPixel(0, gz);
                gridCellAspectError =
                    std::abs(std::abs((horizontal.x - start.x) / (vertical.y - start.y)) - 1);
                for (int z = 0;; z = std::min(g.nz - 1, z + gz)) {
                    for (int x = 0; x < g.nx - 1; x += gx)
                        draw->AddLine(toPixel(x, z), toPixel(std::min(g.nx - 1, x + gx), z),
                                      color3D(palette3D().text, .16f), .5f);
                    if (z == g.nz - 1)
                        break;
                }
                for (int x = 0;; x = std::min(g.nx - 1, x + gx)) {
                    for (int z = 0; z < g.nz - 1; z += gz)
                        draw->AddLine(toPixel(x, z), toPixel(x, std::min(g.nz - 1, z + gz)),
                                      color3D(palette3D().text, .16f), .5f);
                    if (x == g.nx - 1)
                        break;
                }
            }
            for (int x = 0; x < g.nx - 1; ++x)
                draw->AddLine(toPixel(x, 0), toPixel(x + 1, 0), color3D(palette3D().text, .8f), 1);
            const auto mouse = ImGui::GetIO().MousePos;
            const auto distance2 = [&](ImVec2 v) {
                return (v.x - mouse.x) * (v.x - mouse.x) + (v.y - mouse.y) * (v.y - mouse.y);
            };
            int hoveredReceiver = -1;
            float nearest = 64;
            markerReceivers = 0;
            const auto &receivers = simulation.receivers();
            if (showReceivers)
                for (int r = 0; r < static_cast<int>(receivers.size()); ++r) {
                    const auto &position = receivers[r].position;
                    const auto at = ImPlot::PlotToPixels(position[0], position[1]);
                    draw->AddCircleFilled(at, 3.4f, IM_COL32(20, 63, 58, 230));
                    draw->AddCircleFilled(at, 2.4f, IM_COL32(43, 194, 165, 255));
                    ++markerReceivers;
                    if (distance2(at) < nearest) {
                        nearest = distance2(at);
                        hoveredReceiver = r;
                    }
                }
            sourcePixel = toPixel(p.sourceX, p.sourceZ);
            if (showSource) {
                draw->AddCircleFilled(sourcePixel, 6, IM_COL32(93, 47, 13, 255));
                draw->AddCircleFilled(sourcePixel, 4.5f, IM_COL32(246, 147, 39, 255));
            }
            if (ImPlot::IsPlotHovered() && showSource && distance2(sourcePixel) < 100) {
                const auto q = std::size_t(p.sourceZ) * g.nx + p.sourceX;
                ImGui::SetTooltip(
                    l("Source | X %.2f m, Z %.2f m\nNode %d, %d", "震源 | X %.2f m，Z %.2f m\n节点 %d，%d"),
                    g.x[q], g.z[q], p.sourceX, p.sourceZ);
            } else if (ImPlot::IsPlotHovered() && hoveredReceiver >= 0) {
                const auto &r = receivers[hoveredReceiver];
                ImGui::SetTooltip(l("Receiver R%d | X %.2f m, Z %.2f m", "检波点 R%d | X %.2f m，Z %.2f m"),
                                  hoveredReceiver + 1, r.position[0], r.position[1]);
            }
            ImPlot::PopPlotClipRect();
            ImPlot::EndPlot();
        }
        ImGui::PopFont();
        ImPlot::PopStyleVar();
        ImPlot::PopColormap();
        std::vector<SeisWave::Ui::ScalarLegend> legends;
        if (showWave)
            legends.push_back({"p (Pa)", -high, high, [colors](double t) {
                                   return ImGui::ColorConvertFloat4ToU32(
                                       ImPlot::SampleColormap(static_cast<float>(t), colors));
                               }});
        if (showModel)
            legends.push_back({"Vp (m/s)", modelLow, modelHigh, [materialColors](double t) {
                                   return ImGui::ColorConvertFloat4ToU32(
                                       ImPlot::SampleColormap(static_cast<float>(t), materialColors));
                               }});
        SeisWave::Ui::drawScalarLegendStrip("##AcousticFieldLegends", legends);
        ImGui::TextDisabled("%s %d | %s | X/Z %s", l("S: source; R:", "S：震源；R：检波点"), markerReceivers,
                            external ? l("Amber: absorber", "浅橙：吸收区") : "p (Pa)",
                            equalAspect ? "1:1" : l("free", "自由"));
        ImGui::End();
    }
    SeisWave::RecordDataView displayedRecords() const {
        SeisWave::RecordDataView data;
        if (!simulation.initialized())
            return data;
        data.units = "Pa";
        data.components = {"p"};
        data.dt = shown.recordDt;
        data.revision = (recordGeneration << 32) + shown.step + 1;
        const auto &g = simulation.grid();
        const auto &p = simulation.options().model;
        const auto source = std::size_t(p.sourceZ) * g.nx + p.sourceX;
        const auto &positions = simulation.receivers();
        for (std::size_t r = 0; r < recordSamples.size(); ++r) {
            SeisWave::TraceView trace;
            trace.position = {positions[r].position[0], 0, positions[r].position[1]};
            trace.source = {g.x[source], 0, g.z[source]};
            trace.channels[0] = recordSamples[r];
            data.traces.push_back(trace);
        }
        return data;
    }
    void records() {
        const char *title = l("Acoustic records###AcousticRecords", "声学压力 p 记录###AcousticRecords");
        const bool visible = ImGui::Begin(title, nullptr, chrome.views.prepare(title));
        if (visible) {
            chrome.views.tools(title);
            ImGui::SameLine();
            ImGui::TextDisabled("p (Pa)");
        }
        recordsRect = {ImGui::GetWindowPos(),
                       {ImGui::GetWindowPos().x + ImGui::GetWindowSize().x,
                        ImGui::GetWindowPos().y + ImGui::GetWindowSize().y}};
        if (visible && simulation.initialized() && shown.samples > 1) {
            if (fit) {
                recordView.settings.autoFit = true;
                recordView.invalidate();
            }
            ImGui::PushFont(nullptr, ImGui::GetContentRegionAvail().y < 220
                                         ? std::min(16.f, ImGui::GetFontSize())
                                         : ImGui::GetFontSize());
            recordView.drawContent(displayedRecords(), selectedReceiver);
            ImGui::PopFont();
            recordPlotRect = {{recordView.plotBounds[0], recordView.plotBounds[1]},
                              {recordView.plotBounds[2], recordView.plotBounds[3]}};
        } else if (visible)
            ImGui::TextWrapped("%s", l("No pressure records yet. Run [Space] or take one step [N].",
                                       "尚无压力记录，请运行 [Space] 或单步 [N]。"));
        ImGui::End();
    }
    ImVec2 lastViewportSize{};
    void dock() {
        if (!help)
            chrome.views.beginFrame();
        if (SeisWave::Ui::beginWorkbenchMenuBar()) {
            if (ImGui::Button(l("Home", "主页")))
                home = true;
            navigation.draw(options.model, options.compute, static_cast<BackendKind3D>(backend), error);
            if (SeisWave::Ui::MenuBarOverflow more(
                    {l("Workspace", "工作区"), l("Appearance", "界面外观"), l("Help [F1]", "帮助 [F1]")},
                    l("More", "更多"));
                more.open) {
                if (SeisWave::Ui::beginDropdownMenu(l("Workspace", "工作区"))) {
                    bool previous = chrome.floating;
                    chrome.panelMenu();
                    if (previous != chrome.floating)
                        layout = true;
                    ImGui::MenuItem(l("Active parameters", "当前模拟参数"), nullptr, &showSummary);
                    if (ImGui::MenuItem(l("Restore layout", "恢复布局"))) {
                        restoreLayout();
                    }
                    ImGui::EndMenu();
                }
                if (SeisWave::Ui::beginDropdownMenu(l("Appearance", "界面外观"))) {
                    int language = language3D();
                    if (ImGui::Combo("##acousticLanguage", &language, "English\0简体中文\0"))
                        setLanguage3D(language);
                    int theme = themeIndex3D();
                    if (themeCombo3D("##acousticTheme", theme, 240))
                        applyTheme3D(theme);
                    float font = SeisWave::Ui::interfaceFontSize();
                    ImGui::SetNextItemWidth(240);
                    if (ImGui::SliderFloat(l("Font size (px)", "字体大小（px）"), &font, 16, 28, "%.0f")) {
                        SeisWave::Ui::setInterfaceFontSize(font);
                        layout = true;
                    }
                    glassAppearanceControls3D();
                    ImGui::EndMenu();
                }
                if (ImGui::Button(l("Help [F1]", "帮助 [F1]")))
                    help = !help;
            }
            SeisWave::Ui::endWorkbenchMenuBar();
        }
        const auto *viewport = ImGui::GetMainViewport();
        const auto elapsed = std::string(l("Compute time: ", "计算耗时：")) +
                             SeisWave::Ui::computeElapsedText(shown.diagnostics.totalStepMs);
        const char *captions[]{
            l(running ? "Pause [Space]" : "Run [Space]", running ? "暂停 [Space]" : "运行 [Space]"),
            l("Step [N]", "单步 [N]"), l("Clear [C]", "清空 [C]"), l("Apply / rebuild", "应用 / 重建"),
            l("Fit [R]", "适配 [R]")};
        std::array<float, 6> widths{};
        for (int i = 0; i < 5; ++i)
            widths[i] = ImGui::CalcTextSize(captions[i]).x + 2 * ImGui::GetStyle().FramePadding.x;
        widths[5] = ImGui::CalcTextSize(elapsed.c_str()).x;
        SeisWave::Ui::CommandStrip2D strip(
            widths, {0, 0, 0, 1, 2, 3},
            {l("Compute", "计算"), l("Model", "模型"), l("View", "视图"), l("Status", "状态")});
        const int rows = strip.rows(viewport->WorkSize.x - 2 * ImGui::GetStyle().WindowPadding.x);
        const float commandHeight = rows * ImGui::GetFrameHeight() +
                                    (rows - 1) * ImGui::GetStyle().ItemSpacing.y +
                                    2 * ImGui::GetStyle().WindowPadding.y;
        ImGui::SetNextWindowPos(viewport->WorkPos);
        ImGui::SetNextWindowSize({viewport->WorkSize.x, commandHeight});
        ImGui::Begin("##AcousticCommands", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoMove |
                         ImGuiWindowFlags_NoSavedSettings);
        auto next = [&](int i) { strip.next(i); };
        next(0);
        if (functionButton3D(captions[0], FunctionRole3D::Run))
            toggleRun();
        runRect = {ImGui::GetItemRectMin(), ImGui::GetItemRectMax()};
        next(1);
        if (functionButton3D(captions[1], FunctionRole3D::Run))
            singleStep();
        next(2);
        if (functionButton3D(captions[2], FunctionRole3D::Danger))
            clear();
        next(3);
        if (functionButton3D(captions[3], FunctionRole3D::Model))
            apply();
        next(4);
        if (functionButton3D(captions[4], FunctionRole3D::Display))
            fit = true;
        next(5);
        ImGui::TextColored(functionColor3D(FunctionRole3D::Run), "%s", elapsed.c_str());
        if (simulation.initialized()) {
            ImGui::SameLine();
            if (ImGui::GetContentRegionAvail().x > 160) {
                char progress[128];
                std::snprintf(progress, sizeof(progress), "%d/%d | t %.3f s", shown.step,
                              simulation.totalSteps(), shown.step * simulation.dt());
                ImGui::ProgressBar(static_cast<float>(shown.step) / simulation.totalSteps(),
                                   {-1, ImGui::GetFrameHeight()}, progress);
            }
        }
        ImGui::End();
        auto *vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos({vp->WorkPos.x, vp->WorkPos.y + commandHeight});
        ImGui::SetNextWindowSize({vp->WorkSize.x, vp->WorkSize.y - commandHeight});
        ImGui::Begin("##AcousticDockHost", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoDocking |
                         ImGuiWindowFlags_NoSavedSettings);
        auto id = ImGui::GetID("AcousticDock");
        const bool resized = lastViewportSize.x != vp->WorkSize.x;
        auto *sidebar = ImGui::DockBuilderGetNode(parameterDockNode);
        const float sidebarWidth = sidebar ? sidebar->SizeRef.x : 350.f;
        const bool resizedVertical = lastViewportSize.y != vp->WorkSize.y - commandHeight;
        auto *recordNode = ImGui::DockBuilderGetNode(recordDockNode);
        SeisWave::Ui::resizeDockSpace2D(id, lastViewportSize,
                                        {vp->WorkSize.x, vp->WorkSize.y - commandHeight});
        if (resized && sidebar && sidebar->ParentNode && sidebar->ParentNode->SplitAxis == ImGuiAxis_X)
            sidebar->SizeRef.x = std::clamp(sidebarWidth, 280.f, std::max(280.f, vp->WorkSize.x * .38f));
        if (resizedVertical && recordNode && recordNode->ParentNode &&
            recordNode->ParentNode->SplitAxis == ImGuiAxis_Y)
            recordNode->SizeRef.y =
                std::max(recordNode->SizeRef.y, std::min(280.f, (vp->WorkSize.y - commandHeight) * .52f));
        ImGui::DockSpace(id, {0, 0});
        if (layout || !ImGui::DockBuilderGetNode(id)) {
            ImGui::DockBuilderRemoveNode(id);
            ImGui::DockBuilderAddNode(id, ImGuiDockNodeFlags_DockSpace);
            ImGui::DockBuilderSetNodeSize(id, {vp->WorkSize.x, vp->WorkSize.y - commandHeight});
            ImGuiID main = id, left{}, bottom{};
            ImGui::DockBuilderSplitNode(main, ImGuiDir_Left, std::clamp(350.f / vp->WorkSize.x, .22f, .38f),
                                        &left, &main);
            parameterDockNode = left;
            ImGui::DockBuilderSplitNode(main, ImGuiDir_Down, .39f, &bottom, &main);
            recordDockNode = bottom;
            if (!chrome.floating)
                ImGui::DockBuilderDockWindow("Controls###AcousticControls", left);
            ImGui::DockBuilderDockWindow("Active model###AcousticSummary", left);
            ImGui::DockBuilderDockWindow("Pressure###AcousticWave", main);
            ImGui::DockBuilderDockWindow("Records###AcousticRecords", bottom);
            ImGui::DockBuilderFinish(id);
            layout = false;
        }
        ImGui::End();
    }
    void summary() {
        if (!showSummary)
            return;
        ImGui::Begin(l("Active model###AcousticSummary", "当前模拟参数###AcousticSummary"), &showSummary);
        if (simulation.initialized()) {
            const auto &p = simulation.options().model;
            ImGui::TextWrapped("%s",
                               l("Active acoustic solver; edited sidebar inputs apply on Run / Rebuild.",
                                 "此处为已生效的声学会话；左侧修改在运行/重建时应用。"));
            ImGui::Separator();
            ImGui::Text("%d x %d | h %.3g m", p.nx, p.nz, p.spacing);
            ImGui::Text("f %.3g Hz | dt %.5g ms", p.frequency, simulation.dt() * 1000);
            ImGui::Text("%s", precisionName(p.precision));
            const auto &active = simulation.options();
            ImGui::Text("%s: %s", l("Boundary", "边界"),
                        !active.damping                              ? l("Off", "关闭")
                        : active.absorber == AbsorbingBoundary::Cpml ? "CFS-CPML"
                                                                     : l("Legacy sponge", "原阻尼层"));
            ImGui::Text("%s %.2f MiB", l("Host arrays:", "主机数组："),
                        shown.diagnostics.residentHostArrayBytes / 1048576.);
            ImGui::Text("GPU %.2f MiB", shown.diagnostics.residentDeviceBytes / 1048576.);
            ImGui::TextWrapped("%s", simulation.options().propagator == Propagator::P1
                                         ? l("P1 FEM / pressure. No S waves or Q attenuation.",
                                             "P1 有限元压力核，不含 S 波与 Q 衰减。")
                                         : l("Experimental curvilinear DRP7-RK4 / p,vx,vz. No S waves or Q.",
                                             "试验曲网格 DRP7-RK4 / p,vx,vz，不含 S 波与 Q。"));
        }
        ImGui::End();
    }
};
void verifyInteractions(Workspace &app, SeisWave::Ui::AppRuntime &runtime,
                        const std::function<void()> &render) {
    auto require = [](bool value, const char *message) {
        if (!value)
            throw std::runtime_error(message);
    };
    auto &io = ImGui::GetIO();
    const auto key = [&](ImGuiKey code) {
        io.AddKeyEvent(code, true);
        render();
        io.AddKeyEvent(code, false);
        render();
    };
    const auto click = [&](ImRect rect) {
        const auto at = rect.GetCenter();
        io.AddMousePosEvent(at.x, at.y);
        io.AddMouseButtonEvent(0, true);
        render();
        io.AddMouseButtonEvent(0, false);
        render();
    };
    const auto capture = [&](const char *name) {
        int w, h;
        glfwGetFramebufferSize(runtime.window(), &w, &h);
        require(SeisWave::Ui::capture(SeisWave::Ui::outputRoot() / "screenshots/acoustic" / name, w, h),
                "Acoustic interaction screenshot failed");
    };
    const auto revealField = [&](const ImRect &rect) {
        if (auto *fields = ImGui::FindWindowByID(app.fieldsWindowId)) {
            if (rect.Min.y < fields->InnerClipRect.Min.y || rect.Max.y > fields->InnerClipRect.Max.y) {
                ImGui::SetScrollY(fields,
                                  fields->Scroll.y + rect.GetCenter().y - fields->InnerRect.GetCenter().y);
                render();
                render();
            }
        }
    };
    app.wait();
    require(app.simulation.advance(150, app.error), "Record viewer fixture did not advance");
    app.accept(app.publish());
    const auto rawRecords = app.recordSamples;
    const auto rawField = app.simulation.pressureSnapshot();
    app.selectedReceiver = static_cast<int>(app.recordSamples.size() / 2);
    app.recordView.settings.view = 1;
    app.recordView.settings.agc.enabled = true;
    io.AddFocusEvent(true);
    render();
    render();
    capture("single-pressure-trace.ppm");
    require(app.recordSamples == rawRecords && app.simulation.pressureSnapshot() == rawField,
            "Record AGC / single-trace view changed raw pressure");
    const auto records = app.displayedRecords();
    require(records.units == "Pa" && records.components == std::vector<std::string>{"p"} &&
                records.sampleCount(0) == app.simulation.receivers()[0].pressure.size(),
            "Shared acoustic record panel units/component/timeline mismatch");
    app.recordView.settings.view = 0;
    app.recordView.settings.agc.enabled = false;
    app.fieldMode = 2;
    render();
    capture("model-wavefield-overlay.ppm");
    key(ImGuiKey_C);
    require(app.shown.step == 0 && app.shown.diagnostics.totalStepMs == 0, "C did not clear fields/timer");
    key(ImGuiKey_N);
    require(app.shown.step == 1, "N did not advance exactly one step");
    key(ImGuiKey_Space);
    require(app.running, "Space did not start acoustic simulation");
    app.queue();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (app.worker.busy() && std::chrono::steady_clock::now() < deadline) {
        app.poll();
        render();
    }
    require(app.shown.step > 1, "Keyboard start did not advance background solve");
    key(ImGuiKey_Space);
    require(!app.running, "Space did not pause acoustic simulation");
    const int paused = app.shown.step;
    render();
    render();
    require(app.shown.step == paused, "Paused acoustic simulation advanced");
    const auto originalInputs = app.simulation.options();
    app.chrome.category = 0;
    render();
    render();
    const auto fixedButton = app.sessionActions.rebuildRect;
    const auto fieldsCenter = app.fieldsRect.GetCenter();
    io.AddMousePosEvent(fieldsCenter.x, fieldsCenter.y);
    io.AddMouseWheelEvent(0, -20);
    render();
    render();
    require(ImGui::FindWindowByID(app.fieldsWindowId)->Scroll.y > 0 &&
                std::abs(app.sessionActions.rebuildRect.Min.y - fixedButton.Min.y) < .1f,
            "Session actions moved with the parameter scroll region");
    app.options.model.frequency += 2;
    render();
    require(app.dirty(), "Pending acoustic inputs not detected");
    click(app.sessionActions.undoRect);
    require(app.options == originalInputs && app.shown.step == paused,
            "Undo did not preserve active simulation or restore its inputs");
    app.options.model.frequency += 1;
    render();
    click(app.sessionActions.rebuildRect);
    require(app.shown.step == 0 &&
                app.simulation.options().model.frequency == originalInputs.model.frequency + 1,
            "Fixed Reset / apply button failed");
    click(app.sessionActions.defaultsRect);
    auto interactiveDefaults = defaultOptions();
    SeisWave::Ui::applyInitialModel2D(interactiveDefaults.model, true);
    interactiveDefaults.model.autoSurfaceReceivers = true;
    synchronizeSurfaceReceivers(interactiveDefaults.model, true, 4096);
    require(app.options == interactiveDefaults &&
                app.simulation.options().model.frequency == originalInputs.model.frequency + 1,
            "Defaults changed active simulation instead of pending inputs");
    click(app.sessionActions.undoRect);
    require(!app.dirty(), "Fixed Undo did not restore active acoustic options");
    const int color = app.palette;
    key(ImGuiKey_Q);
    require(app.palette == (color + 1) % 14, "Q palette failed");
    io.AddKeyEvent(ImGuiMod_Shift, true);
    key(ImGuiKey_Q);
    io.AddKeyEvent(ImGuiMod_Shift, false);
    render();
    require(app.palette == color, "Shift+Q reverse palette failed");
    const int initialMode = app.fieldMode;
    key(ImGuiKey_W);
    require(app.fieldMode == (initialMode + 1) % 3, "W did not cycle display mode");
    const int material = app.modelPalette;
    key(ImGuiKey_E);
    require(app.modelPalette == (material + 1) % 14, "E material palette failed");
    key(ImGuiKey_W);
    key(ImGuiKey_W);
    require(app.fieldMode == initialMode, "W did not restore overlay view");
    const bool grid = app.gridLines;
    key(ImGuiKey_G);
    require(app.gridLines != grid, "G grid failed");
    key(ImGuiKey_G);
    key(ImGuiKey_B);
    render();
    render();
    require(app.absorberOverlay && app.domainVisible && app.aspectError < .001,
            "External damping display cropped or distorted target model");
    capture("absorber-acquisition.ppm");
    key(ImGuiKey_B);
    const float activeSourceX = app.sourcePixel.x;
    app.options.model.sourceX += 5;
    render();
    render();
    require(std::abs(app.sourcePixel.x - activeSourceX) < .1f, "Pending source moved active model marker");
    key(ImGuiKey_N);
    require(app.shown.step == 1 && app.simulation.options().model.sourceX == app.options.model.sourceX &&
                app.sourcePixel.x > activeSourceX + 1,
            "N did not apply pending source before stepping");
    const auto step = app.shown.step;
    app.chrome.category = 1;
    render();
    render();
    revealField(app.frequencyRect);
    click(app.frequencyRect);
    require(io.WantTextInput || ImGui::IsAnyItemActive(), "Acoustic input did not receive focus");
    const int before = app.palette;
    key(ImGuiKey_N);
    key(ImGuiKey_C);
    key(ImGuiKey_Q);
    require(app.shown.step == step && app.palette == before, "Global shortcut fired inside a text field");
    key(ImGuiKey_Enter);
    render();
    io.AddKeyEvent(ImGuiMod_Ctrl, true);
    key(ImGuiKey_C);
    io.AddKeyEvent(ImGuiMod_Ctrl, false);
    render();
    require(app.shown.step == step, "Ctrl+C incorrectly cleared simulation");
    click(app.navigation.rootRect);
    key(ImGuiKey_N);
    require(app.shown.step == step, "Shortcut fired while a module menu was open");
    key(ImGuiKey_Escape);
    const ImVec2 blank{app.waveRect.Min.x + 20, app.waveRect.Max.y - 15};
    click({blank, {blank.x + 2, blank.y + 2}});
    render();
    key(ImGuiKey_F1);
    require(app.help, "F1 did not open acoustic help");
    key(ImGuiKey_N);
    require(app.shown.step == step, "Shortcut ran while help was open");
    capture("keyboard-help.ppm");
    key(ImGuiKey_Escape);
    require(!app.help, "Esc did not close help");
    app.chrome.floating = true;
    app.chrome.center = true;
    app.layout = true;
    render();
    render();
    key(ImGuiKey_F);
    render();
    render();
    const auto *panel = ImGui::FindWindowByName("Controls###AcousticControls");
    require(!app.chrome.floating && panel && panel->DockId, "F did not restore acoustic layout");
    app.options.model.frequency = -1;
    key(ImGuiKey_N);
    require(!app.error.empty() && app.shown.step == step, "Invalid hotkey rebuild lost previous session");
    app.options = app.simulation.options();
    app.error.clear();
    key(ImGuiKey_R);
    render();
    render();
    require(app.domainVisible && app.aspectError < .001 &&
                app.markerReceivers == static_cast<int>(app.simulation.receivers().size()),
            "Acoustic fit / equal aspect / active acquisition markers failed");
    app.options.model.maxSteps = 2;
    require(app.apply() && app.simulation.advance(2, app.error), "Completed-run fixture failed");
    app.accept(app.publish());
    render();
    key(ImGuiKey_Space);
    require(app.running && app.shown.step == 0, "Completed simulation did not restart from t=0");
    key(ImGuiKey_Space);
    app.options.model.maxSteps = 0;
    app.options.model.importedSurface = true;
    app.options.model.surfaceDatum = 1000;
    const double width = (app.options.model.nx - 1) * app.options.model.spacing;
    app.options.model.surfaceSamples = {{0, 1000}, {width / 3, 1020}, {2 * width / 3, 980}, {width, 1010}};
    require(app.apply(), "Terrain UI fixture failed");
    key(ImGuiKey_R);
    render();
    render();
    require(app.domainVisible && app.aspectError < .001, "Terrain view cropped or distorted model");
    const auto &terrain = app.simulation.grid();
    const auto &active = app.simulation.options().model;
    const auto &receivers = app.simulation.receivers();
    for (int i = 0; i < static_cast<int>(receivers.size()); ++i) {
        const auto q =
            std::size_t(active.receiverDepth) * terrain.nx + active.receiverStart + i * active.receiverStride;
        require(std::abs(receivers[i].position[0] - terrain.x[q]) < 1e-6 &&
                    std::abs(receivers[i].position[1] - terrain.z[q]) < 1e-6,
                "Terrain receiver markers use grid indices instead of physical coordinates");
    }
    app.chrome.category = 3;
    app.fieldMode = 2;
    app.modelPalette = 7;
    render();
    capture("terrain-acquisition.ppm");
    // Place a source near the left interface and stop while its pulse is crossing
    // the PML, so the screenshot and assertions exercise nonzero exterior pressure.
    app.options = defaultOptions();
    app.options.model.nx = 161;
    app.options.model.nz = 101;
    app.options.model.layered = false;
    app.options.model.frequency = 20;
    app.options.model.sourceX = 12;
    app.options.model.sourceZ = 40;
    app.options.model.receiverCount = 60;
    app.backend = 0;
    app.precision = 1;
    require(app.apply(), "Absorber display fixture failed to build");
    require(app.simulation.advance(static_cast<int>(.16 / app.simulation.dt()), app.error),
            "Absorber display fixture failed to advance");
    app.accept(app.publish());
    app.fieldMode = 0;
    app.absorberOverlay = false;
    render();
    const double scaleWithoutAbsorber = app.displayedPressurePeak;
    const auto beforeAbsorberDisplay = app.simulation.pressureSnapshot(true);
    key(ImGuiKey_B);
    render();
    render();
    require(app.absorberOverlay && app.renderedAbsorberCells > 0 && app.domainVisible &&
                app.aspectError < .001 && app.displayedPressurePeak == scaleWithoutAbsorber,
            "Absorber rendering missing, cropped, distorted or independently normalized");
    double exteriorPeak = 0;
    const auto &expandedGrid = app.simulation.grid();
    for (int z = 0; z < expandedGrid.computeNz; ++z)
        for (int x = 0; x < expandedGrid.computeNx; ++x)
            if (x < app.options.absorberCells || x >= expandedGrid.nx + app.options.absorberCells ||
                z >= expandedGrid.nz)
                exteriorPeak =
                    std::max(exteriorPeak,
                             std::abs(double(app.shown.frame[std::size_t(z) * expandedGrid.computeNx + x])));
    require(exteriorPeak > 1e-8 * scaleWithoutAbsorber &&
                app.simulation.pressureSnapshot(true) == beforeAbsorberDisplay,
            "Absorber display is empty or changed the solver state");
    capture("absorber-wavefield.ppm");
    const auto physicalFrame = app.shown.frame;
    const ImVec4 baseColor{.1f, .2f, .8f, 1};
    const auto transparent = SeisWave::Ui::tintAbsorber2D(baseColor, 0);
    const auto opaque = SeisWave::Ui::tintAbsorber2D(baseColor, 1);
    require(transparent.x == baseColor.x && transparent.y == baseColor.y && transparent.z == baseColor.z &&
                std::abs(opaque.x - 237.f / 255) < 1e-6 && std::abs(opaque.y - 139.f / 255) < 1e-6 &&
                std::abs(opaque.z - 35.f / 255) < 1e-6,
            "Shared absorber tint did not preserve original amber hue or zero-opacity field");
    app.absorberOpacity = 0;
    render();
    require(app.shown.frame == physicalFrame && app.displayedPressurePeak == scaleWithoutAbsorber,
            "Acoustic tint changed raw frame or scale");
    app.absorberOpacity = SeisWave::Ui::defaultAbsorberOpacity2D;
    app.fieldMode = 2;
    render();
    capture("absorber-model-wavefield.ppm");
    const auto activeBeforeEditor = app.simulation.options();
    app.chrome.category = 0;
    app.groupsExpanded = true;
    ++app.groupEpoch;
    app.nextCapacityRefresh = 0;
    render();
    render();
    ImGui::SetScrollY(ImGui::FindWindowByID(app.fieldsWindowId), 0);
    render();
    capture("model-capacity.ppm");
    require(app.capacity.aspectNx >= 16 && app.capacity.aspectNz >= 16, "Acoustic capacity estimate missing");
    app.modelEditor.openLayers = true;
    render();
    render();
    app.modelEditor.layerDraft = {{20, 1800, 0, 1800}, {0, 2300, 0, 2000}};
    render();
    capture("shared-layer-editor.ppm");
    click(app.modelEditor.layerUseRect);
    render();
    require(app.options.model.layered && app.options.model.layers.size() == 2 &&
                app.options.model.layers[0].vs == 0 && app.simulation.options() == activeBeforeEditor,
            "Shared layer editor changed active solver or rejected acoustic Vs=0");
    for (int preset = 0; preset < 8; ++preset) {
        require(app.modelEditor.chooseTerrainPreset(preset), "Shared acoustic terrain preset failed");
        for (double presetWidth : {1000., 2000., 4000.}) {
            require(app.modelEditor.setTerrainPresetWidth(presetWidth),
                    "Shared terrain width adaptation failed");
            require(std::abs(app.options.model.surfaceSamples.back()[0] - presetWidth) < 1e-6,
                    "Shared terrain span mismatch");
        }
    }
    app.modelEditor.openTerrain = true;
    render();
    render();
    capture("shared-terrain-editor.ppm");
    click(app.modelEditor.terrainDoneRect);
    app.undoEdits();
    render();
    require(app.options == activeBeforeEditor && !app.dirty(),
            "Editor undo did not restore the active model");
    app.chrome.category = 1;
    render();
    render();
    click(app.surfaceReceiverRect);
    render();
    require(app.options.model.receiverDepth == 1 && app.simulation.options() == activeBeforeEditor &&
                app.dirty(),
            "Surface shortcut changed active session or used zero-pressure surface");
    require(app.apply() && app.simulation.advance(150, app.error),
            "Surface receiver layout did not build / advance");
    app.accept(app.publish());
    render();
    render();
    const auto &surveyGrid = app.simulation.grid();
    for (const auto &receiver : app.simulation.receivers()) {
        const int ix = int(std::llround(receiver.position[0] / app.options.model.spacing));
        require(std::abs(receiver.position[1] - surveyGrid.z[std::size_t(surveyGrid.nx) + ix]) < 1e-6,
                "Acoustic surface receiver did not follow the first terrain row");
    }
    capture("surface-receivers.ppm");
    app.chrome.category = 2;
    app.groupsExpanded = true;
    ++app.groupEpoch;
    render();
    render();
    ImGui::SetScrollY(ImGui::FindWindowByID(app.fieldsWindowId), 0);
    render();
    const auto selectCombo = [&](ImRect rect, int index) {
        click(rect);
        render();
        auto &stack = ImGui::GetCurrentContext()->OpenPopupStack;
        require(!stack.empty() && stack.back().Window, "Capacity device combo did not open");
        const auto *popup = stack.back().Window;
        const ImVec2 at{popup->Pos.x + ImGui::GetStyle().WindowPadding.x + 24,
                        popup->Pos.y + ImGui::GetStyle().WindowPadding.y +
                            (index + .5f) * ImGui::GetTextLineHeightWithSpacing()};
        click({{at.x - 2, at.y - 2}, {at.x + 2, at.y + 2}});
        render();
    };
    if (Seis3D::Curvilinear::queryBackend3D(BackendKind3D::Cuda).available) {
        app.nextCapacityRefresh = 1e100;
        selectCombo(app.backendRect, 1);
        require(app.backend == 1 && app.capacity.freeDeviceMiB > 0 && app.nextCapacityRefresh < 1e50,
                "CUDA dropdown did not immediately refresh capacity/VRAM");
        const double fp32Ram = app.capacity.estimatedRamMiB;
        app.nextCapacityRefresh = 1e100;
        selectCombo(app.precisionRect, 0);
        require(app.precision == 0 && app.capacity.estimatedRamMiB > fp32Ram &&
                    app.nextCapacityRefresh < 1e50,
                "Precision dropdown did not immediately refresh capacity");
    }
    // Larger controls can put this field below the viewport. Scroll the real
    // parameter child before clicking rather than targeting a clipped item.
    revealField(app.autoBudgetRect);
    app.nextCapacityRefresh = 1e100;
    click(app.autoBudgetRect);
    render();
    require(app.options.memoryMiB > 0 && app.nextCapacityRefresh < 1e50,
            "Manual RAM checkbox did not refresh capacity immediately");
    click(app.autoBudgetRect);
    render();
    require(app.options.memoryMiB == 0, "Automatic RAM checkbox did not restore hardware budget");
    app.undoEdits();
    if (auto *fields = ImGui::FindWindowByID(app.fieldsWindowId))
        ImGui::SetScrollY(fields, 0);
    render();
    render();
    app.nextCapacityRefresh = 1e100;
    selectCombo(app.propagatorRect, 1);
    require(app.options.propagator == Propagator::CurvilinearDrp &&
                app.simulation.options().propagator == Propagator::P1 && app.dirty() &&
                app.nextCapacityRefresh < 1e50,
            "DRP selection did not stage independently / immediately refresh capacity");
    require(app.apply() && app.simulation.advance(150, app.error), "DRP UI rebuild/advance failed");
    app.accept(app.publish());
    render();
    render();
    capture("drp-workspace.ppm");
    selectCombo(app.propagatorRect, 0);
    require(app.apply() && app.simulation.options().propagator == Propagator::P1,
            "Returning from experimental DRP did not restore independent P1 solver");
    app.accept(app.publish());
    render();
    const auto folder = SeisWave::Ui::outputRoot() / "validation/acoustic-2d";
    app.chrome.category = 1;
    render();
    render();
    const auto beforeAutomatic = app.simulation.options();
    const int desired = app.options.model.receiverTargetCount;
    app.options.model.nx += 40;
    app.options.model.relief = 5;
    render();
    render();
    const auto aligned = app.options;
    for (int i = 0; i < 5; ++i)
        render();
    require(app.options == aligned && app.options.model.receiverTargetCount == desired &&
                app.options.model.receiverDepth == 1 && app.simulation.options() == beforeAutomatic,
            "Automatic acoustic surface line drifted or modified active data");
    require(app.apply() && app.simulation.advance(150, app.error),
            "Automatic acoustic resize/rebuild failed");
    app.accept(app.publish());
    render();
    render();
    for (std::size_t r = 0; r < app.simulation.receivers().size(); ++r) {
        const auto &p = app.simulation.options().model;
        const auto q = std::size_t(p.nx) + p.receiverStart + int(r) * p.receiverStride;
        require(std::abs(app.simulation.receivers()[r].position[1] - app.simulation.grid().z[q]) < 1e-4,
                "Automatic acoustic resize missed the mapped first row");
    }
    io.AddMousePosEvent(-100, -100);
    render();
    render();
    capture("auto-surface.ppm");
    if (auto *fields = ImGui::FindWindowByID(app.fieldsWindowId))
        ImGui::SetScrollY(fields, 0);
    render();
    click(app.autoReceiverRect);
    require(!app.options.model.autoSurfaceReceivers, "Acoustic auto toggle did not disable");
    app.options.model.receiverStart = 3;
    app.options.model.receiverCount = 5;
    app.options.model.receiverStride = 4;
    app.options.model.receiverDepth = 3;
    require(app.apply(), "Acoustic manual rebuild failed after auto mode");
    require(app.simulation.receivers().size() == 5 && app.simulation.options().model.receiverDepth == 3,
            "Acoustic auto mode overwrote the manual line");
    std::filesystem::create_directories(folder);
    std::ofstream(folder / "ui-interactions.txt")
        << "PASS: Space pause/run + background progression; N single step + pending input apply; C "
           "fields/timer; "
           "Q/Shift-Q + W/E + G/B; R fit + F layout + F1/Esc help; text/menu/Ctrl shortcut suppression; "
           "completed-run restart; equal physical X/Z + full target coverage in 3 sizes; terrain "
           "source/receiver markers; failed "
           "rebuild preserves session; shared fixed footer reset/undo/defaults + scroll anchoring; square "
           "flat grid cells; combined model-wavefield; shared pressure RecordView/AGC raw-data invariance; "
           "real absorber wavefield + same target scale + original amber / adjustable opacity + no solver "
           "mutation; shared layer editor / acoustic Vs=0 / 8 terrain presets at 1,2,4 km / undo / "
           "capacity; actual CPU/CUDA and FP32/FP64 dropdown refresh / auto-manual RAM switch; uniform "
           "surface receiver button / applied geometry; actual DRP selector / staged isolation / capacity "
           "refresh / apply-run / return P1; automatic surface resize / stable desired count / mapped "
           "coordinates / manual override\n";
    std::cout
        << "Acoustic interactions PASS: keyboard / focus guards / equal aspect / acquisition overlays\n";
}
} // namespace
int runWorkbench(bool smoke) {
    SeisWave::Ui::AppRuntime runtime;
    std::string error;
    if (!runtime.open("SeisWave Lab | 2D Forward / Acoustic P", smoke, error, true))
        throw std::runtime_error(error);
    Workspace app;
    if (smoke) {
        app.options = defaultOptions();
        SeisWave::Ui::applyInitialModel2D(app.options.model, true);
        app.options.model.autoSurfaceReceivers = true;
        app.syncSurfaceReceivers();
        app.backend = 0;
        app.precision = 1;
        applyTheme3D(11);
        setLanguage3D(1);
        glfwSetWindowSize(runtime.window(), 1600, 1000);
    }
    if (!app.apply())
        app.status =
            l("Edit parameters and apply to build the acoustic model.", "请修改参数后应用，构建声学模型。");
    if (smoke) {
        if (!app.simulation.initialized())
            throw std::runtime_error(app.error);
        app.running = true;
    }
    const int panelMode2 = SeisWave::Ui::parameterDisplayMode(2);
    const int panelMode3 = SeisWave::Ui::parameterDisplayMode(3);
    int complete = -1;
    auto start = std::chrono::steady_clock::now();
    const auto render = [&] {
        runtime.begin();
        app.dock();
        app.controls();
        app.modelEditor.modelDialogs();
        app.summary();
        app.shortcuts();
        if (app.fit)
            app.fitted.fill(false);
        app.wavefield();
        app.records();
        app.fit = false;
        runtime.present();
        if (smoke && ImGui::GetCurrentContext()->ErrorCountCurrentFrame)
            throw std::runtime_error("Acoustic UI style / font / window stack validation failed");
    };
    for (int frame = 0; !glfwWindowShouldClose(runtime.window()); ++frame) {
        app.poll();
        if (app.simulation.initialized() && app.shown.step >= app.simulation.totalSteps()) {
            app.running = false;
            if (complete < 0)
                complete = frame;
        }
        render();
        app.options.backend = static_cast<BackendKind3D>(app.backend);
        app.options.model.precision = app.precision ? Precision::Float32 : Precision::Float64;
        if (app.simulation.initialized() && app.options != app.simulation.options()) {
            app.running = false;
            app.worker.cancel();
        }
        if (app.home || app.navigation.requested) {
            app.running = false;
            app.wait();
            if (!smoke)
                acousticDraft = app.options;
            return app.home ? SeisWave::Ui::returnHome : app.navigation.requested;
        }
        app.queue();
        if (smoke) {
            if (!app.error.empty())
                throw std::runtime_error(app.error);
            if (std::chrono::steady_clock::now() - start > std::chrono::seconds(90))
                throw std::runtime_error("Acoustic UI timeout");
            if (complete >= 0) {
                int offset = frame - complete;
                if (offset == 6 || offset == 12 || offset == 18) {
                    if (app.recordView.plotBounds[3] - app.recordView.plotBounds[1] < 100)
                        throw std::runtime_error("Pressure record chart too compressed");
                    if (!app.recordView.plotVisible)
                        throw std::runtime_error("Pressure record chart hidden by toolbar in small layout");
                    if (app.recordPlotRect.Max.y > app.recordsRect.Max.y - 2)
                        throw std::runtime_error("Acoustic record axes overflow the available pane");
                    if (app.gridCellAspectError > .001)
                        throw std::runtime_error(
                            "Flat acoustic grid cells are not square after display sampling");
                    for (const auto rect : {app.sessionActions.rebuildRect, app.sessionActions.undoRect,
                                            app.sessionActions.defaultsRect})
                        if (rect.Max.y >
                            ImGui::GetMainViewport()->Pos.y + ImGui::GetMainViewport()->Size.y - 2)
                            throw std::runtime_error("Acoustic fixed session buttons clipped");
                    if (!app.domainVisible || app.aspectError > .001 ||
                        app.markerReceivers != static_cast<int>(app.simulation.receivers().size()))
                        throw std::runtime_error(
                            "Acoustic physical aspect / target bounds / acquisition markers failed");
                    for (auto rect : {app.waveRect, app.recordsRect})
                        if (rect.GetWidth() < 200 || rect.GetHeight() < 120)
                            throw std::runtime_error("Acoustic display clipped");
                    int w, h;
                    glfwGetFramebufferSize(runtime.window(), &w, &h);
                    if (!SeisWave::Ui::capture(SeisWave::Ui::outputRoot() / ("screenshots/acoustic/forward-" +
                                                                             std::to_string(offset) + ".ppm"),
                                               w, h))
                        throw std::runtime_error("Acoustic screenshot failed");
                    if (offset == 6)
                        glfwSetWindowSize(runtime.window(), 1280, 800);
                    if (offset == 12)
                        glfwSetWindowSize(runtime.window(), 960, 640);
                    if (offset == 18) {
                        app.wait();
                        app.clear();
                        if (!app.error.empty())
                            throw std::runtime_error(app.error);
                        if (app.simulation.step() != 0 || app.simulation.diagnostics().totalStepMs != 0)
                            throw std::runtime_error("Acoustic clear/timer reset failed");
                        app.singleStep();
                        if (app.simulation.step() != 1 || app.simulation.diagnostics().totalStepMs <= 0)
                            throw std::runtime_error("Acoustic single-step failed");
                        if (!app.apply() || app.simulation.step() != 0)
                            throw std::runtime_error("Unchanged acoustic rebuild did not reset");
                        for (auto target :
                             {SeisWave::Ui::Module2D::Elastic, SeisWave::Ui::Module2D::Viscoelastic}) {
                            SeisWave::Ui::ModelTransfer2D transfer{SeisWave::Ui::Module2D::AcousticForward,
                                                                   app.options.model, app.options.compute,
                                                                   app.options.backend};
                            if (!SeisWave::Ui::prepareElasticTransfer(transfer, target, app.error))
                                throw std::runtime_error(app.error);
                            SeisWave::Ui::modelMailbox2D().post(target, transfer);
                            auto read = SeisWave::Ui::modelMailbox2D().take(target);
                            if (!read || read->model.attenuation.enabled !=
                                             (target == SeisWave::Ui::Module2D::Viscoelastic))
                                throw std::runtime_error("Explicit model copy failed");
                        }
                        std::cout << "Acoustic UI PASS: live async pressure / pause-clear-step data path / "
                                     "three layouts / elastic-viscoelastic transfer\n";
                        glfwSetWindowSize(runtime.window(), 1600, 1000);
                        app.chrome.category = 2;
                        app.chrome.floating = true;
                        app.chrome.center = true;
                        app.layout = true;
                    }
                }
                if (offset == 24 || offset == 30) {
                    auto *panel = ImGui::FindWindowByName("Controls###AcousticControls");
                    if (!panel || (offset == 24 ? panel->DockId != 0 : panel->DockId == 0))
                        throw std::runtime_error("Independent parameter floating/redocking failed");
                    if (SeisWave::Ui::parameterDisplayMode(2) != panelMode2 ||
                        SeisWave::Ui::parameterDisplayMode(3) != panelMode3)
                        throw std::runtime_error("Workspace changed another module's panel preference");
                    if (offset == 24) {
                        int w, h;
                        glfwGetFramebufferSize(runtime.window(), &w, &h);
                        if (!SeisWave::Ui::capture(SeisWave::Ui::outputRoot() /
                                                       "screenshots/acoustic/floating-parameters.ppm",
                                                   w, h))
                            throw std::runtime_error("Floating parameter capture failed");
                        app.chrome.floating = false;
                        app.layout = true;
                    } else {
                        std::cout << "Workspace panel PASS: local floating / redocking / shared preferences "
                                     "unchanged\n";
                        verifyInteractions(app, runtime, render);
                        return 0;
                    }
                }
            }
        }
    }
    if (!smoke)
        acousticDraft = app.options;
    return 0;
}
int validateCapacityWorkflow() {
    SeisWave::Ui::AppRuntime runtime;
    std::string error;
    if (!runtime.open("Acoustic automatic capacity validation", true, error, true))
        throw std::runtime_error(error);
    setLanguage3D(1);
    applyTheme3D(11);
    glfwSetWindowSize(runtime.window(), 1600, 1000);
    Workspace app;
    app.options = defaultOptions();
    app.backend = 0;
    app.precision = 1;
    if (!app.apply())
        throw std::runtime_error(app.error);
    app.refreshCapacity();
    auto &p = app.options.model;
    p.nx = 5000;
    p.nz = 2000;
    p.spacing = 2;
    p.frequency = 15;
    p.duration = .03;
    p.maxSteps = 8;
    p.sourceX = 2500;
    p.sourceZ = 8;
    p.receiverStart = 1;
    p.receiverCount = 128;
    p.receiverStride = 30;
    p.receiverDepth = 2;
    p.layered = true;
    p.layers = {{50, 500, 0, 1600}, {0, 2000, 0, 2000}};
    p.importedSurface = false;
    p.relief = 0;
    app.options.memoryMiB = 0;
    app.backend = 1;
    app.precision = 1;
    app.nextCapacityRefresh = 1e100;
    app.refreshCapacity();
    const auto before = app.capacity;
    if (!before.withinBudget)
        throw std::runtime_error(
            "Current resources cannot build 5000x2000: free RAM=" + std::to_string(before.freeRamMiB) +
            " MiB, required=" + std::to_string(before.estimatedRamMiB) +
            " MiB, budget=" + std::to_string(before.ramBudgetMiB) + " MiB");
    if (!app.apply() || !app.simulation.advance(8, error))
        throw std::runtime_error(app.error + error);
    app.accept(app.publish());
    const auto field = app.simulation.pressureSnapshot();
    double peak = 0;
    for (double value : field) {
        if (!std::isfinite(value))
            throw std::runtime_error("Nonfinite large GUI field");
        peak = std::max(peak, std::abs(value));
    }
    if (peak <= 0 || app.simulation.grid().nx != 5000 || app.simulation.grid().nz != 2000 ||
        app.simulation.options().memoryMiB != 0)
        throw std::runtime_error("Large automatic GUI session did not retain requested grid / policy");
    app.chrome.category = 0;
    app.status = "CUDA FP32 | 5000 x 2000 | 8 steps | automatic RAM";
    app.fit = true;
    for (int i = 0; i < 6; ++i) {
        runtime.begin();
        app.dock();
        app.controls();
        app.summary();
        app.wavefield();
        app.records();
        runtime.present();
        app.fit = false;
    }
    const auto folder = SeisWave::Ui::outputRoot() / "validation/acoustic-capacity-auto-2026-10-31";
    std::filesystem::create_directories(folder);
    int w{}, h{};
    glfwGetFramebufferSize(runtime.window(), &w, &h);
    if (!SeisWave::Ui::capture(folder / "cuda-5000x2000.ppm", w, h))
        throw std::runtime_error("Large GUI capture failed");
    const auto &diagnostics = app.simulation.diagnostics();
    std::ofstream(folder / "ui-capacity.txt")
        << std::setprecision(17)
        << "PASS: real Workspace apply rebuilt CPU small -> CUDA FP32 5000x2000 with automatic RAM; eight "
           "finite steps\n"
        << "free_ram_before_MiB=" << before.freeRamMiB << "\nfree_vram_before_MiB=" << before.freeDeviceMiB
        << "\nRAM_estimate_MiB=" << before.estimatedRamMiB
        << "\nauto_RAM_budget_MiB=" << diagnostics.ramBudgetMiB
        << "\ncompute_nx=" << app.simulation.grid().computeNx
        << "\ncompute_nz=" << app.simulation.grid().computeNz
        << "\nresident_host_bytes=" << diagnostics.residentHostArrayBytes
        << "\nresident_device_bytes=" << diagnostics.residentDeviceBytes << "\npeak_pressure_Pa=" << peak
        << "\nShort capacity/UI verification; not a full-duration accuracy or performance benchmark.\n";
    std::cout << "Acoustic automatic-capacity GUI PASS: CUDA FP32 5000x2000; eight finite steps\n";
    return 0;
}
} // namespace Seis2D::Acoustic::Ui
