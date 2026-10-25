#pragma once
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <string>
#include <vector>

namespace Seis3D::Curvilinear {
inline constexpr int maxSurfaceAxis3D=4096;
inline constexpr std::size_t maxSurfaceSamples3D=4000000;
// Metre elevations in row-major [y][x]; first row is minimum Y.
// Origin is source-coordinate provenance; the solver uses local metre X/Y.
struct SurfaceGrid3D {
    int nx{},ny{};
    double spacing{},originX{},originY{},datum{};
    std::vector<double> elevations;
    bool operator==(const SurfaceGrid3D&) const = default;
    bool empty() const {return elevations.empty();}
    bool validate(std::string& error) const;
    double width() const {return (nx-1)*spacing;}
    double height() const {return (ny-1)*spacing;}
    // Edge-cell linear extension supports metric derivatives at ghost nodes.
    double elevation(double x,double y) const {
        const double u=x/spacing,v=y/spacing;
        const int i=static_cast<int>(std::floor(std::clamp(u,0.0,static_cast<double>(nx-2))));
        const int j=static_cast<int>(std::floor(std::clamp(v,0.0,static_cast<double>(ny-2))));
        const double a=u-i,b=v-j;
        const auto at=[&](int ix,int iy){return elevations[static_cast<std::size_t>(iy)*nx+ix];};
        return (1-b)*((1-a)*at(i,j)+a*at(i+1,j))+b*((1-a)*at(i,j+1)+a*at(i+1,j+1));
    }
    double displacement(double x,double y) const {return datum-elevation(x,y);}
};
// ASCII VTI ImageData or single-surface VTS StructuredGrid, metre XY/elevations.
// ElevationDatum_m is optional; otherwise the sample mean is used.
bool readSurfaceGrid3D(const std::filesystem::path&,SurfaceGrid3D&,std::string& error);
} // namespace Seis3D::Curvilinear
