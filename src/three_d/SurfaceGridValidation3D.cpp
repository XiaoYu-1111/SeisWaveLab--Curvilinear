#include "Seis3D/CurvilinearSolver.hpp"
#include "Seis3D/MaterialModels3D.hpp"
#include "Seis3D/ViewState3D.hpp"
#include <fstream>
#include <iostream>
#include <limits>
#include <iomanip>
#include <sstream>
#include <stdexcept>
using namespace Seis3D::Curvilinear;
namespace {
int checks=0;
void check(bool ok,const std::string& why){if(!ok)throw std::runtime_error(why);++checks;}
std::string fixture(const SurfaceGrid3D& d,bool image) {
    std::ostringstream out;out.imbue(std::locale::classic());out<<std::setprecision(17);
    const char* type=image?"ImageData":"StructuredGrid";
    const std::string extent="0 "+std::to_string(d.nx-1)+" 0 "+std::to_string(d.ny-1)+" 0 0";
    out<<"<?xml version=\"1.0\"?><VTKFile type=\""<<type<<"\"><"<<type<<" WholeExtent=\""<<extent<<'\"';
    if(image)out<<" Origin=\""<<d.originX<<' '<<d.originY<<" 0\" Spacing=\""<<d.spacing<<' '<<d.spacing<<" 1\"";
    out<<"><FieldData><DataArray type=\"Float64\" Name=\"ElevationDatum_m\" NumberOfTuples=\"1\" format=\"ascii\">"<<d.datum<<"</DataArray></FieldData>"
       <<"<Piece Extent=\""<<extent<<"\"><PointData Scalars=\"Elevation\"><DataArray type=\"Float64\" Name=\"Elevation\" format=\"ascii\">";
    for(double h:d.elevations)out<<h<<' ';
    out<<"</DataArray></PointData><CellData/>";
    if(!image){out<<"<Points><DataArray type=\"Float64\" NumberOfComponents=\"3\" format=\"ascii\">";
        for(int j=0;j<d.ny;++j)for(int i=0;i<d.nx;++i)out<<d.originX+i*d.spacing<<' '<<d.originY+j*d.spacing<<' '<<d.elevations[static_cast<std::size_t>(j)*d.nx+i]<<' ';
        out<<"</DataArray></Points>";}
    out<<"</Piece></"<<type<<"></VTKFile>";return out.str();
}
void exercise(const SimulationParameters& p) {
    std::string error;CpuSimulation3D cpu;
    check(cpu.initialize(p,error,{1}),error);
    const auto& g=cpu.grid();
    for(int j=0;j<p.ny;++j)for(int i=0;i<p.nx;++i) {
        const auto q=g.index(i+g.ghost,j+g.ghost,g.ghost);
        check(std::abs(g.coordinates[q][2]-p.surfaceGrid.displacement(i*p.spacing,j*p.spacing))<1e-9,"Surface elevation/sign/orientation mismatch");
        const auto bottom=g.index(i+g.ghost,j+g.ghost,p.nz-1+g.ghost);
        check(std::abs(g.coordinates[bottom][2]-(p.nz-1)*p.spacing)<1e-9,"DEM bottom is not horizontal");
        auto below=p;below.layerDepthMode=LayerDepthMode3D::BelowSurface;
        check(std::abs(materialDepth3D(below,g,q))<1e-9,"Below-surface material depth is not zero at DEM surface");
    }
    check(cpu.diagnostics().minJacobian>0&&cpu.dt()>0,"Invalid geometry/CFL");
    check(cpu.advance(12,error)&&cpu.diagnostics().allFinite,error);
    const auto before=cpu.revision();auto outside=p;outside.nx=300;
    check(!cpu.initialize(outside,error,{1})&&cpu.revision()==before,"DEM coverage failure did not preserve previous session");
    if(queryBackend3D(BackendKind3D::Cuda).available) {
        CpuSimulation3D gpu(BackendKind3D::Cuda);
        check(gpu.initialize(p,error)&&gpu.advance(12,error)&&gpu.diagnostics().allFinite,error);
        check(gpu.dt()==cpu.dt(),"CPU/CUDA DEM time steps differ");
        const auto a=cpu.snapshot(),b=gpu.snapshot();double peak=0,diff=0;
        for(std::size_t q=0;q<a.vz.size();++q){peak=std::max(peak,std::abs(a.vz[q]));diff=std::max(diff,std::abs(a.vz[q]-b.vz[q]));}
        check(peak>0&&diff/std::max(peak,1e-30)<1e-8,"CPU/CUDA DEM Vz disagreement");
        std::cout<<"CPU/CUDA DEM relative Vz error: "<<diff/peak<<'\n';
    }
}
}
int main(int argc,char** argv){try {
    SurfaceGrid3D d;d.nx=17;d.ny=15;d.spacing=20;d.originX=19300000;d.originY=4150000;d.datum=1500;
    for(int j=0;j<d.ny;++j)for(int i=0;i<d.nx;++i)d.elevations.push_back(1500+.01*i*d.spacing-.02*j*d.spacing);
    std::string error;check(d.validate(error),error);
    const auto parseRoot=std::filesystem::path(SEIS3D_OUTPUT_ROOT)/"validation/dem3d-parser";
    std::filesystem::create_directories(parseRoot);
    for(bool image:{true,false}) {
        const auto path=parseRoot/(image?"asymmetric_h20m.vti":"asymmetric_h20m.vts");
        {std::ofstream out(path);out<<fixture(d,image);}
        SurfaceGrid3D read;check(readSurfaceGrid3D(path,read,error),error);check(read==d,"VTI/VTS round trip changed heights, origin, spacing, orientation or datum");
    }
    const auto valid=fixture(d,true);
    const auto replaced=[&](std::string text,const std::string& a,const std::string& b){const auto pos=text.find(a);check(pos!=std::string::npos,"Invalid parser fixture");text.replace(pos,a.size(),b);return text;};
    const std::vector<std::string> badFiles={"BAD 1\n",valid.substr(0,valid.size()/2),valid+"extra",
        replaced(valid,"Spacing=\"20 20 1\"","Spacing=\"20 30 1\""),
        replaced(valid,"Spacing=\"20 20 1\"","Spacing=\"0 0 1\""),
        replaced(valid,"format=\"ascii\"","format=\"binary\""),
        replaced(valid,"Name=\"Elevation\"","Name=\"NotElevation\""),
        replaced(valid,"WholeExtent=\"0 16 0 14 0 0\"","WholeExtent=\"0 16 0 14 0 2\""),
        replaced(valid,"WholeExtent=\"0 16 0 14 0 0\"","WholeExtent=\"0 4095 0 4095 0 0\""),
        replaced(valid,"1500 1500.2","nan 1500.2"),
        replaced(fixture(d,false),"NumberOfComponents=\"3\"","NumberOfComponents=\"2\"")};
    for(std::size_t i=0;i<badFiles.size();++i) {
        const auto path=parseRoot/("invalid-"+std::to_string(i)+".vti");
        {std::ofstream out(path);out<<badFiles[i];}
        auto retained=d;check(!readSurfaceGrid3D(path,retained,error)&&retained==d,"Invalid file accepted or replaced retained DEM");
    }
    for(const auto* suffix:{".sdem",".asc"}){const auto path=parseRoot/(std::string("unsupported")+suffix);
        {std::ofstream out(path);out<<valid;}auto retained=d;check(!readSurfaceGrid3D(path,retained,error)&&retained==d,"Unsupported interface accepted");}
    check(std::abs(d.elevation(57,83)-(1500+.57-1.66))<1e-10,"Bilinear interpolation failed");
    check(std::abs(d.elevation(-40,-20)-(1500-.4+.4))<1e-10,"Ghost extrapolation failed");
    auto invalid=d;invalid.elevations[3]=std::numeric_limits<double>::quiet_NaN();check(!invalid.validate(error),"NoData accepted");
    SimulationParameters p;p.nx=d.nx;p.ny=d.ny;p.nz=40;p.spacing=d.spacing;p.surfaceGrid=d;p.topographyAmplitude=0;p.dominantFrequency=3;p.duration=.2;p.maxSteps=12;
    exercise(p);
    auto fine=p;fine.nx=33;fine.ny=29;fine.spacing=10;exercise(fine);
    check(geometryChanged3D(p,fine),"Geometry state ignores imported terrain");
    auto changed=p;changed.surfaceGrid.elevations[0]+=.1;check(geometryChanged3D(p,changed),"DEM edit does not trigger geometry framing");
    if(argc>1) {
        SurfaceGrid3D real;check(readSurfaceGrid3D(std::filesystem::path(argv[1]),real,error),error);
        p.surfaceGrid=real;p.nx=real.nx;p.ny=real.ny;p.nz=61;p.spacing=real.spacing;exercise(p);
        std::cout<<"Imported DEM: "<<real.nx<<" x "<<real.ny<<", h="<<real.spacing<<" m, datum="<<real.datum<<" m\n";
    }
    std::cout<<"DEM 3D validation PASS: "<<checks<<" checks\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
