#pragma once
#include "Seis3D/TraceProcessing.hpp"
#include "SeisCommon/SeismicRecords.hpp"
namespace SeisWave::Ui {
struct WorkspaceTools;
namespace TraceProcessing = Seis3D::Curvilinear::TraceProcessing;
struct RecordSettings {
    int view{}, component{}, style{3}, palette{}, first{}, visible{64};
    float gain{1}, heatmapRange{1}, opacity{.85f};
    bool normalizeEach{}, autoFit{true}, showAll{true};
    TraceProcessing::AgcSettings agc;
    bool operator==(const RecordSettings &) const = default;
};
// A presentation-only viewer. Its cache owns processed copies; raw backend samples are never modified.
class RecordView {
  public:
    RecordSettings settings;
    WorkspaceTools* workspace{};
    std::array<float, 4> optionsBounds{}, plotBounds{};
    bool plotVisible{}; // Last rendered plot, for responsive-layout validation.
    void draw(const RecordDataView &, int &selectedReceiver, const char *window, bool *open = nullptr);
    void drawContent(const RecordDataView &, int &selectedReceiver);
    void invalidate() {
        revision_ = 0;
        fit_ = true;
    }
    void releaseCache() {
        std::vector<Series>().swap(series_);
        std::vector<double>().swap(density_);
        invalidate();
    }

  private:
    struct Series {
        int receiver{}, component{};
        double rawPeak{};
        TraceProcessing::TracePreview preview;
    };
    std::vector<Series> series_;
    std::vector<double> density_;
    std::uint64_t revision_{};
    RecordSettings cachedSettings_;
    int cachedSelected_{-1}, first_{}, count_{}, densityRows_{};
    double rawPeak_{}, commonPeak_{}, endTime_{}, startTime_{}, dt_{};
    bool fit_{true};
    double paperReference_{};
    std::array<double,2> signalWindow_{.05,.15},interferenceWindow_{0,.05};
    double thresholdFraction_{.1};std::string analysisNotice_;
    void options(const RecordDataView&);
    void refresh(const RecordDataView &, int selected);
    void section(const RecordDataView &, int &selected);
    void single(const RecordDataView &, int selected);
};
} // namespace SeisWave::Ui
