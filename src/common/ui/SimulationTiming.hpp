#pragma once
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <span>
#include <string>

namespace SeisWave::Ui {
// Format the solver's measured accepted-step time, not the physical time axis.
inline std::string computeElapsedText(double measuredMs) {
    const auto ms=std::llround(std::isfinite(measuredMs)?std::clamp(measuredMs,0.,1e15):0.);
    char text[64];
    std::snprintf(text,sizeof(text),"%02lld:%02lld:%02lld.%03lld",ms/3600000,(ms/60000)%60,(ms/1000)%60,ms%1000);
    return text;
}
// Match the actual item wrapping so the command strip never clips the timer.
inline int commandStripRows(std::span<const float> widths,float available,float gap) {
    int rows=1;float used=0;
    for(float width:widths){if(used>0&&used+gap+width>available){++rows;used=0;}used+=(used>0?gap:0)+width;}
    return rows;
}
inline const char* computeElapsedHelp(bool chinese) {
    return chinese?
        "累计已完成时间步的实测计算用时，含诊断和接收采样；不含初始化、显示、暂停或导出。暂停后当前步结束才停止累加，清空或重建归零。格式：时:分:秒.毫秒。":
        "Measured time of completed steps, including diagnostics and receiver sampling; excludes initialization, display, pauses and export. A pending step finishes before pausing. Clear/rebuild resets the timer. Format: hours:minutes:seconds.milliseconds.";
}
}
