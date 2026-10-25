#pragma once
#include "Seis3D/CurvilinearSolver.hpp"
#include "Seis3D/ViewState3D.hpp"
#include "RecordPalettes3D.hpp"
#include "Seis3D/AbsorbingBoundary3D.hpp"
#include <algorithm>
#include <array>
#include <cmath>

namespace Seis3D::Curvilinear::Ui {
struct VisualSettings3D {
    int component{0};
    int modelComponent{0}; // Vp, Vs, density. Independent of the five wave components.
    int displayMode{0}; // 0 wavefield, 1 material, 2 material + wavefield.
    int palette{1};
    int modelPalette{12};
    float gain{1.0f};
    float velocityReference{0.003f};
    float stressReference{100000.0f};
    bool autoRange{false};
    bool showGrid{true};
    bool showCrosshair{true};
    bool showColorbar{true};
    bool normalizedColorbar{true}; // Wavefield labels only; colors and raw SI arrays are unchanged.
    bool showWaveColorbar{true},showModelColorbar{true};
    bool modelAutoRange{true};
    std::array<float,3> modelMinimum{0,0,0},modelMaximum{4500,2600,2700};
    double modelLow{},modelHigh{4500};
    float waveOpacity{.9f},waveThreshold{.02f};
    bool showDimensions{true},showCoordinates{true};
    float gridOpacity{.28f};
    bool showTopography{true};
    bool colorTopography{false};
    int terrainPalette{7}; // Same palette IDs as wave/material; default Turbo.
    double terrainLow{},terrainHigh{};
    bool showSlices{true};
    bool showReceivers{true};
    bool showAbsorbingLayer{false};
    float absorbingOpacity{.22f};
    double range{0.01};
    std::array<float,3> canvas{.035f,.055f,.052f};
    std::array<float,3> wire{.46f,.86f,.27f};
};
inline std::array<float,4> absorbingColor3D(std::array<float,4> color,const SimulationParameters& p,int i,int j,int k,const VisualSettings3D& v) {
    if(!v.showAbsorbingLayer||!p.absorbingBoundary.enabled)return color;
    const auto f=absorbingFractions3D(p,i,j,k);
    const float alpha=v.absorbingOpacity*static_cast<float>(std::max({f[0],f[1],f[2]})>0);
    const float tint[3]{1.0f,.59f,.08f};for(int a=0;a<3;++a)color[a]=color[a]*(1-alpha)+tint[a]*alpha;
    return color;
}
inline constexpr const char* kComponents3D="Vz\0Vx\0Vy\0Velocity magnitude\0Pressure (-mean stress)\0";
inline constexpr const char* kModelComponents3D="Material Vp\0Material Vs\0Material density\0";
inline constexpr const char* kPalettes3D=RecordPalettes::wavePaletteNames;
inline constexpr int wavePaletteCount3D=14;
enum class VisualShortcut3D {Component,WavePalette,ModelPalette};
inline void cycleVisual3D(VisualSettings3D& v,VisualShortcut3D action,bool reverse=false) {
    const int direction=reverse?-1:1;
    int& value=action==VisualShortcut3D::Component?v.component:action==VisualShortcut3D::WavePalette?v.palette:v.modelPalette;
    const int count=action==VisualShortcut3D::Component?5:wavePaletteCount3D;
    value=(value+direction+count)%count;
}
inline bool waveVisible3D(const VisualSettings3D& v){return v.displayMode!=1;}
inline bool modelVisible3D(const VisualSettings3D& v){return v.displayMode!=0;}
inline bool waveLegendVisible3D(const VisualSettings3D& v){return v.showColorbar&&waveVisible3D(v)&&v.showWaveColorbar;}
inline bool modelLegendVisible3D(const VisualSettings3D& v){return v.showColorbar&&modelVisible3D(v)&&v.showModelColorbar;}

struct ColorbarScale3D {
    bool normalized{};
    double low{},high{},physicalHigh{};
    const char* physicalUnit{};
    double physicalValue(double tick) const {return normalized?tick*physicalHigh:tick;}
};
inline ColorbarScale3D colorbarScale3D(const VisualSettings3D& v) {
    ColorbarScale3D s;s.normalized=v.normalizedColorbar;
    s.physicalUnit=v.component==4?"Pa":"m/s";
    s.physicalHigh=std::max(v.range,1.0e-20)/std::max(static_cast<double>(v.gain),1.0e-20);
    s.high=s.normalized?1.0:s.physicalHigh;s.low=v.component==3?0:-s.high;
    return s;
}
inline ColorbarScale3D modelColorbarScale3D(const VisualSettings3D& v) {
    return {false,v.modelLow,v.modelHigh,v.modelHigh,v.modelComponent==2?"kg/m3":"m/s"};
}
// Model mapping never reads wave gain, normalization, references or peak values.
inline void updateMaterialRange3D(VisualSettings3D& v,const SimulationParameters& p) {
    const int c=std::clamp(v.modelComponent,0,2);
    double low=v.modelMinimum[c],high=v.modelMaximum[c];
    if(v.modelAutoRange) {
        low=high=c==0?p.vp:c==1?p.vs:p.density;
        if(p.layeredMaterial&&!p.layers.empty()) {
            low=1e300;high=-1e300;
            for(const auto& layer:p.layers){const double value=c==0?layer.vp:c==1?layer.vs:layer.density;low=std::min(low,value);high=std::max(high,value);}
        }
        if(low==high)low=0; // Uniform material still needs a non-degenerate SI legend.
    }
    if(!std::isfinite(low)||!std::isfinite(high)||high<=low){low=0;high=1;}
    v.modelLow=low;v.modelHigh=high;
}

inline std::array<float,4> gridColor3D(const VisualSettings3D& v) {
    const auto& zero=RecordPalettes::wavePalette(modelVisible3D(v)?v.modelPalette:v.palette)[2];
    const float ink=.2126f*zero[0]+.7152f*zero[1]+.0722f*zero[2]>.45f?.04f:.94f;
    return {ink,ink,ink,v.gridOpacity};
}

// Samples physical field values without resetting gain or normalization when components change.
inline double frameValue3D(const DisplayFrame3D& frame,int plane,int a,int b) {
    const auto& data=frame.planes[static_cast<std::size_t>(plane)];
    return data.values[static_cast<std::size_t>(a)+static_cast<std::size_t>(b)*data.width];
}

// The same five stops drive the 3D slices, 2D sections and physical legends.
inline std::array<float,4> paletteColor3D(double normalized,int palette) {
    const double t=std::clamp(normalized,0.0,1.0)*4;
    const int i=std::min(3,static_cast<int>(t));const float f=static_cast<float>(t-i);
    const auto& stops=RecordPalettes::wavePalette(palette);
    return {stops[i][0]+f*(stops[i+1][0]-stops[i][0]),stops[i][1]+f*(stops[i+1][1]-stops[i][1]),stops[i][2]+f*(stops[i+1][2]-stops[i][2]),1};
}
inline constexpr const char* kTerrainPalettes3D=kPalettes3D;
inline int terrainPalettePreference3D(int value,int version) {
    constexpr int legacy[]{7,9,1};
    return version<2?legacy[std::clamp(value,0,2)]:std::clamp(value,0,wavePaletteCount3D-1);
}
inline double terrainElevation3D(double depth,const SimulationParameters& p) {
    return (p.surfaceGrid.empty()?0:p.surfaceGrid.datum)-depth;
}
inline void updateTerrainRange3D(VisualSettings3D& v,const Grid3D& grid,const SimulationParameters& p) {
    v.terrainLow=1e300;v.terrainHigh=-1e300;
    const int g=grid.ghost;
    for(int j=g;j<grid.ny-g;++j)for(int i=g;i<grid.nx-g;++i) {
        const double h=terrainElevation3D(grid.coordinates[grid.index(i,j,g)][2],p);
        v.terrainLow=std::min(v.terrainLow,h);v.terrainHigh=std::max(v.terrainHigh,h);
    }
}
inline std::array<float,4> terrainColor3D(double elevation,const VisualSettings3D& v) {
    const double range=v.terrainHigh-v.terrainLow;
    return paletteColor3D(range>1e-12?(elevation-v.terrainLow)/range:.5,
                          std::clamp(v.terrainPalette,0,wavePaletteCount3D-1));
}
inline std::array<float,4> waveColor3D(double value,const VisualSettings3D& v) {
    const double n=value*v.gain/std::max(v.range,1.0e-20);
    return paletteColor3D(v.component==3?n:(n+1)*.5,v.palette);
}
inline std::array<float,4> modelColor3D(double value,const VisualSettings3D& v) {
    return paletteColor3D((value-v.modelLow)/std::max(v.modelHigh-v.modelLow,1e-20),v.modelPalette);
}
inline std::array<float,4> fieldColor3D(const DisplayFrame3D& wave,const DisplayFrame3D& model,int plane,int a,int b,const VisualSettings3D& v) {
    if(!waveVisible3D(v))return modelColor3D(frameValue3D(model,plane,a,b),v);
    const double value=frameValue3D(wave,plane,a,b);
    auto color=waveColor3D(value,v);
    if(modelVisible3D(v)) {
        const auto background=modelColor3D(frameValue3D(model,plane,a,b),v);
        const double amplitude=std::abs(value*v.gain/std::max(v.range,1e-20));
        double t=std::clamp((amplitude-v.waveThreshold)/.15,0.0,1.0);
        const float alpha=v.waveOpacity*static_cast<float>(t*t*(3-2*t));
        for(int c=0;c<3;++c)color[c]=background[c]*(1-alpha)+color[c]*alpha;
    }
    return color;
}
}
