#include "ImagingWorkbench2D.hpp"
#include "AppRuntime.hpp"
#include "CommandStrip2D.hpp"
#include "MenuWidgets.hpp"
#include "ModuleNavigation2D.hpp"
#include "ParameterPanel2D.hpp"
#include "Presentation3D.hpp"
#include "RecordPalettes3D.hpp"
#include "Seis2D/Imaging2D.hpp"
#include "Seis2D/ModelIo2D.hpp"
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
#include <future>
#include <iostream>
#include <mutex>
#include <optional>
#include <stdexcept>
#ifdef _WIN32
// clang-format off
#include <windows.h>
#include <commdlg.h>
// clang-format on
#endif
namespace Seis2D::Imaging::Ui {
using namespace Seis3D::Curvilinear::Ui;
namespace RecordPalettes = Seis3D::Curvilinear::RecordPalettes;
namespace {
std::optional<Options> rtmDraft;
const char *l(const char *en, const char *zh) { return language3D() ? zh : en; }
bool intField(const char *label, int *value) {
    if (label[0] == '#')
        return ImGui::InputInt(label, value, 0, 0);
    ImGui::TextUnformatted(label);
    ImGui::SetNextItemWidth(-1);
    auto id = std::string("##") + label;
    return ImGui::InputInt(id.c_str(), value, 0, 0);
}
bool numberField(const char *label, double *value, double step = 0, double fast = 0,
                 const char *format = "%.5g") {
    if (label[0] == '#')
        return ImGui::InputDouble(label, value, step, fast, format);
    ImGui::TextUnformatted(label);
    ImGui::SetNextItemWidth(-1);
    auto id = std::string("##") + label;
    return ImGui::InputDouble(id.c_str(), value, step, fast, format);
}
std::filesystem::path utf8Path(const char *path) {
    return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t *>(path)));
}
bool browse(std::array<char, 2048> &path, bool surface) {
#ifdef _WIN32
    std::array<wchar_t, 32768> name{};
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.lpstrFile = name.data();
    dialog.nMaxFile = static_cast<DWORD>(name.size());
    dialog.lpstrFilter =
        surface ? L"Elevation CSV\0*.csv;*.txt\0All files\0*.*\0" : L"2D model VTS\0*.vts\0All files\0*.*\0";
    dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (GetOpenFileNameW(&dialog)) {
        auto text = std::filesystem::path(name.data()).u8string();
        if (text.size() >= path.size())
            throw std::runtime_error("Selected path is too long.");
        std::snprintf(path.data(), path.size(), "%s", reinterpret_cast<const char *>(text.c_str()));
        return true;
    }
#else
    (void)path;
    (void)surface;
#endif
    return false;
}
const char *parameters = "Parameters###ImagingParameters";
const char *imageWindow = "RTM###ImagingImage";
const char *illuminationWindow = "Illumination###ImagingIllumination";
const char *modelWindow = "Model###ImagingModel";
const char *recordWindow = "Pressure records###ImagingRecords";
struct Job {
    struct Completion {
        bool ok{};
        bool preparation{};
        Result result;
        std::string error;
    };
    std::future<Completion> future;
    std::atomic_bool cancel{};
    std::atomic<double> progress{};
    std::mutex mutex;
    std::string phase;
    std::optional<Preview> latestPreview;
    std::chrono::steady_clock::time_point start;
    ~Job() {
        cancel = true;
        if (future.valid())
            future.wait();
    }
    bool busy() const { return future.valid(); }
    void launch(Options o, bool preparation = false) {
        cancel = false;
        progress = 0;
        start = std::chrono::steady_clock::now();
        {
            std::lock_guard lock(mutex);
            phase =
                preparation ? "Rebuilding model / surface receivers" : "Preparing geometry / memory guard";
            latestPreview.reset();
        }
        future = std::async(std::launch::async, [this, o, preparation] {
            Completion c;
            c.preparation = preparation;
            if (preparation) {
                c.ok = prepareModel(o, c.result, c.error, &cancel);
                if (c.ok)
                    progress = 1;
                return c;
            }
            c.ok = run(
                o, c.result, c.error, &cancel,
                [this](double value, const std::string &text) {
                    progress = value;
                    std::lock_guard lock(mutex);
                    phase = text;
                },
                [this](Preview frame) {
                    std::lock_guard lock(mutex);
                    latestPreview = std::move(frame);
                });
            return c;
        });
    }
};
struct Workspace {
    Options options = rtmDraft ? *rtmDraft : benchmarkOptions();
    Result result, live, prepared;
    bool modelReady{}, runAfterPrepare{}, focusModel{}, focusImage{};
    int surveyNx{-1}, surveyNz{-1};
    Preview preview;
    bool liveReady{};
    std::vector<double> liveLog;
    Job job;
    bool showingLive() const { return job.busy() && liveReady; }
    const Result &view() const { return showingLive() ? live : result; }
    const Result &modelView() const { return modelReady ? prepared : view(); }
    bool dirty() const { return !modelReady || options != prepared.options; }
    const std::vector<double> &imageValues() const {
        if (showingLive())
            return preview.phase < 2 ? preview.pressure : (normalized ? live.normalizedImage : live.image);
        return normalized ? result.normalizedImage : result.image;
    }
    const std::vector<double> &illuminationValues() const {
        return showingLive() ? liveLog : logIllumination;
    }

