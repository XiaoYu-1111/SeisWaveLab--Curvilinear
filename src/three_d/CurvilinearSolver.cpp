#include "Seis3D/CurvilinearSolver.hpp"
#include "PointKernels3D.hpp"
#include "Seis3D/MaterialModels3D.hpp"
#include "Seis3D/ReceiverGeometry3D.hpp"
#include "Seis3D/AbsorbingBoundary3D.hpp"
#include "Seis3D/ModelPresets3D.hpp"
#include "Seis3D/TimeStepPlan3D.hpp"
#ifdef _WIN32
#include <windows.h>
#elif defined(__linux__)
#include <unistd.h>
#endif
#include <chrono>
#ifdef SEIS3D_HAS_CUDA
#include "CudaEngine3D.hpp"
#endif

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <optional>
#include <type_traits>
#ifdef SEIS3D_HAS_OPENMP
#include <omp.h>
#endif

namespace Seis3D::Curvilinear {
namespace {

using Vector3 = std::array<double, 3>;
constexpr double kPi = 3.14159265358979323846;
constexpr int kVx = 0, kVy = 1, kVz = 2;
constexpr int kSxx = 3, kSyy = 4, kSzz = 5, kSxy = 6, kSxz = 7, kSyz = 8;

struct State {
    std::array<std::vector<double>, 9> field;
    explicit State(std::size_t count = 0) { for (auto& values : field) values.assign(count, 0.0); }
};

Vector3 cross(const Vector3& a, const Vector3& b) {
    return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}

double dot(const Vector3& a, const Vector3& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

double derivativeCoordinate(const Grid3D& grid, int component, int i, int j, int k, int axis) {
    // Coordinate arrays are small compared with wavefields; avoid a permanent duplicate by using direct indexing.
    auto at = [&](int x, int y, int z) { return grid.coordinates[grid.index(x, y, z)][component]; };
    int di = axis == 0 ? 1 : 0, dj = axis == 1 ? 1 : 0, dk = axis == 2 ? 1 : 0;
    const double h = axis == 0 ? grid.dx : axis == 1 ? grid.dy : grid.dz;
    return (at(i - 2 * di, j - 2 * dj, k - 2 * dk) - 8.0 * at(i - di, j - dj, k - dk)
          + 8.0 * at(i + di, j + dj, k + dk) - at(i + 2 * di, j + 2 * dj, k + 2 * dk)) / (12.0 * h);
}

Point::Fields pointFields(State& state) {
    Point::Fields fields;for(int f=0;f<9;++f)fields.f[f]=state.field[f].data();return fields;
}
Point::Mesh pointMesh(const Grid3D& grid,double lambda=0,double mu=0,double density=1) {
    Point::Mesh mesh;mesh.nx=grid.nx;mesh.ny=grid.ny;mesh.nz=grid.nz;mesh.count=grid.coordinates.size();mesh.h=grid.dx;
    mesh.lambda=lambda;mesh.mu=mu;mesh.density=density;
    mesh.metrics=reinterpret_cast<const double*>(grid.inverseMetrics.data());
    mesh.normals=reinterpret_cast<const double*>(grid.surfaceNormal.data());mesh.jacobian=grid.jacobian.data();return mesh;
}
void projectFreeSurface(State& state,const Grid3D& grid) {
    const auto w=pointFields(state);const auto mesh=pointMesh(grid);
    for(int j=grid.ghost;j<grid.ny-grid.ghost;++j)for(int i=grid.ghost;i<grid.nx-grid.ghost;++i)Point::project(w,mesh,i,j);
}

void evaluateRhs(State& work, State& rhs, const Grid3D& grid, const SimulationParameters& p,
                 double lambda, double mu, double time,const std::vector<double>& sourceWeights,
                 const std::vector<double>& dampingRates,const Point::Mesh& materialMesh,int threads,int step,int stage) {
    for (auto& values : rhs.field) std::fill(values.begin(), values.end(), 0.0);
    (void)lambda;(void)mu;
    const int g = grid.ghost;
    const double sourceScale = (p.source.kind==SeisWave::SourceKind::IsotropicMoment?p.sourceMoment:p.sourceForce) / (p.density * grid.dx * grid.dy * grid.dz);
    const double tau = kPi * p.dominantFrequency * (time - 1.5 / p.dominantFrequency);
    const double source = sourceScale * (1.0 - 2.0 * tau * tau) * std::exp(-tau * tau);
    auto mesh=materialMesh;mesh.source=sourceWeights.data();mesh.damping=dampingRates.data();
    const auto input=pointFields(work),output=pointFields(rhs);
#ifdef SEIS3D_HAS_OPENMP
    #pragma omp parallel for schedule(static) num_threads(threads) if(threads>1 && grid.physicalPointCount()>=4096)
#else
    (void)threads;
#endif
    for (int k = g; k < grid.nz - g; ++k) for (int j = g; j < grid.ny - g; ++j) for (int i = g; i < grid.nx - g; ++i) {
        Point::rhs(input,output,mesh,grid.index(i,j,k),source,step,stage);
    }
}

void combine(State& out, const State& base, const State& a, double wa, const State* b = nullptr,
             double wb = 0.0, const State* c = nullptr, double wc = 0.0, const State* d = nullptr, double wd = 0.0,int threads=1) {
#ifdef SEIS3D_HAS_OPENMP
    #pragma omp parallel for schedule(static) num_threads(threads) if(threads>1 && out.field[0].size()>=8192)
#else
    (void)threads;
#endif
    for (int f = 0; f < 9; ++f) for (std::size_t i = 0; i < out.field[f].size(); ++i)
        out.field[f][i] = base.field[f][i] + wa * a.field[f][i]
            + (b ? wb * b->field[f][i] : 0.0) + (c ? wc * c->field[f][i] : 0.0) + (d ? wd * d->field[f][i] : 0.0);
}

} // namespace

std::size_t Grid3D::index(int i, int j, int k) const noexcept {
    return static_cast<std::size_t>(i) + static_cast<std::size_t>(nx) * (static_cast<std::size_t>(j) + static_cast<std::size_t>(ny) * static_cast<std::size_t>(k));
}

std::size_t Grid3D::physicalPointCount() const noexcept {
    return static_cast<std::size_t>(nx - 2 * ghost) * static_cast<std::size_t>(ny - 2 * ghost) * static_cast<std::size_t>(nz - 2 * ghost);
}

GridLayout3D Grid3D::layout() const noexcept {
    return {nx,ny,nz,ghost,static_cast<std::uint64_t>(nx),static_cast<std::uint64_t>(nx)*ny,static_cast<std::uint64_t>(nx)*ny*nz};
}

bool Grid3D::validateGeometry(std::string& error) {
    if (ghost!=ghostWidth3D || nx < 8+2*ghost || ny < 8+2*ghost || nz < 8+2*ghost || nx>maxPhysicalNodesPerAxis3D+2*ghost || ny>maxPhysicalNodesPerAxis3D+2*ghost || nz>maxPhysicalNodesPerAxis3D+2*ghost ||
        !std::isfinite(dx) || !std::isfinite(dy) || !std::isfinite(dz) || dx <= 0.0 || dy <= 0.0 || dz <= 0.0 ||
        !std::isfinite(topographyAmplitude) || !std::isfinite(topographyWavelength) || topographyWavelength<=0.0 ||
        !std::isfinite(terrainTransitionDepth) || terrainTransitionDepth<0.0) {
        error = "Grid dimensions, ghost width, spacing or topography parameters are invalid."; return false;
    }
    const std::size_t count = static_cast<std::size_t>(nx) * ny * nz;
    if(count>maxPaddedNodes3D) { error="Grid exceeds the 28,652,616 padded-node total budget; redistribute X/Y/Z nodes."; return false; }
    const double bottomDepth=(nz-2*ghost-1)*dz;
    if(!surfaceGrid.empty()) {
        if(!surfaceGrid.validate(error))return false;
        if((nx-2*ghost-1)*dx>surfaceGrid.width()+1e-8*surfaceGrid.spacing ||
           (ny-2*ghost-1)*dy>surfaceGrid.height()+1e-8*surfaceGrid.spacing) {
            error="Imported DEM does not cover the staged X/Y model span; reduce dimensions/spacing or export a larger region.";return false;
        }
    }
    double relief=std::abs(topographyAmplitude);
    if(!surfaceGrid.empty()) {
        relief=0;
        for(int j=0;j<ny;++j)for(int i=0;i<nx;++i)
            relief=std::max(relief,std::abs(surfaceGrid.displacement((i-ghost)*dx,(j-ghost)*dy)));
    }
    if(terrainTransitionDepth==0.0)terrainTransitionDepth=std::min(bottomDepth,std::max({0.35*bottomDepth,6.0*dz,4.0*relief+2.0*dz}));
    if(terrainTransitionDepth>bottomDepth || (hasTerrain() && (terrainTransitionDepth<4.0*dz || terrainTransitionDepth<=2.6*relief))) {
        error="Terrain transition must fit the model depth and exceed 2.6 times relief amplitude (at least four cells)."; return false;
    }
    return true;
}
bool Grid3D::buildGeometry(std::string& error) {
    if(!validateGeometry(error))return false;
    const std::size_t count=static_cast<std::size_t>(nx)*ny*nz;
    coordinates.resize(count); inverseMetrics.assign(count, {}); jacobian.assign(count, 0.0); surfaceNormal.assign(count, {});
    for (int k = 0; k < nz; ++k) for (int j = 0; j < ny; ++j) for (int i = 0; i < nx; ++i) {
        const double x = (i - ghost) * dx, y = (j - ghost) * dy, depth = (k - ghost) * dz;
        if(!hasTerrain()){coordinates[index(i,j,k)]={x,y,depth};continue;}
        const double phaseX = 2.0 * kPi * x / topographyWavelength;
        const double phaseY = 2.0 * kPi * y / topographyWavelength;
        const double surface = surfaceGrid.empty()?topographyAmplitude*std::cos(phaseX)*std::cos(phaseY):surfaceGrid.displacement(x,y);
        // Ninth-degree smoothstep has four vanishing derivatives at both ends; deep layers and the bottom are exactly horizontal.
        const double s=std::clamp(depth/terrainTransitionDepth,0.0,1.0);
        const double fade=depth>=terrainTransitionDepth?0.0:1.0-std::pow(s,5)*(126.0+s*(-420.0+s*(540.0+s*(-315.0+70.0*s))));
        coordinates[index(i, j, k)] = {x, y, depth+surface*fade};
    }
    for (int k = ghost; k < nz - ghost; ++k) for (int j = ghost; j < ny - ghost; ++j) for (int i = ghost; i < nx - ghost; ++i) {
        if(!hasTerrain()){const auto q=index(i,j,k);inverseMetrics[q]={1,0,0,0,1,0,0,0,1};jacobian[q]=1;if(k==ghost)surfaceNormal[q]={0,0,1};continue;}
        Vector3 rXi{}, rEta{}, rZeta{};
        for (int c = 0; c < 3; ++c) {
            rXi[c] = derivativeCoordinate(*this, c, i, j, k, 0);
            rEta[c] = derivativeCoordinate(*this, c, i, j, k, 1);
            rZeta[c] = derivativeCoordinate(*this, c, i, j, k, 2);
        }
        const Vector3 etaCrossZeta = cross(rEta, rZeta);
        const Vector3 zetaCrossXi = cross(rZeta, rXi);
        const Vector3 xiCrossEta = cross(rXi, rEta);
        const double jac = dot(rXi, etaCrossZeta);
        if (!(jac > 1.0e-10) || !std::isfinite(jac)) { error = "Curvilinear mapping has a non-positive or non-finite Jacobian."; return false; }
        const std::size_t q = index(i, j, k);
        jacobian[q] = jac;
        for (int c = 0; c < 3; ++c) {
            inverseMetrics[q][c] = etaCrossZeta[c] / jac;
            inverseMetrics[q][3 + c] = zetaCrossXi[c] / jac;
            inverseMetrics[q][6 + c] = xiCrossEta[c] / jac;
        }
        if (k == ghost) {
            Vector3 n = xiCrossEta;
            const double length = std::sqrt(dot(n, n));
            if (!(length > 0.0) || !std::isfinite(length)) { error = "Could not compute topographic surface normal."; return false; }
            for (double& value : n) value /= length;
            surfaceNormal[q] = n;
        }
    }
    return true;
}

// Keeps all large buffers allocated across frames and records receivers at the native simulation interval.
struct CpuSimulation3D::Impl {
    Grid3D grid;
    SimulationParameters parameters;
    Diagnostics3D diagnostics;
    State state, k1, k2, k3, k4, stage, base;
    std::vector<double> sourceWeights;
    std::array<std::vector<double>,3> sourceGradient;
    std::vector<double> dampingRates;
    std::vector<double> materialLambda,materialMu,materialDensity;
    Point::Mesh mesh() const {auto result=pointMesh(grid,lambda,mu,parameters.density);const auto direction=SeisWave::unitSourceDirection(parameters.source);for(int a=0;a<3;++a){result.sourceDirection[a]=direction[a];result.sourceGradient[a]=sourceGradient[a].empty()?nullptr:sourceGradient[a].data();}result.sourceAmplitude=parameters.source.kind==SeisWave::SourceKind::IsotropicMoment?parameters.sourceMoment:parameters.sourceForce;if(!materialLambda.empty()){result.materialLambda=materialLambda.data();result.materialMu=materialMu.data();result.materialDensity=materialDensity.data();}return result;}
    std::vector<ReceiverTrace3D> receiverTraces;
    double dt{}, lambda{}, mu{};
    int step{}, steps{};
    BackendInfo3D backend;
    std::string fault;
    double sourcePeakEnergy{};
#ifdef SEIS3D_HAS_CUDA
    std::unique_ptr<CudaEngine3D> cuda;
#endif

