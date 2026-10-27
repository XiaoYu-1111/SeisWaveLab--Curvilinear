#include "RecordView.hpp"
#include "WorkspaceTools.hpp"
#include "SeisCommon/AcquisitionAnalysis.hpp"
#include <chrono>
#include "Presentation3D.hpp"
#include "RecordPalettes3D.hpp"
#include "implot.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
namespace SeisWave::Ui {
using namespace Seis3D::Curvilinear::Ui;
namespace RecordPalettes = Seis3D::Curvilinear::RecordPalettes;
namespace {
constexpr const char *styles =
    "Wiggle\0Variable area\0Density image\0Density + wiggle\0Signed lobes\0Density + signed lobes\0";
const char *l(const char *en, const char *zh) {
    return language3D() ? zh : en;
}
double valueAt(const RecordDataView &data, int receiver, int component, std::size_t sample) {
    return data.value(receiver, component, sample);
}
ImPlotColormap colormap(int palette, float opacity = 1) {
    const int alpha = std::clamp(static_cast<int>(std::lround(opacity * 100)), 5, 100);
    const auto name = "CurvilinearRecords" + std::to_string(palette) + "Alpha" + std::to_string(alpha);
    const auto cached = ImPlot::GetColormapIndex(name.c_str());
    if (cached >= 0)
        return cached;
    ImVec4 stops[5];
    const auto &colors = RecordPalettes::wavePalette(palette);
    for (int i = 0; i < 5; ++i)
        stops[i] = {colors[i][0], colors[i][1], colors[i][2], alpha * .01f};
    return ImPlot::AddColormap(name.c_str(), stops, 5, false);
}
ImVec4 polarity(int palette, bool positive) {
    const auto &c = RecordPalettes::wavePalette(palette)[positive ? 4 : 0];
    return {c[0], c[1], c[2], 1};
}
struct Curve {
    const TraceProcessing::TracePreview *data{};
    double baseline{}, scale{1};
    int lobe{};
    bool single{};
};
ImPlotPoint curve(int i, void *pointer) {
    const auto &p = *static_cast<Curve *>(pointer);
    const double v = p.data->values[i];
    const double x = (p.lobe > 0 ? std::max(0.0, v) : p.lobe < 0 ? std::min(0.0, v) : v) * p.scale;
    return p.single ? ImPlotPoint(p.data->times[i], x) : ImPlotPoint(p.baseline + x, p.data->times[i]);
}
ImPlotPoint baseline(int i, void *pointer) {
    const auto &p = *static_cast<Curve *>(pointer);
    return p.single ? ImPlotPoint(p.data->times[i], 0) : ImPlotPoint(p.baseline, p.data->times[i]);
}
void releaseAutoFit(bool &automatic, bool &fit, ImVec2 size) {
    const auto at = ImGui::GetCursorScreenPos(), mouse = ImGui::GetIO().MousePos;
    const bool inside =
        mouse.x >= at.x && mouse.x <= at.x + size.x && mouse.y >= at.y && mouse.y <= at.y + size.y;
    if (inside && ImGui::IsWindowHovered() &&
        (ImGui::GetIO().MouseWheel != 0 || ImGui::IsMouseDragging(ImGuiMouseButton_Left) ||
         ImGui::IsMouseDragging(ImGuiMouseButton_Right))) {
        automatic = false;
        fit = false;
    }
}
void plotColors() {
    ImPlot::PushStyleColor(ImPlotCol_FrameBg, palette3D().background);
    ImPlot::PushStyleColor(ImPlotCol_PlotBg, palette3D().background);
    ImPlot::PushStyleColor(ImPlotCol_PlotBorder, palette3D().border);
    ImPlot::PushStyleColor(ImPlotCol_AxisText, palette3D().text);
    ImPlot::PushStyleColor(ImPlotCol_AxisGrid, color3D(palette3D().muted, .18f));
    ImPlot::PushStyleVar(ImPlotStyleVar_PlotMinSize, ImVec2(120, 60));
}
void finishPlotColors() {
    ImPlot::PopStyleVar();
    ImPlot::PopStyleColor(5);
}
void readableScale(int palette, double range, ImVec2 size) {
    const auto at = ImGui::GetCursorScreenPos();
    auto *draw = ImGui::GetWindowDrawList();
    const float font = ImGui::GetFontSize(), top = at.y + font * .55f, bottom = at.y + size.y - font * .55f;
    const auto &stops = RecordPalettes::wavePalette(palette);
    for (int i = 0; i < 64; ++i) {
        const double t = 1 - i / 63., p = t * 4;
        const int k = std::min(3, static_cast<int>(p));
        const float f = static_cast<float>(p - k);
        const ImVec4 color{stops[k][0] * (1 - f) + stops[k + 1][0] * f,
                           stops[k][1] * (1 - f) + stops[k + 1][1] * f,
                           stops[k][2] * (1 - f) + stops[k + 1][2] * f, 1};
        draw->AddRectFilled({at.x + 4, top + (bottom - top) * i / 64},
                            {at.x + 22, top + (bottom - top) * (i + 1) / 64},
                            ImGui::ColorConvertFloat4ToU32(color));
    }
    const int ticks = size.y > font * 6 ? 5 : size.y > font * 3.8f ? 3 : 2;
    for (int i = 0; i < ticks; ++i) {
        char value[32];
        std::snprintf(value, sizeof(value), "%.2g", range * (1 - 2. * i / (ticks - 1)));
        const float y = top + (bottom - top) * i / (ticks - 1);
        draw->AddText({at.x + 28, y - font * .5f}, color3D(palette3D().text), value);
    }
    ImGui::InvisibleButton("##RecordScale", size);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s: %.4g .. %.4g", tr3D("Normalized amplitude"), -range, range);
}
} // namespace
void RecordView::options(const RecordDataView& records) {
    const int total=static_cast<int>(records.traces.size());
    if (ImGui::Button(label3D("Options"), {-1, 0}))
        ImGui::OpenPopup("RecordOptions3D");
    const auto lo = ImGui::GetItemRectMin(), hi = ImGui::GetItemRectMax();
    optionsBounds = {lo.x, lo.y, hi.x, hi.y};
    const auto *vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowSizeConstraints({std::min(420.0f, vp->WorkSize.x * .8f), 0},
                                        {std::min(650.0f, vp->WorkSize.x * .9f), vp->WorkSize.y * .8f});
    if (!ImGui::BeginPopup("RecordOptions3D"))
        return;
    const auto old = settings;
    const auto field = [](const char *label) {
        ImGui::TextUnformatted(tr3D(label));
        ImGui::SetNextItemWidth(-1);
    };
    field("Record style");
    ImGui::Combo("##style", &settings.style, choices3D(styles));
    field("Record palette");
    ImGui::Combo("##palette", &settings.palette, choices3D(RecordPalettes::wavePaletteNames));
    field("Trace gain");
    ImGui::SliderFloat("##gain", &settings.gain, .1f, 20, "%.2fx", ImGuiSliderFlags_Logarithmic);
    field("Heatmap range");
    ImGui::SliderFloat("##range", &settings.heatmapRange, .05f, 1, "%.2f");
    field("Density opacity");
    ImGui::SliderFloat("##opacity", &settings.opacity, .05f, 1, "%.2f");
    ImGui::Checkbox(label3D("Normalize each trace"), &settings.normalizeEach);
    ImGui::TextWrapped(tr3D(settings.normalizeEach
                                ? "Individual scales: relative channel amplitudes are not comparable."
                                : "Shared scale: relative amplitudes of visible channels are preserved."));
    ImGui::Checkbox(label3D("Automatic gain compensation"), &settings.agc.enabled);
    if (settings.agc.enabled) {
        field("AGC window (ms)");
        ImGui::SliderFloat("##agcWindow", &settings.agc.windowMs, 5, 2000, "%.0f");
        field("AGC strength");
        ImGui::SliderFloat("##agcStrength", &settings.agc.strength, 0, 1, "%.2f");
        field("Maximum AGC boost");
        ImGui::SliderFloat("##agcMax", &settings.agc.maxBoost, 1, 200, "%.0fx");
        field("Noise floor (percent of raw peak)");
        ImGui::SliderFloat("##agcFloor", &settings.agc.noiseFloorPercent, 0, 2, "%.3f%%");
    }
    ImGui::Checkbox(label3D("Show all traces"), &settings.showAll);
    if (!settings.showAll) {
        int window[2]{settings.first + 1, settings.visible};
        field("First / visible traces");
        if (ImGui::InputInt2("##traceWindow", window)) {
            settings.first = std::clamp(window[0] - 1, 0, total - 1);
            settings.visible = std::clamp(window[1], 1, total - settings.first);
        }
    }
    ImGui::Checkbox(label3D("Follow full record"), &settings.autoFit);
    if (ImGui::Button(label3D("Fit records")))
        fit_ = true;
    ImGui::SameLine();
    if (ImGui::Button(label3D("Reset record display"))) {
        settings = RecordSettings{};
        fit_ = true;
    }
    ImGui::TextWrapped(tr3D(
        "Display gain, AGC and normalization affect preview copies only. Raw CSV samples remain unchanged."));
    if(ImGui::CollapsingHeader(language3D()?"论文输出与采集评价":"Paper export / acquisition metrics")) {
        ImGui::InputDouble(language3D()?"统一幅值参考（0=自动）":"Shared amplitude reference (0=auto)",&paperReference_,0,0,"%.5g");
        const int component=std::clamp(settings.component,0,static_cast<int>(records.components.size()));
        const auto output=[&](const char* suffix){return outputRoot()/"exports/records"/(std::to_string(std::chrono::system_clock::now().time_since_epoch().count())+suffix);};
        if(ImGui::Button(language3D()?"导出论文 SVG":"Export paper SVG")) {
            const auto path=output("_record.svg");std::string error;PaperRecordOptions options;options.amplitudeReference=paperReference_;
            const auto name=path.generic_u8string();analysisNotice_=writePaperRecordSvg(records,component,path,options,error)?(language3D()?"已导出：":"Exported: ")+std::string(name.begin(),name.end()):error;
        }
        ImGui::TextWrapped("%s",language3D()?"原始样本、白底细线、统一幅值；跨工况比较请设置相同参考幅值。":"Raw samples, white background, fine lines and a shared amplitude scale. Use the same reference for case comparisons.");
        ImGui::InputScalarN(language3D()?"目标波窗 [起,止) s":"Target window [start,end) s",ImGuiDataType_Double,signalWindow_.data(),2,nullptr,nullptr,"%.4f");
        ImGui::InputScalarN(language3D()?"干扰波窗 [起,止) s":"Interference window [start,end) s",ImGuiDataType_Double,interferenceWindow_.data(),2,nullptr,nullptr,"%.4f");
        ImGui::InputDouble(language3D()?"到时阈值 / 道峰值":"Arrival threshold / trace peak",&thresholdFraction_,.01,0,"%.3f");
        if(ImGui::Button(language3D()?"导出逐道评价 CSV":"Export per-trace metrics CSV")) {
            const auto path=output("_metrics.csv");std::string error;
            const auto name=path.generic_u8string();analysisNotice_=writeAcquisitionCsv(records,component,{signalWindow_[0],signalWindow_[1]},{interferenceWindow_[0],interferenceWindow_[1]},thresholdFraction_,path,error)?(language3D()?"已导出：":"Exported: ")+std::string(name.begin(),name.end()):error;
        }
        ImGui::TextWrapped("%s",language3D()?"能量 / RMS 比对应指定的两个波窗；阈值到时用于辅助比较。评价不使用显示 AGC。":"Energy/RMS ratios compare the two specified windows; threshold times assist comparisons. Display AGC is excluded.");
        if(!analysisNotice_.empty())ImGui::TextWrapped("%s",analysisNotice_.c_str());
    }
    if (!(settings == old))
        fit_ = true;
    ImGui::EndPopup();
}
void RecordView::refresh(const RecordDataView &s, int selected) {
    const int selectedKey = settings.view == 1 ? selected : -1;
    if (revision_ == s.revision && cachedSettings_ == settings && cachedSelected_ == selectedKey)
        return;
    const auto &traces = s.traces;
    const int total = static_cast<int>(traces.size());
    first_ = settings.showAll ? 0 : std::clamp(settings.first, 0, total - 1);
    count_ = settings.showAll ? total : std::clamp(settings.visible, 1, total - first_);
    dt_ = s.dt;
    startTime_ = std::numeric_limits<double>::infinity();
    endTime_ = -startTime_;
    series_.clear();
    rawPeak_ = commonPeak_ = 0;
    std::size_t maxSamples = 1;
    const int seriesCount = settings.view == 1 ? static_cast<int>(s.components.size()) : count_;
    for (int r = 0; r < seriesCount; ++r) {
        Series series;
        series.receiver = settings.view == 1 ? selected : first_ + r;
        series.component = settings.view == 1 ? r : settings.component;
        const auto n = s.sampleCount(series.receiver);
        std::vector<double> values(n), times(n);
        maxSamples = std::max(maxSamples, n);
        for (std::size_t t = 0; t < n; ++t) {
            times[t] = traces[series.receiver].startTime + t * dt_;
            values[t] = s.value(series.receiver, series.component, t);
            series.rawPeak = std::max(series.rawPeak, std::abs(values[t]));
        }
        startTime_ = std::min(startTime_, times.front());
        endTime_ = std::max(endTime_, times.back());
        rawPeak_ = std::max(rawPeak_, series.rawPeak);
        TraceProcessing::applyAgc(values, times, settings.agc);
        series.preview = TraceProcessing::reduceExtrema(values, times);
        commonPeak_ = std::max(commonPeak_, series.preview.peak);
        series_.push_back(std::move(series));
    }
    endTime_ = std::max(startTime_ + dt_, endTime_);
    densityRows_ = static_cast<int>(std::min<std::size_t>(1024, std::max<std::size_t>(2, maxSamples)));
    density_.clear();
    if (settings.view == 0)
        density_ = TraceProcessing::buildTraceDensity(
            count_, densityRows_, startTime_, endTime_, commonPeak_, settings.normalizeEach,
            [&](int i) -> const auto & { return series_[i].preview.times; },
            [&](int i) -> const auto & { return series_[i].preview.values; },
            [&](int i) { return series_[i].preview.peak; });
    cachedSettings_ = settings;
    cachedSelected_ = selectedKey;
    revision_ = s.revision;
}
void RecordView::section(const RecordDataView &s, int &selected) {
    const bool density = settings.style == 2 || settings.style == 3 || settings.style == 5;
    const bool wiggle = settings.style != 2, fill = settings.style == 1 || settings.style == 3 ||
                                                    settings.style == 4 || settings.style == 5;
    const bool signedFill = settings.style == 4 || settings.style == 5;
    const auto available = ImGui::GetContentRegionAvail();
    const bool scaleBar = density && available.x > 400 && available.y > 75;
    const float scaleWidth = ImGui::GetFontSize() * 3.4f + 24;
    const ImVec2 size{available.x - (scaleBar ? scaleWidth + ImGui::GetStyle().ItemSpacing.x : 0),
                      std::max(60.0f, available.y)};
    releaseAutoFit(settings.autoFit, fit_, size);
    plotColors();
    const auto limitsFlags = size.y < 160 ? ImPlotAxisFlags_NoLabel : ImPlotAxisFlags_None;
    if (ImPlot::BeginPlot("##ReceiverSection3D", size, ImPlotFlags_NoTitle | ImPlotFlags_NoLegend)) {
        ImPlot::SetupAxes(tr3D("Receiver ID"), tr3D("Time (s)"), limitsFlags,
                          ImPlotAxisFlags_Invert | limitsFlags);
        ImPlot::SetupAxisFormat(ImAxis_X1, "R%.0f");
        if (size.y < ImGui::GetFontSize() * 12) {
            const int ticks =
                std::clamp(static_cast<int>((size.y - 40) / (ImGui::GetFontSize() * 1.7f)), 2, 6);
            ImPlot::SetupAxisTicks(ImAxis_Y1, startTime_, endTime_, ticks);
            ImPlot::SetupAxisFormat(ImAxis_Y1, "%.2g");
            const int maxTicks = std::max(2, static_cast<int>((size.x - 80) / (ImGui::GetFontSize() * 4)));
            const int step = std::max(1, (count_ + maxTicks - 1) / maxTicks);
            std::vector<double> values;
            for (int i = first_ + 1; i <= first_ + count_; i += step)
                values.push_back(i);
            ImPlot::SetupAxisTicks(ImAxis_X1, values.data(), static_cast<int>(values.size()));
        }
        if (fit_ || settings.autoFit) {
            ImPlot::SetupAxisLimits(ImAxis_X1, first_ + .4, first_ + count_ + .6, ImPlotCond_Always);
            ImPlot::SetupAxisLimits(ImAxis_Y1, startTime_, endTime_, ImPlotCond_Always);
        }
        if (density && !density_.empty()) {
            ImPlot::PushColormap(colormap(settings.palette, settings.opacity));
            const double range = settings.heatmapRange / settings.gain;
            ImPlot::PlotHeatmap("##Density", density_.data(), densityRows_, count_, -range, range, nullptr,
                                {first_ + .5, endTime_}, {first_ + count_ + .5, startTime_});
            ImPlot::PopColormap();
        }
        if (wiggle)
            for (auto &series : series_) {
                const double peak = settings.normalizeEach ? series.preview.peak : commonPeak_;
                Curve p{&series.preview, series.receiver + 1.0, .44 * settings.gain / std::max(peak, 1e-30),
                        1, false};
                char label[40];
                std::snprintf(label, sizeof(label), "R%d", series.receiver + 1);
                if (fill) {
                    ImPlot::SetNextFillStyle(polarity(settings.palette, true), .35f);
                    ImPlot::PlotShadedG(label, curve, &p, baseline, &p,
                                        static_cast<int>(series.preview.values.size()));
                    if (signedFill) {
                        p.lobe = -1;
                        ImPlot::SetNextFillStyle(polarity(settings.palette, false), .35f);
                        ImPlot::PlotShadedG((std::string(label) + "##negative").c_str(), curve, &p, baseline,
                                            &p, static_cast<int>(series.preview.values.size()));
                    }
                }
                const auto &neutral = RecordPalettes::wavePalette(settings.palette)[2];
                const bool lightDensity =
                    .2126f * neutral[0] + .7152f * neutral[1] + .0722f * neutral[2] > .5f;
                const auto ink =
                    density ? (lightDensity ? ImVec4(.06f, .08f, .1f, 1) : ImVec4(.94f, .96f, .98f, 1))
                            : palette3D().text;
                p.lobe = 0;
                ImPlot::SetNextLineStyle(series.receiver == selected ? palette3D().accent : ink,
                                         series.receiver == selected ? 1.8f : 1.0f);
                ImPlot::PlotLineG(label, curve, &p, static_cast<int>(series.preview.values.size()));
            }
        if (ImPlot::IsPlotHovered()) {
            const auto mouse = ImPlot::GetPlotMousePos();
            const int receiver =
                std::clamp(static_cast<int>(std::lround(mouse.x)) - 1, first_, first_ + count_ - 1);
            const auto sample = static_cast<std::size_t>(
                std::clamp(std::llround((mouse.y - s.traces[receiver].startTime) / dt_), 0LL,
                           static_cast<long long>(s.sampleCount(receiver) - 1)));
            ImGui::SetTooltip("R%d | t %.5f s | raw %.5e %s", receiver + 1,
                              s.traces[receiver].startTime + sample * dt_,
                              valueAt(s, receiver, settings.component, sample), s.units.c_str());
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left))
                selected = receiver;
        }
        ImPlot::EndPlot();
    }
    if (scaleBar) {
        ImGui::SameLine();
        readableScale(settings.palette, settings.heatmapRange / settings.gain, {scaleWidth, size.y});
    }
    finishPlotColors();
}
void RecordView::single(const RecordDataView &s, int selected) {
    const auto size = ImGui::GetContentRegionAvail();
    const ImVec2 plotSize{size.x, std::max(60.0f, size.y)};
    releaseAutoFit(settings.autoFit, fit_, plotSize);
    plotColors();
    if (ImPlot::BeginPlot("##SingleReceiver3D", plotSize, ImPlotFlags_NoTitle)) {
        const auto flags = size.y < 160 ? ImPlotAxisFlags_NoLabel : ImPlotAxisFlags_None;
        ImPlot::SetupAxes(tr3D("Time (s)"),
                          settings.normalizeEach ? tr3D("Normalized amplitude")
                          : settings.agc.enabled ? tr3D("Display amplitude (AGC)")
                                                 : s.units.c_str(),
                          flags, flags);
        ImPlot::SetupAxisFormat(ImAxis_Y1, "%.2g");
        ImPlot::SetupLegend(ImPlotLocation_NorthEast, ImPlotLegendFlags_Horizontal);
        if (fit_ || settings.autoFit) {
            const double peak = settings.normalizeEach ? 1 : std::max(commonPeak_, 1e-20);
            ImPlot::SetupAxisLimits(ImAxis_X1, startTime_, endTime_, ImPlotCond_Always);
            ImPlot::SetupAxisLimits(ImAxis_Y1, -1.15 * peak / settings.gain, 1.15 * peak / settings.gain,
                                    ImPlotCond_Always);
        }

        const bool light = palette3D().light;
        const ImVec4 colors[]{light ? ImVec4(.12f, .34f, .75f, 1) : ImVec4(.35f, .65f, 1, 1),
                              light ? ImVec4(.80f, .28f, .04f, 1) : ImVec4(1, .60f, .25f, 1),
                              light ? ImVec4(.02f, .48f, .29f, 1) : ImVec4(.30f, .85f, .55f, 1)};
        for (int c = 0; c < static_cast<int>(s.components.size()); ++c) {
            auto &series = series_[c];
            Curve p{&series.preview, 0, settings.normalizeEach ? 1 / std::max(series.preview.peak, 1e-30) : 1,
                    0, true};
            ImPlot::SetNextLineStyle(colors[c], 1.6f);
            ImPlot::PlotLineG(s.components[c].c_str(), curve, &p,
                              static_cast<int>(series.preview.values.size()));
        }
        if (ImPlot::IsPlotHovered()) {
            const auto mouse = ImPlot::GetPlotMousePos();
            const auto sample = static_cast<std::size_t>(
                std::clamp(std::llround((mouse.x - s.traces[selected].startTime) / dt_), 0LL,
                           static_cast<long long>(s.sampleCount(selected) - 1)));
            std::string tooltip = "R" + std::to_string(selected + 1) + " | t " +
                                  std::to_string(s.traces[selected].startTime + sample * dt_) + " s";
            for (int c = 0; c < static_cast<int>(s.components.size()); ++c) {
                char value[128];
                std::snprintf(value, sizeof(value), "%s %.5e %s", s.components[c].c_str(),
                              s.value(selected, c, sample), s.units.c_str());
                tooltip += "\n" + std::string(value);
            }
            ImGui::SetTooltip("%s", tooltip.c_str());
        }
        ImPlot::EndPlot();
    }
    finishPlotColors();
}
void RecordView::draw(const RecordDataView &s, int &selected, const char *window, bool *open) {
    if (open && !*open)
        return;
    if (ImGui::Begin(window, open,workspace?workspace->prepare(window):0)) {
        if(workspace)workspace->tools(window);
        drawContent(s, selected);
    }
    ImGui::End();
}
void RecordView::drawContent(const RecordDataView &s, int &selected) {
    const auto &receivers = s.traces;
    const int total = static_cast<int>(receivers.size());
    if (total < 1 || s.components.empty()) {
        ImGui::TextUnformatted(tr3D("No receivers in the active model."));
        return;
    }
    // Empty or inconsistent imported traces must not reach times.front/back or
    // the hover sample-index calculation. Keep the previous plot cache unused.
    if (s.components.size() > 3 || !std::isfinite(s.dt) || s.dt <= 0 ||
        std::any_of(receivers.begin(), receivers.end(), [&](const auto &trace) {
            return !std::isfinite(trace.startTime) ||
                   std::any_of(trace.channels.begin(), trace.channels.begin() + s.components.size(),
                               [&](const auto &channel) {
                                   return channel.empty() || channel.size() != trace.channels[0].size();
                               });
        })) {
        ImGui::TextWrapped("%s", l("Records need a positive sample interval and nonempty component traces.",
                                   "记录需要有效的正采样间隔，且各分量道不能为空。"));
        return;
    }
    selected = std::clamp(selected, 0, total - 1);
    const int choices = static_cast<int>(s.components.size()) + (s.components.size() > 1 ? 1 : 0);
    settings.component = std::clamp(settings.component, 0, choices - 1);
    if (ImGui::BeginTable("RecordToolbar", 4, ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("view", ImGuiTableColumnFlags_WidthStretch, 1.25f);
        ImGui::TableSetupColumn("component", ImGuiTableColumnFlags_WidthStretch, .7f);
        ImGui::TableSetupColumn("receiver", ImGuiTableColumnFlags_WidthStretch, 1);
        ImGui::TableSetupColumn("options", ImGuiTableColumnFlags_WidthStretch, 1.1f);
        ImGui::TableNextColumn();
        ImGui::SetNextItemWidth(-1);
        const auto singleLabel = s.components.size() == 3   ? l("Single (3C)", "单道三分量")
                                 : s.components.size() == 2 ? l("Single (2C)", "单道双分量")
                                                            : l("Single trace", "单道波形");
        if (ImGui::BeginCombo("##view", settings.view ? singleLabel : tr3D("Receiver section"))) {
            if (ImGui::Selectable(tr3D("Receiver section"), settings.view == 0)) {
                settings.view = 0;
                fit_ = true;
            }
            if (ImGui::Selectable(singleLabel, settings.view == 1)) {
                settings.view = 1;
                fit_ = true;
            }
            ImGui::EndCombo();
        }
        ImGui::TableNextColumn();
        ImGui::BeginDisabled(settings.view == 1);
        ImGui::SetNextItemWidth(-1);
        if (ImGui::BeginCombo("##component", settings.component < static_cast<int>(s.components.size())
                                                 ? s.components[settings.component].c_str()
                                                 : "|V|")) {
            for (int c = 0; c < choices; ++c)
                if (ImGui::Selectable(c < static_cast<int>(s.components.size()) ? s.components[c].c_str()
                                                                                : "|V|",
                                      settings.component == c)) {
                    settings.component = c;
                    fit_ = true;
                }
            ImGui::EndCombo();
        }
        ImGui::EndDisabled();
        ImGui::TableNextColumn();
        ImGui::SetNextItemWidth(-1);
        int receiver = selected + 1;
        if (ImGui::SliderInt("##receiver", &receiver, 1, total, "R%d"))
            selected = receiver - 1;
        if (ImGui::IsItemHovered()) {
            const auto &p = receivers[selected].position;
            ImGui::SetTooltip(tr3D("R%d XYZ: %.1f, %.1f, %.1f m"), selected + 1, p[0], p[1], p[2]);
        }
        ImGui::TableNextColumn();
        options(s);
        ImGui::EndTable();
    }
    refresh(s, selected);
    ImGui::TextDisabled("%d %s | %zu %s | dt %.4f ms", total, l("traces", "道"), s.sampleCount(selected),
                        l("samples", "样本"), s.dt * 1000);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("raw peak %.5e %s", rawPeak_, s.units.c_str());
    if (settings.agc.enabled || settings.normalizeEach)
        ImGui::TextColored(palette3D().accent, "%s",
                           tr3D(settings.agc.enabled
                                    ? "AGC preview; exported samples remain raw."
                                    : "Per-trace normalization; relative amplitudes are not comparable."));
    if (ImGui::GetContentRegionAvail().y >= 60) {
        if (settings.view == 0)
            section(s, selected);
        else
            single(s, selected);
    }
    fit_ = false;
}
} // namespace SeisWave::Ui
