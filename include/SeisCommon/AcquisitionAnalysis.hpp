#pragma once
#include "SeismicRecords.hpp"
#include <filesystem>
#include <optional>
namespace SeisWave {
struct TimeWindow {double begin{},end{};}; // Half-open physical seconds.
struct TraceMetrics {
    std::size_t trace{},signalSamples{},interferenceSamples{};
    double peak{},signalEnergy{},interferenceEnergy{},signalRms{},interferenceRms{};
    std::optional<double> thresholdTime,energyRatio,rmsRatio;
};
bool analyzeAcquisition(const RecordDataView&,int component,TimeWindow signal,TimeWindow interference,
                        double thresholdFraction,std::vector<TraceMetrics>&,std::string&);
bool writeAcquisitionCsv(const RecordDataView&,int component,TimeWindow signal,TimeWindow interference,
                         double thresholdFraction,const std::filesystem::path&,std::string&);
struct PaperRecordOptions {int maxTraces{64};double amplitudeReference{},gain{1};std::string title{"Seismic record"};};
bool writePaperRecordSvg(const RecordDataView&,int component,const std::filesystem::path&,
                         const PaperRecordOptions&,std::string&);
}
