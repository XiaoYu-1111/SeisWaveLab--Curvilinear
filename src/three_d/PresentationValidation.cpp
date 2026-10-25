#include "VisualSettings3D.hpp"
#include "SceneCoordinates3D.hpp"
#include "Seis3D/ModelGeometryInfo3D.hpp"
#include "Seis3D/MaterialModels3D.hpp"
#include <iostream>
#include <stdexcept>
using namespace Seis3D::Curvilinear;
using namespace Seis3D::Curvilinear::Ui;
void check(bool good,const char* why){if(!good)throw std::runtime_error(why);}
int main(){try {
    SimulationParameters p;p.nx=100;p.ny=20;p.nz=100;p.spacing=1;p.topographyAmplitude=2;p.topographyWavelength=60;
    check(nominalDimensions3D(p)==std::array<double,3>{99,19,99},"Physical span counted nodes instead of intervals");
    Grid3D g;g.nx=p.nx+2*ghostWidth3D;g.ny=p.ny+2*ghostWidth3D;g.nz=p.nz+2*ghostWidth3D;g.dx=g.dy=g.dz=1;g.topographyAmplitude=2;g.topographyWavelength=60;
    std::string error;check(g.buildGeometry(error),error.c_str());const auto bounds=physicalModelBounds3D(g);
    check(bounds.minimum[0]==0&&bounds.maximum[0]==99&&bounds.minimum[1]==0&&bounds.maximum[1]==19&&bounds.maximum[2]==99,"Bounds included ghosts");
    check(std::abs(bounds.minimum[2]+2)<1e-12&&std::abs(bounds.dimensions()[2]-101)<1e-12,"Curved vertical bounds ignored terrain elevation");
    auto material=applyMaterialPreset3D(1,p);VisualSettings3D v;v.displayMode=2;
    for(int c=0;c<3;++c){v.modelComponent=c;updateMaterialRange3D(v,material);const auto before=modelColor3D(c==0?1500:c==1?700:2050,v);const auto legend=modelColorbarScale3D(v);v.gain=87;v.range=1e-8;v.component=4;v.normalizedColorbar=false;v.palette=9;updateMaterialRange3D(v,material);check(modelColor3D(c==0?1500:c==1?700:2050,v)==before,"Wave parameters changed material RGB");const auto after=modelColorbarScale3D(v);check(after.low==legend.low&&after.high==legend.high&&!after.normalized,"Wave parameters changed material legend");}
    v=VisualSettings3D{};const auto wave=waveColor3D(.001,v);v.modelPalette=8;v.modelComponent=2;v.modelAutoRange=false;v.modelMinimum[2]=1000;v.modelMaximum[2]=3000;updateMaterialRange3D(v,material);check(waveColor3D(.001,v)==wave,"Material settings changed wave RGB");
    const auto scale=modelColorbarScale3D(v);check(scale.low==1000&&scale.high==3000&&std::string(scale.physicalUnit)=="kg/m3","Manual material SI legend incorrect");
    DisplayFrame3D a,b;a.planes[0]={1,1,{0}};b.planes[0]={1,1,{2050}};v.displayMode=2;check(fieldColor3D(a,b,0,0,0,v)==modelColor3D(2050,v),"Zero wave masked the model in overlay mode");v.displayMode=0;check(fieldColor3D(a,{},0,0,0,v)==waveColor3D(0,v),"Wave-only mode reads missing material data");v.displayMode=1;check(fieldColor3D({},b,0,0,0,v)==modelColor3D(2050,v),"Model-only mode reads missing wave data");
    v.displayMode=2;v.showModelColorbar=false;check(waveLegendVisible3D(v)&&!modelLegendVisible3D(v),"Legend visibility coupled");v.showModelColorbar=true;v.showWaveColorbar=false;check(!waveLegendVisible3D(v)&&modelLegendVisible3D(v),"Model legend visibility coupled");
    v=VisualSettings3D{};for(int c=0;c<5;++c)cycleVisual3D(v,VisualShortcut3D::Component);check(v.component==0,"W left the five wave components");
    // Distinct slopes in X/Y expose transpose, either mirror and elevation sign errors.
    p.nx=9;p.ny=9;p.nz=30;p.spacing=10;p.topographyAmplitude=0;
    auto& dem=p.surfaceGrid;dem.nx=p.nx;dem.ny=p.ny;dem.spacing=p.spacing;dem.datum=1500;
    for(int j=0;j<dem.ny;++j)for(int i=0;i<dem.nx;++i)dem.elevations.push_back(1490+2*i+3*j);
    Grid3D terrain;terrain.nx=p.nx+6;terrain.ny=p.ny+6;terrain.nz=p.nz+6;terrain.dx=terrain.dy=terrain.dz=p.spacing;terrain.surfaceGrid=dem;terrain.topographyAmplitude=0;
    if(!terrain.buildGeometry(error))throw std::runtime_error(error);updateTerrainRange3D(v,terrain,p);
    check(v.terrainLow==1490&&v.terrainHigh==1530,"Terrain range used depth or ghost extrema instead of absolute DEM heights");
    const auto lower=scenePosition3D(terrain.coordinates[terrain.index(3,3,3)],80,80,290,290);
    const auto right=scenePosition3D(terrain.coordinates[terrain.index(11,3,3)],80,80,290,290);
    const auto upper=scenePosition3D(terrain.coordinates[terrain.index(3,11,3)],80,80,290,290);
    check(right[0]>lower[0]&&upper[2]<lower[2],"DEM map orientation reflected X or Y in the scene");
    check(right[1]>lower[1]&&upper[1]>lower[1],"Higher elevations must rise in scene space");
    check(sliceVerticalOffset3D(60,30,2)<0&&sliceVerticalOffset3D(60,30,0)>0,"XY must have Y up while XZ retains depth down");
    check(kTerrainPalettes3D==kPalettes3D,"Terrain and wave palette options diverged");
    check(terrainPalettePreference3D(0,1)==7&&terrainPalettePreference3D(1,1)==9&&terrainPalettePreference3D(2,1)==1,"Legacy terrain preferences changed colors");
    for(int palette=0;palette<wavePaletteCount3D;++palette){v.terrainPalette=palette;const auto before=terrainColor3D(1507,v);
        check(terrainPalettePreference3D(palette,2)==palette,"Shared terrain preference did not round trip");
        v.palette=palette;v.component=3;v.gain=1;v.range=1;
        for(int n=0;n<=40;++n){const double t=n/40.;const auto terrainRgb=terrainColor3D(v.terrainLow+(v.terrainHigh-v.terrainLow)*t,v),waveRgb=waveColor3D(t,v);
            for(int c=0;c<4;++c)check(std::abs(terrainRgb[c]-waveRgb[c])<1e-6,"Terrain RGB differs from wave palette at the same normalized value");}
        v.gain=100;v.range=1e12;v.palette=13;v.modelPalette=4;v.modelLow=-1e9;v.modelHigh=1e9;
        check(terrainColor3D(1507,v)==before,"Wave/material controls changed elevation colors");
        check(terrainColor3D(1490,v)!=terrainColor3D(1530,v),"Elevation palette has no height contrast");}
    v.terrainLow=v.terrainHigh=1500;const auto flat=terrainColor3D(1500,v);
    for(float c:flat)check(std::isfinite(c),"Flat DEM produced an invalid terrain color");
    std::cout<<"PASS physical bounds, independent field/elevation colors, asymmetric DEM orientation, flat terrain and legends\n";
}catch(const std::exception& e){std::cerr<<"FAIL: "<<e.what()<<'\n';return 1;}}