    // Updates observable diagnostics after a batch, including the unprojected traction-rate residual.
    void updateDiagnostics() {
#ifdef SEIS3D_HAS_CUDA
        if(cuda){cuda->diagnostics(diagnostics);return;}
#endif
        diagnostics.maxAbsVelocity=0.0;
        diagnostics.maxTractionRateRelative=0.0;
        diagnostics.allFinite=true;
        diagnostics.displayPeaks={};
        diagnostics.elasticEnergy=0;
        const auto material=mesh();const auto fields=pointFields(state);
        double maxRateNorm=0.0, maxRateTraction=0.0;
        const int g=grid.ghost;
        for(int k=g;k<grid.nz-g;++k) for(int j=g;j<grid.ny-g;++j) for(int i=g;i<grid.nx-g;++i) {
            const auto q=grid.index(i,j,k);
            for(int f=0;f<9;++f) diagnostics.allFinite=diagnostics.allFinite && std::isfinite(state.field[f][q]);
            const double v2=state.field[0][q]*state.field[0][q]+state.field[1][q]*state.field[1][q]+state.field[2][q]*state.field[2][q];
            diagnostics.maxAbsVelocity=std::max(diagnostics.maxAbsVelocity,std::sqrt(v2));
            const std::array<double,5> display{std::abs(state.field[kVz][q]),std::abs(state.field[kVx][q]),std::abs(state.field[kVy][q]),std::sqrt(v2),std::abs((state.field[kSxx][q]+state.field[kSyy][q]+state.field[kSzz][q])/3.0)};
            for(int component=0;component<5;++component)diagnostics.displayPeaks[component]=std::max(diagnostics.displayPeaks[component],display[component]);
            diagnostics.elasticEnergy+=Point::energyDensity(fields,material,q);
            diagnostics.allFinite=diagnostics.allFinite&&std::isfinite(v2);
            if(k==g) {
                const auto& n=grid.surfaceNormal[q];
                const Vector3 traction{
                    k4.field[kSxx][q]*n[0]+k4.field[kSxy][q]*n[1]+k4.field[kSxz][q]*n[2],
                    k4.field[kSxy][q]*n[0]+k4.field[kSyy][q]*n[1]+k4.field[kSyz][q]*n[2],
                    k4.field[kSxz][q]*n[0]+k4.field[kSyz][q]*n[1]+k4.field[kSzz][q]*n[2]};
                maxRateTraction=std::max(maxRateTraction,std::sqrt(dot(traction,traction)));
                double norm2=0.0;
                for(int f=3;f<9;++f) norm2+=(f>=6?2.0:1.0)*k4.field[f][q]*k4.field[f][q];
                maxRateNorm=std::max(maxRateNorm,std::sqrt(norm2));
            }
        }
        diagnostics.maxTractionRateRelative=maxRateTraction/std::max(maxRateNorm,1.0e-30);
        diagnostics.allFinite=diagnostics.allFinite&&std::isfinite(diagnostics.elasticEnergy)&&std::isfinite(diagnostics.maxTractionRateRelative);
        diagnostics.peakEnergy=std::max(diagnostics.peakEnergy,diagnostics.elasticEnergy);
    }
};

// Creates an empty session; initialization performs all allocations before replacing existing state.
CpuSimulation3D::CpuSimulation3D(BackendKind3D kind):requestedBackend_(kind){}
CpuSimulation3D::~CpuSimulation3D()=default;

// Validates finite inputs, positive elastic bulk modulus, a CPU memory limit and a metric-aware CFL estimate.
bool validateSimulationConfiguration3D(const SimulationParameters& p,BackendKind3D kind,std::string& error,const ComputeSettings3D& settings) {
    error.clear();
    try {
    const auto availability=queryBackend3D(kind,settings.cudaDevice);
    if(!availability.available){error=availability.description;return false;}
    if(!std::isfinite(p.cflSafety)||p.cflSafety<minCflCoefficient3D||p.cflSafety>maxCflCoefficient3D){error="CFL coefficient must be finite and in 0.02..0.8.";return false;}
    if(p.maxSteps<0 || p.maxSteps>maxTimeSteps3D) {error="Maximum steps must be 0 (duration) or 1..20,000.";return false;}
    if(!std::isfinite(p.absorbingBoundary.strength)||p.absorbingBoundary.strength<0.1||p.absorbingBoundary.strength>10.0){error="Sponge strength must be in 0.1..10.";return false;}
    for(int width:p.absorbingBoundary.cells)if(width<1||width>64){error="Sponge widths must be 1..64 cells.";return false;}
    const std::array<double,9> scalars{p.spacing,p.vp,p.vs,p.density,p.dominantFrequency,p.duration,p.topographyAmplitude,p.topographyWavelength,p.terrainTransitionDepth};
    for(double value:scalars) if(!std::isfinite(value)) { error="Simulation parameters must be finite."; return false; }
    if(p.nx<8 || p.ny<8 || p.nz<8 || p.nx>maxPhysicalNodesPerAxis3D || p.ny>maxPhysicalNodesPerAxis3D || p.nz>maxPhysicalNodesPerAxis3D ||
       p.spacing<=0.0 || p.vp<=0.0 || p.vs<=0.0 || p.density<=0.0 ||
       p.vp*p.vp<=4.0*p.vs*p.vs/3.0 || p.dominantFrequency<=0.0 || p.dominantFrequency>1.0e6 || p.duration<=0.0 || p.duration>10.0 ||
       p.topographyWavelength<=0.0 || p.terrainTransitionDepth<0.0 || settings.cpuThreads<0 || settings.cpuThreads>maxCpuThreads3D ||
       (p.surfaceGrid.empty() && p.topographyAmplitude!=0.0 && p.topographyWavelength<4.0*p.spacing)) {
        error="Require 8..4096 nodes per axis within the total node/memory budget, positive finite parameters and resolved topography.";
        return false;
    }
    const std::size_t points=static_cast<std::size_t>(p.nx+2*ghostWidth3D)*(p.ny+2*ghostWidth3D)*(p.nz+2*ghostWidth3D);
    if(points>maxPaddedNodes3D) { error="Grid exceeds the 28,652,616 padded-node total budget; redistribute X/Y/Z nodes."; return false; }
    if(!SeisWave::validateSource(p.source,false,error))return false;
    if(!std::isfinite(p.sourceForce)||!std::isfinite(p.sourceMoment)||(p.source.kind==SeisWave::SourceKind::IsotropicMoment?p.sourceMoment:p.sourceForce)<=0){error="Source amplitudes must be finite and the active force (N) or moment (N*m) must be positive.";return false;}
    const auto source=sourceIndices3D(p);
    if(p.source.kind==SeisWave::SourceKind::IsotropicMoment &&
       (p.nx<11||p.ny<11||p.nz<11||source[0]<5||source[0]>p.nx-6||source[1]<5||source[1]>p.ny-6||source[2]<5||source[2]>p.nz-6)) {
        error="Isotropic moment needs 11+ nodes per axis and source center five nodes inside each face.";return false;
    }
    if(p.sourceX< -1||p.sourceY< -1||source[0]<2||source[0]>p.nx-3||source[1]<2||source[1]>p.ny-3||source[2]<2||source[2]>p.nz-3) {
        error="Source must stay two nodes inside every physical face; X/Y may be -1 for center.";return false;
    }
    if(!validateMaterial3D(p,error))return false;
    std::vector<std::array<int,3>> receiverNodes;if(!receiverNodes3D(p,receiverNodes,error))return false;
    Grid3D grid;grid.nx=p.nx+2*ghostWidth3D;grid.ny=p.ny+2*ghostWidth3D;grid.nz=p.nz+2*ghostWidth3D;
    grid.dx=grid.dy=grid.dz=p.spacing;grid.topographyAmplitude=p.topographyAmplitude;
    grid.topographyWavelength=p.topographyWavelength;grid.terrainTransitionDepth=p.terrainTransitionDepth;grid.surfaceGrid=p.surfaceGrid;
    if(!grid.validateGeometry(error))return false;
    const double lambda=p.density*(p.vp*p.vp-2*p.vs*p.vs),mu=p.density*p.vs*p.vs;
    if(!std::isfinite(lambda)||!std::isfinite(mu)||mu<=0){error="Derived elastic coefficients are out of range.";return false;}
    return true;
    } catch(const std::exception& ex){error=std::string("Configuration validation failed: ")+ex.what();return false;}
}

bool CpuSimulation3D::initialize(const SimulationParameters& p,std::string& error,const ComputeSettings3D& settings) {
    const auto initializationStart=std::chrono::steady_clock::now();
    error.clear();
    if(!validateSimulationConfiguration3D(p,requestedBackend_,error,settings))return false;
    const auto availability=queryBackend3D(requestedBackend_,settings.cudaDevice);
    const auto source=sourceIndices3D(p);
    std::vector<std::array<int,3>> receiverNodes;if(!receiverNodes3D(p,receiverNodes,error))return false;
    const std::size_t points=static_cast<std::size_t>(p.nx+2*ghostWidth3D)*(p.ny+2*ghostWidth3D)*(p.nz+2*ghostWidth3D);
    if(!fitsMemory3D(estimateModelMemory3D(p),requestedBackend_,queryBackendMemory3D(requestedBackend_,availability.cudaDevice))){error="Insufficient available memory for the requested grid. Current session retained; reduce dimensions or use Fit to available memory.";return false;}
    try {
        auto next=std::make_unique<Impl>();
        next->parameters=p;
        next->backend.kind=requestedBackend_;next->backend.deviceName=availability.description;next->backend.cudaDevice=availability.cudaDevice;
#ifdef SEIS3D_HAS_OPENMP
        next->backend.openMpAvailable=true;
        next->backend.workerThreads=settings.cpuThreads==0?std::min(8,std::max(1,omp_get_num_procs())):settings.cpuThreads;
#else
        if(requestedBackend_==BackendKind3D::Cpu&&settings.cpuThreads>1){error="This build has no OpenMP support. Select auto or one CPU thread.";return false;}
#endif
        auto& grid=next->grid;
        grid.nx=p.nx+2*ghostWidth3D; grid.ny=p.ny+2*ghostWidth3D; grid.nz=p.nz+2*ghostWidth3D;
        grid.dx=grid.dy=grid.dz=p.spacing;
        grid.topographyAmplitude=p.topographyAmplitude;
        grid.topographyWavelength=p.topographyWavelength;
        grid.terrainTransitionDepth=p.terrainTransitionDepth;
        grid.surfaceGrid=p.surfaceGrid;
        if(!grid.buildGeometry(error)) return false;
        next->diagnostics.minJacobian=std::numeric_limits<double>::max();
        double maxMetricNorm=0.0;
        for(int k=grid.ghost;k<grid.nz-grid.ghost;++k) for(int j=grid.ghost;j<grid.ny-grid.ghost;++j) for(int i=grid.ghost;i<grid.nx-grid.ghost;++i) {
            const auto q=grid.index(i,j,k);
            next->diagnostics.minJacobian=std::min(next->diagnostics.minJacobian,grid.jacobian[q]);
            double norm2=0.0;
            for(double value:grid.inverseMetrics[q]) norm2+=value*value;
            maxMetricNorm=std::max(maxMetricNorm,std::sqrt(norm2));
        }
        next->diagnostics.maxMetricNorm=maxMetricNorm;
        // CGFD3D blk_dt_esti_curv: smallest distance from a node to each of eight adjacent planes.
        double minLength=!grid.hasTerrain()?p.spacing/std::sqrt(3.0):std::numeric_limits<double>::max();
        if(grid.hasTerrain())for(int k=grid.ghost;k<grid.nz-grid.ghost;++k)for(int j=grid.ghost;j<grid.ny-grid.ghost;++j)for(int i=grid.ghost;i<grid.nx-grid.ghost;++i) {
            const auto& center=grid.coordinates[grid.index(i,j,k)];
            for(int sk:{-1,1})for(int sj:{-1,1})for(int si:{-1,1}) {
                const auto& a=grid.coordinates[grid.index(i+si,j,k)];const auto& b=grid.coordinates[grid.index(i,j+sj,k)];const auto& c=grid.coordinates[grid.index(i,j,k+sk)];
                Vector3 ab{},ac{},ap{};for(int axis=0;axis<3;++axis){ab[axis]=b[axis]-a[axis];ac[axis]=c[axis]-a[axis];ap[axis]=center[axis]-a[axis];}
                const auto normal=cross(ab,ac);const double length=std::sqrt(dot(normal,normal));
                const double distance=std::abs(dot(ap,normal))/length;
                if(!std::isfinite(distance)||distance<=0){error="Degenerate adjacent cell plane in CFL geometry.";return false;}
                minLength=std::min(minLength,distance);
            }
        }
        next->diagnostics.minCellLength=minLength;
        double maxEdge=!grid.hasTerrain()?p.spacing:0;
        if(grid.hasTerrain())for(int k=grid.ghost;k<grid.nz-grid.ghost;++k)for(int j=grid.ghost;j<grid.ny-grid.ghost;++j)for(int i=grid.ghost;i<grid.nx-grid.ghost;++i) {
            const auto& a=grid.coordinates[grid.index(i,j,k)];
            for(int axis=0;axis<3;++axis) {
                const int ni=i+(axis==0),nj=j+(axis==1),nk=k+(axis==2);
                if(ni>=grid.nx-grid.ghost||nj>=grid.ny-grid.ghost||nk>=grid.nz-grid.ghost)continue;
                const auto& b=grid.coordinates[grid.index(ni,nj,nk)];Vector3 d{};
                for(int c=0;c<3;++c)d[c]=b[c]-a[c];maxEdge=std::max(maxEdge,std::sqrt(dot(d,d)));
            }
        }
        next->diagnostics.maxCellEdge=maxEdge;
        next->lambda=p.density*(p.vp*p.vp-2.0*p.vs*p.vs);
        next->mu=p.density*p.vs*p.vs;
        if(!std::isfinite(next->lambda) || !std::isfinite(next->mu) || next->mu<=0.0 || !std::isfinite(maxMetricNorm)) { error="Derived elastic coefficients or grid metrics are out of range."; return false; }
        double maxVp=p.vp,minVs=p.vs,maxVs=p.vs,maxDensity=p.density;
        if(p.layeredMaterial) {
            maxVp=maxVs=maxDensity=0;minVs=std::numeric_limits<double>::max();
            for(const auto& layer:p.layers){maxVp=std::max(maxVp,layer.vp);minVs=std::min(minVs,layer.vs);maxVs=std::max(maxVs,layer.vs);maxDensity=std::max(maxDensity,layer.density);}
            next->materialLambda.resize(points);next->materialMu.resize(points);next->materialDensity.resize(points);
            std::vector<std::size_t> counts(p.layers.size());
            for(std::size_t q=0;q<points;++q){const int id=materialLayerAtDepth3D(p,materialDepth3D(p,grid,q));const auto& l=p.layers[id];
                next->materialLambda[q]=l.density*(l.vp*l.vp-2*l.vs*l.vs);next->materialMu[q]=l.density*l.vs*l.vs;next->materialDensity[q]=l.density;
                const int i=static_cast<int>(q%grid.nx),j=static_cast<int>((q/grid.nx)%grid.ny),k=static_cast<int>(q/(static_cast<std::size_t>(grid.nx)*grid.ny));
                if(i>=grid.ghost&&i<grid.nx-grid.ghost&&j>=grid.ghost&&j<grid.ny-grid.ghost&&k>=grid.ghost&&k<grid.nz-grid.ghost)++counts[id];
            }
            next->diagnostics.minLayerCells=std::numeric_limits<double>::max();
            const double layerSpacing=p.layerDepthMode==LayerDepthMode3D::GridIndex?p.spacing:maxEdge;
            for(std::size_t id=0;id<p.layers.size();++id){const bool finite=id+1<p.layers.size();if(finite)next->diagnostics.minLayerCells=std::min(next->diagnostics.minLayerCells,p.layers[id].thickness/layerSpacing);if(counts[id]==0||(finite&&p.layers[id].thickness/layerSpacing<2))++next->diagnostics.unresolvedLayers;}
        }
        next->diagnostics.materialMaxVp=maxVp;next->diagnostics.materialMinVs=minVs;next->diagnostics.materialMaxVs=maxVs;next->diagnostics.materialMaxDensity=maxDensity;
        double maxDamping=0;
        next->dampingRates.assign(points,0.0);
        for(int k=grid.ghost;k<grid.nz-grid.ghost;++k)for(int j=grid.ghost;j<grid.ny-grid.ghost;++j)for(int i=grid.ghost;i<grid.nx-grid.ghost;++i) {
            const double rate=absorbingRate3D(p,i-grid.ghost,j-grid.ghost,k-grid.ghost,maxVp);next->dampingRates[grid.index(i,j,k)]=rate;maxDamping=std::max(maxDamping,rate);
        }
        TimeStepPlan3D plan;if(!timeStepPlan3D(p,maxVp,maxMetricNorm,minLength,maxDamping,plan,error))return false;
        next->diagnostics.waveDtLimit=plan.waveLimit;next->diagnostics.dampingDtLimit=plan.dampingLimit;next->diagnostics.sourceDtLimit=plan.sourceLimit;
        next->dt=plan.dt;
        next->diagnostics.maxDampingDt=maxDamping*next->dt;
        next->diagnostics.stepsPerDominantPeriod=1.0/(p.dominantFrequency*next->dt);
        next->steps=plan.steps;
        if(requestedBackend_==BackendKind3D::Cpu)
            for(State* state:{&next->state,&next->k1,&next->k2,&next->k3,&next->k4,&next->stage,&next->base})*state=State(points);
        // A normalized compact Gaussian reduces short-wave source content; it does not eliminate Nyquist content.
        // The physical support is two cells per direction; this finite-source approximation is recorded in the guide.
        next->sourceWeights.assign(points,0.0);
        double sourceSum=0.0;
        for(int k=-2;k<=2;++k)for(int j=-2;j<=2;++j)for(int i=-2;i<=2;++i)sourceSum+=std::exp(-0.5*(i*i+j*j+k*k));
        for(int k=-2;k<=2;++k)for(int j=-2;j<=2;++j)for(int i=-2;i<=2;++i) {
            const auto q=grid.index(grid.ghost+source[0]+i,grid.ghost+source[1]+j,grid.ghost+source[2]+k);
            next->sourceWeights[q]=std::exp(-0.5*(i*i+j*j+k*k))/(sourceSum*grid.jacobian[q]);
        }
        if(p.source.kind==SeisWave::SourceKind::IsotropicMoment) {
            for(auto &v:next->sourceGradient)v.assign(points,0);
            const auto flux=[&](std::ptrdiff_t q,int axis,int component) {return grid.jacobian[q]*grid.inverseMetrics[q][3*axis+component]*next->sourceWeights[q];};
            for(int k=source[2]-4;k<=source[2]+4;++k)for(int j=source[1]-4;j<=source[1]+4;++j)for(int i=source[0]-4;i<=source[0]+4;++i) {
                const auto q=grid.index(i+grid.ghost,j+grid.ghost,k+grid.ghost);
                const auto force=SeisWave::isotropicMomentPattern(q,3,{1,grid.nx,static_cast<std::ptrdiff_t>(grid.nx)*grid.ny},p.spacing,grid.jacobian[q],flux);
                for(int a=0;a<3;++a)next->sourceGradient[a][q]=force[a];
            }
        }
        const int receiverCount=static_cast<int>(receiverNodes.size());
        next->receiverTraces.resize(receiverCount);
        for(int r=0;r<receiverCount;++r) {
            const auto& node=receiverNodes[r];
            auto& receiver=next->receiverTraces[r];
            receiver.gridIndex=grid.index(node[0]+grid.ghost,node[1]+grid.ghost,node[2]+grid.ghost);
            receiver.position=grid.coordinates[receiver.gridIndex];
            receiver.samples.reserve(static_cast<std::size_t>(next->steps)+1);
            receiver.samples.push_back(0.0);
            receiver.samplesVx.reserve(static_cast<std::size_t>(next->steps)+1);receiver.samplesVy.reserve(static_cast<std::size_t>(next->steps)+1);
            receiver.samplesVx.push_back(0.0);receiver.samplesVy.push_back(0.0);
        }
        auto memoryParameters=p;memoryParameters.maxSteps=next->steps;
        const auto memory=estimateModelMemory3D(memoryParameters);
        next->diagnostics.estimatedMemoryMiB=requestedBackend_==BackendKind3D::Cuda?memory.cudaHostMiB+memory.deviceMiB:memory.cpuMiB;
        next->diagnostics.pointsPerShortestWavelength=minVs/(2.5*p.dominantFrequency*maxEdge);
#ifdef SEIS3D_HAS_CUDA
        if(requestedBackend_==BackendKind3D::Cuda) {
            auto mesh=next->mesh();mesh.source=next->sourceWeights.data();mesh.damping=next->dampingRates.data();
            next->cuda=std::make_unique<CudaEngine3D>(mesh,next->receiverTraces,availability.cudaDevice);
            next->backend.workerThreads=0;
        }
#endif
        next->updateDiagnostics();
        next->diagnostics.timing.initializationMs=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-initializationStart).count();
        impl_=std::move(next);
        ++generation_;
        return true;
    } catch(const std::exception& ex) { error=std::string("Backend initialization failed: ")+ex.what();return false; }
}

// Advances a bounded number of complete RK4 steps and leaves the session inspectable on failure.
bool CpuSimulation3D::advance(int count,std::string& error) {
    error.clear();
    if(!impl_) { error="Initialize a model before stepping."; return false; }
    if(count<0 || count>maxTimeSteps3D) { error="Invalid step batch."; return false; }
    auto& s=*impl_;
    if(!s.fault.empty()){error=s.fault;return false;}
    if(!s.diagnostics.allFinite) { error="Fields are non-finite. Rebuild the session."; return false; }
    const auto& p=s.parameters;
    const int end=s.step+std::min(count,s.steps-s.step);
    auto acceptedDiagnostics=s.diagnostics;
    try {
    for(;s.step<end;++s.step) {
        const auto stepStart=std::chrono::steady_clock::now();
        const auto previousDiagnostics=s.diagnostics;
#ifdef SEIS3D_HAS_CUDA
        if(s.cuda)s.cuda->step(s.step*s.dt,s.dt,p.dominantFrequency,s.step);
        else {
#endif
        s.base=s.state;
        const double t=s.step*s.dt;
        evaluateRhs(s.state,s.k1,s.grid,p,s.lambda,s.mu,t,s.sourceWeights,s.dampingRates,s.mesh(),s.backend.workerThreads,s.step,0);
        combine(s.stage,s.base,s.k1,0.5*s.dt,nullptr,0,nullptr,0,nullptr,0,s.backend.workerThreads); projectFreeSurface(s.stage,s.grid);
        evaluateRhs(s.stage,s.k2,s.grid,p,s.lambda,s.mu,t+0.5*s.dt,s.sourceWeights,s.dampingRates,s.mesh(),s.backend.workerThreads,s.step,1);
        combine(s.stage,s.base,s.k2,0.5*s.dt,nullptr,0,nullptr,0,nullptr,0,s.backend.workerThreads); projectFreeSurface(s.stage,s.grid);
        evaluateRhs(s.stage,s.k3,s.grid,p,s.lambda,s.mu,t+0.5*s.dt,s.sourceWeights,s.dampingRates,s.mesh(),s.backend.workerThreads,s.step,2);
        combine(s.stage,s.base,s.k3,s.dt,nullptr,0,nullptr,0,nullptr,0,s.backend.workerThreads); projectFreeSurface(s.stage,s.grid);
        evaluateRhs(s.stage,s.k4,s.grid,p,s.lambda,s.mu,t+s.dt,s.sourceWeights,s.dampingRates,s.mesh(),s.backend.workerThreads,s.step,3);
        combine(s.state,s.base,s.k1,s.dt/6.0,&s.k2,s.dt/3.0,&s.k3,s.dt/3.0,&s.k4,s.dt/6.0,s.backend.workerThreads);
        projectFreeSurface(s.state,s.grid);
#ifdef SEIS3D_HAS_CUDA
        }
#endif
        s.updateDiagnostics();
        if((s.step+1)*s.dt<=3.0/p.dominantFrequency)s.sourcePeakEnergy=std::max(s.sourcePeakEnergy,s.diagnostics.elasticEnergy);
        const bool growth=(s.step+1)*s.dt>3.0/p.dominantFrequency && s.sourcePeakEnergy>0 && s.diagnostics.elasticEnergy>1e4*s.sourcePeakEnergy;
        if(!s.diagnostics.allFinite||growth) {
#ifdef SEIS3D_HAS_CUDA
            if(s.cuda)s.cuda->rollback();else
#endif
            s.state=s.base;
            s.diagnostics=previousDiagnostics;
            s.fault=growth?"Energy grew above 10,000 times the source-window peak. Last accepted step retained; rebuild with smaller CFL or gentler terrain.":"Non-finite RK4 step rejected. Last accepted fields retained; rebuild with smaller CFL.";
            error=s.fault;return false;
        }
#ifdef SEIS3D_HAS_CUDA
        if(s.cuda)s.cuda->sample(s.receiverTraces);else
#endif
            for(auto& receiver:s.receiverTraces) {
                receiver.samples.push_back(s.state.field[kVz][receiver.gridIndex]);
                receiver.samplesVx.push_back(s.state.field[kVx][receiver.gridIndex]);receiver.samplesVy.push_back(s.state.field[kVy][receiver.gridIndex]);
            }
        auto& timing=s.diagnostics.timing;
        timing.lastStepMs=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-stepStart).count();
        timing.totalStepMs+=timing.lastStepMs;timing.minStepMs=timing.measuredSteps?std::min(timing.minStepMs,timing.lastStepMs):timing.lastStepMs;timing.maxStepMs=std::max(timing.maxStepMs,timing.lastStepMs);++timing.measuredSteps;
        acceptedDiagnostics=s.diagnostics;
    }
    } catch(const std::exception& ex){
        s.diagnostics=acceptedDiagnostics;
        try {
#ifdef SEIS3D_HAS_CUDA
            if(s.cuda)s.cuda->rollback();else
#endif
            s.state=s.base;
        } catch(...){s.diagnostics.allFinite=false;}
        s.fault=std::string("Compute backend failed: ")+ex.what()+". Rebuild the session.";error=s.fault;return false;
    }
    if(!s.diagnostics.allFinite) { error="Simulation produced non-finite fields; stop and reduce the time/grid distortion."; return false; }
    return true;
}
bool CpuSimulation3D::resetFields(std::string& error) {
    error.clear();if(!impl_){error="Initialize a model before stepping.";return false;}
    auto& s=*impl_;
    try {
#ifdef SEIS3D_HAS_CUDA
        if(s.cuda)s.cuda->resetFields();else
#endif
        for(State* state:{&s.state,&s.k1,&s.k2,&s.k3,&s.k4,&s.stage,&s.base})for(auto& f:state->field)std::fill(f.begin(),f.end(),0);
        s.step=0;s.sourcePeakEnergy=0;s.fault.clear();s.diagnostics.peakEnergy=0;
        const double initialization=s.diagnostics.timing.initializationMs;s.diagnostics.timing={};s.diagnostics.timing.initializationMs=initialization;
        for(auto& r:s.receiverTraces){r.samples.assign(1,0);r.samplesVx.assign(1,0);r.samplesVy.assign(1,0);}
        s.updateDiagnostics();++generation_;return true;
    }catch(const std::exception& ex){s.fault=ex.what();error=s.fault;return false;}
}

