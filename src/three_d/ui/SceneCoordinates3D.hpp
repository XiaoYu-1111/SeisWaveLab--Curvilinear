#pragma once
#include <array>

namespace Seis3D::Curvilinear::Ui {
// Physical X/Y are map axes and Z is depth positive down. OpenGL uses Y up
// and Z toward the viewer. Map north (+Y) must point away from the front
// camera, so overhead views have X right and Y up, as in the DEM export.
inline std::array<float,3> scenePosition3D(const std::array<double,3>& p,
                                        double lx,double ly,double lz,double scale) {
    return {static_cast<float>((p[0]-lx*.5)/scale),
            static_cast<float>(-(p[2]-lz*.5)/scale),
            static_cast<float>(-(p[1]-ly*.5)/scale)};
}
inline double sliceVerticalOffset3D(double value,double center,int axis) {
    return (axis==2?-1:1)*(value-center); // XY: Y up; XZ/YZ: depth down.
}
}
