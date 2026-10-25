#pragma once
#include "Seis3D/CurvilinearSolver.hpp"
#include <algorithm>
#include <array>
#include <cmath>

namespace Seis3D::Curvilinear {
struct SliceIndices3D { int x{},y{},z{3}; };
struct SliceView3D { float zoom{1}; std::array<float,2> pan{}; };

// Includes every parameter that changes physical geometry; material, source and compute settings preserve framing.
inline bool geometryChanged3D(const SimulationParameters& a,const SimulationParameters& b) {
    return a.nx!=b.nx || a.ny!=b.ny || a.nz!=b.nz || a.spacing!=b.spacing ||
        a.topographyAmplitude!=b.topographyAmplitude || a.topographyWavelength!=b.topographyWavelength ||
        a.terrainTransitionDepth!=b.terrainTransitionDepth || a.surfaceGrid!=b.surfaceGrid;
}

// Maps positions by normalized index, preventing old absolute indices from drifting to an edge after resizing.
inline int remapIndex3D(int value,int previousCount,int nextCount) {
    if(previousCount<=1)return std::max(0,nextCount/2);
    const double fraction=std::clamp(value,0,previousCount-1)/static_cast<double>(previousCount-1);
    return std::clamp(static_cast<int>(std::lround(fraction*(nextCount-1))),0,nextCount-1);
}

// Apply only after a successful backend rebuild; failed initialization must leave the complete view state untouched.
inline bool reconcileViews3D(const SimulationParameters* previous,const SimulationParameters& next,
    int previousReceivers,int nextReceivers,SliceIndices3D& slices,std::array<SliceView3D,3>& views,
    int& selectedReceiver,bool autoFit) {
    const bool changed=!previous || geometryChanged3D(*previous,next);
    if(!previous) {slices={next.nx/2,next.ny/2,std::min(3,next.nz-1)};selectedReceiver=nextReceivers/2;}
    else {
        slices={remapIndex3D(slices.x,previous->nx,next.nx),remapIndex3D(slices.y,previous->ny,next.ny),remapIndex3D(slices.z,previous->nz,next.nz)};
        selectedReceiver=remapIndex3D(selectedReceiver,previousReceivers,nextReceivers);
    }
    selectedReceiver=std::clamp(selectedReceiver,0,std::max(0,nextReceivers-1));
    if(changed && autoFit)views={};
    return changed && autoFit;
}
}