    SeisWave::Ui::ModuleNavigation2D navigation{SeisWave::Ui::Module2D::AcousticRtm};
    SeisWave::Ui::WorkspaceSidebar2D chrome;
    bool showSummary{true};
    std::string error, status;
    bool normalized{true}, showBackground{}, layout{true}, home{}, fit{true};
    bool ready{};
    std::array<bool, 5> fitted{};
    int imagePalette{1}, recordPalette{}, backend{}, precision{}, lastCompact{-1};
    float clip{.65f};
    std::array<char, 2048> modelPath{}, surfacePath{};
    std::vector<double> logIllumination, recordView;
    std::array<ImRect, 5> windows{};
    ImRect runRect, rebuildRect, surfaceReceiverRect, autoReceiverRect, nxRect;
    SeisWave::Ui::ParameterActions2D sessionActions;
    Workspace() {
        if (auto input = SeisWave::Ui::modelMailbox2D().take(SeisWave::Ui::Module2D::AcousticRtm)) {
            options.model = std::move(input->model);
            options.model.attenuation.enabled = false;
            options.compute = input->compute;
            options.backend = input->backend;
            options.backgroundVp = 0;
            options.firstShot = std::max(1, options.model.nx / 10);
            options.lastShot = options.model.nx - 1 - options.firstShot;
            options.receiverCount = options.model.receiverCount;
            options.receiverTargetCount = options.model.autoSurfaceReceivers
                                              ? options.model.receiverTargetCount
                                              : options.model.receiverCount;
            options.receiverStart = options.model.receiverStart;
            options.receiverStride = options.model.receiverStride;
            options.receiverDepth = std::max(1, options.model.receiverDepth);
        }
        backend = static_cast<int>(options.backend);
        precision = options.model.precision == Precision::Float32 ? 1 : 0;
        if (!rtmDraft)
            options.autoSurfaceReceivers = true;
        synchronizeSurvey();
    }
    void synchronizeSurvey() {
        auto &p = options.model;
        p.autoSurfaceReceivers = options.autoSurfaceReceivers;
        p.receiverTargetCount = options.receiverTargetCount;
        if (p.nx >= 16 && p.nx <= 4096 && p.nz >= 16 && p.nz <= 4096 &&
            (surveyNx != p.nx || surveyNz != p.nz)) {
            options.firstShot = std::clamp(options.firstShot, 1, p.nx - 2);
            options.lastShot = std::clamp(options.lastShot, options.firstShot, p.nx - 2);
            options.sourceDepth = std::clamp(options.sourceDepth, 1, p.nz - 2);
            surveyNx = p.nx;
            surveyNz = p.nz;
        }
        if (options.autoSurfaceReceivers)
            alignSurfaceReceivers(options);
        else {
            p.receiverStart = options.receiverStart;
            p.receiverCount = options.receiverCount;
            if (options.receiverCount == 0 && options.receiverStride > 0 && options.receiverStart >= 0 &&
                options.receiverStart <= p.nx - 2)
                p.receiverCount = 1 + (p.nx - 2 - options.receiverStart) / options.receiverStride;
            p.receiverStride = options.receiverStride;
            p.receiverDepth = options.receiverDepth;
        }
    }
    void synchronizeInputs() {
        options.backend = static_cast<BackendKind3D>(backend);
        options.model.precision = precision ? Precision::Float32 : Precision::Float64;
        synchronizeSurvey();
    }
    void rebuild(bool thenRun = false) {
        if (job.busy())
            return;
        synchronizeInputs();
        error.clear();
        runAfterPrepare = thenRun;
        liveReady = false;
        job.launch(options, true);
        status = l("Rebuilding model and receiver coordinates...", "正在重建模型和检波器坐标……");
    }
    void defaults() {
        options = benchmarkOptions();
        options.autoSurfaceReceivers = true;
        backend = 0;
        precision = 1;
        synchronizeSurvey();
    }
    void undo() {
        if (modelReady)
            options = prepared.options;
        else if (ready)
            options = result.options;
        else
            return;
        backend = static_cast<int>(options.backend);
        precision = options.model.precision == Precision::Float32 ? 1 : 0;
        synchronizeSurvey();
        error.clear();
    }
    void collect() {
        std::optional<Preview> update;
        {
            std::lock_guard lock(job.mutex);
            update = std::move(job.latestPreview);
            job.latestPreview.reset();
        }
        if (update) {
            preview = std::move(*update);
            live.nx = preview.nx;
            live.nz = preview.nz;
            live.x = preview.x;
            live.z = preview.z;
            live.vp = preview.vp;
            live.backgroundVp = preview.backgroundVp;
            live.image = preview.image;
            live.illumination = preview.illumination;
            const double maximum =
                std::max(1e-100, *std::max_element(live.illumination.begin(), live.illumination.end()));
            live.normalizedImage.resize(live.image.size());
            liveLog.resize(live.image.size());
            for (std::size_t i = 0; i < live.image.size(); ++i) {
                live.normalizedImage[i] =
                    live.image[i] / (live.illumination[i] + options.illuminationFloor * maximum);
                liveLog[i] = std::log10(std::max(1e-8, live.illumination[i] / maximum));
            }
            if (!liveReady)
                fitted.fill(false);
            liveReady = true;
            // Present at least the newest progress frame before committing completion.
            return;
        }
        if (job.busy() && job.future.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready) {
            auto done = job.future.get();
            if (done.preparation) {
                if (done.ok) {
                    prepared = std::move(done.result);
                    options = prepared.options;
                    modelReady = true;
                    error.clear();
                    fit = true;
                    fitted.fill(false);
                    focusModel = !runAfterPrepare;
                    {
                        std::lock_guard lock(job.mutex);
                        job.phase = "Model ready / run RTM";
                    }
                    status = l("Model rebuilt; surface receivers applied. Run RTM to update images.",
                               "模型已重建，检波器设置已生效；运行RTM后更新成像结果。");
                    const bool next = runAfterPrepare && !job.cancel.load();
                    runAfterPrepare = false;
                    if (next)
                        start();
                } else {
                    error = done.error;
                    runAfterPrepare = false;
                }
                return;
            }
            if (done.ok) {
                result = std::move(done.result);
                ready = true;
                fitted.fill(false);
                fit = true;
                error.clear();
                status = l("Completed: raw image, illumination and final-shot pressure residual.",
                           "已完成：原始图像、照明与最后一炮压力残差。");
                double max = *std::max_element(result.illumination.begin(), result.illumination.end());
                logIllumination.resize(result.illumination.size());
                for (std::size_t q = 0; q < logIllumination.size(); ++q)
                    logIllumination[q] = std::log10(std::max(result.illumination[q] / max, 1e-8));
                // ImPlot heatmap row 0 maps to bounds_max.y. Reverse rows for positive-down time.
                recordView.resize(result.records.size());
                for (int n = 0; n < result.steps; ++n)
                    std::copy_n(result.records.begin() + std::size_t(n) * result.receivers, result.receivers,
                                recordView.begin() + std::size_t(result.steps - 1 - n) * result.receivers);
            } else
                error = done.error;
            liveReady = false;
            fitted.fill(false);
            fit = true;
        }
    }
    void start() {
        if (job.busy())
            return;
        synchronizeInputs();
        if (!modelReady || dirty()) {
            rebuild(true);
            return;
        }
        error.clear();
        liveReady = false;
        live = {};
        preview = {};
        liveLog.clear();
        options.backend = static_cast<BackendKind3D>(backend);
        options.model.precision = precision ? Precision::Float32 : Precision::Float64;
        focusImage = true;
        job.launch(options);
    }
    void fileControls() {
        ImGui::TextWrapped("%s", l("Load a versioned 2D model VTS, or transfer the current model from the 2D "
                                   "simulation workspace.",
                                   "载入二维模型 VTS，或在二维正演工作区点击“二维成像”传入当前待应用模型。"));
        ImGui::SetNextItemWidth(-1);
        ImGui::InputText("##imagingModelPath", modelPath.data(), modelPath.size());
        if (ImGui::Button(l("Browse model", "选择模型")))
            browse(modelPath, false);
        ImGui::SameLine();
        if (ImGui::Button(l("Load model VTS", "载入模型 VTS"))) {
            SeisWave::Ui::ModelTransfer2D input;
            bool acoustic = false;
            if (SeisWave::Ui::loadInputModel2D(utf8Path(modelPath.data()), input, acoustic, error)) {
                options.model = std::move(input.model);
                options.compute = input.compute;
                options.compute.cudaDevice = -1;
                options.backend = input.backend;
                backend = static_cast<int>(input.backend);
                precision = options.model.precision == Precision::Float32 ? 1 : 0;
                options.firstShot = std::max(1, options.model.nx / 10);
                options.lastShot = options.model.nx - 1 - options.firstShot;
                options.receiverCount = options.model.receiverCount;
                options.receiverStart = options.model.receiverStart;
                options.receiverStride = options.model.receiverStride;
                options.receiverDepth = std::max(1, options.model.receiverDepth);
                options.receiverTargetCount = options.model.autoSurfaceReceivers
                                                  ? options.model.receiverTargetCount
                                                  : options.model.receiverCount;
                synchronizeSurvey();
                status = l("Model parameters loaded. RTM creates its own synthetic pressure data.",
                           "已载入模型参数；RTM 仍独立生成合成压力数据。");
            }
        }
        ImGui::SetNextItemWidth(-1);
        ImGui::InputText("##imagingSurfacePath", surfacePath.data(), surfacePath.size());
        if (ImGui::Button(l("Browse profile", "选择高程")))
            browse(surfacePath, true);
        ImGui::SameLine();
        if (ImGui::Button(l("Load elevation CSV", "载入高程 CSV"))) {
            SurfaceSamples samples;
            if (readSurfaceProfile(utf8Path(surfacePath.data()), samples, error)) {
                double sum = 0;
                for (auto &p : samples) {
                    sum += p[1];
                }
                options.model.surfaceDatum = sum / samples.size();
                options.model.surfaceSamples = std::move(samples);
                options.model.importedSurface = true;
                status =
                    l("Profile loaded; X must cover the model span.", "高程已载入；X 坐标须覆盖模型全宽。");
            }
        }
    }
    void controls() {
        const char *title = l("Parameters and run###ImagingParameters", "参数与运行###ImagingParameters");
        chrome.prepare(title);
        ImGui::Begin(title, nullptr, chrome.floating ? ImGuiWindowFlags_NoDocking : 0);
        chrome.navigation();
        ImGui::Separator();
        windows[0] = {ImGui::GetWindowPos(),
                      {ImGui::GetWindowPos().x + ImGui::GetWindowSize().x,
                       ImGui::GetWindowPos().y + ImGui::GetWindowSize().y}};
        SeisWave::Ui::ParameterSession2D session;
        session.ready = modelReady || ready;
        session.dirty = dirty();
        session.error = error;
        session.frequency = modelReady ? prepared.options.model.frequency : options.model.frequency;
        session.dt = modelReady ? prepared.dt : 0;
        ImGui::BeginChild("##RTMParameterFields",
                          {0, std::max(1.f, ImGui::GetContentRegionAvail().y - session.height())});
        ImGui::PushItemWidth(-ImGui::GetFontSize() * 7.5f);
        ImGui::TextDisabled("%s", l("Acoustic RTM | pressure imaging", "声学 RTM · 压力成像"));
        if (!error.empty())
            ImGui::TextWrapped("%s", error.c_str());
        if (!status.empty())
            ImGui::TextWrapped("%s", status.c_str());
        ImGui::BeginDisabled(job.busy());
        if (chrome.category == 0 &&
            ImGui::CollapsingHeader(l("Model and terrain", "模型与地形"), ImGuiTreeNodeFlags_DefaultOpen)) {
            if (ImGui::Button(l("Verified layered benchmark", "恢复分层验证模型"))) {
                defaults();
                status = l("Benchmark restored; existing results retained until a successful run.",
                           "已恢复验证模型；成功重算后才替换现有结果。");
            }
            intField("Nx", &options.model.nx);
            nxRect = {ImGui::GetItemRectMin(), ImGui::GetItemRectMax()};
            intField("Nz", &options.model.nz);
            numberField(l("Spacing (m)", "间距 (m)"), &options.model.spacing, 0, 0, "%.2f");
            ImGui::Checkbox(l("Layered model", "分层模型"), &options.model.layered);
            if (options.model.layered) {
                if (ImGui::BeginTable("##layersRTM", 3,
                                      ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_Borders)) {
                    ImGui::TableSetupColumn(l("H (m)", "厚度 m"));
                    ImGui::TableSetupColumn("Vp m/s");
                    ImGui::TableSetupColumn("ρ kg/m³");
                    ImGui::TableHeadersRow();
                    for (int j = 0; j < static_cast<int>(options.model.layers.size()); ++j) {
                        ImGui::PushID(j);
                        auto &v = options.model.layers[j];
                        for (auto [id, value] :
                             {std::pair{"##h", &v.thickness}, {"##vp", &v.vp}, {"##rho", &v.density}}) {
                            ImGui::TableNextColumn();
                            ImGui::SetNextItemWidth(-1);
                            numberField(id, value, 0, 0, "%.1f");
                        }
                        ImGui::PopID();
                    }
                    ImGui::EndTable();
                }
                if (options.model.layers.size() < 16 && ImGui::Button(l("Add layer", "增加地层"))) {
                    if (!options.model.layers.empty() && options.model.layers.back().thickness == 0)
                        options.model.layers.back().thickness = 100;
                    options.model.layers.push_back({0, 2500, 1200, 2200});
                }
                ImGui::SameLine();
                if (options.model.layers.size() > 1 && ImGui::Button(l("Remove last", "删除末层")))
                    options.model.layers.pop_back();
            } else {
                numberField("Vp (m/s)", &options.model.vp);
                numberField(l("Density (kg/m3)", "密度 (kg/m3)"), &options.model.density);
            }
            ImGui::Checkbox(l("Imported terrain", "使用外部高程"), &options.model.importedSurface);
            numberField(l("Relief (m)", "起伏 (m)"), &options.model.relief);
            numberField(l("Wavelength (m)", "地形波长 (m)"), &options.model.wavelength);
            numberField(l("Flatten depth; 0 auto", "展平深度；0 自动"), &options.model.flattenDepth);
            numberField(l("Elevation datum (m)", "高程基准 (m)"), &options.model.surfaceDatum);
            ImGui::Checkbox(l("GSLS enabled (unsupported)", "GSLS 开关（成像暂不支持）"),
                            &options.model.attenuation.enabled);
            fileControls();
        }
        if (chrome.category == 1 && ImGui::CollapsingHeader(l("Background and survey", "背景与观测系统"),
                                                            ImGuiTreeNodeFlags_DefaultOpen)) {
            if (functionButton3D(l("Uniform surface receivers", "地表均匀布设"), FunctionRole3D::Model,
                                 {-1, 0})) {
                if (!options.autoSurfaceReceivers)
                    options.receiverTargetCount = options.receiverCount;
                options.autoSurfaceReceivers = true;
                synchronizeSurvey();
                status = l("Pressure receivers follow the first subsurface row across the target; current X "
                           "count quantized to uniform node spacing.",
                           "已按当前道数沿地表下首层均匀布设；重建或运行后生效。");
            }
            surfaceReceiverRect = {ImGui::GetItemRectMin(), ImGui::GetItemRectMax()};
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s",
                                  l("RTM pressure at the free surface is zero; use the first subsurface row.",
                                    "RTM声学自由面压力为零，因此使用地表下首层节点。"));
            if (ImGui::Checkbox(l("Auto surface receivers", "自动沿地表均匀布设"),
                                &options.autoSurfaceReceivers) &&
                options.autoSurfaceReceivers)
                options.receiverTargetCount = options.receiverCount;
            autoReceiverRect = {ImGui::GetItemRectMin(), ImGui::GetItemRectMax()};
            synchronizeSurvey();
            ImGui::TextWrapped(
                "%s",
                l("Auto mode controls start/stride/depth; edit count, or turn it off for a manual line. "
                  "Pressure uses row 1.",
                  "自动模式控制起点、间隔和深度，可修改道数；关闭后手动设置。声学接收点位于地表下首层。"));
            ImGui::BeginDisabled(options.autoSurfaceReceivers);
            intField(l("Receiver start X", "接收起点 X 索引"), &options.receiverStart);
            ImGui::EndDisabled();
            if (options.autoSurfaceReceivers) {
                intField(l("Requested receiver count", "期望接收点数"), &options.receiverTargetCount);
                ImGui::Text("%s %d", l("Actual count", "实际道数"), options.receiverCount);
            } else
                intField(l("Receiver count; 0 legacy auto", "接收点数；0 沿用旧间隔"),
                         &options.receiverCount);
            ImGui::BeginDisabled(options.autoSurfaceReceivers);
            intField(l("Receiver stride", "接收间隔索引"), &options.receiverStride);
            intField(l("Receiver depth index", "接收深度索引"), &options.receiverDepth);
            ImGui::EndDisabled();
            numberField(l("Background Vp; 0 smooth", "背景 Vp；0 平滑"), &options.backgroundVp);
            intField(l("Smoothing iterations", "平滑次数"), &options.smoothIterations);
            intField(l("Shots", "炮数"), &options.shots);
            intField(l("First X index", "首炮 X 索引"), &options.firstShot);
            intField(l("Last X index", "末炮 X 索引"), &options.lastShot);
            intField(l("Source depth index", "震源深度索引"), &options.sourceDepth);
            numberField(l("Frequency (Hz)", "主频 (Hz)"), &options.model.frequency);
            numberField(l("Duration (s)", "记录时长 (s)"), &options.model.duration, 0, 0, "%.3f");
        }
        if (chrome.category == 2 &&
            ImGui::CollapsingHeader(l("Device and checkpoint budget", "计算设备与检查点预算"),
                                    ImGuiTreeNodeFlags_DefaultOpen)) {
            SeisWave::Ui::choiceField2D(l("Backend", "计算核心"), &backend, "CPU / OpenMP\0CUDA\0");
            SeisWave::Ui::choiceField2D(l("Precision", "计算精度"), &precision, "FP64\0FP32\0");
            intField(l("CPU threads; 0 auto", "CPU 线程；0 自动"), &options.compute.cpuThreads);
            intField(l("CUDA ordinal; -1 auto", "CUDA 编号；-1 自动"), &options.compute.cudaDevice);
            numberField(l("RAM budget (MiB)", "内存预算 (MiB)"), &options.memoryMiB);
            intField(l("Checkpoint interval; 0 auto", "检查点间隔；0 自动"), &options.checkpointInterval);
            intField(l("External damping cells", "外扩阻尼层格数"), &options.absorberCells);
            ImGui::TextWrapped("%s",
                               l("Damping is separate from the elastic CFS-PML. Full-duration steps are "
                                 "used; the forward step cap is ignored.",
                                 "此处阻尼层独立于弹性 CFS-PML。按完整记录时长计算，不沿用正演步数截断。"));
        }
        ImGui::EndDisabled();
        if (chrome.category == 3 &&
            ImGui::CollapsingHeader(l("Display", "显示"), ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::Checkbox(l("Normalize by illumination", "照明归一化"), &normalized);
            ImGui::Checkbox(l("Show background Vp", "显示背景 Vp"), &showBackground);
            ImGui::SliderFloat(l("Image clip / peak", "图像限幅 / 峰值"), &clip, .01f, 1.f, "%.2f");
            SeisWave::Ui::choiceField2D(l("Image palette", "成像配色"), &imagePalette,
                                        RecordPalettes::wavePaletteNames);
            SeisWave::Ui::choiceField2D(l("Record palette", "记录配色"), &recordPalette,
                                        RecordPalettes::wavePaletteNames);
            if (ImGui::Button(l("Fit all plots", "适配全部图件")))
                fit = true;
        }
        if (chrome.category == 4 &&
            ImGui::CollapsingHeader(l("Export completed experiment", "导出已完成实验"),
                                    ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::BeginDisabled(!ready || job.busy());
            if (functionButton3D(l("Export experiment", "导出实验结果"), FunctionRole3D::Export, {-1, 0})) {
                auto destination =
                    SeisWave::Ui::outputRoot() / "imaging" /
                    ("experiment-" +
                     std::to_string(std::chrono::system_clock::now().time_since_epoch().count()));
                if (exportResult(result, destination, error)) {
                    auto s = destination.u8string();
                    status =
                        std::string(l("Exported: ", "已导出：")) + reinterpret_cast<const char *>(s.c_str());
                }
            }
            ImGui::EndDisabled();
            ImGui::TextWrapped(
                "%s", l("VTS images + exact true/background model CSV + last-shot pressure CSV + receiver "
                        "coordinates + experiment parameters. Existing experiments are preserved.",
                        "导出 VTS 图像、真实/背景模型 CSV、末炮压力 "
                        "CSV、接收坐标及实验参数，已有实验目录会保留。"));
        }
        ImGui::PopItemWidth();
        ImGui::EndChild();
        synchronizeInputs();
        session.dirty = dirty();
        session.error = error;
        ImGui::BeginDisabled(job.busy());
        sessionActions = SeisWave::Ui::parameterSession2D(session);
        if (sessionActions.rebuild)
            rebuild();
        if (sessionActions.undo)
            undo();
        if (sessionActions.defaults)
            defaults();
        ImGui::EndDisabled();
        ImGui::End();
    }
    ImPlotColormap cmap(int palette) {
        std::string name = "ImagingPalette" + std::to_string(palette);
        int id = ImPlot::GetColormapIndex(name.c_str());
        if (id < 0) {
            ImVec4 colors[5];
            const auto &p = RecordPalettes::wavePalette(palette);
            for (int i = 0; i < 5; ++i)
                colors[i] = {p[i][0], p[i][1], p[i][2], 1};
            id = ImPlot::AddColormap(name.c_str(), colors, 5, false);
        }
        return id;
    }
    void field(const char *title, const std::vector<double> &values, int palette, bool signedValues,
               bool logarithmic, int index) {
        if (index == 3 && focusModel) {
            ImGui::SetNextWindowFocus();
            focusModel = false;
        }
        if (index == 1 && focusImage) {
            ImGui::SetNextWindowFocus();
            focusImage = false;
        }
        const bool visible = ImGui::Begin(title, nullptr, chrome.views.prepare(title));
        if (visible)
            chrome.views.tools(title);
        windows[index] = {ImGui::GetWindowPos(),
                          {ImGui::GetWindowPos().x + ImGui::GetWindowSize().x,
                           ImGui::GetWindowPos().y + ImGui::GetWindowSize().y}};
        if (!visible) {
            ImGui::End();
            return;
        }
        const auto &geometry = index == 3 ? modelView() : view();
        if (!ready && !showingLive() && !(index == 3 && modelReady)) {
            ImGui::TextWrapped("%s", l("Run the layered benchmark or load a 2D model to generate this view.",
                                       "运行分层验证模型，或传入二维模型后生成图件。"));
            ImGui::End();
            return;
        }
        if (index != 3 && ready && modelReady && result.options != prepared.options && !showingLive())
            ImGui::TextWrapped("%s", l("Previous completed result; run the rebuilt model to update.",
                                       "上次已完成的结果；新模型运行后更新。"));
        if (showingLive())
            ImGui::TextWrapped("%s | %d/%d | t %.3f s",
                               preview.phase == 0 ? l("True pressure forward", "真实模型正演")
                               : preview.phase == 1
                                   ? l("Background / illumination", "背景正演 / 照明累积")
                                   : l("Source replay / image accumulation", "源场回放 / 成像累积"),
                               preview.shot, preview.shots, preview.time);
        double low = 0, high = 1;
        if (logarithmic) {
            low = -6;
            high = 0;
        } else if (signedValues) {
            high = 1e-100;
            for (double v : values)
                high = std::max(high, std::abs(v));
            high *= clip;
            low = -high;
        } else {
            auto [a, b] = std::minmax_element(values.begin(), values.end());
            low = *a;
            high = std::max(*b, low + 1);
        }
        const auto colors = cmap(palette);
        const auto size = ImGui::GetContentRegionAvail();
        if (!fitted[index]) {
            double zmin = *std::min_element(geometry.z.begin(), geometry.z.end());
            ImPlot::SetNextAxesLimits(geometry.x.front(), geometry.x[geometry.nx - 1], zmin,
                                      geometry.z.back(), ImGuiCond_Always);
        }
        ImPlot::PushColormap(colors);
        if (ImPlot::BeginPlot("##rtmField", {std::max(120.f, size.x - 115), std::max(120.f, size.y - 34)},
                              ImPlotFlags_NoLegend)) {
            fitted[index] = true;
            ImPlot::SetupAxes("X (m)", l("Z positive down (m)", "Z 向下 (m)"), 0, ImPlotAxisFlags_Invert);
            auto *draw = ImPlot::GetPlotDrawList();
            ImPlot::PushPlotClipRect();
            const auto originalFlags = draw->Flags;
            draw->Flags &= ~ImDrawListFlags_AntiAliasedFill;
            auto pixels = ImPlot::GetPlotSize();
            int dx = std::max(1, static_cast<int>(std::ceil(geometry.nx / std::max(32.f, pixels.x / 2)))),
                dz = std::max(1, static_cast<int>(std::ceil(geometry.nz / std::max(32.f, pixels.y / 2))));
            for (int z = 0; z < geometry.nz - 1; z += dz)
                for (int x = 0; x < geometry.nx - 1; x += dx) {
                    int x1 = std::min(geometry.nx - 1, x + dx), z1 = std::min(geometry.nz - 1, z + dz);
                    int ids[4]{z * geometry.nx + x, z * geometry.nx + x1, z1 * geometry.nx + x1,
                               z1 * geometry.nx + x};
                    ImVec2 p[4];
                    for (int k = 0; k < 4; ++k)
                        p[k] = ImPlot::PlotToPixels(geometry.x[ids[k]], geometry.z[ids[k]]);
                    double value = 0;
                    for (int q : ids)
                        value += values[q] * .25;
                    auto color = ImPlot::SampleColormap(
                        static_cast<float>(std::clamp((value - low) / (high - low), 0., 1.)), colors);
                    draw->AddConvexPolyFilled(p, 4, ImGui::ColorConvertFloat4ToU32(color));
                }
            for (int i = 0; i < geometry.nx - 1; ++i)
                draw->AddLine(ImPlot::PlotToPixels(geometry.x[i], geometry.z[i]),
                              ImPlot::PlotToPixels(geometry.x[i + 1], geometry.z[i + 1]),
                              color3D(palette3D().text), 1);
            if (index == 3) {
                for (std::size_t r = 0; r < geometry.receiverX.size(); ++r)
                    draw->AddCircleFilled(ImPlot::PlotToPixels(geometry.receiverX[r], geometry.receiverZ[r]),
                                          2.8f, IM_COL32(35, 175, 125, 255));
                const auto &o = geometry.options;
                for (int shot = 0; shot < o.shots; ++shot) {
                    const int sx =
                        o.shots == 1
                            ? (o.firstShot + o.lastShot) / 2
                            : o.firstShot +
                                  int(std::lround(double(shot) * (o.lastShot - o.firstShot) / (o.shots - 1)));
                    const auto q = std::size_t(o.sourceDepth) * geometry.nx + sx;
                    if (q < geometry.x.size())
                        draw->AddCircleFilled(ImPlot::PlotToPixels(geometry.x[q], geometry.z[q]), 4,
                                              IM_COL32(245, 150, 25, 255));
                }
            }
            draw->Flags = originalFlags;
            ImPlot::PopPlotClipRect();
            ImPlot::EndPlot();
        }
        ImGui::SameLine();
        ImPlot::ColormapScale("##fieldScale", low, high, {105, std::max(120.f, size.y - 34)},
                              signedValues ? "%.0e" : "%.2g", 0, colors);
        ImPlot::PopColormap();
        ImGui::TextUnformatted(logarithmic    ? "log10(I / max I)"
                               : signedValues ? (showingLive() && preview.phase < 2
                                                     ? l("Live source pressure p", "实时源场压力 p")
                                                     : l("Arbitrary image scale; zero-lag correlation",
                                                         "图像为任意幅值；零延迟互相关"))
                                              : "Vp (m/s)");
        ImGui::End();
    }
    void records() {
        const bool visible =
            ImGui::Begin(l("Pressure records###ImagingRecords", "压力记录###ImagingRecords"));
        windows[4] = {ImGui::GetWindowPos(),
                      {ImGui::GetWindowPos().x + ImGui::GetWindowSize().x,
                       ImGui::GetWindowPos().y + ImGui::GetWindowSize().y}};
        if (!visible) {
            ImGui::End();
            return;
        }
        if (ready) {
            if (modelReady && result.options != prepared.options)
                ImGui::TextWrapped("%s", l("Previous completed shot gather; rebuilt model has not run yet.",
                                           "上次已完成的炮集；新模型尚未运行。"));
            ImGui::TextUnformatted(
                l("Final-shot pressure residual: true - background; shared signed palette; no AGC.",
                  "末炮压力残差：真实模型 − 背景模型；沿用双极配色，无 AGC。"));
            double peak = 1e-100;
            for (double v : result.records)
                peak = std::max(peak, std::abs(v));
            ImPlot::PushColormap(cmap(recordPalette));
            const double half = result.receivers > 1 ? .5 * (result.receiverX[1] - result.receiverX[0])
                                                     : .5 * result.options.model.spacing;
            if (!fitted[4])
                ImPlot::SetNextAxesLimits(result.receiverX.front() - half, result.receiverX.back() + half,
                                          .5 * result.dt, (result.steps + .5) * result.dt, ImGuiCond_Always);
            if (ImPlot::BeginPlot("##rtmRecords", {-1, -1}, ImPlotFlags_NoLegend)) {
                fitted[4] = true;
                ImPlot::SetupAxes("Receiver X (m)", "Time (s)", 0, ImPlotAxisFlags_Invert);
                ImPlot::PlotHeatmap("Pressure (Pa)", recordView.data(), result.steps, result.receivers, -peak,
                                    peak, nullptr, {result.receiverX.front() - half, .5 * result.dt},
                                    {result.receiverX.back() + half, (result.steps + .5) * result.dt});
                ImPlot::EndPlot();
            }
            ImPlot::PopColormap();
        } else
            ImGui::TextWrapped(
                "%s", l("Acoustic pressure will appear here. Elastic Vx/Vz cannot be used as pressure input.",
                        "此处显示声学压力记录。弹性 Vx/Vz 不能直接当作压力数据输入。"));
        ImGui::End();
    }
    void summary() {
        if (!showSummary)
            return;
        ImGui::Begin(l("Active experiment###ImagingSummary", "当前成像参数###ImagingSummary"), &showSummary);
        if (modelReady) {
            ImGui::Text("%s %d x %d | %d %s", l("Applied model", "已应用模型"), prepared.nx, prepared.nz,
                        prepared.receivers, l("receivers", "道"));
            ImGui::Text("%s %d | %s %d | %s %d", l("Start", "起点"), prepared.options.receiverStart,
                        l("Stride", "间隔"), prepared.options.receiverStride, l("Depth", "深度"),
                        prepared.options.receiverDepth);
            if (dirty())
                ImGui::TextWrapped(
                    "%s", l("Draft changes: rebuild/apply or run.", "参数待应用：重建或运行后刷新。"));
        }
        if (ready) {
            ImGui::TextWrapped("%s", l("Completed result; sidebar edits affect the next run only.",
                                       "此处为已完成实验；左侧修改仅影响下一次运行。"));
            ImGui::Separator();
            ImGui::Text("%d x %d | %d shots", result.nx, result.nz, result.shots);
            ImGui::Text("dt %.5g ms | %d steps", result.dt * 1000, result.steps);
            ImGui::Text("%.3f s | %.1f MiB budget", result.seconds, result.estimatedMiB);
            ImGui::TextWrapped("%s", result.device.c_str());
        } else
            ImGui::TextWrapped(
                "%s", l("Run RTM to publish a completed result.", "完成一次 RTM 后显示生效参数与结果。"));
        ImGui::End();
    }
    ImVec2 lastViewportSize{};
    void dock() {
        chrome.views.beginFrame();
        if (ImGui::BeginMainMenuBar()) {
            if (ImGui::Button(l("Home", "主页")))
                home = true;
            navigation.draw(options.model, options.compute, static_cast<BackendKind3D>(backend), error);
            if (SeisWave::Ui::beginDropdownMenu(l("Workspace", "工作区"))) {
                bool previous = chrome.floating;
                chrome.panelMenu();
                if (previous != chrome.floating)
                    layout = true;
                ImGui::MenuItem(l("Active parameters", "当前成像参数"), nullptr, &showSummary);
                if (ImGui::MenuItem(l("Restore layout", "恢复布局"))) {
                    layout = true;
                    chrome.floating = false;
                    showSummary = true;
                    chrome.views.forgetFocus();
                }
                ImGui::EndMenu();
            }
            if (SeisWave::Ui::beginDropdownMenu(l("Appearance", "界面外观"))) {
                int language = language3D();
                if (ImGui::Combo("##imagingLanguage", &language, "English\0简体中文\0"))
                    setLanguage3D(language);
                int theme = themeIndex3D();
                if (themeCombo3D("##imagingTheme", theme, 240))
                    applyTheme3D(theme);
                glassAppearanceControls3D();
                ImGui::EndMenu();
            }
            ImGui::EndMainMenuBar();
        }
        const auto *viewport = ImGui::GetMainViewport();
        const double milliseconds =
            job.busy()
                ? std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - job.start)
                      .count()
            : ready ? result.seconds * 1000
                    : 0;
        const auto elapsed =
            std::string(l("Elapsed: ", "实际耗时：")) + SeisWave::Ui::computeElapsedText(milliseconds);
        std::array<const char *, 5> captions{l("Run RTM", "运行 RTM"), l("Cancel", "取消"),
                                             l("Rebuild / apply", "重建 / 应用"),
                                             l("Clear results", "清空结果"), l("Fit", "适配")};
        std::array<float, 6> widths{};
        for (int i = 0; i < 5; ++i)
            widths[i] = ImGui::CalcTextSize(captions[i]).x + 2 * ImGui::GetStyle().FramePadding.x;
        widths[5] = ImGui::CalcTextSize(elapsed.c_str()).x;
        SeisWave::Ui::CommandStrip2D strip(
            widths, {0, 0, 1, 1, 2, 3},
            {l("Imaging", "成像"), l("Model", "模型"), l("View", "视图"), l("Status", "状态")});
        const int rows = strip.rows(viewport->WorkSize.x - 2 * ImGui::GetStyle().WindowPadding.x);
        float commandHeight = rows * ImGui::GetFrameHeight() + (rows - 1) * ImGui::GetStyle().ItemSpacing.y +
                              2 * ImGui::GetStyle().WindowPadding.y;
        ImGui::SetNextWindowPos(viewport->WorkPos);
        ImGui::SetNextWindowSize({viewport->WorkSize.x, commandHeight});
        ImGui::Begin("##ImagingCommands", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoDocking |
                         ImGuiWindowFlags_NoSavedSettings);
        auto next = [&](int i) { strip.next(i); };
        next(0);
        ImGui::BeginDisabled(job.busy());
        if (functionButton3D(captions[0], FunctionRole3D::Run))
            start();
        runRect = {ImGui::GetItemRectMin(), ImGui::GetItemRectMax()};
        ImGui::EndDisabled();
        next(1);
        ImGui::BeginDisabled(!job.busy());
        if (functionButton3D(captions[1], FunctionRole3D::Danger))
            job.cancel = true;
        ImGui::EndDisabled();
        next(2);
        ImGui::BeginDisabled(job.busy());
        if (functionButton3D(captions[2], FunctionRole3D::Model))
            rebuild();
        rebuildRect = {ImGui::GetItemRectMin(), ImGui::GetItemRectMax()};
        ImGui::EndDisabled();
        next(3);
        ImGui::BeginDisabled(job.busy());
        if (functionButton3D(captions[3], FunctionRole3D::Display)) {
            result = {};
            ready = false;
            logIllumination.clear();
            recordView.clear();
            fit = true;
            focusModel = modelReady;
            status = l("Completed results cleared; applied model and survey retained.",
                       "已清空成像结果；保留已应用的模型和观测系统。");
        }
        ImGui::EndDisabled();
        next(4);
        if (functionButton3D(captions[4], FunctionRole3D::Display))
            fit = true;
        next(5);
        ImGui::TextColored(functionColor3D(FunctionRole3D::Run), "%s", elapsed.c_str());
        ImGui::SameLine();
        if (ImGui::GetContentRegionAvail().x > 180) {
            std::string phase;
            std::optional<Preview> latestPreview;
            {
                std::lock_guard lock(job.mutex);
                phase = job.phase;
            }
            ImGui::ProgressBar(static_cast<float>(job.progress.load()), {-1, ImGui::GetFrameHeight()},
                               phase.c_str());
        }
        ImGui::End();
        auto *vp = ImGui::GetMainViewport();
        int compact = vp->WorkSize.x < 1800 ? 1 : 0;
        if (lastCompact != compact) {
            layout = true;
            lastCompact = compact;
        }
        ImGui::SetNextWindowPos({vp->WorkPos.x, vp->WorkPos.y + commandHeight});
        ImGui::SetNextWindowSize({vp->WorkSize.x, vp->WorkSize.y - commandHeight});
        ImGui::Begin("##ImagingDockHost", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoMove |
                         ImGuiWindowFlags_NoSavedSettings);
        auto id = ImGui::GetID("ImagingDock");
        SeisWave::Ui::resizeDockSpace2D(id, lastViewportSize,
                                        {vp->WorkSize.x, vp->WorkSize.y - commandHeight});
        ImGui::DockSpace(id, {0, 0});
        if (layout || !ImGui::DockBuilderGetNode(id)) {
            ImGui::DockBuilderRemoveNode(id);
            ImGui::DockBuilderAddNode(id, ImGuiDockNodeFlags_DockSpace);
            ImGui::DockBuilderSetNodeSize(id, {vp->WorkSize.x, vp->WorkSize.y - commandHeight});
            ImGuiID main = id, left{}, bottom{}, right{};
            ImGui::DockBuilderSplitNode(main, ImGuiDir_Left,
                                        std::clamp((compact ? 310.f : 350.f) / vp->WorkSize.x, .22f, .38f),
                                        &left, &main);
            ImGui::DockBuilderSplitNode(main, ImGuiDir_Down, compact ? .44f : .34f, &bottom, &main);
            if (!compact)
                ImGui::DockBuilderSplitNode(main, ImGuiDir_Right, .43f, &right, &main);
            else
                right = main;
            if (!chrome.floating)
                ImGui::DockBuilderDockWindow(parameters, left);
            ImGui::DockBuilderDockWindow("Summary###ImagingSummary", left);
            ImGui::DockBuilderDockWindow(imageWindow, main);
            ImGui::DockBuilderDockWindow(modelWindow, main);
            ImGui::DockBuilderDockWindow(illuminationWindow, right);
            ImGui::DockBuilderDockWindow(recordWindow, bottom);
            ImGui::DockBuilderFinish(id);
            layout = false;
        }
        ImGui::End();
    }
};
void verifyRebuildSurvey(Workspace &app, SeisWave::Ui::AppRuntime &runtime) {
    auto require = [](bool ok, const char *message) {
        if (!ok)
            throw std::runtime_error(message);
    };
    auto draw = [&] {
        app.collect();
        runtime.begin();
        app.dock();
        app.controls();
        app.summary();
        if (app.fit)
            app.fitted.fill(false);
        app.field(l("RTM image###ImagingImage", "RTM 图像###ImagingImage"), app.imageValues(),
                  app.imagePalette, true, false, 1);
        app.field(l("Source illumination###ImagingIllumination", "源照明###ImagingIllumination"),
                  app.illuminationValues(), 7, false, true, 2);
        app.field(l("Model###ImagingModel", "模型###ImagingModel"),
                  app.showBackground ? app.modelView().backgroundVp : app.modelView().vp, 7, false, false, 3);
        app.records();
        app.fit = false;
        runtime.present();
    };
    auto &io = ImGui::GetIO();
    auto click = [&](ImRect rect) {
        const auto at = rect.GetCenter();
        io.AddMousePosEvent(at.x, at.y);
        io.AddMouseButtonEvent(0, true);
        draw();
        io.AddMouseButtonEvent(0, false);
        draw();
    };
    auto wait = [&] {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
        while (app.job.busy() && std::chrono::steady_clock::now() < deadline)
            draw();
        require(!app.job.busy(), "RTM rebuild/rerun interaction timed out");
        draw();
    };
    auto capture = [&](const char *name) {
        int w{}, h{};
        glfwGetFramebufferSize(runtime.window(), &w, &h);
        require(SeisWave::Ui::capture(SeisWave::Ui::outputRoot() / "screenshots/imaging" / name, w, h),
                "RTM survey screenshot failed");
    };
    require(app.ready && app.modelReady && !app.job.busy(), "RTM interaction needs a completed first run");
    const auto oldImage = app.result.image;
    const auto oldX = app.prepared.x;
    app.chrome.category = 0;
    draw();
    draw();
    if (auto *panel = ImGui::FindWindowByName("##RTMParameterFields"))
        ImGui::SetScrollY(panel, 0);
    draw();
    click(app.nxRect);
    io.AddKeyEvent(ImGuiMod_Ctrl, true);
    io.AddKeyEvent(ImGuiKey_A, true);
    draw();
    io.AddKeyEvent(ImGuiKey_A, false);
    io.AddKeyEvent(ImGuiMod_Ctrl, false);
    draw();
    io.AddInputCharactersUTF8("161");
    draw();
    io.AddKeyEvent(ImGuiKey_Enter, true);
    draw();
    io.AddKeyEvent(ImGuiKey_Enter, false);
    draw();
    require(
        app.options.model.nx == 161 && app.options.autoSurfaceReceivers && app.options.receiverDepth == 1 &&
            app.options.receiverStart + (app.options.receiverCount - 1) * app.options.receiverStride < 160,
        "Actual Nx edit did not automatically realign surface receivers");
    require(app.dirty() && app.prepared.x == oldX && app.result.image == oldImage,
            "Draft edit mutated applied geometry or completed image");
    app.options.model.relief = 8;
    draw();
    click(app.sessionActions.rebuildRect);
    wait();
    require(app.error.empty() && app.prepared.nx == 161 && !app.dirty() && app.result.image == oldImage,
            "Footer rebuild after completion failed or destroyed the previous image");
    for (int r = 0; r < app.prepared.receivers; ++r) {
        const auto &o = app.prepared.options;
        const auto q = std::size_t(app.prepared.nx) + o.receiverStart + r * o.receiverStride;
        require(std::abs(app.prepared.receiverZ[r] - app.prepared.z[q]) < 1e-12,
                "Rebuilt receivers are not on the first mapped terrain row");
    }
    app.chrome.category = 1;
    draw();
    draw();
    ImGui::SetWindowFocus(modelWindow);
    draw();
    capture("rebuild-surface.ppm");
    click(app.rebuildRect);
    wait();
    require(app.error.empty() && app.result.image == oldImage,
            "Toolbar rebuild is unavailable after completion");
    click(app.autoReceiverRect);
    require(!app.options.autoSurfaceReceivers, "Auto receiver toggle did not disable");
    app.options.receiverStart = 3;
    app.options.receiverCount = 9;
    app.options.receiverStride = 4;
    app.options.receiverDepth = 3;
    draw();
    click(app.sessionActions.rebuildRect);
    wait();
    require(app.error.empty() && app.prepared.receivers == 9 && app.prepared.options.receiverStart == 3 &&
                app.prepared.options.receiverStride == 4 && app.prepared.options.receiverDepth == 3,
            "Manual receiver settings not applied");
    const auto valid = app.prepared.options;
    const auto model = app.prepared.x;
    app.options.model.nx = 1;
    draw();
    click(app.sessionActions.rebuildRect);
    wait();
    require(!app.error.empty() && app.prepared.x == model && app.result.image == oldImage,
            "Failed rebuild did not retain both model and result");
    click(app.sessionActions.undoRect);
    require(app.error.empty() && app.options == valid, "Shared undo did not restore applied RTM parameters");
    click(app.runRect);
    wait();
    require(app.error.empty() && app.ready && app.result.nx == 161 && app.result.receivers == 9 &&
                app.result.options == valid,
            "Second RTM run did not use the rebuilt manual survey");
    ImGui::SetWindowFocus(modelWindow);
    draw();
    capture("manual-receivers.ppm");
    click(app.sessionActions.defaultsRect);
    require(app.options.autoSurfaceReceivers && app.dirty(),
            "RTM defaults did not restore auto surface survey");
    click(app.sessionActions.undoRect);
    require(app.options == valid, "RTM undo after defaults changed applied survey");
    app.options.receiverCount = 1;
    draw();
    click(app.runRect);
    wait();
    require(app.error.empty() && app.ready && app.result.receivers == 1, "Single-receiver rerun/plot failed");
    const auto folder = SeisWave::Ui::outputRoot() / "validation/rtm-survey-2026-10-31";
    std::filesystem::create_directories(folder);
    std::ofstream(folder / "ui-interactions.txt")
        << "PASS: actual Nx edit / automatic surface receiver realignment / persistent footer rebuild after "
           "completed RTM / toolbar rebuild / applied terrain coordinates / manual start-count-stride-depth "
           "/ failed rebuild preserves model and image / undo / second RTM run / single-receiver rerun and "
           "plot limits / defaults / exact model acquisition overlays\n";
    std::cout << "RTM rebuild/survey interactions PASS\n";
}
} // namespace
int runWorkbench(bool smoke) {
    SeisWave::Ui::AppRuntime runtime;
    std::string error;
    if (!runtime.open("SeisWave Lab | 2D Imaging / Acoustic RTM", smoke, error, true))
        throw std::runtime_error(error);
    Workspace app;
    const int panelMode2 = SeisWave::Ui::parameterDisplayMode(2);
    const int panelMode3 = SeisWave::Ui::parameterDisplayMode(3);
    int completedFrame = -1;
    bool previewCaptured = false;
    if (smoke) {
        app.options = benchmarkOptions();
        app.options.autoSurfaceReceivers = true;
        app.options.shots = 3;
        app.options.backend = BackendKind3D::Cpu;
        app.backend = 0;
        app.precision = 1;
        applyTheme3D(11);
        setLanguage3D(1);
        glfwSetWindowSize(runtime.window(), 1600, 1000);
        app.start();
    } else
        app.rebuild();
    auto started = std::chrono::steady_clock::now();
    for (int frame = 0; !glfwWindowShouldClose(runtime.window()); ++frame) {
        app.collect();
        if (smoke && app.ready && completedFrame < 0)
            completedFrame = frame;
        runtime.begin();
        app.dock();
        app.controls();
        app.options.backend = static_cast<BackendKind3D>(app.backend);
        app.options.model.precision = app.precision ? Precision::Float32 : Precision::Float64;
        app.summary();
        if (app.fit)
            app.fitted.fill(false);
        app.field(l("RTM image###ImagingImage", "RTM 图像###ImagingImage"), app.imageValues(),
                  app.imagePalette, true, false, 1);
        app.field(l("Source illumination###ImagingIllumination", "源照明###ImagingIllumination"),
                  app.illuminationValues(), 7, false, true, 2);
        app.field(l("Model###ImagingModel", "模型###ImagingModel"),
                  app.showBackground ? app.modelView().backgroundVp : app.modelView().vp, 7, false, false, 3);
        app.records();
        app.fit = false;
        runtime.present();
        if (smoke && app.showingLive() && !previewCaptured) {
            int w{}, h{};
            glfwGetFramebufferSize(runtime.window(), &w, &h);
            if (!SeisWave::Ui::capture(SeisWave::Ui::outputRoot() / "screenshots/imaging/running-preview.ppm",
                                       w, h))
                throw std::runtime_error("RTM live preview capture failed");
            previewCaptured = true;
        }
        if (app.home || app.navigation.requested) {
            app.job.cancel = true;
            if (!smoke)
                rtmDraft = app.options;
            return app.home ? SeisWave::Ui::returnHome : app.navigation.requested;
        }
        if (smoke) {
            if (!app.error.empty())
                throw std::runtime_error(app.error);
            if (std::chrono::steady_clock::now() - started > std::chrono::seconds(60))
                throw std::runtime_error("RTM UI computation timed out");
            if (completedFrame >= 0 && frame >= completedFrame + 6) {
                int offset = frame - completedFrame;
                if (offset == 6 || offset == 12 || offset == 18) {
                    const auto *vp = ImGui::GetMainViewport();
                    for (int j : {0, 1, 2, 4}) {
                        const auto &rect = app.windows[j];
                        if (rect.GetWidth() < 200 || rect.GetHeight() < 120 || rect.Min.x < vp->Pos.x - 2 ||
                            rect.Max.x > vp->Pos.x + vp->Size.x + 2) {
                            int w, h;
                            glfwGetFramebufferSize(runtime.window(), &w, &h);
                            SeisWave::Ui::capture(SeisWave::Ui::outputRoot() /
                                                      "screenshots/module-ui/rtm-layout-failure.ppm",
                                                  w, h);
                            throw std::runtime_error(
                                "RTM workspace layout clipped: window " + std::to_string(j) +
                                " width=" + std::to_string(rect.GetWidth()) + " height=" +
                                std::to_string(rect.GetHeight()) + " x=" + std::to_string(rect.Min.x) + ".." +
                                std::to_string(rect.Max.x) + " viewport=" + std::to_string(vp->Pos.x) + ".." +
                                std::to_string(vp->Pos.x + vp->Size.x));
                        }
                    }
                    int w, h;
                    glfwGetFramebufferSize(runtime.window(), &w, &h);
                    auto path = SeisWave::Ui::outputRoot() /
                                ("screenshots/imaging/rtm-" + std::to_string(offset) + ".ppm");
                    if (!SeisWave::Ui::capture(path, w, h))
                        throw std::runtime_error("RTM screenshot failed");
                    if (offset == 6)
                        glfwSetWindowSize(runtime.window(), 1280, 800);
                    if (offset == 12)
                        glfwSetWindowSize(runtime.window(), 960, 640);
                    if (offset == 18) {
                        if (!previewCaptured)
                            throw std::runtime_error("RTM did not display a live process frame");
                        std::cout << "RTM UI PASS: asynchronous solve / full model / pressure records / 3 "
                                     "layouts / red-orange shared theme\n";
                        std::cout << "RTM independent workspace PASS\n";
                        glfwSetWindowSize(runtime.window(), 1600, 1000);
                        app.chrome.category = 2;
                        app.chrome.floating = true;
                        app.chrome.center = true;
                        app.layout = true;
                    }
                }
                if (offset == 24 || offset == 30) {
                    auto *panel = ImGui::FindWindowByName(parameters);
                    if (!panel || (offset == 24 ? panel->DockId != 0 : panel->DockId == 0))
                        throw std::runtime_error("Independent parameter floating/redocking failed");
                    if (SeisWave::Ui::parameterDisplayMode(2) != panelMode2 ||
                        SeisWave::Ui::parameterDisplayMode(3) != panelMode3)
                        throw std::runtime_error("Workspace changed another module's panel preference");
                    if (offset == 24) {
                        int w, h;
                        glfwGetFramebufferSize(runtime.window(), &w, &h);
                        if (!SeisWave::Ui::capture(SeisWave::Ui::outputRoot() /
                                                       "screenshots/imaging/floating-parameters.ppm",
                                                   w, h))
                            throw std::runtime_error("Floating parameter capture failed");
                        app.chrome.floating = false;
                        app.layout = true;
                    } else {
                        std::cout << "Workspace panel PASS: local floating / redocking / shared preferences "
                                     "unchanged\n";
                        verifyRebuildSurvey(app, runtime);
                        return 0;
                    }
                }
            }
        }
    }
    if (!smoke)
        rtmDraft = app.options;
    return 0;
}
} // namespace Seis2D::Imaging::Ui