// Returns lightweight session metadata and const field storage for renderers.
bool CpuSimulation3D::initialized() const noexcept { return bool(impl_); }
bool CpuSimulation3D::completed() const noexcept { return impl_ && impl_->step>=impl_->steps; }
int CpuSimulation3D::currentStep() const noexcept { return impl_?impl_->step:0; }
int CpuSimulation3D::totalSteps() const noexcept { return impl_?impl_->steps:0; }
double CpuSimulation3D::dt() const noexcept { return impl_?impl_->dt:0.0; }
const Grid3D& CpuSimulation3D::grid() const { if(!impl_) throw std::logic_error("Uninitialized session"); return impl_->grid; }
const SimulationParameters& CpuSimulation3D::parameters() const { if(!impl_) throw std::logic_error("Uninitialized session"); return impl_->parameters; }
const Diagnostics3D& CpuSimulation3D::diagnostics() const { if(!impl_) throw std::logic_error("Uninitialized session"); return impl_->diagnostics; }
const std::vector<double>& CpuSimulation3D::field(Field3D component) const { if(!impl_) throw std::logic_error("Uninitialized session");if(requestedBackend_!=BackendKind3D::Cpu)throw std::logic_error("Use snapshot() for explicit CUDA volume downloads");return impl_->state.field.at(static_cast<std::size_t>(component)); }
const std::vector<ReceiverTrace3D>& CpuSimulation3D::receivers() const { if(!impl_) throw std::logic_error("Uninitialized session"); return impl_->receiverTraces; }

