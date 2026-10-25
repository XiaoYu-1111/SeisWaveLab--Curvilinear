#include "Seis3D/SurfaceGrid3D.hpp"
#include <fstream>
#include <locale>
#include <cctype>
#include "SurfaceVtkIo3D.hpp"

namespace Seis3D::Curvilinear {
bool SurfaceGrid3D::validate(std::string& error) const {
    if(nx<2||ny<2||nx>maxSurfaceAxis3D||ny>maxSurfaceAxis3D||
       static_cast<std::size_t>(nx)*ny>maxSurfaceSamples3D||elevations.size()!=static_cast<std::size_t>(nx)*ny||
       !std::isfinite(spacing)||spacing<=0||!std::isfinite(originX)||!std::isfinite(originY)||!std::isfinite(datum)||
       !std::isfinite(width())||!std::isfinite(height())) {
        error="DEM array requires 2..4096 rows/columns, at most 4 million samples, positive metre spacing and finite origin/datum.";return false;
    }
    for(double h:elevations)if(!std::isfinite(h)||std::abs(h)>1e7){error="DEM contains missing, nonfinite or unreasonable elevations.";return false;}
    error.clear();return true;
}
bool readSurfaceGrid3D(const std::filesystem::path& path,SurfaceGrid3D& output,std::string& error) {
    try {
        auto suffix=path.extension().string();
        for(char& c:suffix)c=static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        SurfaceVtk::require(suffix==".vti"||suffix==".vts","Surface import supports only .vti and .vts files.");
        std::ifstream in(path);in.imbue(std::locale::classic());
        SurfaceVtk::require(static_cast<bool>(in),"Cannot open surface VTI/VTS file.");
        if(in.peek()==0xEF){char bom[3]{};in.read(bom,3);SurfaceVtk::require(static_cast<unsigned char>(bom[1])==0xBB&&static_cast<unsigned char>(bom[2])==0xBF,"Invalid VTK text encoding.");}
        auto next=SurfaceVtk::read(in);
        if(!next.validate(error))return false;
        output=std::move(next);error.clear();return true;
    } catch(const std::exception& ex){error=ex.what();return false;}
}

} // namespace Seis3D::Curvilinear
