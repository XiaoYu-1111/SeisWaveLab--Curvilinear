#pragma once
#include "Seis3D/SurfaceGrid3D.hpp"
#include "SeisCommon/VtsModelIo.hpp"
#include <array>
#include <fstream>
#include <locale>
#include <sstream>

namespace Seis3D::Curvilinear::SurfaceVtk {
using SeisWave::VtsModel::require;
using SeisWave::VtsModel::tag;
using SeisWave::VtsModel::attribute;
using SeisWave::VtsModel::expected;
inline std::string optional(const std::string& t,const char* name,const char* fallback="") {
    return t.find(std::string(name)+"=")==std::string::npos?fallback:attribute(t,name);
}
template<class T,std::size_t N> std::array<T,N> numbers(const std::string& text) {
    std::istringstream in(text);in.imbue(std::locale::classic());std::array<T,N> a{};
    for(auto& x:a)require(static_cast<bool>(in>>x),"Invalid VTK extent/origin/spacing.");
    in>>std::ws;require(in.eof(),"Unexpected VTK coordinate values.");return a;
}
inline std::size_t components(const std::string& t) {
    const auto n=numbers<int,1>(optional(t,"NumberOfComponents","1"))[0];
    require(n>=1&&n<=9,"Unsupported VTK array components.");return static_cast<std::size_t>(n);
}
inline std::vector<double> array(std::istream& in,const std::string& t,std::size_t count,bool retain=true) {
    require(t.starts_with("<DataArray ")&&optional(t,"format","ascii")=="ascii"&&
            (attribute(t,"type")=="Float64"||attribute(t,"type")=="Float32"||attribute(t,"type")=="Int32"),
            "Surface import requires ASCII VTI/VTS arrays; resave binary/appended/compressed data as ASCII.");
    std::vector<double> values;if(retain)values.reserve(count);
    for(std::size_t n=0;n<count;++n){double v=0;require(static_cast<bool>(in>>v)&&std::isfinite(v),"Truncated or nonfinite surface VTK array.");if(retain)values.push_back(v);}
    expected(in,"</DataArray>");return values;
}
inline bool closeEnough(double a,double b,double h) {return std::abs(a-b)<=std::max(1e-8,1e-8*std::abs(h));}
inline SurfaceGrid3D read(std::istream& in) {
    auto root=tag(in);if(root.starts_with("<?xml"))root=tag(in);
    require(root.starts_with("<VTKFile "),"Only VTI/VTS surface files are supported.");
    const auto type=attribute(root,"type");const bool image=type=="ImageData";
    require(image||type=="StructuredGrid","Expected VTI ImageData or VTS StructuredGrid.");
    const auto grid=tag(in);
    require(grid.starts_with(image?"<ImageData ":"<StructuredGrid "),"Missing VTK grid element.");
    const auto extent=numbers<int,6>(attribute(grid,"WholeExtent"));
    const long long nx=static_cast<long long>(extent[1])-extent[0]+1,ny=static_cast<long long>(extent[3])-extent[2]+1;
    require(nx>=2&&ny>=2&&nx<=maxSurfaceAxis3D&&ny<=maxSurfaceAxis3D&&extent[4]==extent[5]&&
            static_cast<std::size_t>(nx)*ny<=maxSurfaceSamples3D,
            "Surface VTK requires one Z plane, 2..4096 X/Y points and at most 4 million samples; volume/model VTS is not a terrain file.");
    SurfaceGrid3D next;next.nx=static_cast<int>(nx);next.ny=static_cast<int>(ny);
    const auto count=static_cast<std::size_t>(nx)*ny;
    if(image) {
        const auto origin=numbers<double,3>(attribute(grid,"Origin")),spacing=numbers<double,3>(attribute(grid,"Spacing"));
        for(double v:origin)require(std::isfinite(v),"Nonfinite VTI origin.");
        for(double v:spacing)require(std::isfinite(v)&&v>0,"VTI spacing must be positive metres.");
        require(closeEnough(spacing[0],spacing[1],spacing[0]),"Surface VTI needs equal X/Y spacing; resample the region.");
        const auto direction=optional(grid,"Direction");
        if(!direction.empty())require(numbers<double,9>(direction)==std::array<double,9>{1,0,0,0,1,0,0,0,1},"Rotated VTI is unsupported; resample to map X/Y axes.");
        next.spacing=spacing[0];next.originX=origin[0]+extent[0]*spacing[0];next.originY=origin[1]+extent[2]*spacing[1];
    }
    bool hasDatum=false,hasSpacing=false;double datum=0,declaredSpacing=0;auto t=tag(in);
    if(t=="<FieldData>") {
        for(int fields=0;;++fields) {
            t=tag(in);if(t=="</FieldData>")break;
            require(fields<64,"Too many surface metadata arrays.");
            const auto name=attribute(t,"Name");const auto tuples=numbers<int,1>(optional(t,"NumberOfTuples","1"))[0];
            require(tuples>=1&&tuples<=64,"Oversized surface metadata array.");
            const auto a=array(in,t,components(t)*tuples);
            if(name=="ElevationDatum_m"){require(!hasDatum&&a.size()==1,"Invalid or duplicate elevation datum.");datum=a[0];hasDatum=true;}
            if(name=="HorizontalSpacing_m"){require(!hasSpacing&&a.size()==1&&a[0]>0,"Invalid or duplicate horizontal spacing.");declaredSpacing=a[0];hasSpacing=true;}
            if(name=="HorizontalUnitMetres"||name=="ElevationUnitMetres"||name=="ElevationPositiveUp")
                require(a.size()==1&&a[0]==1,"Surface coordinates/heights must be metres, with elevation positive up.");
        }
        t=tag(in);
    } else if(t=="<FieldData/>")t=tag(in);
    require(t.starts_with("<Piece ")&&numbers<int,6>(attribute(t,"Extent"))==extent,"Surface import needs a single Piece covering WholeExtent.");
    t=tag(in);std::vector<double> heights,coordinates;bool namedElevation=false;
    require(t.starts_with("<PointData"),"Missing surface PointData.");
    const auto active=optional(t,"Scalars","Elevation");
    if(!t.ends_with("/>")) {
        for(int fields=0;;++fields) {
            t=tag(in);if(t=="</PointData>")break;
            require(fields<32,"Too many surface point arrays.");
            const auto name=optional(t,"Name");const auto nc=components(t);
            const bool named=name=="Elevation";
            const bool elevation=named||(image&&name==active&&heights.empty());
            if(elevation)require(nc==1,"Elevation must be a scalar point array.");
            auto a=array(in,t,count*nc,elevation);
            if(elevation){require(!namedElevation,"Duplicate surface elevation array.");heights=std::move(a);namedElevation=named;}
        }
    }
    t=tag(in);
    if(t=="<CellData/>"||t=="<CellData />")t=tag(in);
    else if(t=="<CellData>"){expected(in,"</CellData>");t=tag(in);}
    if(!image) {
        require(t=="<Points>","Missing VTS surface point coordinates.");
        t=tag(in);require(components(t)==3,"VTS points require XYZ triples.");coordinates=array(in,t,count*3);
        expected(in,"</Points>");t=tag(in);
        // Use the full span to avoid amplifying cancellation at large projected
        // origins (e.g. X ~19 million m with a 3.333... m output interval).
        const double x0=coordinates[0],y0=coordinates[1];
        double dx=(coordinates[(next.nx-1)*3]-x0)/(next.nx-1);
        double dy=(coordinates[static_cast<std::size_t>(next.ny-1)*next.nx*3+1]-y0)/(next.ny-1);
        require(std::abs(dx)>0&&closeEnough(std::abs(dx),std::abs(dy),dx),"VTS surface XY must be a square-spaced regular grid.");
        if(hasSpacing){require(closeEnough(std::abs(dx),declaredSpacing,declaredSpacing),"VTS spacing metadata disagrees with coordinates.");dx=std::copysign(declaredSpacing,dx);dy=std::copysign(declaredSpacing,dy);}
        next.spacing=std::abs(dx);next.originX=std::min(x0,x0+(next.nx-1)*dx);next.originY=std::min(y0,y0+(next.ny-1)*dy);
        next.elevations.resize(count);
        for(int j=0;j<next.ny;++j)for(int i=0;i<next.nx;++i) {
            const auto q=static_cast<std::size_t>(j)*next.nx+i;const double z=coordinates[3*q+2];
            require(closeEnough(coordinates[3*q],x0+i*dx,dx)&&closeEnough(coordinates[3*q+1],y0+j*dy,dy),"Irregular/skewed VTS XY: resample to a square-spaced DEM grid.");
            if(!heights.empty())require(closeEnough(z,heights[q],next.spacing),"VTS Z and Elevation disagree; terrain Z must be positive-up elevation.");
            const int col=dx>0?i:next.nx-1-i,row=dy>0?j:next.ny-1-j;
            next.elevations[static_cast<std::size_t>(row)*next.nx+col]=z;
        }
    } else {require(heights.size()==count,"VTI needs a scalar Elevation point array.");if(hasSpacing)require(closeEnough(declaredSpacing,next.spacing,next.spacing),"VTI spacing metadata disagrees with image spacing.");next.elevations=std::move(heights);}
    require(t=="</Piece>","Unexpected or multiple VTK Pieces.");
    expected(in,image?"</ImageData>":"</StructuredGrid>");expected(in,"</VTKFile>");
    in>>std::ws;require(in.eof(),"Unexpected trailing surface data.");
    long double sum=0;for(double h:next.elevations)sum+=h;
    next.datum=hasDatum?datum:static_cast<double>(sum/count);
    return next;
}
} // namespace Seis3D::Curvilinear::SurfaceVtk