std::uint64_t CpuSimulation3D::revision() const noexcept {return (generation_<<32)|static_cast<std::uint64_t>(currentStep());}
BackendInfo3D CpuSimulation3D::backendInfo() const noexcept {return impl_?impl_->backend:BackendInfo3D{};}

// Transfers only the three requested scalar planes; CUDA gathers them directly from resident device fields.
bool CpuSimulation3D::readDisplayFrame(const DisplayRequest3D& request,DisplayFrame3D& frame,std::string& error) const {
    error.clear();
    if(!impl_){error="Initialize a model before requesting slices.";return false;}
    const auto& s=*impl_;const auto& p=s.parameters;const auto& g=s.grid;
    if(request.x<0 || request.x>=p.nx || request.y<0 || request.y>=p.ny || request.z<0 || request.z>=p.nz || request.component<0 || request.component>7) {
        error="Display request is outside the active physical grid.";return false;
    }
    if(!s.diagnostics.allFinite || (request.component<5?s.diagnostics.displayPeaks[request.component]:request.component==5?s.diagnostics.materialMaxVp:request.component==6?s.diagnostics.materialMaxVs:s.diagnostics.materialMaxDensity)>std::numeric_limits<float>::max()) {error="Field is outside the display buffer range.";return false;}
    auto value=[&](int i,int j,int k) {
        const auto q=g.index(i+g.ghost,j+g.ghost,k+g.ghost);
        if(request.component>=5){const auto m=s.mesh();const double density=Point::localDensity(m,q),mu=Point::localMu(m,q);return request.component==5?std::sqrt((Point::localLambda(m,q)+2*mu)/density):request.component==6?std::sqrt(mu/density):density;}
        if(request.component==0)return s.state.field[kVz][q];
        if(request.component==1)return s.state.field[kVx][q];
        if(request.component==2)return s.state.field[kVy][q];
        if(request.component==4)return -(s.state.field[kSxx][q]+s.state.field[kSyy][q]+s.state.field[kSzz][q])/3.0;
        return std::sqrt(s.state.field[kVx][q]*s.state.field[kVx][q]+s.state.field[kVy][q]*s.state.field[kVy][q]+s.state.field[kVz][q]*s.state.field[kVz][q]);
    };
    try {
#ifdef SEIS3D_HAS_CUDA
        if(s.cuda){s.cuda->slices(request,frame);frame.request=request;frame.revision=revision();return true;}
#endif
        const std::array<int,3> widths{p.nx,p.ny,p.nx},heights{p.nz,p.nz,p.ny};frame.bytes=0;
        for(int plane=0;plane<3;++plane) {
            auto& output=frame.planes[plane];output.width=widths[plane];output.height=heights[plane];
            output.values.resize(static_cast<std::size_t>(output.width)*output.height);
            for(int b=0;b<output.height;++b)for(int a=0;a<output.width;++a)
                output.values[static_cast<std::size_t>(a)+static_cast<std::size_t>(b)*output.width]=static_cast<float>(plane==0?value(a,request.y,b):plane==1?value(request.x,a,b):value(a,b,request.z));
            frame.bytes+=output.values.size()*sizeof(float);
        }
        frame.request=request;frame.revision=revision();return true;
    } catch(const std::exception& ex){error=std::string("Slice acquisition failed: ")+ex.what();return false;}
}

