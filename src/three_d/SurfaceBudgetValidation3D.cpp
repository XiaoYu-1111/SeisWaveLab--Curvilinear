#include "Seis3D/ModelPresets3D.hpp"
#include <fstream>
#include <iomanip>
#include <iostream>
#include <stdexcept>
using namespace Seis3D::Curvilinear;
namespace {
int count=0;
void check(bool ok,const std::string& text){if(!ok)throw std::runtime_error(text);++count;}
}
int main(int argc,char** argv){try {
    constexpr std::uint64_t gib=1024ull*1024*1024;
    SimulationParameters p;p.topographyAmplitude=0;p.maxSteps=2;p.dominantFrequency=.5;
    auto& d=p.surfaceGrid;d.nx=1000;d.ny=800;d.spacing=20;d.datum=1500;d.elevations.assign(800000,1500);
    SimulationParameters full,tiny;std::string error;
    check(d.validate(error),error);
    check(fitSurfaceModel3D(p,BackendKind3D::Cuda,{64*gib,16*gib},full,error),error);
    check(full.nx==1000&&full.ny==800&&full.nz==8,"Per-axis 300 restriction still clips a thin 1000 x 800 grid");
    check(estimateModelMemory3D(full).withinBudget,"Thin wide grid unexpectedly exceeds total point budget");
    check(fitSurfaceModel3D(p,BackendKind3D::Cuda,{gib,gib/4},tiny,error),error);
    check(fitsMemory3D(estimateModelMemory3D(tiny),BackendKind3D::Cuda,{gib,gib/4}),"Dynamic grid exceeds a constrained budget");
    check(tiny.surfaceGrid==d&&tiny.nx<full.nx&&tiny.ny<full.ny,"Budget adaptation destroyed original source array");
    check(d.width()-(tiny.nx-1)*tiny.spacing<tiny.spacing+1e-6&&d.height()-(tiny.ny-1)*tiny.spacing<tiny.spacing+1e-6,"Budget adaptation lost more than one numerical cell at a boundary");
    auto huge=full;huge.nx=huge.ny=4096;huge.nz=8;
    check(!estimateModelMemory3D(huge).withinBudget,"Dynamic axes bypass total padded-node cap");
    if(argc>1) {
        SurfaceGrid3D real;check(readSurfaceGrid3D(std::filesystem::path(argv[1]),real,error),error);
        check(real.nx==1000&&real.ny==800&&real.elevations.size()==800000,"Large DEM shape/count mismatch");
        p.surfaceGrid=std::move(real);
        const auto folder=std::filesystem::path(SEIS3D_OUTPUT_ROOT)/"validation/dem3d-highres";
        std::filesystem::create_directories(folder);
        std::ofstream csv(folder/"result.csv");csv<<std::setprecision(17);
        csv<<"backend,source_nx,source_ny,nx,ny,nz,h_m,physical_nodes,padded_nodes,width_m,height_m,depth_m,dt_s,min_jacobian,finite\n";
        for(auto kind:{BackendKind3D::Cuda,BackendKind3D::Cpu}) {
            if(!queryBackend3D(kind).available)continue;
            auto budget=queryBackendMemory3D(kind);budget.hostAvailableBytes=budget.hostAvailableBytes/100*95;budget.deviceAvailableBytes=budget.deviceAvailableBytes/100*95;
            SimulationParameters fitted;check(fitSurfaceModel3D(p,kind,budget,fitted,error),error);
            const auto memory=estimateModelMemory3D(fitted);
            check(fitted.surfaceGrid==p.surfaceGrid&&memory.withinBudget,"High resolution source was lost or point budget was exceeded");
            std::cout<<"ALLOCATE "<<(kind==BackendKind3D::Cuda?"CUDA":"CPU")<<' '<<fitted.nx<<'x'<<fitted.ny<<'x'<<fitted.nz<<" h="<<fitted.spacing<<" padded="<<memory.points<<'\n'<<std::flush;
            CpuSimulation3D session(kind);check(session.initialize(fitted,error,{8}),error);check(session.advance(2,error),error);
            check(session.diagnostics().allFinite&&session.diagnostics().minJacobian>0,"Large DEM mapping or wavefield is invalid");
            const auto& g=session.grid();
            for(int j:{0,fitted.ny/2,fitted.ny-1})for(int i:{0,fitted.nx/2,fitted.nx-1}) {
                const auto q=g.index(i+g.ghost,j+g.ghost,g.ghost);
                check(std::abs(g.coordinates[q][2]-p.surfaceGrid.displacement(i*fitted.spacing,j*fitted.spacing))<1e-8,"High resolution DEM orientation/height mismatch");
            }
            DisplayFrame3D frame;
            check(session.readDisplayFrame({fitted.nx/2,fitted.ny/2,3,0},frame,error)&&frame.bytes>0,"Large DEM compact display read failed");
            csv<<(kind==BackendKind3D::Cuda?"CUDA":"CPU")<<",1000,800,"<<fitted.nx<<','<<fitted.ny<<','<<fitted.nz<<','<<fitted.spacing<<','
               <<static_cast<std::uint64_t>(fitted.nx)*fitted.ny*fitted.nz<<','<<memory.points<<','<<(fitted.nx-1)*fitted.spacing<<','<<(fitted.ny-1)*fitted.spacing<<','<<(fitted.nz-1)*fitted.spacing<<','<<session.dt()<<','<<session.diagnostics().minJacobian<<",1\n";
            csv.flush();
        }
    }
    std::cout<<"Dynamic DEM budget PASS: "<<count<<" checks\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
