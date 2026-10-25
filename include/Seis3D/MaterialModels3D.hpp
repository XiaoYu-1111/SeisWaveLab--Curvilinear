#pragma once
#include "CurvilinearSolver.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
namespace Seis3D::Curvilinear {
inline constexpr const char* materialPresetNames3D="Uniform elastic\0Loess profile (provided table)\0Low-velocity cover / basement\0Weathered rock (three layers)\0Sedimentary sequence (four layers)\0";
inline SimulationParameters applyMaterialPreset3D(int index,SimulationParameters p) {
    p.layeredMaterial=index!=0;p.layers.clear();
    switch(index) {
    case 1:p.layers={{"Dry loess",5,550,250,1550},{"Wet / compacted loess",5,800,350,1800},{"Clay / paleosol",20,1500,700,2050},{"Sandstone",20,2500,1300,2250},{"Hard-rock basement",0,4000,2300,2650}};break;
    case 2:p.layers={{"Low-velocity cover",15,800,350,1800},{"Basement",0,2500,1300,2300}};break;
    case 3:p.layers={{"Strongly weathered rock",8,900,400,1900},{"Moderately weathered rock",17,2000,1000,2250},{"Fresh rock",0,4000,2300,2650}};break;
    case 4:p.layers={{"Loose cover",5,650,280,1700},{"Clay",10,1400,600,2000},{"Sandstone",25,2600,1400,2350},{"Basement",0,4500,2600,2700}};break;
    default:p.layeredMaterial=false;break;
    }
    return p;
}
inline double materialDepth3D(const SimulationParameters& p,const Grid3D& g,std::size_t q) {
    const auto& xyz=g.coordinates[q];
    if(p.layerDepthMode==LayerDepthMode3D::GridIndex)return (static_cast<int>(q/(static_cast<std::size_t>(g.nx)*g.ny))-g.ghost)*g.dz;
    if(p.layerDepthMode==LayerDepthMode3D::Datum)return xyz[2];
    // Use the actual mapped surface, including imported DEM and ghost extension.
    const auto plane=static_cast<std::size_t>(g.nx)*g.ny;
    const auto surfaceIndex=static_cast<std::size_t>(g.ghost)*plane+q%plane;
    return xyz[2]-g.coordinates[surfaceIndex][2];
}
inline int materialLayerAtDepth3D(const SimulationParameters& p,double depth) {
    double bottom=0;for(std::size_t i=0;i+1<p.layers.size();++i){bottom+=p.layers[i].thickness;if(depth<bottom)return static_cast<int>(i);}
    return static_cast<int>(p.layers.size())-1;
}
inline bool validateMaterial3D(const SimulationParameters& p,std::string& error) {
    if(!p.layeredMaterial)return true;
    if(p.layerDepthMode<LayerDepthMode3D::GridIndex||p.layerDepthMode>LayerDepthMode3D::Datum){error="Invalid layer depth coordinate mode.";return false;}
    if(p.layers.size()<2||p.layers.size()>8){error="Layered material requires 2..8 layers.";return false;}
    for(std::size_t i=0;i<p.layers.size();++i){const auto& l=p.layers[i];
        if(!std::isfinite(l.thickness)||!std::isfinite(l.vp)||!std::isfinite(l.vs)||!std::isfinite(l.density)||l.vp<=0||l.vs<=0||l.density<=0||l.vp*l.vp<=4*l.vs*l.vs/3||!std::isfinite(l.density*l.vp*l.vp)||l.density*l.vs*l.vs<=0||
            (i+1==p.layers.size()?l.thickness!=0:l.thickness<=0)) {error="Invalid layer: positive finite Vp/Vs/density and bulk modulus required; last thickness must be zero (auto).";return false;}
    }
    return true;
}
}