BackendMemory3D CpuSimulation3D::residentMemory() const noexcept {
    BackendMemory3D m;if(!impl_)return m;const auto& s=*impl_;
    m.hostAvailableBytes=sizeof(Impl);
    const auto add=[&](const auto& v){m.hostAvailableBytes+=v.capacity()*sizeof(typename std::decay_t<decltype(v)>::value_type);};
    add(s.parameters.surfaceGrid.elevations);add(s.grid.surfaceGrid.elevations);
    add(s.grid.coordinates);add(s.grid.inverseMetrics);add(s.grid.jacobian);add(s.grid.surfaceNormal);
    for(const auto* state:{&s.state,&s.k1,&s.k2,&s.k3,&s.k4,&s.stage,&s.base})for(const auto& v:state->field)add(v);
    add(s.sourceWeights);add(s.dampingRates);for(const auto& v:s.sourceGradient)add(v);
    add(s.materialLambda);add(s.materialMu);add(s.materialDensity);add(s.receiverTraces);
    for(const auto& r:s.receiverTraces){add(r.samples);add(r.samplesVx);add(r.samplesVy);}
#ifdef SEIS3D_HAS_CUDA
    if(s.cuda){const auto cuda=s.cuda->residentMemory();m.hostAvailableBytes+=cuda.hostAvailableBytes;m.deviceAvailableBytes=cuda.deviceAvailableBytes;}
#endif
    return m;
}

BackendMemory3D memoryForRebuild3D(const ISimulationBackend3D* active,BackendKind3D kind,int device) {
    auto available=queryBackendMemory3D(kind,device);
    if(!active||!active->initialized())return available;
    const auto owned=active->residentMemory();const auto old=active->backendInfo();
    const auto add=[](std::uint64_t free,std::uint64_t released){return free?free+std::min(released,std::numeric_limits<std::uint64_t>::max()-free):0;};
    available.hostAvailableBytes=add(available.hostAvailableBytes,owned.hostAvailableBytes);
    if(kind==BackendKind3D::Cuda&&old.kind==kind&&queryBackend3D(kind,device).cudaDevice==old.cudaDevice)
        available.deviceAvailableBytes=add(available.deviceAvailableBytes,owned.deviceAvailableBytes);
    return available;
}

RebuildOutcome3D rebuildSimulation3D(std::unique_ptr<ISimulationBackend3D>& active,const SimulationParameters& parameters,BackendKind3D kind,const ComputeSettings3D& settings,std::string& error) {
    RebuildOutcome3D result;
    try {
    // Own the inputs even if a caller passed active->parameters().
    const auto requested=parameters;
    if(!validateSimulationConfiguration3D(requested,kind,error,settings))return result;
    if(!fitsMemory3D(estimateModelMemory3D(requested),kind,memoryForRebuild3D(active.get(),kind,settings.cudaDevice))) {
        error="Requested model exceeds memory available after releasing the active grid. Current session retained.";return result;
    }
    auto candidate=createSimulationBackend3D(kind,error);if(!candidate)return result;
    const bool hadActive=active&&active->initialized();
    const std::optional<SimulationParameters> previous=hadActive?std::optional<SimulationParameters>(active->parameters()):std::nullopt;
    const auto previousBackend=hadActive?active->backendInfo():BackendInfo3D{};
    if(hadActive){result.reclaimed=active->residentMemory();result.released=true;}
    active.reset(); // Frees CPU vectors and CUDA buffers before any new volume allocation.
    active=std::move(candidate);
    if(active->initialize(requested,error,settings)){result.success=true;return result;}
    const std::string failure=error;
    if(previous) {
        auto restored=createSimulationBackend3D(previousBackend.kind,error);
        const ComputeSettings3D oldSettings{previousBackend.kind==BackendKind3D::Cpu?previousBackend.workerThreads:0,previousBackend.cudaDevice};
        if(restored&&restored->initialize(*previous,error,oldSettings)) {
            active=std::move(restored);result.restored=true;
            error=failure+" Previous model/settings restored at t=0; old wavefields and records were released.";return result;
        }
    }
    // If external pressure prevents restoring the old volume, keep a small,
    // usable CPU session so the UI can display the error and accept edits.
    auto fallback=createSimulationBackend3D(BackendKind3D::Cpu,error);SimulationParameters small;
    small.nx=small.ny=small.nz=8;small.topographyAmplitude=0;small.maxSteps=1;
    if(fallback&&fallback->initialize(small,error,{1})) {active=std::move(fallback);result.fallback=true;}
    error=failure+" Previous model could not be restored. "+(result.fallback?"A small CPU session is active; review parameters and rebuild.":"No model could be allocated; return to Home and free system memory.");
    return result;
    } catch(const std::exception& ex){error=std::string("Rebuild failed: ")+ex.what();return result;}
}

std::unique_ptr<ISimulationBackend3D> createSimulationBackend3D(BackendKind3D kind,std::string& error) {
    error.clear();const auto availability=queryBackend3D(kind);
    if(!availability.available){error=availability.description;return nullptr;}
    return std::make_unique<CpuSimulation3D>(kind);
}

// Copies a point-in-time export snapshot; normal rendering reads fields without this full-volume copy.
SimulationResult CpuSimulation3D::snapshot() const {
    if(!impl_) throw std::logic_error("Uninitialized session");
    const auto& s=*impl_;
    SimulationResult result;
    result.grid=s.grid; result.steps=s.step; result.dt=s.dt;
    result.modelVp=s.parameters.vp; result.modelVs=s.parameters.vs;
    result.modelDensity=s.parameters.density;
    result.sourceMechanism=s.parameters.source;result.sourceForce=s.parameters.sourceForce;
    result.sourceMoment=s.parameters.sourceMoment;result.sourceFrequency=s.parameters.dominantFrequency;
    if(s.parameters.layeredMaterial){const auto count=s.grid.coordinates.size();result.materialVp.resize(count);result.materialVs.resize(count);result.materialDensity=s.materialDensity;result.layerIds.resize(count);for(std::size_t q=0;q<count;++q){result.materialVp[q]=std::sqrt((s.materialLambda[q]+2*s.materialMu[q])/s.materialDensity[q]);result.materialVs[q]=std::sqrt(s.materialMu[q]/s.materialDensity[q]);result.layerIds[q]=materialLayerAtDepth3D(s.parameters,materialDepth3D(s.parameters,s.grid,q));}}

    const auto source=sourceIndices3D(s.parameters);result.sourcePosition=s.grid.coordinates[s.grid.index(source[0]+s.grid.ghost,source[1]+s.grid.ghost,source[2]+s.grid.ghost)];
    const auto* fields=&s.state.field;
#ifdef SEIS3D_HAS_CUDA
    std::array<std::vector<double>,9> downloaded;
    if(s.cuda){s.cuda->download(downloaded);fields=&downloaded;}
#endif
    result.vx=(*fields)[0]; result.vy=(*fields)[1]; result.vz=(*fields)[2];
    result.sxx=(*fields)[3]; result.syy=(*fields)[4]; result.szz=(*fields)[5];
    result.sxy=(*fields)[6]; result.sxz=(*fields)[7]; result.syz=(*fields)[8];
    result.allFinite=s.diagnostics.allFinite;
    result.diagnostics=s.diagnostics;
    result.computeBackend=s.backend;
    const int g=s.grid.ghost;
    for(int k=g;k<s.grid.nz-g;++k)for(int j=g;j<s.grid.ny-g;++j)for(int i=g;i<s.grid.nx-g;++i)
        result.maxAbsVz=std::max(result.maxAbsVz,std::abs(result.vz[s.grid.index(i,j,k)]));
    for(int j=g;j<s.grid.ny-g;++j) for(int i=g;i<s.grid.nx-g;++i) {
        const auto q=s.grid.index(i,j,g);
        const auto& n=s.grid.surfaceNormal[q];
        const Vector3 traction{result.sxx[q]*n[0]+result.sxy[q]*n[1]+result.sxz[q]*n[2],
            result.sxy[q]*n[0]+result.syy[q]*n[1]+result.syz[q]*n[2],
            result.sxz[q]*n[0]+result.syz[q]*n[1]+result.szz[q]*n[2]};
        result.maxFreeSurfaceTraction=std::max(result.maxFreeSurfaceTraction,std::sqrt(dot(traction,traction)));
    }
    return result;
}



// Runs the application-preferred backend to completion for command-line exports.
bool runDemo(const SimulationParameters& parameters,SimulationResult& result,std::string& error) {
    CpuSimulation3D simulation(defaultBackend3D());
    if(!simulation.initialize(parameters,error)) return false;
    while(!simulation.completed()) if(!simulation.advance(std::min(32,simulation.totalSteps()-simulation.currentStep()),error)) return false;
    result=simulation.snapshot();
    return true;
}

} // namespace Seis3D::Curvilinear
