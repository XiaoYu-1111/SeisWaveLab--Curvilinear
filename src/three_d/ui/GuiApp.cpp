#include "ScalarLegendStrip.hpp"
#include "MenuWidgets.hpp"
#include <GL/glew.h>
#include <GLFW/glfw3.h>
#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#include "SceneRenderer3D.hpp"
#include "SceneCoordinates3D.hpp"
#include "AppRuntime.hpp"
#include "GlassMaterial.hpp"
#include "ParameterPanelHost.hpp"
#include "ThemeValidation.hpp"
#include "WorkspaceTools.hpp"
#include "SimulationTiming.hpp"
#include <sstream>
#include "SourceControls.hpp"
#include "Workbench3D.hpp"
#include "Presentation3D.hpp"
#include "RecordView3D.hpp"
#include "Seis3D/ModelPresets3D.hpp"
#include "Seis3D/ModelGeometryInfo3D.hpp"
#include "Seis3D/TimeStepPlan3D.hpp"
#include "NumberFormat3D.hpp"
#include "Seis3D/MaterialModels3D.hpp"
#include "Seis3D/ReceiverGeometry3D.hpp"
#include "Seis3D/PerformanceRuns3D.hpp"
#include "implot.h"
#include "Seis3D/ReceiverIo.hpp"
#include "Seis3D/VtsWriter.hpp"
#include "Seis3D/ModelIo3D.hpp"
#include "Seis3D/AsyncSimulation3D.hpp"
#include <algorithm>
#include <bit>
#include <array>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <string>
#include <vector>
#include <optional>
#include <unordered_map>
#ifdef _WIN32
#include <windows.h>
#define GLFW_EXPOSE_NATIVE_WIN32
#define GLFW_NATIVE_INCLUDE_NONE
#include <GLFW/glfw3native.h>
#include <commdlg.h>
#endif

namespace Seis3D::Curvilinear::Ui {
namespace {


// Both dimensions and auxiliary tools share the same relocatable runtime data root.
std::filesystem::path outputRoot3D() {
    return SeisWave::Ui::outputRoot();
}
std::string utf8Path3D(const std::filesystem::path& path) {const auto s=path.u8string();return {s.begin(),s.end()};}
std::filesystem::path fromUtf8Path3D(const char* value) {
    std::u8string text;for(const unsigned char c:std::string(value))text.push_back(static_cast<char8_t>(c));return std::filesystem::path(text);
}

// Resolves deployed shaders beside the executable without depending on the process working directory.
std::filesystem::path executableDirectory3D(const char* argv0) {
#ifdef _WIN32
    std::vector<wchar_t> buffer(32768);
    const DWORD count=GetModuleFileNameW(nullptr,buffer.data(),static_cast<DWORD>(buffer.size()));
    if(count>0 && count<buffer.size()) return std::filesystem::path(std::wstring(buffer.data(),count)).parent_path();
#endif
    return std::filesystem::absolute(argv0).parent_path();
}

// Offers native file/folder selection while leaving the editable UTF-8 path available on every platform.
void browseSave3D(char* value,std::size_t capacity,bool vts,bool save=true,bool surface=false) {
#ifdef _WIN32
    const auto initial=fromUtf8Path3D(value);
    std::vector<wchar_t> buffer(32768,L'\0');
    const auto filename=initial.filename().wstring();std::copy(filename.begin(),filename.end(),buffer.begin());
    const auto directory=initial.parent_path().wstring();
    OPENFILENAMEW dialog{};dialog.lStructSize=sizeof(dialog);dialog.lpstrFile=buffer.data();dialog.nMaxFile=static_cast<DWORD>(buffer.size());
    dialog.lpstrInitialDir=directory.c_str();dialog.lpstrFilter=surface?L"VTK terrain (*.vti;*.vts)\0*.vti;*.vts\0VTK ImageData (*.vti)\0*.vti\0VTK StructuredGrid (*.vts)\0*.vts\0\0":vts?L"VTK StructuredGrid (*.vts)\0*.vts\0\0":L"Receiver CSV (*.csv)\0*.csv\0\0";
    dialog.lpstrDefExt=surface?L"vti":vts?L"vts":L"csv";dialog.Flags=(save?OFN_OVERWRITEPROMPT:OFN_FILEMUSTEXIST)|OFN_PATHMUSTEXIST|OFN_NOCHANGEDIR;
    if(save?GetSaveFileNameW(&dialog):GetOpenFileNameW(&dialog)) { const auto path=utf8Path3D(buffer.data());std::snprintf(value,capacity,"%s",path.c_str()); }
#else
    (void)value;(void)capacity;(void)vts;(void)save;(void)surface;
#endif
}

// Emits a quad with independent vertex colors so physical grid cells interpolate smoothly on screen.
void colorQuad3D(ImDrawList* draw,const std::array<ImVec2,4>& p,const std::array<ImU32,4>& colors) {
    draw->PrimReserve(6,4);const ImDrawIdx base=static_cast<ImDrawIdx>(draw->_VtxCurrentIdx);
    for(int n:{0,1,2,0,2,3})draw->PrimWriteIdx(static_cast<ImDrawIdx>(base+n));
    const auto uv=ImGui::GetFontTexUvWhitePixel();for(int n=0;n<4;++n)draw->PrimWriteVtx(p[n],uv,colors[n]);
}
ImU32 packedColor3D(const std::array<float,4>& color) {return ImGui::ColorConvertFloat4ToU32({color[0],color[1],color[2],color[3]});}

struct Workbench3D {
    std::unique_ptr<ISimulationBackend3D> simulation;
    SimulationParameters edited;
    VisualSettings3D visuals;
    SliceIndices3D slices;
    std::array<SliceView3D,3> sliceViews;
    std::array<std::size_t,3> gridSegments{};
    SceneRenderer3D renderer;
    RecordView3D recordView;
    DisplayFrame3D displayFrame,modelFrame;
    std::uint64_t modelFrameSession{};
    ModelBounds3D modelBounds;
    std::size_t modelSliceReads{};
    ComputeSettings3D computeSettings;
    ComputeSettings3D activeComputeSettings;
    int selectedBackend{};
    BackendAvailability3D cudaAvailability;
    CudaDeviceInventory3D cudaDevices;
    std::vector<std::string> graphicsAdapters;
    bool autoFitGeometry{true};
    std::array<bool,3> receiverExportComponents{false,false,true};
    std::array<bool,7> panels{true,true,true,true,true,true,true};
    static constexpr const char* panelNames[]{"Controls","Numerical diagnostics","Spatial exploration","Inline X-Z","Crossline Y-Z","Depth X-Y","Receiver records"};
    void restoreWorkspace(){panels.fill(true);SeisWave::Ui::setParameterDisplayMode(3,0);workspace.forgetFocus();resetLayout=true;}
    SeisWave::Ui::ParameterPanelHost parameterHost;
    SeisWave::Ui::WorkspaceTools workspace;
    int floatingCategory{2},floatingRequestedTab{7},floatingNextRequestedTab{-1},lastParameterMode{};
    int materialPreset{};
    bool layerEditorOpen{},requestLayerEditor{};
    int requestedTab{7};
    int nextRequestedTab{-1},controlCategory{2};
    std::array<ImRect,5> workflowRects;
    std::uint64_t performanceSession{};
    std::vector<PerformanceRun3D> performanceRuns;
    int layoutPreset{};
    int layoutBucket{-1};
    float toolbarHeight{};
    float layoutFontSize{};
    float uiFontSize{22.0f};
    int groupEpoch{};
    bool groupsExpanded{};
    std::unordered_map<ImGuiID,int> groupApplied;
    bool running{},dirty{},resetLayout{true};
    bool shortcutHelp{},homeRequested{};ImRect homeRect,elapsedRect;
    BackendMemory3D memoryAvailable;
    BackendKind3D memoryBackend{BackendKind3D::Cpu};
    int memoryDevice{-2};
    double memoryQueryTime{-10};
    int stepsPerFrame{1},theme{themeIndex3D()},selectedReceiver{};
    double lastBatchMs{};
    double meanStepMs{};
    float frameBudgetMs{16.0f};
    std::string error,status{"Ready. Build a model, then Run or Step."};
    std::array<char,2048> vtsPath{},csvPath{},modelPath{},surfacePath{};
    bool snapshotFollowDisplay{true};
    VtsExportOptions3D snapshotOptions{{false,false,true,false,false,false,false,false,false,false,false},true,false};
    bool pendingModelEdits() const {
        return dirty||edited!=simulation->parameters()||computeSettings!=activeComputeSettings||
            selectedBackend!=static_cast<int>(simulation->backendInfo().kind);
    }
    void saveModel() {
        error.clear();
        if(pendingModelEdits()){error="Apply pending parameters before saving the model.";return;}
        if(writeModelVts3D(*simulation,fromUtf8Path3D(modelPath.data()),activeComputeSettings,error))
            status="Reproducible 3D model saved. Import starts a new simulation at t=0.";
    }
    bool loadModel() {
        ModelDocument3D model;error.clear();
        if(!readModelVts3D(fromUtf8Path3D(modelPath.data()),model,error))return false;
        const auto previous=edited;const auto settings=computeSettings;const auto backend=selectedBackend;const auto wasDirty=dirty;
        edited=model.parameters;computeSettings=model.compute;computeSettings.cudaDevice=-1;
        const bool fallback=model.backend==BackendKind3D::Cuda&&!cudaAvailability.available;
        selectedBackend=static_cast<int>(fallback?BackendKind3D::Cpu:model.backend);
        if(!rebuild(true)){edited=previous;computeSettings=settings;selectedBackend=backend;dirty=wasDirty;return false;}
        status=fallback?"Model restored at t=0 using CPU; CUDA unavailable.":"3D model restored at t=0; dt and receiver layout recalculated.";
        return true;
    }
    VtsExportOptions3D selectedSnapshotOptions() const {
        auto options=snapshotOptions;
        if(snapshotFollowDisplay){options.fields.fill(false);constexpr int mapping[]{2,0,1,3,4};options.fields[mapping[std::clamp(visuals.component,0,4)]]=true;options.includeVelocityVector=false;}
        return options;
    }
    void snapshotControls() {
        auto options=selectedSnapshotOptions();
        if(ImGui::Checkbox(language3D()?"跟随当前显示分量":"Follow displayed component",&snapshotFollowDisplay)&&!snapshotFollowDisplay)snapshotOptions=options;
        if(!snapshotFollowDisplay) {
            constexpr const char* names[]{"Vx","Vy","Vz","|V|","Pressure","Sxx","Syy","Szz","Sxy","Sxz","Syz"};
            for(int i=0;i<11;++i){if(i%3)ImGui::SameLine();ImGui::Checkbox(names[i],&snapshotOptions.fields[i]);}
            ImGui::Checkbox(language3D()?"三分量速度矢量":"Velocity vector (3 components)",&snapshotOptions.includeVelocityVector);
        }
        ImGui::Checkbox(language3D()?"附带物性参数":"Include material properties",&snapshotOptions.includeMaterial);
    }
    void modelFiles() {
        ImGui::TextWrapped("%s",language3D()?"保存几何、物性及完整模拟参数；导入后从零时刻重建。":"Save geometry, material and full simulation parameters; import restarts at t=0.");
        ImGui::SetNextItemWidth(-1);ImGui::InputText("##modelPath",modelPath.data(),modelPath.size());
        if(ImGui::Button(language3D()?"选择模型文件...":"Choose model..."))browseSave3D(modelPath.data(),modelPath.size(),true,false);
        if(ImGui::Button(language3D()?"模型另存为...":"Save model as..."))browseSave3D(modelPath.data(),modelPath.size(),true);
        const bool pending=pendingModelEdits();ImGui::BeginDisabled(pending);
        if(ImGui::Button(language3D()?"保存模型 VTS":"Save model VTS",{-1,0}))saveModel();ImGui::EndDisabled();
        if(pending)ImGui::TextWrapped("%s",language3D()?"参数尚未应用，请先重建再保存。":"Apply pending edits before saving.");
        if(ImGui::Button(language3D()?"导入并重建模型":"Import and rebuild model",{-1,0}))(void)loadModel();
        ImGui::TextWrapped("%s",language3D()?"模型文件用于复现；波场快照用于分析，不是续算检查点。":"Model files reproduce a run; wavefield snapshots are analysis data, not restart checkpoints.");
    }

    Workbench3D() {
        refreshComputeDevices();
        selectedBackend=cudaAvailability.available?1:0;
        simulation=createSimulationBackend3D(static_cast<BackendKind3D>(selectedBackend),error);
    }
    void refreshComputeDevices() {
        cudaDevices=queryCudaDevices3D();graphicsAdapters=queryGraphicsAdapters3D();
        cudaAvailability=selectCudaDevice3D(cudaDevices);memoryQueryTime=-10;
    }

    // UI preferences live outside physical state and are never read as simulation commands.
    void loadPreferences() {
        std::ifstream input(outputRoot3D()/"ui"/"preferences.cfg");std::string key,line;double value=0;
        theme=themeIndex3D();
        int terrainPaletteVersion=1;bool terrainPaletteLoaded=false;
        while(std::getline(input,line)) {
            std::istringstream row(line);if(!(row>>key>>value))continue;
            if(!std::isfinite(value))continue;
            if(key=="font")uiFontSize=static_cast<float>(std::clamp(value,16.0,28.0));
            else if(key=="theme")continue; // Appearance is resolved by stable key before entering the workspace.
            else if(key=="language")setLanguage3D(static_cast<int>(value!=0));
            else if(key=="layout")layoutPreset=static_cast<int>(std::clamp(value,0.0,3.0));
            else if(key=="autofit")autoFitGeometry=value!=0;
            else if(key=="panels"){const int mask=static_cast<int>(std::clamp(value,0.0,127.0));for(int i=0;i<7;++i)panels[i]=(mask&(1<<i))!=0;}
            else if(key=="workers")computeSettings.cpuThreads=static_cast<int>(std::clamp(value,0.0,double(maxCpuThreads3D)));
            else if(key=="normalized_colorbar")visuals.normalizedColorbar=value!=0;
            else if(key=="display_mode")visuals.displayMode=static_cast<int>(std::clamp(value,0.0,2.0));
            else if(key=="wave_component")visuals.component=static_cast<int>(std::clamp(value,0.0,4.0));
            else if(key=="model_component")visuals.modelComponent=static_cast<int>(std::clamp(value,0.0,2.0));
            else if(key=="wave_palette")visuals.palette=static_cast<int>(std::clamp(value,0.0,13.0));
            else if(key=="model_palette")visuals.modelPalette=static_cast<int>(std::clamp(value,0.0,13.0));
            else if(key=="terrain_color")visuals.colorTopography=value!=0;
            else if(key=="terrain_palette_version")terrainPaletteVersion=static_cast<int>(std::clamp(value,1.0,2.0));
            else if(key=="terrain_palette"){visuals.terrainPalette=static_cast<int>(std::clamp(value,0.0,double(wavePaletteCount3D-1)));terrainPaletteLoaded=true;}
            else if(key=="wave_colorbar")visuals.showWaveColorbar=value!=0;
            else if(key=="model_colorbar")visuals.showModelColorbar=value!=0;
            else if(key=="show_colorbars")visuals.showColorbar=value!=0;
            else if(key=="wave_opacity")visuals.waveOpacity=static_cast<float>(std::clamp(value,0.0,1.0));
            else if(key=="wave_threshold")visuals.waveThreshold=static_cast<float>(std::clamp(value,0.0,.5));
            else if(key=="model_auto_range")visuals.modelAutoRange=value!=0;
            else if(key=="show_dimensions")visuals.showDimensions=value!=0;
            else if(key=="show_coordinates")visuals.showCoordinates=value!=0;
            else for(int c=0;c<3;++c){if(key=="model_min_"+std::to_string(c))visuals.modelMinimum[c]=static_cast<float>(std::clamp(value,-1e8,1e8));if(key=="model_max_"+std::to_string(c))visuals.modelMaximum[c]=static_cast<float>(std::clamp(value,-1e8,1e8));}
        }
        if(terrainPaletteLoaded)visuals.terrainPalette=terrainPalettePreference3D(visuals.terrainPalette,terrainPaletteVersion);
        applyTheme3D(theme);
    }
    void savePreferences() const {
        std::ofstream output(outputRoot3D()/"ui"/"preferences.cfg");
        int mask=0;for(int i=0;i<7;++i)if(panels[i])mask|=1<<i;output<<"panels "<<mask<<'\n';
        output<<"font "<<uiFontSize<<"\nlanguage "<<language3D()<<"\ntheme "<<theme<<"\nlayout "<<layoutPreset<<"\nautofit "<<autoFitGeometry<<"\nworkers "<<activeComputeSettings.cpuThreads<<'\n';
        output<<"parameters_2d "<<SeisWave::Ui::parameterDisplayMode(2)<<"\nparameters_3d "<<parameterHost.mode()<<'\n';
        output<<"theme_key "<<themeKey3D(theme)<<"\ntheme_schema 2\nglass_finish "<<glassFinish3D()<<"\nglass_strength "<<glassStrength3D()<<"\nglass_motion "<<glassMotion3D()<<"\nglass_quality "<<glassQuality3D()<<'\n';
        output<<"normalized_colorbar "<<visuals.normalizedColorbar<<'\n';
        output<<"terrain_color "<<visuals.colorTopography<<"\nterrain_palette_version 2\nterrain_palette "<<visuals.terrainPalette<<'\n';
        output<<"display_mode "<<visuals.displayMode<<"\nwave_component "<<visuals.component<<"\nmodel_component "<<visuals.modelComponent<<"\nwave_palette "<<visuals.palette<<"\nmodel_palette "<<visuals.modelPalette<<'\n';
        output<<"wave_colorbar "<<visuals.showWaveColorbar<<"\nmodel_colorbar "<<visuals.showModelColorbar<<"\nmodel_auto_range "<<visuals.modelAutoRange<<"\nshow_dimensions "<<visuals.showDimensions<<"\nshow_coordinates "<<visuals.showCoordinates<<'\n';
        output<<"show_colorbars "<<visuals.showColorbar<<"\nwave_opacity "<<visuals.waveOpacity<<"\nwave_threshold "<<visuals.waveThreshold<<'\n';
        for(int c=0;c<3;++c)output<<"model_min_"<<c<<' '<<visuals.modelMinimum[c]<<"\nmodel_max_"<<c<<' '<<visuals.modelMaximum[c]<<'\n';
    }

    // Builds physical state while preserving palette, gain, camera, and slice view adjustments.
    bool rebuild(bool useEdited) {
        if(auto* background=dynamic_cast<AsyncSimulation3D*>(simulation.get()))background->synchronize();
        try {
        rememberPerformance();
        const bool first=!simulation->initialized();
        const std::optional<SimulationParameters> previous=first?std::nullopt:std::optional<SimulationParameters>(simulation->parameters());
        const int previousReceivers=first?0:static_cast<int>(simulation->receivers().size());
        const auto parameters=useEdited?edited:simulation->parameters();
        const auto kind=useEdited?static_cast<BackendKind3D>(selectedBackend):simulation->backendInfo().kind;
        const auto settings=useEdited?computeSettings:activeComputeSettings;
        if(!validateSimulationConfiguration3D(parameters,kind,error,settings))return false;
        displayFrame={};modelFrame={};recordView.releaseCache();
        const auto rebuilt=rebuildSimulation3D(simulation,parameters,kind,settings,error);
        if(!rebuilt.success&&!rebuilt.released)return false;
        memoryQueryTime=-10;
        if(!simulation||!simulation->initialized()){running=false;homeRequested=true;return false;}
        if(!dynamic_cast<AsyncSimulation3D*>(simulation.get()))simulation=std::make_unique<AsyncSimulation3D>(std::move(simulation));
        renderer.invalidate();
        if(rebuilt.success&&useEdited)activeComputeSettings=computeSettings;
        if(rebuilt.fallback)activeComputeSettings={1};
        ++performanceSession;
        modelBounds=physicalModelBounds3D(simulation->grid());modelFrameSession=0;
        running=false;lastBatchMs=0;meanStepMs=0;
        displayFrame.revision=0;recordView.invalidate();
        if(rebuilt.success&&useEdited)dirty=false;
        const auto& p=simulation->parameters();
        const bool fit=reconcileViews3D(previous?&*previous:nullptr,p,previousReceivers,static_cast<int>(simulation->receivers().size()),slices,sliceViews,selectedReceiver,autoFitGeometry);
        if(fit)renderer.fitCamera();
        status=rebuilt.success?(fit?"Geometry updated: relative slices preserved; views fitted. Gain and palette retained.":"Fields reset: views, gain and receiver selection retained."):error;
        if(!rebuilt.success&&language3D())status=(rebuilt.restored?"新网格创建失败，已按原参数恢复到零时刻，原波场和记录已释放。 ":"模型恢复失败，已切换到小型 CPU 会话，请调整参数后重建。 ")+error;
        return rebuilt.success;
        } catch(const std::exception& ex){running=false;error=std::string("Rebuild failed: ")+ex.what();if(!simulation||!simulation->initialized())homeRequested=true;return false;}
    }
    bool clearFields() {
        if(auto* background=dynamic_cast<AsyncSimulation3D*>(simulation.get()))background->synchronize();
        rememberPerformance();
        if(!simulation->resetFields(error))return false;
        ++performanceSession;running=false;meanStepMs=lastBatchMs=0;
        displayFrame.revision=0;modelFrameSession=0;recordView.invalidate();
        status="Fields cleared at t=0. Active dt retained; staged edits are not applied.";
        return true;
    }
    void memorySummary() {
        const auto kind=static_cast<BackendKind3D>(selectedBackend);
        if(memoryBackend!=kind||memoryDevice!=computeSettings.cudaDevice||ImGui::GetTime()-memoryQueryTime>1){memoryAvailable=memoryForRebuild3D(simulation.get(),kind,computeSettings.cudaDevice);memoryBackend=kind;memoryDevice=computeSettings.cudaDevice;memoryQueryTime=ImGui::GetTime();}
        const auto memory=estimateModelMemory3D(edited);constexpr double gib=1024.0*1024*1024;
        ImGui::TextWrapped("%s %zu / %zu",language3D()?"含幽灵层节点 / 总预算：":"Padded nodes / total budget:",memory.points,maxPaddedNodes3D);
        ImGui::TextWrapped(tr3D("Required RAM %.2f GiB | VRAM %.2f GiB"),(kind==BackendKind3D::Cuda?memory.cudaHostMiB:memory.cpuMiB)/1024,kind==BackendKind3D::Cuda?memory.deviceMiB/1024:0);
        ImGui::TextWrapped(language3D()?"重建可用 RAM %.2f GiB | VRAM %.2f GiB（含旧模型释放）":"Rebuild RAM %.2f GiB | VRAM %.2f GiB (includes released model)",memoryAvailable.hostAvailableBytes/gib,memoryAvailable.deviceAvailableBytes/gib);
        if(!fitsMemory3D(memory,kind,memoryAvailable))ImGui::TextWrapped(tr3D("Requested grid exceeds limits or currently available memory. It will not be allocated."));
        if(functionButton3D("Fit to available memory",FunctionRole3D::Model,{-1,0})) {
            memoryAvailable=memoryForRebuild3D(simulation.get(),kind,computeSettings.cudaDevice);SimulationParameters fitted;
            const bool success=edited.surfaceGrid.empty()?fitModelToMemory3D(edited,kind,memoryAvailable,fitted):fitSurfaceModel3D(edited,kind,memoryAvailable,fitted,error);
            if(success){edited=std::move(fitted);dirty=true;error.clear();status=edited.surfaceGrid.empty()?"Grid fitted to available memory. Spacing retained; review physical dimensions and apply rebuild.":"DEM grid redistributed over X/Y/Z. Review numerical spacing and covered span, then apply rebuild.";}
            else error="Even the minimum grid cannot fit the selected backend or receiver settings.";
        }
        ImGui::TextWrapped("%s",edited.surfaceGrid.empty()?tr3D("Fit is explicit: it reduces node counts, retains spacing, and fits source/receiver positions. The actual physical size changes."):(language3D()?"DEM 按总点数与可用内存分配 XYZ；保留原始数组，明确调整数值间距与层数。":"DEM XYZ allocation uses total nodes and memory; original samples are retained, numerical spacing and layers are reported."));
    }
    bool stageSurface() {
        SurfaceGrid3D data;
        if(!readSurfaceGrid3D(fromUtf8Path3D(surfacePath.data()),data,error))return false;
        const int minimum=edited.source.kind==SeisWave::SourceKind::IsotropicMoment?11:8;
        if(data.nx<minimum||data.ny<minimum){error="DEM needs at least "+std::to_string(minimum)+" samples per axis for the current source.";return false;}
        auto requested=edited;requested.surfaceGrid=std::move(data);
        auto available=memoryForRebuild3D(simulation.get(),static_cast<BackendKind3D>(selectedBackend),computeSettings.cudaDevice);
        // Leave an additional planning margin for UI caches and resource changes.
        available.hostAvailableBytes=available.hostAvailableBytes/100*95;
        available.deviceAvailableBytes=available.deviceAvailableBytes/100*95;
        SimulationParameters next;
        if(!fitSurfaceModel3D(requested,static_cast<BackendKind3D>(selectedBackend),available,next,error))return false;
        edited=std::move(next);dirty=true;error.clear();
        status=(language3D()?"DEM 原始数组已保留；数值网格分配为 ":"Original DEM retained; numerical grid allocated as ")+std::to_string(edited.nx)+" x "+std::to_string(edited.ny)+" x "+std::to_string(edited.nz)+(language3D()?"。核对间距与范围后应用/重建。":". Review spacing/span, then apply / rebuild.");
        return true;
    }
    bool stageDefaultSurface() {
        const auto path=SeisWave::Ui::elevationExampleRoot().parent_path()/"terrain3d/surface_grid_400x288_h25m.vts";
        std::snprintf(surfacePath.data(),surfacePath.size(),"%s",utf8Path3D(path).c_str());
        return stageSurface();
    }
    void surfaceControls() {
        ImGui::TextWrapped("%s",language3D()?"导入地表 VTI/VTS（二维规则高程，XY 和高程单位为米）。VTS 为单层起伏地表，不是三维模型体。":"Import a metre-based terrain VTI/VTS. VTS must be a single surface, not a model volume.");
        ImGui::SetNextItemWidth(-1);ImGui::InputText("##surfaceGridPath",surfacePath.data(),surfacePath.size());
        if(ImGui::Button(language3D()?"选择高程数组...":"Choose elevation array..."))browseSave3D(surfacePath.data(),surfacePath.size(),false,false,true);
        if(ImGui::Button(language3D()?"读取 DEM（待应用）":"Stage DEM array"))(void)stageSurface();
        ImGui::SameLine();if(ImGui::Button(language3D()?"恢复默认地表":"Default terrain"))(void)stageDefaultSurface();
        if(!edited.surfaceGrid.empty()) {
            const auto& d=edited.surfaceGrid;
            const auto mm=std::minmax_element(d.elevations.begin(),d.elevations.end());
            ImGui::TextWrapped("%s %d x %d | h=%.3f m | %.1f x %.1f m",language3D()?"高程数组":"DEM array",d.nx,d.ny,d.spacing,d.width(),d.height());
            ImGui::TextWrapped("%s %d x %d x %d | h=%.3f m",language3D()?"实际待计算网格":"Staged numerical grid",edited.nx,edited.ny,edited.nz,edited.spacing);
            ImGui::TextWrapped("%s %.3f .. %.3f m | %s %.3f m",language3D()?"高程":"Elevation",*mm.first,*mm.second,language3D()?"基准":"Datum",d.datum);
            ImGui::TextWrapped("%s",language3D()?"行沿 Y 增大，列沿 X 增大；地表 Z = 基准 - 高程。材料深度按所选坐标模式计算。":"Rows increase in Y, columns in X; surface Z = datum - elevation. Material depths follow the selected coordinate mode.");
            if(ImGui::Button(language3D()?"清除外部高程":"Clear external DEM")){edited.surfaceGrid={};dirty=true;}
        }
    }
    void stagedTimeStepSummary() {
        if(edited==simulation->parameters())return;
        const auto& a=simulation->parameters();const auto& d=simulation->diagnostics();
        const bool terrain=edited.topographyAmplitude!=0||!edited.surfaceGrid.empty();
        if(terrain&&geometryChanged3D(edited,a)) {
            ImGui::TextWrapped(tr3D("Geometry edits pending: dt will be recalculated from the new mesh on Apply / rebuild."));return;
        }
        if(edited.nx<8||edited.ny<8||edited.nz<8||edited.nx>maxPhysicalNodesPerAxis3D||edited.ny>maxPhysicalNodesPerAxis3D||edited.nz>maxPhysicalNodesPerAxis3D)return;
        double vp=edited.vp;if(edited.layeredMaterial){vp=0;for(const auto& l:edited.layers)vp=std::max(vp,l.vp);}
        std::string why;TimeStepPlan3D preview;
        const double metric=!terrain?std::sqrt(3.0):d.maxMetricNorm;
        const double length=!terrain?edited.spacing/std::sqrt(3.0):d.minCellLength;
        const double damping=absorbingRate3D(edited,0,0,edited.nz-1,vp);
        if(timeStepPlan3D(edited,vp,metric,length,damping,preview,why))ImGui::TextWrapped(tr3D("Staged dt %.6f ms | %d steps | stop %.6f s (not applied)"),preview.dt*1000,preview.steps,preview.dt*preview.steps);
        else ImGui::TextWrapped("%s",tr3D(why.c_str()));
    }

    // Wave slices change each accepted step; material slices change only after
    // a rebuild, slice move or material-component selection.
    bool refreshDisplayFrame() {
        if(waveVisible3D(visuals)) {
            const DisplayRequest3D request{slices.x,slices.y,slices.z,visuals.component};
            if((displayFrame.revision!=simulation->revision()||displayFrame.request!=request)&&!simulation->readDisplayFrame(request,displayFrame,error))return false;
        }
        if(modelVisible3D(visuals)) {
            const DisplayRequest3D request{slices.x,slices.y,slices.z,5+visuals.modelComponent};
            if(modelFrameSession!=performanceSession||modelFrame.request!=request) {
                if(!simulation->readDisplayFrame(request,modelFrame,error))return false;
                modelFrameSession=performanceSession;++modelSliceReads;
            }
        }
        return true;
    }
    bool verifyMaterialCache() {
        const auto saved=visuals;visuals.displayMode=2;
        if(!refreshDisplayFrame())return false;
        const auto reads=modelSliceReads;
        if(!simulation->advance(1,error)||!refreshDisplayFrame()||modelSliceReads!=reads){error="Static material slices were downloaded again after a wave step.";return false;}
        visuals.modelComponent=(visuals.modelComponent+1)%3;
        if(!refreshDisplayFrame()||modelSliceReads!=reads+1){error="Material component change did not refresh the model slices.";return false;}
        visuals=saved;
        std::cout<<"PASS independent wave/material slice cache across accepted CUDA/CPU steps\n";
        return rebuild(true);
    }

    void resetViews() {sliceViews={};renderer.resetCamera();}

    // Drives the real rebuild path without synthetic mouse input: grow, material edit, reset, rejection and shrink.
    bool verifyResizePolicy() {
        auto check=[](bool okay,const char* message){if(!okay)throw std::runtime_error(message);};
        const auto original=edited;const auto savedVisuals=visuals;
        try {
            visuals.gain=2.75f;visuals.velocityReference=0.004f;visuals.component=1;visuals.palette=2;
            slices={original.nx-3,original.ny/2,3};sliceViews[0].zoom=4;sliceViews[0].pan={30,-25};selectedReceiver=5;
            renderer.orbit(30,10);renderer.zoom(2);renderer.pan(20,10);const auto oldCamera=renderer.cameraState();
            const int oldSlice=slices.x;
            edited.nx=42;edited.ny=18;edited.nz=40;edited.spacing=8;edited.topographyAmplitude=12;edited.terrainTransitionDepth=0;
            check(rebuild(true),"Resize grow failed");
            check(simulation->parameters()==edited&&simulation->grid().nx==edited.nx+2*ghostWidth3D,"Resize retained stale model parameters or storage dimensions");
            check(slices.x==remapIndex3D(oldSlice,original.nx,edited.nx),"Resize lost relative slice position");
            check(sliceViews[0].zoom==1 && sliceViews[0].pan[0]==0,"Geometry edit did not fit slices");
            check(renderer.cameraState()[0]==oldCamera[0] && renderer.cameraState()[1]==oldCamera[1] && renderer.cameraState()[2]==2.0f && renderer.cameraState()[3]==0,"Camera fit changed orbit or retained old pan");
            check(visuals.gain==2.75f && visuals.velocityReference==0.004f && visuals.component==1 && visuals.palette==2,"Resize reset display settings");
            const int grownReceiver=selectedReceiver;
            sliceViews[1].zoom=2.5f;sliceViews[1].pan={17,22};renderer.zoom(1);const auto materialCamera=renderer.cameraState();
            const double previousDt=simulation->dt();edited.vp+=200;check(rebuild(true),"Material rebuild failed");
            check(simulation->parameters()==edited&&simulation->dt()<previousDt,"GUI apply retained old parameters or dt after increasing Vp");
            check(sliceViews[1].zoom==2.5f && sliceViews[1].pan[0]==17 && renderer.cameraState()==materialCamera && selectedReceiver==grownReceiver,"Material edit reset view or receiver selection");
            check(rebuild(false) && sliceViews[1].zoom==2.5f && renderer.cameraState()==materialCamera,"Field reset changed the view");
            const auto revision=simulation->revision();edited.nx=maxPhysicalNodesPerAxis3D+1;running=true;
            check(!rebuild(true) && simulation->revision()==revision && running && sliceViews[1].zoom==2.5f,"Rejected model change mutated the active session/view");
            running=false;edited=simulation->parameters();edited.nx=12;edited.ny=10;edited.nz=16;edited.spacing=10;edited.topographyAmplitude=8;
            check(rebuild(true) && slices.x<12 && slices.y<10 && slices.z<16 && selectedReceiver<8,"Resize shrink left stale indices");
            check(refreshDisplayFrame(),"Shrunk display frame failed");
            check(renderer.render(*simulation,displayFrame,modelFrame,visuals,slices,640,400,error),"Shrunk mesh render failed");
            edited=original;visuals=savedVisuals;check(rebuild(true),"Restoring default model failed");
            slices={original.nx/2,original.ny/2,3};selectedReceiver=static_cast<int>(simulation->receivers().size()/2);resetViews();
            std::cout<<"GUI resize policy PASS: grow/shrink, material/reset preservation, rejected transaction, gain/receiver retention, changed slice payload render\n";
            return true;
        } catch(const std::exception& e){error=e.what();return false;}
    }

    bool verifyBackendPolicy() {
        if(!cudaAvailability.available)return true;
        try {
            const auto original=edited;const auto active=simulation->backendInfo().kind;
            const auto oldVisuals=visuals;const auto oldCamera=renderer.cameraState();
            const auto* originalSession=simulation.get();const auto revision=simulation->revision();
            selectedBackend=active==BackendKind3D::Cpu?1:0;edited.nx=maxPhysicalNodesPerAxis3D+1;running=true;
            if(rebuild(true)||simulation.get()!=originalSession||simulation->revision()!=revision||!running)throw std::runtime_error("Failed backend switch mutated active session");
            edited=original;
            if(!rebuild(true)||simulation->backendInfo().kind==active||simulation->currentStep()!=0||running)throw std::runtime_error("Backend switch did not commit a fresh session");
            if(renderer.cameraState()!=oldCamera||visuals.gain!=oldVisuals.gain||visuals.palette!=oldVisuals.palette)throw std::runtime_error("Backend switch discarded view settings");
            if(!simulation->advance(3,error)||!refreshDisplayFrame()||!renderer.render(*simulation,displayFrame,modelFrame,visuals,slices,640,400,error))throw std::runtime_error(error);
            selectedBackend=static_cast<int>(active);
            if(!rebuild(true)||simulation->backendInfo().kind!=active||simulation->receivers().front().samples.size()!=1)throw std::runtime_error("Return backend switch failed");
            std::cout<<"GUI backend switch PASS: CPU/CUDA, failed transaction, fresh fields/receivers, display/render, camera/gain retained\n";return true;
        } catch(const std::exception& ex){error=ex.what();return false;}
    }

    bool verifyDevicePolicy() {
        if(!cudaAvailability.available)return true;
        try {
            const auto settings=computeSettings;const int backend=selectedBackend;
            const auto devices=cudaDevices.devices;
            for(const auto& device:devices) {
                if(!device.available)continue;
                computeSettings.cudaDevice=device.index;selectedBackend=1;
                if(!rebuild(true)||simulation->backendInfo().cudaDevice!=device.index)throw std::runtime_error("GUI selected the wrong CUDA device");
                if(!simulation->advance(2,error))throw std::runtime_error(error);
                const auto revision=simulation->revision();const auto* session=simulation.get();
                refreshComputeDevices();
                if(simulation.get()!=session||simulation->revision()!=revision||simulation->currentStep()!=2)throw std::runtime_error("GPU refresh changed the running session");
                computeSettings.cudaDevice=1000000;
                if(rebuild(true)||simulation.get()!=session||simulation->revision()!=revision||simulation->backendInfo().cudaDevice!=device.index)throw std::runtime_error("Failed GPU selection discarded the session");
                computeSettings.cudaDevice=device.index;
                if(!simulation->advance(1,error)||!refreshDisplayFrame()||!renderer.render(*simulation,displayFrame,modelFrame,visuals,slices,640,400,error))throw std::runtime_error(error);
            }
            computeSettings=settings;selectedBackend=backend;
            if(!rebuild(true))throw std::runtime_error(error);
            std::cout<<"GUI device selection PASS: per-device rebuild, refresh, rejected switch, continued compute and render\n";return true;
        } catch(const std::exception& ex){error=ex.what();return false;}
    }

    // Writes raw physical field values; display normalization and gain do not affect exports.
    void exportSnapshot() {
        error.clear();
        try {
            const auto options=selectedSnapshotOptions();
            if(!options.includeVelocityVector&&std::none_of(options.fields.begin(),options.fields.end(),[](bool v){return v;})){error="Select at least one wavefield component.";return;}
            if(writeVts(simulation->snapshot(),fromUtf8Path3D(vtsPath.data()),options,error))status="VTS snapshot exported at current simulation time.";
        } catch(const std::exception& ex) {error=ex.what();}
    }
    void exportTraces() {
        error.clear();
        try { if(writeReceiversCsv(*simulation,fromUtf8Path3D(csvPath.data()),receiverExportComponents,error))status="Time-zero-inclusive receiver CSV exported."; }
        catch(const std::exception& ex) {error=ex.what();}
    }

    // Rebuilds a deterministic docking layout and preserves the user's custom layout between ordinary starts.
    void docking() {
        const auto* vp=ImGui::GetMainViewport();
        if(lastParameterMode!=parameterHost.mode()){resetLayout=!workspace.changeMode(lastParameterMode,parameterHost.mode());lastParameterMode=parameterHost.mode();}
        const int bucket=vp->WorkSize.x<1500?0:vp->WorkSize.x<1600?1:2;
        layoutBucket=bucket;
        layoutFontSize=ImGui::GetFontSize();
        const float statusHeight=ImGui::GetTextLineHeightWithSpacing()*2+ImGui::GetStyle().WindowPadding.y*2+4;
        ImGui::SetNextWindowPos({vp->WorkPos.x,vp->WorkPos.y+toolbarHeight});ImGui::SetNextWindowSize({vp->WorkSize.x,vp->WorkSize.y-statusHeight-toolbarHeight});ImGui::SetNextWindowViewport(vp->ID);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,{0,0});ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize,0);
        ImGui::Begin(label3D("Workspace##Host"),nullptr,ImGuiWindowFlags_NoTitleBar|ImGuiWindowFlags_NoMove|ImGuiWindowFlags_NoResize|ImGuiWindowFlags_NoDocking|ImGuiWindowFlags_NoBringToFrontOnFocus|ImGuiWindowFlags_NoNavFocus);
        const ImGuiID dock=ImGui::GetID("CurvilinearDockspace3D.v5");
        if(resetLayout || !ImGui::DockBuilderGetNode(dock)) {
            ImGui::DockBuilderRemoveNode(dock);ImGui::DockBuilderAddNode(dock,ImGuiDockNodeFlags_DockSpace);
            ImGui::DockBuilderSetNodeSize(dock,{vp->WorkSize.x,vp->WorkSize.y-statusHeight-toolbarHeight});
            ImGuiID main=dock,controls=0,records=0,sections=0,xz=0,yz=0,xy=0;
            if(parameterHost.mode()!=1)ImGui::DockBuilderSplitNode(main,ImGuiDir_Left,std::clamp(ImGui::GetFontSize()*20.0f/vp->WorkSize.x,0.22f,0.38f),&controls,&main);
            const float availableHeight=vp->WorkSize.y-statusHeight-toolbarHeight;
            const float recordMinimum=ImGui::GetFrameHeight()*3+ImGui::GetTextLineHeightWithSpacing()+90;
            const float recordRatio=layoutPreset==3?.52f:std::clamp(recordMinimum/std::max(1.0f,availableHeight),0.24f,0.48f);
            ImGui::DockBuilderSplitNode(main,ImGuiDir_Down,recordRatio,&records,&main);
            if(layoutPreset==2 || layoutPreset==3 || vp->WorkSize.x<1500) {xz=yz=xy=main;}
            else if(layoutPreset==1) {ImGui::DockBuilderSplitNode(main,ImGuiDir_Right,0.48f,&sections,&main);xz=yz=xy=sections;}
            else {
                ImGuiID left=0,right=0;
                ImGui::DockBuilderSplitNode(main,ImGuiDir_Left,0.50f,&left,&right);
                ImGui::DockBuilderSplitNode(left,ImGuiDir_Down,0.50f,&xy,&xz);
                ImGui::DockBuilderSplitNode(right,ImGuiDir_Down,0.50f,&main,&yz);
            }
            ImGui::DockBuilderDockWindow(label3D("Spatial exploration"),main);if(parameterHost.mode()!=1)ImGui::DockBuilderDockWindow(label3D("Controls"),controls);
            if(parameterHost.mode()!=1)ImGui::DockBuilderDockWindow(label3D("Numerical diagnostics"),controls);
            ImGui::DockBuilderDockWindow(label3D("Inline X-Z"),xz);ImGui::DockBuilderDockWindow(label3D("Crossline Y-Z"),yz);ImGui::DockBuilderDockWindow(label3D("Depth X-Y"),xy);
            ImGui::DockBuilderDockWindow(label3D("Receiver records"),records);ImGui::DockBuilderFinish(dock);resetLayout=false;
        }
        ImGui::DockSpace(dock);ImGui::End();ImGui::PopStyleVar(2);
        ImGui::SetNextWindowPos({vp->WorkPos.x,vp->WorkPos.y+vp->WorkSize.y-statusHeight});ImGui::SetNextWindowSize({vp->WorkSize.x,statusHeight});
        ImGui::SetNextWindowViewport(vp->ID);
        ImGui::Begin(label3D("Status##Fixed"),nullptr,ImGuiWindowFlags_NoTitleBar|ImGuiWindowFlags_NoMove|ImGuiWindowFlags_NoResize|ImGuiWindowFlags_NoDocking|ImGuiWindowFlags_NoScrollbar|ImGuiWindowFlags_NoScrollWithMouse);
        const auto backend=simulation->backendInfo();
        char engine[80];
        if(backend.kind==BackendKind3D::Cuda)std::snprintf(engine,sizeof(engine),"CUDA Float64");
        else std::snprintf(engine,sizeof(engine),tr3D("CPU %s (%d workers)"),tr3D(backend.openMpAvailable?"OpenMP":"serial"),backend.workerThreads);
        ImGui::Text(tr3D("%s | t %.4f s | %d / %d | dt %.4f ms | compute %.3f s | mean %.3f ms/step"),engine,simulation->currentStep()*simulation->dt(),simulation->currentStep(),simulation->totalSteps(),simulation->dt()*1000,simulation->diagnostics().timing.totalStepMs/1000,simulation->diagnostics().timing.averageStepMs());
        if(!error.empty()) ImGui::TextColored(palette3D().light?ImVec4(.72f,.16f,.12f,1):ImVec4(1,.43f,.43f,1),"%s",tr3D(error.c_str()));else ImGui::TextDisabled("%s",tr3D(status.c_str()));
        if(ImGui::IsItemHovered())ImGui::SetTooltip("%s",tr3D(error.empty()?status.c_str():error.c_str()));
        ImGui::End();
    }

    bool themeSelector(const char* id,float width) {
        return themeCombo3D(id,theme,width);
    }
    void toolbar() {
        SeisWave::Ui::setGlassSceneActive(running);
        if(workspace.beginFrame()){resetLayout=false;lastParameterMode=parameterHost.mode();}
        if(!workspace.message.empty())status=std::exchange(workspace.message,{});
        dirty=edited!=simulation->parameters()||selectedBackend!=static_cast<int>(simulation->backendInfo().kind)||computeSettings!=activeComputeSettings;
        if(SeisWave::Ui::beginWorkbenchMenuBar()) {
            if(functionButton3D("Home",FunctionRole3D::Model))homeRequested=true;
            homeRect={ImGui::GetItemRectMin(),ImGui::GetItemRectMax()};ImGui::SameLine();
            ImGui::TextColored(palette3D().accent,"SEIS3D");
            const float menuWidth=SeisWave::Ui::MenuBarOverflow::width({label3D("Workspace"),label3D("Appearance")})+ImGui::GetFontSize()*18+48;
            if(SeisWave::Ui::MenuBarOverflow more(menuWidth,language3D()?"更多":"More");more.open){
            if(SeisWave::Ui::beginDropdownMenu(label3D("Workspace"))) {
                if(ImGui::MenuItem(label3D("Reset layout [F]")))restoreWorkspace();
                if(workspace.menu(3,panels)){resetLayout=false;lastParameterMode=parameterHost.mode();}
                if(SeisWave::Ui::beginDropdownMenu(language3D()?"参数面板显示":"Parameter panels")){
                    if(parameterHost.menu())panels[0]=true;
                    ImGui::EndMenu();
                }
                ImGui::Separator();
                for(int i=0;i<7;++i)ImGui::MenuItem(label3D(panelNames[i]),nullptr,&panels[i]);
                ImGui::Separator();
                const char* layouts[]{"Balanced 2x2","Spatial focus","Tabbed views","Records focus"};
                for(int i=0;i<4;++i)if(ImGui::MenuItem(label3D(layouts[i]),nullptr,layoutPreset==i)){layoutPreset=i;resetLayout=true;}
                ImGui::EndMenu();
            }
            if(SeisWave::Ui::beginDropdownMenu(label3D("Appearance"))) {
                ImGui::TextUnformatted(tr3D("Interface font size (px)"));
                ImGui::SliderFloat("##menuFont",&uiFontSize,16,28,"%.0f px");glassAppearanceControls3D();ImGui::EndMenu();
            }
            const float languageWidth=std::max(140.0f,ImGui::GetFontSize()*7),themeWidth=std::max(185.0f,ImGui::GetFontSize()*11);
            const float right=SeisWave::Ui::workbenchMenuRightEdge()-languageWidth-themeWidth-24;
            if(!more.collapsed&&ImGui::GetCursorPosX()<right)ImGui::SetCursorPosX(right);
            int lang=language3D();ImGui::SetNextItemWidth(languageWidth);
            if(ImGui::Combo("##language",&lang,"English\0简体中文\0"))setLanguage3D(lang);
            themeSelector("##topTheme",themeWidth);
            }
            SeisWave::Ui::endWorkbenchMenuBar();
        }
        const auto* vp=ImGui::GetMainViewport();
        const char* commands[]{running?"Pause [Space]":"Run [Space]","Step [N]","Clear fields [C]","Fit views [R]","Shortcuts [F1]","Apply / rebuild"};
        const auto elapsed=std::string(language3D()?"计算耗时：":"Compute time: ")+SeisWave::Ui::computeElapsedText(simulation->diagnostics().timing.totalStepMs);
        char progress[128];std::snprintf(progress,sizeof(progress),"%d / %d | t %.4f s | dt %.4f ms",simulation->currentStep(),simulation->totalSteps(),simulation->currentStep()*simulation->dt(),simulation->dt()*1000);
        const auto& style=ImGui::GetStyle();std::vector<float> widths;
        for(int i=0;i<(dirty?6:5);++i)widths.push_back(ImGui::CalcTextSize(tr3D(commands[i])).x+2*style.FramePadding.x);
        widths.push_back(ImGui::CalcTextSize(elapsed.c_str()).x);widths.push_back(ImGui::CalcTextSize(progress).x+2*style.FramePadding.x);
        const int rows=SeisWave::Ui::commandStripRows(widths,vp->WorkSize.x-2*style.WindowPadding.x,style.ItemSpacing.x);
        toolbarHeight=rows*ImGui::GetFrameHeight()+(rows-1)*style.ItemSpacing.y+2*style.WindowPadding.y;
        ImGui::SetNextWindowPos(vp->WorkPos);ImGui::SetNextWindowSize({vp->WorkSize.x,toolbarHeight});
        ImGui::Begin("##Commands3D",nullptr,ImGuiWindowFlags_NoTitleBar|ImGuiWindowFlags_NoMove|ImGuiWindowFlags_NoResize|ImGuiWindowFlags_NoDocking|ImGuiWindowFlags_NoScrollbar);
        bool first=true;const auto command=[&](const char* label,FunctionRole3D role){if(!first)ImGui::SameLine();first=false;const float width=ImGui::CalcTextSize(tr3D(label)).x+2*ImGui::GetStyle().FramePadding.x;if(ImGui::GetContentRegionAvail().x<width)ImGui::NewLine();return functionButton3D(label,role);};
        ImGui::BeginDisabled(simulation->completed());
        if(command(commands[0],FunctionRole3D::Run))running=!running;
        if(command(commands[1],FunctionRole3D::Run)){running=false;(void)simulation->advance(1,error);}
        ImGui::EndDisabled();if(command(commands[2],FunctionRole3D::Danger))(void)clearFields();
        if(command(commands[3],FunctionRole3D::Display))resetViews();
        if(command(commands[4],FunctionRole3D::Display))shortcutHelp=!shortcutHelp;
        if(dirty&&command(commands[5],FunctionRole3D::Model))(void)rebuild(true);
        ImGui::SameLine();if(ImGui::GetContentRegionAvail().x<widths[widths.size()-2])ImGui::NewLine();ImGui::AlignTextToFramePadding();
        const auto currentElapsed=std::string(language3D()?"计算耗时：":"Compute time: ")+SeisWave::Ui::computeElapsedText(simulation->diagnostics().timing.totalStepMs);
        ImGui::TextColored(functionColor3D(FunctionRole3D::Run),"%s",currentElapsed.c_str());elapsedRect={ImGui::GetItemRectMin(),ImGui::GetItemRectMax()};
        if(ImGui::IsItemHovered())ImGui::SetTooltip("%s",SeisWave::Ui::computeElapsedHelp(language3D()!=0));
        ImGui::SameLine();if(ImGui::GetContentRegionAvail().x<widths.back())ImGui::NewLine();
        std::snprintf(progress,sizeof(progress),"%d / %d | t %.4f s | dt %.4f ms",simulation->currentStep(),simulation->totalSteps(),simulation->currentStep()*simulation->dt(),simulation->dt()*1000);
        ImGui::ProgressBar(static_cast<float>(simulation->currentStep())/std::max(1,simulation->totalSteps()),{-1,ImGui::GetFrameHeight()},progress);
        ImGui::End();
    }

    // Start with only the compute core open, including tabs first visited later.
    // Later epochs apply Expand/Collapse once to every group without resetting manual changes.
    bool group(const char* label,bool open=false) {
        const auto id=ImGui::GetID(label);
        auto [entry,firstVisit]=groupApplied.try_emplace(id,-1);
        if(firstVisit||entry->second!=groupEpoch){ImGui::SetNextItemOpen(groupEpoch==0?std::strcmp(label,"Compute backend")==0:groupsExpanded,ImGuiCond_Always);entry->second=groupEpoch;}
        FunctionRole3D role=FunctionRole3D::Display;
        const std::string name=label;
        if(name.find("Grid")!=std::string::npos||name.find("Absorbing")!=std::string::npos)role=FunctionRole3D::Warning;
        else if(name.find("VTS")!=std::string::npos||name.find("CSV")!=std::string::npos)role=FunctionRole3D::Export;
        else if(name.find("material")!=std::string::npos||name.find("Geometry")!=std::string::npos||name.find("backend")!=std::string::npos)role=FunctionRole3D::Model;
        else if(name.find("Run")!=std::string::npos||name.find("Time")!=std::string::npos)role=FunctionRole3D::Run;
        pushFunctionStyle3D(role);const bool expanded=ImGui::CollapsingHeader(label3D(label),open?ImGuiTreeNodeFlags_DefaultOpen:0);popFunctionStyle3D();return expanded;
    }
    bool parameter(const char* label,double& value,double step,const char* format) {
        ImGui::TextUnformatted(tr3D(label));ImGui::SetNextItemWidth(-1);
        return ImGui::InputDouble((std::string("##")+label).c_str(),&value,step,step*5,format);
    }

    void modelPresets() {
        dimensionSummary();
        ImGui::TextUnformatted(tr3D("Model preset (X / Y / Z)"));
        int matching=-1;
        for(int i=0;i<static_cast<int>(modelPresets3D.size());++i){const auto& p=modelPresets3D[i];if(edited.nx==p.nx&&edited.ny==p.ny&&edited.nz==p.nz&&edited.spacing==p.spacing&&edited.topographyAmplitude==p.relief&&edited.topographyWavelength==p.wavelength&&(i!=0||edited==applyModelPreset3D(0,edited)))matching=i;}
        ImGui::SetNextItemWidth(-1);
        if(ImGui::BeginCombo("##modelPreset",matching>=0?tr3D(modelPresets3D[matching].name):tr3D("Custom geometry"))) {
            for(int i=0;i<static_cast<int>(modelPresets3D.size());++i)if(ImGui::Selectable(label3D(modelPresets3D[i].name),matching==i)){
                edited=applyModelPreset3D(i,edited);if(i==0)materialPreset=0;
                dirty=true;status=i==0?"Benchmark staged: 12 Hz, source (100,10,50), 0.5 s, uniform material and 6-cell sponge. Apply to rebuild.":"Geometry preset staged. Valid source and stop settings are retained; out-of-range sources are fitted.";
            }
            ImGui::EndCombo();
        }
        if(ImGui::BeginTable("##cubicPresets",3,ImGuiTableFlags_SizingStretchSame)){for(int i=4;i<7;++i){ImGui::TableNextColumn();char label[16];std::snprintf(label,sizeof(label),"%d^3",modelPresets3D[i].nx);if(functionButton3D(label,FunctionRole3D::Model,{-1,0})){edited=applyModelPreset3D(i,edited);dirty=true;}}ImGui::EndTable();}
        memorySummary();stagedTimeStepSummary();
        ImGui::TextWrapped(tr3D("The benchmark preset loads the full simulation case. Other presets edit geometry and fit out-of-range sources. Apply to rebuild; display settings are retained."));
        if(matching==0)ImGui::TextWrapped(tr3D("Benchmark: 12 Hz | source (100,10,50) | 0.5 s | Vp/Vs 3000/1700 | rho 2200 | sponge 6/6/6. Flat switches to the upper-row case."));
        ImGui::Separator();
    }

    void layerTable(const char* id,float maxHeight=310) {
        const float height=std::min(maxHeight,ImGui::GetTextLineHeightWithSpacing()*(edited.layers.size()+2)+20);
        const bool expanded=maxHeight>310;const auto widthFlags=expanded?ImGuiTableColumnFlags_WidthStretch:ImGuiTableColumnFlags_WidthFixed;
        if(ImGui::BeginTable(id,5,ImGuiTableFlags_Borders|ImGuiTableFlags_RowBg|ImGuiTableFlags_ScrollX|ImGuiTableFlags_ScrollY,{0,height})) {
            ImGui::TableSetupColumn(tr3D("Layer"),widthFlags,expanded?1.8f:165);ImGui::TableSetupColumn(tr3D("Thickness (m)"),widthFlags,expanded?1.3f:88);
            ImGui::TableSetupColumn("Vp (m/s)",widthFlags,expanded?1.3f:90);ImGui::TableSetupColumn("Vs (m/s)",widthFlags,expanded?1.3f:90);ImGui::TableSetupColumn(tr3D("Density (kg/m3)"),widthFlags,expanded?2.0f:110);ImGui::TableSetupScrollFreeze(1,1);ImGui::TableHeadersRow();
            for(std::size_t i=0;i<edited.layers.size();++i){auto& l=edited.layers[i];ImGui::PushID(static_cast<int>(i));ImGui::TableNextRow();ImGui::TableNextColumn();ImGui::TextWrapped("%s",tr3D(l.name.c_str()));ImGui::TableNextColumn();
                if(i+1==edited.layers.size())ImGui::TextDisabled("%s",tr3D("Auto"));else{ImGui::SetNextItemWidth(-1);dirty|=ImGui::InputDouble("##thickness",&l.thickness,0,0,"%.2f");}
                ImGui::TableNextColumn();ImGui::SetNextItemWidth(-1);dirty|=ImGui::InputDouble("##vp",&l.vp,0,0,"%.1f");ImGui::TableNextColumn();ImGui::SetNextItemWidth(-1);dirty|=ImGui::InputDouble("##vs",&l.vs,0,0,"%.1f");ImGui::TableNextColumn();ImGui::SetNextItemWidth(-1);dirty|=ImGui::InputDouble("##rho",&l.density,0,0,"%.1f");ImGui::PopID();}
            ImGui::EndTable();
        }
    }

    void materialControls() {
        ImGui::TextColored(functionColor3D(FunctionRole3D::Model),tr3D("Layered material"));
        ImGui::SetNextItemWidth(-1);
        if(ImGui::Combo("##materialPreset",&materialPreset,choices3D(materialPresetNames3D))){edited=applyMaterialPreset3D(materialPreset,edited);dirty=true;}
        ImGui::TextWrapped(tr3D("Material presets stage physical properties only. Apply changes to rebuild. Other than the supplied loess table, presets are illustrative test models."));
        if(ImGui::Checkbox(label3D("Enable layered material"),&edited.layeredMaterial)){if(edited.layeredMaterial&&edited.layers.empty())edited=applyMaterialPreset3D(1,edited);dirty=true;}
        if(!edited.layeredMaterial){dirty|=parameter("Vp (m/s)",edited.vp,100,"%.1f");dirty|=parameter("Vs (m/s)",edited.vs,100,"%.1f");dirty|=parameter("Density (kg/m3)",edited.density,50,"%.1f");return;}
        ImGui::TextUnformatted(tr3D("Layer depth coordinates"));ImGui::SetNextItemWidth(-1);int depthMode=static_cast<int>(edited.layerDepthMode);
        if(ImGui::Combo("##layerDepthMode",&depthMode,choices3D("Grid layers (same k, same material)\0Physical depth below terrain\0Physical datum Z\0"))){edited.layerDepthMode=static_cast<LayerDepthMode3D>(depthMode);dirty=true;}
        ImGui::TextWrapped(tr3D(edited.layerDepthMode==LayerDepthMode3D::GridIndex?"Default: thickness uses computational depth k*h. All horizontal nodes at the same k share Vp, Vs and density; actual thickness follows mesh deformation.":"Physical-depth modes rasterize interfaces onto nodes and can produce stair steps. Last layer fills remaining model depth."));
        layerTable("##layerTable");
        if(ImGui::Button(label3D("Expand layer editor"),{-1,0})||requestLayerEditor){layerEditorOpen=true;requestLayerEditor=false;ImGui::OpenPopup(label3D("Layer editor"));}
        const auto* vp=ImGui::GetMainViewport();ImGui::SetNextWindowSize({std::min(1000.0f,vp->WorkSize.x*.92f),std::min(520.0f,vp->WorkSize.y*.85f)},ImGuiCond_Appearing);
        if(ImGui::BeginPopupModal(label3D("Layer editor"),&layerEditorOpen,ImGuiWindowFlags_NoSavedSettings)) {
            layerTable("##expandedLayers",350);
            ImGui::TextWrapped(tr3D("Edits are staged. Close this editor, then Apply changes / rebuild."));
            if(functionButton3D("Done",FunctionRole3D::Model,{-1,0})){layerEditorOpen=false;ImGui::CloseCurrentPopup();}ImGui::EndPopup();
        }
        ImGui::BeginDisabled(edited.layers.size()>=8);if(ImGui::Button(label3D("Add layer"))){edited.layers.insert(edited.layers.end()-1,{"New layer",5,1500,700,2050});dirty=true;}ImGui::EndDisabled();ImGui::SameLine();ImGui::BeginDisabled(edited.layers.size()<=2);if(ImGui::Button(label3D("Remove layer"))){edited.layers.erase(edited.layers.end()-2);dirty=true;}ImGui::EndDisabled();
        if(functionButton3D("Stage 1 m layered example / full duration",FunctionRole3D::Model,{-1,0})){
            edited.nx=100;edited.ny=20;edited.nz=100;edited.spacing=1;edited.topographyAmplitude=0;edited.topographyWavelength=60;edited.terrainTransitionDepth=0;edited.sourceX=edited.sourceY=-1;edited.sourceZ=3;edited.dominantFrequency=12;edited.duration=.40;edited.maxSteps=0;dirty=true;
        }
        ImGui::TextWrapped(tr3D("Stages 100 x 20 x 100, flat terrain, 1 m spacing, 12 Hz, 0.40 s and max steps 0. This resolves a 5 m layer with five cells; apply to start at t=0."));
        if(functionButton3D("View active Vp layers",FunctionRole3D::Display,{-1,0})){visuals.displayMode=1;visuals.modelComponent=0;slices.y=simulation->parameters().ny/2;}
        const auto& d=simulation->diagnostics();if(simulation->parameters().layeredMaterial){ImGui::TextWrapped(tr3D("Active material: Vp max %.0f | Vs min %.0f m/s | thinnest layer %.1f cells"),d.materialMaxVp,d.materialMinVs,d.minLayerCells);if(d.unresolvedLayers)ImGui::TextColored(functionColor3D(FunctionRole3D::Warning),tr3D("Missing / under-resolved layers: %d. Refine spacing or extend model depth."),d.unresolvedLayers);}
    }

    void receiverControls() {
        auto& a=edited.receiverArray;int mode=static_cast<int>(a.kind);
        if(group("Receiver geometry",true)) {
            ImGui::SetNextItemWidth(-1);
            if(ImGui::Combo("##receiverMode",&mode,choices3D("Automatic X line\0Custom X line\0Custom Y line\0X-Y receiver grid\0"))){a.kind=static_cast<ReceiverArrayKind3D>(mode);a.startX=mode==2?-1:2;a.startY=mode==1?-1:2;a.countX=std::max(1,std::min(16,edited.nx-4));a.countY=mode==3?std::max(1,std::min(8,edited.ny-4)):mode==2?std::max(1,std::min(16,edited.ny-4)):1;a.strideX=a.strideY=1;dirty=true;}
            ImGui::TextUnformatted(tr3D("Receiver depth index (zero-based)"));ImGui::SetNextItemWidth(-1);dirty|=ImGui::InputInt("##receiverZ",&a.z);
            if(a.kind!=ReceiverArrayKind3D::AutoXLine) {
                int start[]{a.startX,a.startY};ImGui::TextUnformatted(tr3D("Start X / Y (-1 = center)"));ImGui::SetNextItemWidth(-1);if(ImGui::InputInt2("##receiverStart",start)){a.startX=start[0];a.startY=start[1];dirty=true;}
                if(a.kind!=ReceiverArrayKind3D::YLine){ImGui::TextUnformatted(tr3D("X count / stride (nodes)"));int x[]{a.countX,a.strideX};ImGui::SetNextItemWidth(-1);if(ImGui::InputInt2("##receiverCountX",x)){a.countX=x[0];a.strideX=x[1];dirty=true;}}
                if(a.kind!=ReceiverArrayKind3D::XLine){ImGui::TextUnformatted(tr3D("Y count / stride (nodes)"));int y[]{a.countY,a.strideY};ImGui::SetNextItemWidth(-1);if(ImGui::InputInt2("##receiverCountY",y)){a.countY=y[0];a.strideY=y[1];dirty=true;}}
            }
            std::vector<std::array<int,3>> nodes;std::string why;
            const auto limits=receiverLimits3D(edited);
            ImGui::TextWrapped(tr3D("Position capacity X / Y: %d / %d | total cap %d"),limits.x,limits.y,maxReceiverTraces3D);
            if(a.kind==ReceiverArrayKind3D::GridXY)ImGui::TextWrapped(tr3D("At current counts: X <= %d; Y <= %d (coupled total limit)"),limits.xAtCurrentY,limits.yAtCurrentX);
            if(a.kind!=ReceiverArrayKind3D::AutoXLine&&functionButton3D("Fit receiver counts",FunctionRole3D::Warning,{-1,0})) {
                if(fitReceiverCounts3D(edited)){dirty=true;error.clear();status="Receiver counts fitted to start, stride, bounds and total cap. Apply to deploy.";}
                else error="Receiver start, depth or active stride is invalid; correct it before fitting counts.";
            }
            if(receiverNodes3D(edited,nodes,why)){ImGui::TextWrapped(tr3D("Staged receivers: %d | X spacing %.2f m | Y spacing %.2f m"),static_cast<int>(nodes.size()),a.kind==ReceiverArrayKind3D::YLine?0:edited.spacing*(a.kind==ReceiverArrayKind3D::AutoXLine?1:a.strideX),(a.kind==ReceiverArrayKind3D::YLine||a.kind==ReceiverArrayKind3D::GridXY)?edited.spacing*a.strideY:0);}
            else ImGui::TextWrapped("%s",tr3D(why.c_str()));
            ImGui::TextWrapped(tr3D("Capacity = 1 + floor((N - 1 - start) / stride). Auto X excludes two nodes at each end. A grid is numbered X-fast, then Y; counts mean positions, not intervals. Apply to deploy."));
        }
        if(group("Receiver sampling",true)) {
            if(functionButton3D("Locate active receiver",FunctionRole3D::Display,{-1,0})&&!simulation->receivers().empty()) {
                const auto& g=simulation->grid();const auto q=simulation->receivers()[std::clamp(selectedReceiver,0,static_cast<int>(simulation->receivers().size())-1)].gridIndex;
                slices={static_cast<int>(q%g.nx)-g.ghost,static_cast<int>((q/g.nx)%g.ny)-g.ghost,static_cast<int>(q/(static_cast<std::size_t>(g.nx)*g.ny))-g.ghost};sliceViews={};
            }
            ImGui::Text(tr3D("Active receivers: %d"),static_cast<int>(simulation->receivers().size()));
            ImGui::Text(tr3D("Native sampling dt: %.6f ms"),simulation->dt()*1000);
            ImGui::TextWrapped(tr3D("Vx / Vy / Vz are sampled after every accepted step, with t=0 included. Rebuild resets all traces. Identical arrays are required for CPU/CUDA comparisons."));
        }
    }

    void rememberPerformance() {
        if(!simulation||!simulation->initialized()||!simulation->diagnostics().timing.measuredSteps)return;
        PerformanceRun3D row{performanceSession,simulation->parameters(),simulation->backendInfo(),simulation->diagnostics().timing,simulation->dt(),simulation->currentStep(),static_cast<int>(simulation->receivers().size())};
        auto found=std::find_if(performanceRuns.begin(),performanceRuns.end(),[&](const auto& run){return run.session==row.session;});
        if(found!=performanceRuns.end())*found=std::move(row);else {performanceRuns.push_back(std::move(row));if(performanceRuns.size()>16)performanceRuns.erase(performanceRuns.begin());}
    }
    void timingSummary() {
        const auto& t=simulation->diagnostics().timing;
        ImGui::TextWrapped(tr3D("Compute total: %.6f s | measured steps: %llu"),t.totalStepMs/1000,static_cast<unsigned long long>(t.measuredSteps));
        ImGui::TextWrapped(tr3D("Last / mean step: %.4f / %.4f ms"),t.lastStepMs,t.averageStepMs());
        ImGui::TextWrapped(tr3D("Initialization: %.4f s (separate)"),t.initializationMs/1000);
        if(t.totalStepMs>0)ImGui::TextWrapped(tr3D("%.1f steps/s | simulated / wall: %.3fx"),1000*t.measuredSteps/t.totalStepMs,simulation->currentStep()*simulation->dt()*1000/t.totalStepMs);
        if(t.measuredSteps)ImGui::Text(tr3D("Remaining compute ~%.2f s"),(simulation->totalSteps()-simulation->currentStep())*t.averageStepMs()/1000);
        if(const auto* worker=dynamic_cast<const AsyncSimulation3D*>(simulation.get())) {
            const auto time=worker->backgroundTiming();const auto cache=renderer.cacheStatistics();
            ImGui::TextWrapped(language3D()?"后台推进 %.3f s | 发布传输 %.3f s | 批次 %llu":"Background stepping %.3f s | publication transfer %.3f s | batches %llu",time.computeMs/1000,time.transferMs/1000,static_cast<unsigned long long>(time.batches));
            ImGui::TextWrapped(language3D()?"场景重绘 / 复用 %llu / %llu | 磨砂刷新 / 绘制 %d / %d":"Scene builds / reuse %llu / %llu | glass refresh / draw %d / %d",static_cast<unsigned long long>(cache[0]),static_cast<unsigned long long>(cache[1]),SeisWave::Ui::glassBackdropRefreshes(),SeisWave::Ui::glassBackdropPasses());
        }
    }
    void computeControls() {
    const auto backend=simulation->backendInfo();
    ImGui::TextUnformatted(tr3D("Compute core (apply on rebuild)"));ImGui::SetNextItemWidth(-1);
    if(ImGui::BeginCombo("##backend",tr3D(selectedBackend==0?"CPU / OpenMP (Float64)":"CUDA GPU (Float64)"))) {
        if(ImGui::Selectable(label3D("CPU / OpenMP (Float64)"),selectedBackend==0)){selectedBackend=0;dirty=true;}
        ImGui::BeginDisabled(!cudaAvailability.available);
        if(ImGui::Selectable(label3D("CUDA GPU (Float64)"),selectedBackend==1)){selectedBackend=1;dirty=true;}
        ImGui::EndDisabled();ImGui::EndCombo();
    }
    ImGui::TextUnformatted(tr3D("CUDA device (apply on rebuild)"));ImGui::SetNextItemWidth(-1);
    const auto staged=selectCudaDevice3D(cudaDevices,computeSettings.cudaDevice);
    const std::string preview=computeSettings.cudaDevice<0?tr3D("Auto (largest usable VRAM)"):staged.description;
    if(ImGui::BeginCombo("##cudaDevice",preview.c_str())) {
        if(ImGui::Selectable(tr3D("Auto (largest usable VRAM)"),computeSettings.cudaDevice<0)){computeSettings.cudaDevice=-1;dirty=true;}
        for(const auto& device:cudaDevices.devices) {
            const auto label="GPU "+std::to_string(device.index)+" | "+device.name;
            ImGui::BeginDisabled(!device.available);
            if(ImGui::Selectable(label.c_str(),computeSettings.cudaDevice==device.index)){computeSettings.cudaDevice=device.index;dirty=true;}
            if(ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))ImGui::SetTooltip("%s",device.description.c_str());
            ImGui::EndDisabled();
        }
        ImGui::EndCombo();
    }
    if(functionButton3D("Refresh GPU devices",FunctionRole3D::Model,{-1,0}))refreshComputeDevices();
    ImGui::TextWrapped(tr3D("CUDA: %s"),staged.description.c_str());
    ImGui::TextWrapped(tr3D("Active: %s"),tr3D(backend.deviceName.c_str()));
    if(ImGui::TreeNodeEx(label3D("Detected graphics adapters"),ImGuiTreeNodeFlags_DefaultOpen)) {
        for(const auto& name:graphicsAdapters)ImGui::BulletText("%s",name.c_str());
        ImGui::TextWrapped(tr3D("Intel/AMD adapters are display devices; CUDA requires NVIDIA."));
        if(cudaDevices.devices.empty())ImGui::TextWrapped("%s",cudaDevices.description.c_str());
        else ImGui::TextWrapped(tr3D("%d CUDA device(s) detected."),static_cast<int>(cudaDevices.devices.size()));
        for(const auto& device:cudaDevices.devices){ImGui::Separator();ImGui::TextWrapped(tr3D("GPU %d | %s | CC %d.%d | %.2f / %.2f GiB free"),device.index,device.name.c_str(),device.computeMajor,device.computeMinor,device.freeMemoryBytes/(1024.0*1024*1024),device.totalMemoryBytes/(1024.0*1024*1024));ImGui::TextWrapped("%s",device.available?tr3D("Available"):device.description.c_str());if(!device.pciBusId.empty())ImGui::Text("PCI: %s",device.pciBusId.c_str());}
        ImGui::TreePop();
    }
    ImGui::TextWrapped(tr3D("Core changes start a new session at t=0. Failed initialization retains the active session."));
    ImGui::BeginDisabled(selectedBackend!=0);
    ImGui::TextUnformatted(tr3D("CPU workers (0 = auto, 1 = serial)"));ImGui::SetNextItemWidth(-1);
    dirty|=ImGui::SliderInt(label3D("##workers"),&computeSettings.cpuThreads,0,backend.openMpAvailable?maxCpuThreads3D:1);
    ImGui::Text(tr3D("OpenMP: %s | configured workers: %d"),tr3D(backend.openMpAvailable?"available":"unavailable"),backend.workerThreads);
    ImGui::TextWrapped(tr3D("Worker changes apply on rebuild. Auto uses at most 8 workers; small grids may remain serial to avoid overhead."));
    ImGui::EndDisabled();
    }

    void performanceControls() {
        if(group("Compute backend",true))computeControls();
        if(group("Measured compute time",true)) {
            timingSummary();const auto& t=simulation->diagnostics().timing;
            ImGui::Text(tr3D("Step min / max: %.4f / %.4f ms"),t.minStepMs,t.maxStepMs);
            ImGui::TextWrapped(tr3D("Wall time of accepted RK4 steps, diagnostics and receiver sampling. CUDA completion is synchronized by readback. Initialization, rendering, pauses and export are excluded; warm-up steps are included."));
        }
        if(group("CPU / CUDA comparison",true)) {
            rememberPerformance();
            ImGui::TextWrapped(tr3D("Runs are retained when rebuilding or switching cores. Speedup requires identical physical settings, receiver arrays, dt and completed step count. CPU worker count is recorded."));
            const PerformanceRun3D* cpu=nullptr;const PerformanceRun3D* gpu=nullptr;
            for(auto i=performanceRuns.rbegin();i!=performanceRuns.rend()&&!cpu;++i)for(auto j=performanceRuns.rbegin();j!=performanceRuns.rend();++j)if(i->backend.kind!=j->backend.kind&&comparableRuns3D(*i,*j)){cpu=i->backend.kind==BackendKind3D::Cpu?&*i:&*j;gpu=i->backend.kind==BackendKind3D::Cuda?&*i:&*j;break;}
            if(cpu)ImGui::TextColored(functionColor3D(FunctionRole3D::Run),tr3D("Matched %d steps | CPU / CUDA: %.3fx"),cpu->steps,cpu->timing.totalStepMs/gpu->timing.totalStepMs);
            else ImGui::TextWrapped(tr3D("No matched CPU/CUDA pair yet. Repeat the same model and step count on the other core."));
            if(ImGui::BeginTable("##performanceRuns",5,ImGuiTableFlags_Borders|ImGuiTableFlags_RowBg|ImGuiTableFlags_ScrollX|ImGuiTableFlags_ScrollY,{0,210})) {
                ImGui::TableSetupColumn(tr3D("Core"),ImGuiTableColumnFlags_WidthFixed,80);ImGui::TableSetupColumn(tr3D("Steps"),ImGuiTableColumnFlags_WidthFixed,60);ImGui::TableSetupColumn(tr3D("Compute (s)"),ImGuiTableColumnFlags_WidthFixed,100);ImGui::TableSetupColumn("ms/step",ImGuiTableColumnFlags_WidthFixed,90);ImGui::TableSetupColumn(tr3D("Config"),ImGuiTableColumnFlags_WidthFixed,145);ImGui::TableSetupScrollFreeze(0,1);ImGui::TableHeadersRow();
                for(const auto& r:performanceRuns){ImGui::TableNextRow();ImGui::TableNextColumn();if(r.backend.kind==BackendKind3D::Cpu)ImGui::Text("CPU %d",r.backend.workerThreads);else ImGui::TextUnformatted("CUDA");ImGui::TableNextColumn();ImGui::Text("%d",r.steps);ImGui::TableNextColumn();ImGui::Text("%.6f",r.timing.totalStepMs/1000);ImGui::TableNextColumn();ImGui::Text("%.4f",r.timing.averageStepMs());ImGui::TableNextColumn();ImGui::TextUnformatted(configurationId3D(r.parameters).c_str());}ImGui::EndTable();
            }
            if(functionButton3D("Export timing CSV",FunctionRole3D::Export,{-1,0})){if(writePerformanceCsv3D(performanceRuns,outputRoot3D()/"performance_runs.csv",error))status="Timing report exported to out/performance_runs.csv.";}
            if(ImGui::Button(label3D("Clear comparison history"),{-1,0}))performanceRuns.clear();
        }
    }

    void sourceControls() {
        ImGui::TextColored(functionColor3D(FunctionRole3D::Warning),tr3D("Source"));
        if(SeisWave::Ui::sourceControls(edited.source,edited.sourceForce,edited.sourceMoment,false)) {
            dirty=true;
            if(edited.source.kind==SeisWave::SourceKind::IsotropicMoment && edited.nx>=11 && edited.ny>=11 && edited.nz>=11) {
                if(edited.sourceX>=0)edited.sourceX=std::clamp(edited.sourceX,5,edited.nx-6);
                if(edited.sourceY>=0)edited.sourceY=std::clamp(edited.sourceY,5,edited.ny-6);
                edited.sourceZ=std::clamp(edited.sourceZ,5,edited.nz-6);
            }
        }
        ImGui::TextUnformatted(tr3D("Source node X / Y / Z (zero-based)"));ImGui::SetNextItemWidth(-1);
        int source[3]{edited.sourceX,edited.sourceY,edited.sourceZ};
        if(ImGui::InputInt3("##sourceNodes",source)){edited.sourceX=source[0];edited.sourceY=source[1];edited.sourceZ=source[2];dirty=true;}
        ImGui::TextWrapped(edited.source.kind==SeisWave::SourceKind::IsotropicMoment?(language3D()?"X/Y=-1 表示居中；爆炸源心距各物理边界至少 5 个节点。":"X/Y=-1 centers source; isotropic moment center needs five nodes inside each physical face."):tr3D("X/Y = -1 centers the source. Keep the center at least two nodes inside each physical face."));
        if(ImGui::Button(label3D("Center source"),{-1,0})){edited.sourceX=edited.sourceY=-1;edited.sourceZ=edited.source.kind==SeisWave::SourceKind::IsotropicMoment?5:3;dirty=true;}
        if(ImGui::Button(label3D("Source slices"),{-1,0})){const auto index=sourceIndices3D(simulation->parameters());slices={index[0],index[1],index[2]};sliceViews={};}
        const auto activeSource=sourceIndices3D(simulation->parameters());const auto& activeGrid=simulation->grid();
        const auto& xyz=activeGrid.coordinates[activeGrid.index(activeSource[0]+activeGrid.ghost,activeSource[1]+activeGrid.ghost,activeSource[2]+activeGrid.ghost)];
        const double surfaceZ=activeGrid.coordinates[activeGrid.index(activeSource[0]+activeGrid.ghost,activeSource[1]+activeGrid.ghost,activeGrid.ghost)][2];
        ImGui::TextWrapped(tr3D("Active source XYZ: %.1f, %.1f, %.1f m | below surface %.1f m"),xyz[0],xyz[1],xyz[2],xyz[2]-surfaceZ);
        dirty|=parameter("Ricker f0 (Hz)",edited.dominantFrequency,1,"%.2f");
        const auto& d=simulation->diagnostics();
        if(edited.dominantFrequency!=simulation->parameters().dominantFrequency)ImGui::TextWrapped(tr3D("Frequency edit pending; active wavefield is unchanged."));
        ImGui::TextWrapped(tr3D("Sampling reference: %.2f Hz at 5 PPW; this is not a frequency input limit."),d.materialMinVs/(12.5*d.maxCellEdge));
        double minVs=edited.vs;if(edited.layeredMaterial){minVs=std::numeric_limits<double>::max();for(const auto& l:edited.layers)minVs=std::min(minVs,l.vs);}
        if(std::isfinite(minVs)&&minVs>0&&std::isfinite(edited.dominantFrequency)&&edited.dominantFrequency>0&&std::isfinite(edited.spacing)&&edited.spacing>0) {
            ImGui::TextWrapped(tr3D("Edited f0 %.1f Hz: target largest physical cell <= %.3f m for 8 PPW at 2.5 f0."),edited.dominantFrequency,minVs/(20*edited.dominantFrequency));
            ImGui::TextWrapped(tr3D("Nominal PPW at f0 / 2.5 f0: %.2f / %.2f (before terrain stretching)."),minVs/(edited.dominantFrequency*edited.spacing),minVs/(2.5*edited.dominantFrequency*edited.spacing));
        }
        ImGui::TextWrapped(tr3D("Refining spacing also shrinks physical size and changes terrain compression. Review source depth, flatten depth, duration and boundaries together."));
        samplingSummary();
        ImGui::TextWrapped(tr3D("Duration and maximum steps are configured in the Run tab."));
        ImGui::TextWrapped(language3D()?"体力源采用半径 2 格的高斯核；爆炸源采用其保守梯度，半径 4 格。类型、方向、幅值或主频改变后重建生效。":"Body force: radius-two Gaussian; explosion: its conservative gradient, radius four. Mechanism, direction, amplitude and frequency apply on rebuild.");
    }

    void workflowNavigation() {
        const char* shortNames[]{"Build","Survey","Run","View","Export"};
        const char* descriptions[]{"Modeling","Acquisition","Run & performance","Display","Export"};
        const FunctionRole3D roles[]{FunctionRole3D::Model,FunctionRole3D::Warning,FunctionRole3D::Run,FunctionRole3D::Display,FunctionRole3D::Export};
        const int defaults[]{1,4,0,2,3};
        const float width=std::max(1.0f,(ImGui::GetContentRegionAvail().x-4*ImGui::GetStyle().ItemSpacing.x)/5);
        float longest=1;for(const auto* label:shortNames)longest=std::max(longest,ImGui::CalcTextSize(tr3D(label)).x);
        const float font=std::clamp(ImGui::GetFontSize()*(width-10)/longest,12.0f,ImGui::GetFontSize());
        ImGui::PushFont(nullptr,font);ImGui::PushID("WorkflowNavigation");ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,{4,6});
        for(int i=0;i<5;++i) {
            if(i)ImGui::SameLine();
            pushFunctionStyle3D(roles[i]);
            if(ImGui::Button(label3D(shortNames[i]),{width,0})&&controlCategory!=i){controlCategory=i;requestedTab=defaults[i];}
            popFunctionStyle3D();workflowRects[i]={ImGui::GetItemRectMin(),ImGui::GetItemRectMax()};
            if(controlCategory==i){const auto& r=workflowRects[i];ImGui::GetWindowDrawList()->AddLine({r.Min.x+4,r.Max.y-2},{r.Max.x-4,r.Max.y-2},color3D(functionColor3D(roles[i])),3);}
            if(ImGui::IsItemHovered())ImGui::SetTooltip("%s",tr3D(descriptions[i]));
        }
        ImGui::PopStyleVar();ImGui::PopID();ImGui::PopFont();
    }

    // Scrollable parameter groups and a fixed footer keep Apply/Fit accessible at larger text sizes.
    void boundaryControls() {
        if(group("Absorbing boundary",true)) {
            auto& boundary=edited.absorbingBoundary;
            dirty|=ImGui::Checkbox(label3D("Enable numerical absorption"),&boundary.enabled);
            ImGui::TextUnformatted(tr3D("Sponge cells X / Y / bottom Z"));ImGui::SetNextItemWidth(-1);
            dirty|=ImGui::InputInt3("##spongeCells",boundary.cells.data());
            dirty|=parameter("Sponge strength (0.1..10)",boundary.strength,.1,"%.2f");
            ImGui::Checkbox(label3D("Show absorbing region"),&visuals.showAbsorbingLayer);
            ImGui::TextUnformatted(tr3D("Absorbing overlay opacity"));
            ImGui::SetNextItemWidth(-1);ImGui::SliderFloat("##absorbingOpacity",&visuals.absorbingOpacity,.05f,.6f,"%.2f");
            const auto widths=absorbingWidths3D(simulation->parameters());
            ImGui::TextWrapped(tr3D("Active sponge cells: %d / %d / %d"),widths[0],widths[1],widths[2]);
            ImGui::TextWrapped(tr3D("Display toggles apply immediately. Absorption edits require rebuild. Each width is capped at one third of its axis. Top remains traction-free."));
            ImGui::TextWrapped(tr3D("Disabling absorption leaves reflecting outer boundaries; it does not create an infinite domain."));
        }
    }
    void controls() {recordView.workspace=&workspace;if(parameterHost.mode()!=1)controlsPanel(false);}
    void floatingControls() {
        if(parameterHost.mode()==0)return;
        std::swap(controlCategory,floatingCategory);std::swap(requestedTab,floatingRequestedTab);std::swap(nextRequestedTab,floatingNextRequestedTab);
        controlsPanel(true);
        std::swap(controlCategory,floatingCategory);std::swap(requestedTab,floatingRequestedTab);std::swap(nextRequestedTab,floatingNextRequestedTab);
    }
    void controlsPanel(bool floating) {
        if(!floating&&!panels[0])return;
        const char* title=floating?(language3D()?"悬浮参数 · 三维###FloatingParameters3D":"Floating parameters · 3D###FloatingParameters3D"):label3D("Controls");
        if(floating)parameterHost.prepare(title);
        bool open=true;
        const bool visible=ImGui::Begin(title,floating?&open:&panels[0],floating?parameterHost.flags():0);
        if(floating&&!open)SeisWave::Ui::setParameterDisplayMode(3,0);
        if(!visible){ImGui::End();return;}
        if(floating)parameterHost.tools();
        if(!simulation||!simulation->initialized()){ImGui::TextWrapped("%s",error.c_str());homeRequested=true;ImGui::End();return;}
        const float footer=ImGui::GetFrameHeight()+ImGui::GetTextLineHeightWithSpacing()*2+16;
        if(nextRequestedTab>=0){requestedTab=nextRequestedTab;nextRequestedTab=-1;}
        if(requestedTab>=0)controlCategory=requestedTab==1||requestedTab==5||requestedTab==8?0:requestedTab==4||requestedTab==6?1:requestedTab==0||requestedTab==7?2:requestedTab==2?3:4;
        workflowNavigation();
        if(ImGui::SmallButton(label3D("Expand all"))){++groupEpoch;groupsExpanded=true;}ImGui::SameLine();
        if(ImGui::SmallButton(label3D("Collapse all"))){++groupEpoch;groupsExpanded=false;}
        ImGui::BeginChild("ControlScroll",{0,-footer},ImGuiChildFlags_None);
        if(ImGui::BeginTabBar("ControlTabs")) {
            if(controlCategory==2&&ImGui::BeginTabItem(label3D("Performance"),nullptr,requestedTab==7?ImGuiTabItemFlags_SetSelected:0)){performanceControls();ImGui::EndTabItem();}
            if(controlCategory==1&&ImGui::BeginTabItem(label3D("Receivers"),nullptr,requestedTab==6?ImGuiTabItemFlags_SetSelected:0)){receiverControls();ImGui::EndTabItem();}
            if(controlCategory==2&&ImGui::BeginTabItem(label3D("Run"),nullptr,requestedTab==0?ImGuiTabItemFlags_SetSelected:0)) {
                ImGui::TextColored(functionColor3D(FunctionRole3D::Run),tr3D("01   SIMULATION CONTROL"));
                ImGui::TextUnformatted(tr3D(simulation->completed()?"Completed":running?"Running":"Paused / ready"));
                const float progress=static_cast<float>(simulation->currentStep())/std::max(1,simulation->totalSteps());
                ImGui::ProgressBar(progress,{-1,0});
                ImGui::Text(tr3D("Step %d / %d"),simulation->currentStep(),simulation->totalSteps());
                ImGui::Text(tr3D("Time %.5f / %.5f s"),simulation->currentStep()*simulation->dt(),simulation->totalSteps()*simulation->dt());
                if(group("Measured compute time",true))timingSummary();
                ImGui::BeginDisabled(simulation->completed());
                if(functionButton3D(running?"Pause":"Run",FunctionRole3D::Run,{-1,0}))running=!running;
                if(functionButton3D("Single step",FunctionRole3D::Run,{-1,0})){running=false;(void)simulation->advance(1,error);}
                ImGui::EndDisabled();
                if(group("Stop conditions (apply on rebuild)",true)) {
                dirty|=parameter("Duration (s)",edited.duration,0.02,"%.3f");
                ImGui::TextUnformatted(tr3D("Maximum steps (0 = duration)"));ImGui::SetNextItemWidth(-1);
                dirty|=ImGui::InputInt("##maxSteps",&edited.maxSteps,100,1000);
                ImGui::TextWrapped(tr3D("Stop at duration or maximum steps, whichever comes first. Range: 0..20,000. CFL time step is preserved."));
                ImGui::TextDisabled(tr3D("Active limit: %d | dt %.6f ms"),simulation->parameters().maxSteps,simulation->dt()*1000);
                ImGui::TextUnformatted(tr3D("DRP CFL coefficient (0.02..0.8)"));ImGui::SetNextItemWidth(-1);
                dirty|=ImGui::InputDouble("##cflSafety",&edited.cflSafety,0.01,0.02,"%.3f");
                ImGui::TextWrapped(tr3D("Default 0.8; 0.22 retains the earlier conservative setting. Both cores enforce geometry, sponge and source sampling limits."));
                timeStepLimitSummary();
                }
                if(group("Interface responsiveness")) {
                ImGui::TextUnformatted(tr3D("Maximum steps per frame"));
                ImGui::SetNextItemWidth(-1);ImGui::SliderInt(label3D("##stepsPerFrame"),&stepsPerFrame,1,8);
                ImGui::TextUnformatted(tr3D("Compute budget per frame (ms)"));
                ImGui::SetNextItemWidth(-1);ImGui::SliderFloat(label3D("##frameBudget"),&frameBudgetMs,4,50,"%.0f");
                ImGui::TextWrapped(tr3D("Yields between complete steps when the budget is reached. A single RK4 step may exceed this budget."));
                }
                ImGui::EndTabItem();
            }
            if(controlCategory==0&&ImGui::BeginTabItem(label3D("Model"),nullptr,requestedTab==1?ImGuiTabItemFlags_SetSelected:0)) {
                modelPresets();
                ImGui::TextColored(functionColor3D(FunctionRole3D::Model),tr3D("01   MODEL & NUMERICS"));
                if(group("Grid & shallow terrain",true)) {
                    int dims[3]{edited.nx,edited.ny,edited.nz};ImGui::TextUnformatted(tr3D("Physical nodes X / Y / Z"));ImGui::SetNextItemWidth(-1);
                    if(ImGui::InputInt3("##Nodes",dims)){edited.nx=dims[0];edited.ny=dims[1];edited.nz=dims[2];dirty=true;}
                    dirty|=parameter("Grid spacing (m)",edited.spacing,1,"%.2f");
                    const auto extent=nominalDimensions3D(edited);
                    ImGui::TextWrapped(tr3D("Staged grid span X/Y/depth: %s x %s x %s m"),formatMeasurement3D(extent[0]).c_str(),formatMeasurement3D(extent[1]).c_str(),formatMeasurement3D(extent[2]).c_str());
                    ImGui::TextWrapped(tr3D("Node span = (N - 1) * spacing; excludes ghost nodes. Apply rebuild for true terrain bounds."));
                    ImGui::BeginDisabled(!edited.surfaceGrid.empty());
                    dirty|=parameter("Terrain amplitude (m)",edited.topographyAmplitude,1,"%.2f");
                    dirty|=parameter("Terrain wavelength (m)",edited.topographyWavelength,10,"%.1f");
                    ImGui::EndDisabled();
                    dirty|=parameter("Flatten depth (m), 0 = auto",edited.terrainTransitionDepth,10,"%.1f");
                    if(ImGui::Button(label3D("Flat"))){edited.surfaceGrid={};edited.topographyAmplitude=0;dirty=true;}ImGui::SameLine();
                    if(ImGui::Button(label3D("Low relief"))){edited.surfaceGrid={};edited.topographyAmplitude=10;edited.terrainTransitionDepth=0;dirty=true;}ImGui::SameLine();
                    if(ImGui::Button(label3D("Undulating"))){edited.surfaceGrid={};edited.topographyAmplitude=25;edited.topographyWavelength=240;edited.terrainTransitionDepth=0;dirty=true;}
                    ImGui::TextWrapped(tr3D("Terrain follows the surface only near the top. Deeper mesh layers and the bottom are horizontal."));
                    ImGui::TextDisabled(tr3D("Active flatten depth: %.1f m"),simulation->grid().terrainTransitionDepth);
                    ImGui::TextWrapped("%s",language3D()?"XYZ 动态分配，总预算含幽灵节点为 28,652,616；单轴安全上限 4096，仍受内存与几何约束。":"Dynamic XYZ allocation: 28,652,616 total padded nodes; axis safety cap 4096. Memory and geometry limits still apply.");
                }
                if(group(language3D()?"外部 DEM 地表高程":"External DEM surface",true))surfaceControls();
                if(group("Elastic material")) {
                    ImGui::TextWrapped(tr3D("Configure uniform / layered properties in the Materials tab."));
                    if(functionButton3D("Open Materials",FunctionRole3D::Model,{-1,0}))nextRequestedTab=5;
                }
                if(functionButton3D("Compute core / performance",FunctionRole3D::Run,{-1,0}))nextRequestedTab=7;
                ImGui::EndTabItem();
            }
            if(controlCategory==0&&ImGui::BeginTabItem(label3D("Materials"),nullptr,requestedTab==5?ImGuiTabItemFlags_SetSelected:0)){materialControls();ImGui::EndTabItem();}
            if(controlCategory==0&&ImGui::BeginTabItem(label3D("Boundaries"),nullptr,requestedTab==8?ImGuiTabItemFlags_SetSelected:0)){boundaryControls();ImGui::EndTabItem();}
            if(controlCategory==1&&ImGui::BeginTabItem(label3D("Source"),nullptr,requestedTab==4?ImGuiTabItemFlags_SetSelected:0)) {
                sourceControls();ImGui::EndTabItem();
            }
            if(controlCategory==3&&ImGui::BeginTabItem(label3D("Display"),nullptr,requestedTab==2?ImGuiTabItemFlags_SetSelected:0)) {
                ImGui::TextColored(functionColor3D(FunctionRole3D::Display),tr3D("02   FIELD & WORKSPACE"));
                ImGui::TextUnformatted(tr3D("Display layers"));ImGui::SetNextItemWidth(-1);
                ImGui::Combo("##displayLayers",&visuals.displayMode,choices3D("Wavefield only\0Model only\0Model + wavefield\0"));
                if(group("Wavefield appearance",true)) {
                    ImGui::TextUnformatted(tr3D("Wave component [W]"));ImGui::SetNextItemWidth(-1);ImGui::Combo("##component",&visuals.component,choices3D(kComponents3D));
                    ImGui::TextUnformatted(tr3D("Wave palette [Q]"));ImGui::SetNextItemWidth(-1);ImGui::Combo("##palette",&visuals.palette,choices3D(kPalettes3D));
                    ImGui::Checkbox(label3D("Wavefield colorbar"),&visuals.showWaveColorbar);
                    ImGui::Checkbox(label3D("Normalized wavefield colorbar"),&visuals.normalizedColorbar);
                    ImGui::TextWrapped(tr3D("Signed fields: -1 to 1; magnitude: 0 to 1. Physical scale is shown below. Material legends keep SI units."));
                    ImGui::TextWrapped(tr3D("This option changes legend units. Enable Auto range to follow the current peak amplitude."));
                    ImGui::TextUnformatted(tr3D("Display gain"));ImGui::SetNextItemWidth(-1);ImGui::SliderFloat(label3D("##gain"),&visuals.gain,0.05f,100,"%.2fx",ImGuiSliderFlags_Logarithmic);
                    ImGui::Checkbox(label3D("Auto range (physical nodes)"),&visuals.autoRange);
                    ImGui::TextUnformatted(tr3D("Fixed velocity range (m/s)"));ImGui::SetNextItemWidth(-1);ImGui::SliderFloat(label3D("##velocityRange"),&visuals.velocityReference,1e-6f,1,"%.5f",ImGuiSliderFlags_Logarithmic);
                    ImGui::TextUnformatted(tr3D("Fixed pressure range (Pa)"));ImGui::SetNextItemWidth(-1);ImGui::SliderFloat(label3D("##stressRange"),&visuals.stressReference,1,1e8f,"%.0f",ImGuiSliderFlags_Logarithmic);
                    if(visuals.displayMode==2){ImGui::TextUnformatted(tr3D("Wave overlay opacity / threshold"));ImGui::SetNextItemWidth(-1);ImGui::SliderFloat("##waveOpacity",&visuals.waveOpacity,0,1,"%.2f");ImGui::SetNextItemWidth(-1);ImGui::SliderFloat("##waveThreshold",&visuals.waveThreshold,0,.5f,"%.3f");}
                    ImGui::TextWrapped(tr3D("Gain, palettes and ranges survive model rebuilds. Only Reset visual parameters restores their defaults."));
                }
                if(group("Model colors",true)) {
                    ImGui::TextUnformatted(tr3D("Material property"));ImGui::SetNextItemWidth(-1);ImGui::Combo("##modelComponent",&visuals.modelComponent,choices3D(kModelComponents3D));
                    ImGui::TextUnformatted(tr3D("Model palette [E]"));ImGui::SetNextItemWidth(-1);ImGui::Combo("##modelPalette",&visuals.modelPalette,choices3D(kPalettes3D));
                    ImGui::Checkbox(label3D("Model colorbar (SI)"),&visuals.showModelColorbar);
                    ImGui::Checkbox(label3D("Material auto range"),&visuals.modelAutoRange);
                    const int c=visuals.modelComponent;
                    if(!visuals.modelAutoRange){ImGui::TextUnformatted(tr3D("Material range min / max"));ImGui::SetNextItemWidth(-1);ImGui::InputFloat("##materialMin",&visuals.modelMinimum[c],0,0,"%.3g");ImGui::SetNextItemWidth(-1);ImGui::InputFloat("##materialMax",&visuals.modelMaximum[c],0,0,"%.3g");}
                    updateMaterialRange3D(visuals,simulation->parameters());
                    ImGui::TextWrapped(tr3D("Active material range: %.3g .. %.3g %s"),visuals.modelLow,visuals.modelHigh,modelColorbarScale3D(visuals).physicalUnit);
                    if(!visuals.modelAutoRange&&(!std::isfinite(visuals.modelMinimum[c])||!std::isfinite(visuals.modelMaximum[c])||visuals.modelMaximum[c]<=visuals.modelMinimum[c]))ImGui::TextWrapped(tr3D("Invalid material range: using 0..1 until max > min."));
                    ImGui::TextWrapped(tr3D("Material colors use their own physical range. Q: wave palette; W: wave component; E: model palette. Shift reverses."));
                }
                if(functionButton3D("Reset visual parameters",FunctionRole3D::Danger))visuals=VisualSettings3D{};
                if(group("Views & rebuild policy",true)) {
                    SeisWave::Ui::runtimeDisplayOptions();
                    ImGui::Checkbox(label3D("Auto-fit after geometry changes"),&autoFitGeometry);
                    ImGui::TextWrapped(tr3D("Resize maps slices and receiver selection by relative position. Geometry edits fit slices and camera; material/source edits preserve framing."));
                    ImGui::Checkbox(label3D("Slice grid overlay"),&visuals.showGrid);
                    ImGui::Checkbox(label3D("Show absorbing region"),&visuals.showAbsorbingLayer);
                    ImGui::Checkbox(label3D("Slice crosshairs"),&visuals.showCrosshair);
                    ImGui::Checkbox(label3D("Show colorbars"),&visuals.showColorbar);
                    ImGui::Checkbox(label3D("Physical model dimensions"),&visuals.showDimensions);ImGui::Checkbox(label3D("Slice coordinates (m)"),&visuals.showCoordinates);
                    ImGui::TextUnformatted(tr3D("Grid opacity"));ImGui::SetNextItemWidth(-1);ImGui::SliderFloat("##gridOpacity",&visuals.gridOpacity,.05f,.8f,"%.2f");ImGui::Checkbox(label3D("Terrain wireframe"),&visuals.showTopography);
                    ImGui::Checkbox(language3D()?"地表按高程着色":"Color terrain by elevation",&visuals.colorTopography);
                    if(visuals.colorTopography){ImGui::SetNextItemWidth(-1);ImGui::Combo("##terrainPaletteSettings",&visuals.terrainPalette,choices3D(kTerrainPalettes3D));}
                    ImGui::Checkbox(label3D("Three planes in 3D"),&visuals.showSlices);ImGui::Checkbox(label3D("Receiver markers"),&visuals.showReceivers);
                    ImGui::SetNextItemWidth(-1);if(ImGui::Combo("##layout",&layoutPreset,choices3D("Balanced 2x2\0Spatial focus\0Tabbed views\0Records focus\0")))resetLayout=true;
                }
                if(group("Theme & text",true)) {
                    themeSelector("##theme",ImGui::GetContentRegionAvail().x);
                    ImGui::TextUnformatted(tr3D("Interface font size (px)"));ImGui::SetNextItemWidth(-1);ImGui::SliderFloat(label3D("##fontSize"),&uiFontSize,16,28,"%.0f px");
                }
                ImGui::EndTabItem();
            }
            if(controlCategory==4&&ImGui::BeginTabItem(label3D("Export"),nullptr,requestedTab==3?ImGuiTabItemFlags_SetSelected:0)) {
                ImGui::TextColored(functionColor3D(FunctionRole3D::Export),tr3D("03   DATA EXPORT"));
                if(group("Reproducible model VTS",true))modelFiles();
                if(group("Wavefield snapshot VTS",true)) {
                    snapshotControls();
                    ImGui::TextWrapped(tr3D("Full physical mesh and raw SI fields. Display gain is excluded."));
                    ImGui::SetNextItemWidth(-1);ImGui::InputText("##vtsPath",vtsPath.data(),vtsPath.size());
                    if(ImGui::Button(label3D("Browse VTS...")))browseSave3D(vtsPath.data(),vtsPath.size(),true);
                    if(functionButton3D("Export current snapshot",FunctionRole3D::Export,{-1,0}))exportSnapshot();
                }
                if(group("Receiver CSV",true)) {
                    ImGui::TextUnformatted(language3D()?"接收记录分量（可多选）":"Receiver components (multiple)");
                    for(int c=0;c<3;++c){if(c)ImGui::SameLine();constexpr const char* names[]{"Vx##receiver","Vy##receiver","Vz##receiver"};ImGui::Checkbox(names[c],&receiverExportComponents[c]);}
                    ImGui::SetNextItemWidth(-1);ImGui::InputText("##csvPath",csvPath.data(),csvPath.size());
                    if(ImGui::Button(label3D("Browse CSV...")))browseSave3D(csvPath.data(),csvPath.size(),false);
                    if(functionButton3D("Export all receiver samples",FunctionRole3D::Export,{-1,0}))exportTraces();
                    ImGui::TextWrapped(tr3D("Receiver geometry is configured in Acquisition / Receivers. Raw samples include t=0; interval equals dt."));
                }
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();requestedTab=-1;
        }
        ImGui::Separator();ImGui::TextWrapped(tr3D("Alternating MacCormack-DRP / traction-image surface / side-bottom sponge. Full CGFD3D waveform equivalence is pending."));
        ImGui::EndChild();
        if(functionButton3D(dirty?"Apply changes / rebuild":"Rebuild session",FunctionRole3D::Model,{-1,0}))(void)rebuild(true);
        if(simulation&&simulation->initialized()){const auto& p=simulation->parameters();ImGui::TextWrapped(tr3D("%s | active %d x %d x %d"),tr3D(dirty?"Edits pending":"Ready"),p.nx,p.ny,p.nz);}else ImGui::TextWrapped("%s",error.c_str());
        ImGui::End();
    }

    void samplingSummary() {
        const auto& p=simulation->parameters();const auto& d=simulation->diagnostics();
        ImGui::TextWrapped(tr3D("Active f0: %.2f Hz | peak at %.4f s"),p.dominantFrequency,1.5/p.dominantFrequency);
        dimensionSummary();
        const auto warn=palette3D().light?ImVec4(.68f,.32f,.04f,1):ImVec4(1,.75f,.3f,1);
        ImGui::TextColored(d.pointsPerShortestWavelength<5?warn:palette3D().accent,tr3D("Nodes at 2.5 f0: %.1f"),d.pointsPerShortestWavelength);
        if(d.pointsPerShortestWavelength<5) {
            ImGui::PushStyleColor(ImGuiCol_Text,warn);ImGui::TextWrapped(tr3D("Under-resolved: finite fields do not imply accurate waveforms."));ImGui::PopStyleColor();
        }
        if(simulation->totalSteps()*simulation->dt()<1.5/p.dominantFrequency)ImGui::TextWrapped(tr3D("Wavelet peak is outside the active stop time."));
    }
    void dimensionSummary(bool detailed=true) const {
        const auto span=modelBounds.dimensions();
        ImGui::TextWrapped(tr3D("Active physical size X/Y/Z: %s x %s x %s m"),formatMeasurement3D(span[0]).c_str(),formatMeasurement3D(span[1]).c_str(),formatMeasurement3D(span[2]).c_str());
        if(detailed)ImGui::TextWrapped(tr3D("Physical Z: %s .. %s m | terrain: %s .. %s m"),formatMeasurement3D(modelBounds.minimum[2]).c_str(),formatMeasurement3D(modelBounds.maximum[2]).c_str(),formatMeasurement3D(modelBounds.surfaceMinimum).c_str(),formatMeasurement3D(modelBounds.surfaceMaximum).c_str());
    }
    void timeStepLimitSummary() {
        const auto& d=simulation->diagnostics();
        const double ceiling=std::min({d.waveDtLimit,d.dampingDtLimit,d.sourceDtLimit});
        const char* limit=ceiling==d.waveDtLimit?"Wave propagation":ceiling==d.dampingDtLimit?"Absorbing damping":"Source sampling";
        ImGui::TextWrapped(tr3D("Time step limited by: %s"),tr3D(limit));
        ImGui::TextWrapped(tr3D("Vp max * dt / h: %.4f"),d.materialMaxVp*simulation->dt()/simulation->parameters().spacing);
        const auto& p=simulation->parameters();ImGui::TextWrapped(tr3D("Active grid %d x %d x %d | h %.3f m | Vp max %.1f m/s"),p.nx,p.ny,p.nz,p.spacing,d.materialMaxVp);
        stagedTimeStepSummary();
    }
    void diagnosticsWindow() {
        if(!panels[1]||parameterHost.mode()==1)return;
        if(!ImGui::Begin(label3D("Numerical diagnostics"),&panels[1])){ImGui::End();return;}samplingSummary();ImGui::Separator();
        {
            const auto& d=simulation->diagnostics();const auto backend=simulation->backendInfo();
            ImGui::TextWrapped(tr3D("dt: %.6f ms"),simulation->dt()*1000);ImGui::TextWrapped(tr3D("Min Jacobian: %.6f"),d.minJacobian);
            timeStepLimitSummary();
            ImGui::TextWrapped(tr3D("Min adjacent-plane distance: %.4f m"),d.minCellLength);
            ImGui::TextWrapped(tr3D("Wave dt limit: %.6f ms"),d.waveDtLimit*1000);
            ImGui::TextWrapped(tr3D("Sponge dt limit: %.6f ms"),d.dampingDtLimit*1000);
            ImGui::TextWrapped(tr3D("Max damping * dt: %.4f"),d.maxDampingDt);
            ImGui::TextWrapped(tr3D("Elastic energy: %.4e J"),d.elasticEnergy);
            ImGui::TextWrapped(tr3D("Peak energy: %.4e J"),d.peakEnergy);
            ImGui::TextWrapped(tr3D("Max inverse metric norm: %.4f"),d.maxMetricNorm);ImGui::TextWrapped(tr3D("Working set: ~%.1f MiB"),d.estimatedMemoryMiB);
            ImGui::TextWrapped(tr3D("CPU workers: %d"),backend.workerThreads);ImGui::TextWrapped(tr3D("Slice payload: %.1f KiB"),((waveVisible3D(visuals)?displayFrame.bytes:0)+(modelVisible3D(visuals)?modelFrame.bytes:0))/1024.0);
            ImGui::TextWrapped(tr3D("Peak |V|: %.3e m/s"),d.maxAbsVelocity);ImGui::TextWrapped(tr3D("Traction-rate residual: %.2e"),d.maxTractionRateRelative);
            ImGui::TextWrapped(tr3D("Source dt limit: %.6f ms"),d.sourceDtLimit*1000);
            ImGui::TextWrapped(tr3D("Steps per dominant period: %.1f"),d.stepsPerDominantPeriod);
            ImGui::TextWrapped(tr3D("Largest physical cell edge: %.4f m"),d.maxCellEdge);
        }
        ImGui::TextWrapped(tr3D("Alternating MacCormack-DRP / traction-image surface / side-bottom sponge. Full CGFD3D waveform equivalence is pending."));
        ImGui::End();
    }

    std::vector<SeisWave::Ui::ScalarLegend> fieldLegends(bool terrain=false) const {
        std::vector<SeisWave::Ui::ScalarLegend> legends;
        for(bool material:{false,true}){
            if(material?!modelLegendVisible3D(visuals):!waveLegendVisible3D(visuals))continue;
            const auto scale=material?modelColorbarScale3D(visuals):colorbarScale3D(visuals);
            const char* waves[]{"Vz","Vx","Vy","|V|","p"};const char* models[]{"Vp","Vs","Density"};
            char title[192];
            if(scale.normalized)std::snprintf(title,sizeof(title),"%s | 1 = %.3g %s",waves[visuals.component],scale.physicalHigh,scale.physicalUnit);
            else std::snprintf(title,sizeof(title),"%s (%s)",material?models[visuals.modelComponent]:waves[visuals.component],scale.physicalUnit);
            legends.push_back({title,scale.low,scale.high,[this,material,scale](double t){const double value=scale.physicalValue(scale.low+(scale.high-scale.low)*t);return packedColor3D(material?modelColor3D(value,visuals):waveColor3D(value,visuals));}});
        }
        if(terrain&&visuals.colorTopography&&visuals.showColorbar){
            const auto title=language3D()?(simulation->parameters().surfaceGrid.empty()?"相对高程 (m)":"DEM 高程 (m)"):(simulation->parameters().surfaceGrid.empty()?"Relative elevation (m)":"DEM elevation (m)");
            legends.push_back({title,visuals.terrainLow,visuals.terrainHigh,[this](double t){return packedColor3D(terrainColor3D(visuals.terrainLow+(visuals.terrainHigh-visuals.terrainLow)*t,visuals));}});
        }
        return legends;
    }
    float colorbarHeight(bool terrain=false) const {
        return SeisWave::Ui::scalarLegendHeight(static_cast<int>(fieldLegends(terrain).size()),ImGui::GetContentRegionAvail().x);
    }
    void colorbar(bool terrain=false) {
        const auto legends=fieldLegends(terrain);
        SeisWave::Ui::drawScalarLegendStrip("##3DFieldLegends",legends);
    }

    // Draws physical X-Z/Y-Z sections (including topographic warping) with cursor-centered zoom and linked picking.
    void sliceWindow(int axis,const char* title) {
        if(!panels[3+axis])return;
        if(!ImGui::Begin(label3D(title),&panels[3+axis],workspace.prepare(label3D(title)))){ImGui::End();return;}workspace.tools(label3D(title));const auto& grid=simulation->grid();const auto& p=simulation->parameters();const int g=grid.ghost;
        int* slice=axis==0?&slices.y:axis==1?&slices.x:&slices.z;const int limit=axis==0?p.ny:axis==1?p.nx:p.nz;
        const int nu=axis==1?p.ny:p.nx,nv=axis==2?p.ny:p.nz;
        ImGui::SetNextItemWidth(std::max(65.0f,ImGui::GetContentRegionAvail().x*.36f));ImGui::SliderInt(label3D("##slice"),slice,0,limit-1);
        ImGui::SameLine();char gridLabel[96];std::snprintf(gridLabel,sizeof(gridLabel),"%s %dx%d###GridToggle",tr3D("Grid"),nu,nv);ImGui::Checkbox(gridLabel,&visuals.showGrid);
        auto& view=sliceViews[static_cast<std::size_t>(axis)];
        ImGui::SameLine();if(ImGui::SmallButton(label3D("Fit")))view=SliceView3D{};
        if(ImGui::IsItemHovered())ImGui::SetTooltip(tr3D("%.0f%% | %s = %.0f m"),view.zoom*100,axis==0?"Y":axis==1?"X":"zeta",*slice*p.spacing);
        if(!refreshDisplayFrame()){if(!error.empty())running=false;ImGui::TextWrapped("%s",error.empty()?(language3D()?"后台正在刷新切片…":"Refreshing slices in background..."):error.c_str());ImGui::End();return;}
        auto size=ImGui::GetContentRegionAvail();size.y-=colorbarHeight();
        if(size.x<30 || size.y<30){ImGui::End();return;}
        const auto origin=ImGui::GetCursorScreenPos();ImGui::InvisibleButton("##physicalSlice",size,ImGuiButtonFlags_MouseButtonLeft|ImGuiButtonFlags_MouseButtonMiddle);
        const bool axes=visuals.showCoordinates&&size.x>220&&size.y>125;
        const float left=axes?ImGui::CalcTextSize("-2000").x+14:14,right=18,top=ImGui::GetTextLineHeight()+16,bottom=axes?ImGui::GetTextLineHeight()+24:14;
        const bool hovered=ImGui::IsItemHovered();auto& io=ImGui::GetIO();const ImVec2 center{origin.x+left+(size.x-left-right)*.5f,origin.y+top+(size.y-top-bottom)*.5f};
        if(hovered && io.MouseWheel!=0) {
            const float previous=view.zoom;view.zoom=std::clamp(view.zoom*std::exp(io.MouseWheel*0.15f),0.5f,20.0f);const float factor=view.zoom/previous;
            view.pan={io.MousePos.x-center.x-(io.MousePos.x-center.x-view.pan[0])*factor,io.MousePos.y-center.y-(io.MousePos.y-center.y-view.pan[1])*factor};
        }
        if(hovered && ImGui::IsMouseDragging(ImGuiMouseButton_Middle)){view.pan[0]+=io.MouseDelta.x;view.pan[1]+=io.MouseDelta.y;}
        auto coordinate=[&](int a,int b) {
            const int i=axis==1?g+*slice:g+a,j=axis==0?g+*slice:axis==1?g+a:g+b,k=axis==2?g+*slice:g+b;
            return grid.index(i,j,k);
        };
        double minU=1e30,maxU=-1e30,minV=1e30,maxV=-1e30;
        auto physical=[&](int a,int b) {const auto& xyz=grid.coordinates[coordinate(a,b)];return std::array<double,2>{xyz[axis==1?1:0],xyz[axis==2?1:2]};};
        for(int b=0;b<nv;++b)for(int a=0;a<nu;++a){const auto xy=physical(a,b);minU=std::min(minU,xy[0]);maxU=std::max(maxU,xy[0]);minV=std::min(minV,xy[1]);maxV=std::max(maxV,xy[1]);}
        const double fit=std::max(0.001,std::min((size.x-left-right)/(maxU-minU),(size.y-top-bottom)/(maxV-minV)))*view.zoom;
        auto screen=[&](int a,int b) {const auto xy=physical(a,b);return ImVec2{center.x+view.pan[0]+static_cast<float>((xy[0]-(minU+maxU)*0.5)*fit),center.y+view.pan[1]+static_cast<float>(sliceVerticalOffset3D(xy[1],(minV+maxV)*.5,axis)*fit)};};
        auto* draw=ImGui::GetWindowDrawList();draw->AddRectFilled(origin,{origin.x+size.x,origin.y+size.y},color3D(palette3D().background));draw->PushClipRect(origin,{origin.x+size.x,origin.y+size.y},true);
        auto sliceColor=[&](int a,int b) {
            const int i=axis==1?*slice:a,j=axis==0?*slice:axis==1?a:b,k=axis==2?*slice:b;
            return packedColor3D(absorbingColor3D(fieldColor3D(displayFrame,modelFrame,axis,a,b,visuals),p,i,j,k,visuals));
        };
        // One coordinate transform and color evaluation per node, shared by
        // adjacent cells instead of recomputing each corner four times.
        std::vector<ImVec2> positions(static_cast<std::size_t>(nu)*nv);std::vector<ImU32> colors(positions.size());
        for(int b=0;b<nv;++b)for(int a=0;a<nu;++a){const auto n=static_cast<std::size_t>(b)*nu+a;positions[n]=screen(a,b);colors[n]=sliceColor(a,b);}
        for(int b=0;b<nv-1;++b)for(int a=0;a<nu-1;++a) {
            const auto n=static_cast<std::size_t>(b)*nu+a;
            const std::array<ImVec2,4> pos{positions[n],positions[n+1],positions[n+nu+1],positions[n+nu]};
            const std::array<ImU32,4> color{colors[n],colors[n+1],colors[n+nu+1],colors[n+nu]};
            colorQuad3D(draw,pos,color);
        }
        if(visuals.showGrid) {
            const int stride=1;
            for(int a=0;a<nu;a+=stride)for(int b=0;b<nv-1;++b){draw->AddLine(screen(a,b),screen(a,b+1),packedColor3D(gridColor3D(visuals)));++gridSegments[axis];}
            for(int b=0;b<nv;b+=stride)for(int a=0;a<nu-1;++a){draw->AddLine(screen(a,b),screen(a+1,b),packedColor3D(gridColor3D(visuals)));++gridSegments[axis];}
        }
        const int pickU=axis==1?slices.y:slices.x,pickV=axis==2?slices.y:slices.z;
        if(visuals.showCrosshair)for(int b=0;b<nv-1;++b)draw->AddLine(screen(pickU,b),screen(pickU,b+1),color3D(palette3D().accent,.5f));
        if(visuals.showCrosshair)for(int a=0;a<nu-1;++a)draw->AddLine(screen(a,pickV),screen(a+1,pickV),color3D(palette3D().accent,.5f));
        const auto source=sourceIndices3D(p);
        if(visuals.showReceivers)for(std::size_t r=0;r<simulation->receivers().size();++r) {
            const auto q=simulation->receivers()[r].gridIndex;const int x=static_cast<int>(q%grid.nx)-g,y=static_cast<int>((q/grid.nx)%grid.ny)-g,z=static_cast<int>(q/(static_cast<std::size_t>(grid.nx)*grid.ny))-g;
            if((axis==0&&*slice==y)||(axis==1&&*slice==x)||(axis==2&&*slice==z))draw->AddCircleFilled(screen(axis==1?y:x,axis==2?y:z),static_cast<int>(r)==selectedReceiver?4.0f:2.4f,IM_COL32(20,175,185,255));
        }
        if((axis==0 && *slice==source[1])||(axis==1 && *slice==source[0])||(axis==2 && *slice==source[2]))draw->AddCircleFilled(screen(axis==1?source[1]:source[0],axis==2?source[1]:source[2]),4,IM_COL32(236,119,35,255));
        if(hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            float distance=std::numeric_limits<float>::max();int bestA=0,bestB=0;
            for(int b=0;b<nv;++b)for(int a=0;a<nu;++a){const auto point=screen(a,b);const float d=(point.x-io.MousePos.x)*(point.x-io.MousePos.x)+(point.y-io.MousePos.y)*(point.y-io.MousePos.y);if(d<distance){distance=d;bestA=a;bestB=b;}}
            if(axis==1)slices.y=bestA;else slices.x=bestA;if(axis==2)slices.y=bestB;else slices.z=bestB;
        }
        if(visuals.showDimensions) {
            char caption[160];std::snprintf(caption,sizeof(caption),"%s / %s: %s x %s m",axis==1?"Y":"X",axis==2?"Y":"Z",formatMeasurement3D(maxU-minU).c_str(),formatMeasurement3D(maxV-minV).c_str());
            draw->AddText({origin.x+7,origin.y+5},color3D(palette3D().text),caption);
        }
        if(axes) {
            const auto ink=color3D(palette3D().text);const float baseline=origin.y+size.y-bottom+5,axisX=origin.x+left-5;
            draw->AddLine({axisX,origin.y+top-4},{axisX,baseline},ink);draw->AddLine({axisX,baseline},{origin.x+size.x-right+4,baseline},ink);
            const int ticksU=(maxU-minU)*fit>=300?5:(maxU-minU)*fit>=100?3:2;
            const int ticksV=(maxV-minV)*fit>=200?5:(maxV-minV)*fit>=80?3:2;
            for(int n=0;n<ticksU;++n){const double value=minU+(maxU-minU)*n/(ticksU-1);const float x=center.x+view.pan[0]+static_cast<float>((value-(minU+maxU)*.5)*fit);if(x<axisX||x>origin.x+size.x-right)continue;const auto tick=formatMeasurement3D(value);const float w=ImGui::CalcTextSize(tick.c_str()).x;float labelX=x-w*.5f;if(ticksU==2&&(maxU-minU)*fit<80)labelX=n==0?x-w-3:x+3;draw->AddLine({x,baseline},{x,baseline+4},ink);draw->AddText({std::clamp(labelX,origin.x,origin.x+size.x-w),baseline+5},ink,tick.c_str());}
            for(int n=0;n<ticksV;++n){const double value=minV+(maxV-minV)*n/(ticksV-1);const float y=center.y+view.pan[1]+static_cast<float>(sliceVerticalOffset3D(value,(minV+maxV)*.5,axis)*fit);if(y<origin.y+top-4||y>baseline)continue;const auto tick=formatMeasurement3D(value);const auto w=ImGui::CalcTextSize(tick.c_str());draw->AddLine({axisX-4,y},{axisX,y},ink);draw->AddText({std::max(origin.x,axisX-w.x-7),y-w.y*.5f},ink,tick.c_str());}
        }
        if(visuals.showAbsorbingLayer&&p.absorbingBoundary.enabled)draw->AddText({origin.x+7,origin.y+size.y-ImGui::GetTextLineHeight()-5},IM_COL32(230,145,30,255),tr3D("Amber overlay: absorbing region"));
        draw->PopClipRect();
        if(hovered)ImGui::SetTooltip(tr3D("Wheel: zoom at cursor | MMB: pan | click: link slices"));
        colorbar();
        ImGui::End();
    }

    void spatialWindow() {
        if(!panels[2])return;
        if(!ImGui::Begin(label3D("Spatial exploration"),&panels[2],workspace.prepare(label3D("Spatial exploration")))){ImGui::End();return;}
        const bool compactPane=ImGui::GetContentRegionAvail().y<360;
        workspace.tools(label3D("Spatial exploration"));ImGui::SameLine();if(ImGui::SmallButton(label3D("Reset camera [R]")))renderer.resetCamera();
        ImGui::SameLine();if(ImGui::SmallButton(language3D()?"DEM 俯视":"DEM top view"))renderer.topViewCamera();
        ImGui::Checkbox(language3D()?"高程着色":"Elevation colors",&visuals.colorTopography);
        if(visuals.colorTopography){ImGui::SameLine();ImGui::SetNextItemWidth(150);ImGui::Combo("##terrainPalette",&visuals.terrainPalette,choices3D(kTerrainPalettes3D));}
        if(!compactPane)ImGui::TextDisabled(tr3D("LMB orbit / wheel zoom / MMB pan"));
        if(visuals.showDimensions&&!compactPane)dimensionSummary(false);
        auto size=ImGui::GetContentRegionAvail();size.y=std::max(48.f,size.y-colorbarHeight(true)-ImGui::GetStyle().ItemSpacing.y);
        if(size.x>16 && size.y>16) {
            const auto& io=ImGui::GetIO();
            // The viewport's optional scale can be (0,0) on the main GLFW
            // viewport. Use actual framebuffer/client sizes for both native
            // windows and mixed-DPI monitors instead of allocating a 16px view.
            auto scale=io.DisplayFramebufferScale;
            if(auto* native=static_cast<GLFWwindow*>(ImGui::GetWindowViewport()->PlatformHandle)) {
                int clientW=0,clientH=0,pixelW=0,pixelH=0;glfwGetWindowSize(native,&clientW,&clientH);glfwGetFramebufferSize(native,&pixelW,&pixelH);
                if(clientW>0&&clientH>0&&pixelW>0&&pixelH>0)scale={static_cast<float>(pixelW)/clientW,static_cast<float>(pixelH)/clientH};
            }
            if(!std::isfinite(scale.x)||scale.x<=0)scale.x=1;
            if(!std::isfinite(scale.y)||scale.y<=0)scale.y=1;
            if(refreshDisplayFrame() && renderer.render(*simulation,displayFrame,modelFrame,visuals,slices,static_cast<int>(size.x*scale.x),static_cast<int>(size.y*scale.y),error)) {
                ImGui::Image(static_cast<ImTextureID>(renderer.texture3D()),size,{0,1},{1,0});
                const auto corner=ImGui::GetItemRectMin();const auto axes=renderer.orientationAxes();
                auto* draw=ImGui::GetWindowDrawList();const ImVec2 base{corner.x+48,corner.y+48};
                const ImU32 inks[]{IM_COL32(240,95,75,255),IM_COL32(70,195,120,255),IM_COL32(80,155,255,255)};
                const char* names[]{"+X","+Y","Up"};
                for(int a=0;a<3;++a){const ImVec2 end{base.x+30*axes[a][0],base.y+30*axes[a][1]};draw->AddLine(base,end,inks[a],2);draw->AddText({end.x+3,end.y-8},inks[a],names[a]);}
                if(ImGui::IsItemHovered()) {
                    if(ImGui::IsMouseDragging(ImGuiMouseButton_Left))renderer.orbit(io.MouseDelta.x,io.MouseDelta.y);
                    if(ImGui::IsMouseDragging(ImGuiMouseButton_Middle))renderer.pan(io.MouseDelta.x,io.MouseDelta.y);
                    if(ImGui::IsMouseDragging(ImGuiMouseButton_Right))renderer.zoom(-io.MouseDelta.y*0.05f);
                    renderer.zoom(io.MouseWheel);
                }

            } else {if(!error.empty())running=false;ImGui::TextWrapped("%s",error.empty()?(language3D()?"后台正在刷新视图…":"Refreshing view in background..."):error.c_str());}
        }
        colorbar(true);
        ImGui::End();
    }

    void records() {recordView.draw(*simulation,selectedReceiver,&panels[6]);}

    // Computes the current color range once per frame; fixed scales remain stable unless explicitly changed.
    void updateRange() {
        const auto& colors=palette3D();visuals.canvas={colors.background.x,colors.background.y,colors.background.z};visuals.wire={colors.accent.x,colors.accent.y,colors.accent.z};
        visuals.range=visuals.component==4?visuals.stressReference:visuals.velocityReference;
        if(visuals.autoRange)visuals.range=std::max(1.0e-15,simulation->diagnostics().displayPeaks[visuals.component==4?4:3]);
        updateMaterialRange3D(visuals,simulation->parameters());
        updateTerrainRange3D(visuals,simulation->grid(),simulation->parameters());
    }
    void shortcuts() {
        if(ImGui::GetIO().WantTextInput || ImGui::IsAnyItemActive()||ImGui::IsPopupOpen(nullptr,ImGuiPopupFlags_AnyPopupId|ImGuiPopupFlags_AnyPopupLevel))return;
        const auto& io=ImGui::GetIO();
        if(ImGui::IsKeyPressed(ImGuiKey_F1,false)){shortcutHelp=!shortcutHelp;return;}
        if(shortcutHelp){if(ImGui::IsKeyPressed(ImGuiKey_Escape,false))shortcutHelp=false;return;}
        if(io.KeyCtrl&&ImGui::IsKeyPressed(ImGuiKey_S,false))exportSnapshot();
        if(io.KeyCtrl||io.KeyAlt||io.KeySuper)return;
        if(ImGui::IsKeyPressed(ImGuiKey_Space,false) && !simulation->completed())running=!running;
        if(ImGui::IsKeyPressed(ImGuiKey_N,false)){running=false;(void)simulation->advance(1,error);}
        if(ImGui::IsKeyPressed(ImGuiKey_C,false))(void)clearFields();
        if(ImGui::IsKeyPressed(ImGuiKey_Q,false))cycleVisual3D(visuals,VisualShortcut3D::WavePalette,io.KeyShift);
        if(ImGui::IsKeyPressed(ImGuiKey_W,false))cycleVisual3D(visuals,VisualShortcut3D::Component,io.KeyShift);
        if(ImGui::IsKeyPressed(ImGuiKey_E,false))cycleVisual3D(visuals,VisualShortcut3D::ModelPalette,io.KeyShift);
        if(ImGui::IsKeyPressed(ImGuiKey_G,false))visuals.showGrid=!visuals.showGrid;
        if(ImGui::IsKeyPressed(ImGuiKey_B,false))visuals.showAbsorbingLayer=!visuals.showAbsorbingLayer;
        if(ImGui::IsKeyPressed(ImGuiKey_V,false))visuals.showSlices=!visuals.showSlices;
        if(ImGui::IsKeyPressed(ImGuiKey_R,false))resetViews();if(ImGui::IsKeyPressed(ImGuiKey_F,false))restoreWorkspace();
    }
    void shortcutWindow() {
        if(!shortcutHelp)return;
        const auto* viewport=ImGui::GetMainViewport();ImGui::SetNextWindowPos(viewport->GetCenter(),ImGuiCond_Appearing,{.5f,.5f});ImGui::SetNextWindowSize({std::min(1120.0f,viewport->WorkSize.x*.94f),std::min(390.0f,viewport->WorkSize.y*.75f)},ImGuiCond_Appearing);
        if(ImGui::Begin(label3D("Keyboard shortcuts"),&shortcutHelp,ImGuiWindowFlags_NoDocking|ImGuiWindowFlags_NoCollapse)) {
            const char* rows[][2]{{"Space","Run / pause"},{"N","Single step"},{"C","Clear active fields and records"},{"R","Fit views [R]"},{"Q / Shift+Q","Next / previous wave palette"},{"W / Shift+W","Next / previous wave component"},{"E / Shift+E","Next / previous model palette"},{"G","Slice grid overlay"},{"B","Show absorbing region"},{"V","Three planes in 3D"},{"F1 / Esc","Open / close shortcut help"},{"F","Reset layout [F]"},{"Ctrl+S","Export current snapshot"},{"Tab","Move focus between controls"}};
            if(ImGui::BeginTable("##shortcutTable",6,ImGuiTableFlags_BordersInnerH|ImGuiTableFlags_RowBg)){int column=0;for(const auto& row:rows){if(column++%3==0)ImGui::TableNextRow();ImGui::TableNextColumn();ImGui::TextUnformatted(row[0]);ImGui::TableNextColumn();ImGui::TextWrapped("%s",tr3D(row[1]));}ImGui::EndTable();}
            ImGui::TextWrapped("%s",language3D()?"编辑控件或打开菜单时暂停快捷键。主页结束当前会话并返回维度选择。":"Editing or open menus suspend shortcuts. Home ends this session and returns to dimension selection.");
            ImGui::TextWrapped(tr3D("C clears the active run in place, retaining geometry, dt, gains and staged edits. Apply / rebuild is required to apply edited parameters."));
        }
        ImGui::End();
    }
};

// Captures the actual rendered framebuffer for a deterministic GUI smoke run without desktop automation.
bool captureFrame3D(const std::filesystem::path& output,int width,int height) {
    std::vector<unsigned char> pixels(static_cast<std::size_t>(width)*height*3);glPixelStorei(GL_PACK_ALIGNMENT,1);
    glReadBuffer(GL_BACK);glReadPixels(0,0,width,height,GL_RGB,GL_UNSIGNED_BYTE,pixels.data());
    std::ofstream out(output,std::ios::binary);out<<"P6\n"<<width<<' '<<height<<"\n255\n";
    for(int y=height-1;y>=0;--y)out.write(reinterpret_cast<const char*>(pixels.data()+static_cast<std::size_t>(y)*width*3),static_cast<std::streamsize>(width)*3);
    return out.good();
}
// Real OpenGL readback of asymmetric imported terrain: verifies screen X/Y,
// absolute height colors, the shared wave palette library and the off switch.
bool terrainReview3D(Workbench3D& app) {
    const auto directory=outputRoot3D()/"validation/terrain-display";
    std::filesystem::create_directories(directory);
    app.visuals.showSlices=app.visuals.showTopography=app.visuals.showReceivers=false;
    app.visuals.colorTopography=true;app.updateRange();app.renderer.topViewCamera();
    if(!app.refreshDisplayFrame())return false;
    constexpr int width=900,height=700;
    const auto& p=app.simulation->parameters();const auto& grid=app.simulation->grid();const int g=grid.ghost;
    const double lx=(p.nx-1)*p.spacing,ly=(p.ny-1)*p.spacing,lz=(p.nz-1)*p.spacing;
    const double scale=std::max({lx,ly,lz+2*std::abs(grid.topographyAmplitude)});
    const auto revision=app.simulation->revision();
    for(int palette=0;palette<wavePaletteCount3D;++palette) {
        app.visuals.terrainPalette=palette;
        if(!app.renderer.render(*app.simulation,app.displayFrame,app.modelFrame,app.visuals,app.slices,width,height,app.error))return false;
        std::vector<unsigned char> rgba(static_cast<std::size_t>(width)*height*4);
        glBindTexture(GL_TEXTURE_2D,app.renderer.texture3D());glGetTexImage(GL_TEXTURE_2D,0,GL_RGBA,GL_UNSIGNED_BYTE,rgba.data());
        std::ofstream out(directory/("terrain_top_"+std::to_string(palette)+".ppm"),std::ios::binary);
        out<<"P6\n"<<width<<' '<<height<<"\n255\n";
        for(int y=height-1;y>=0;--y)for(int x=0;x<width;++x)out.write(reinterpret_cast<const char*>(rgba.data()+(static_cast<std::size_t>(y)*width+x)*4),3);
        if(!out.good())return false;
        for(int j:{p.ny/4,p.ny*3/4})for(int i:{p.nx/4,p.nx*3/4}) {
            const auto& pos=grid.coordinates[grid.index(i+g,j+g,g)];const auto world=scenePosition3D(pos,lx,ly,lz,scale);
            const double cot=1/std::tan(.42),depth=app.renderer.cameraState()[2]-world[1];
            const int x=static_cast<int>((1+cot*height/width*world[0]/depth)*width*.5);
            const int y=static_cast<int>((1-cot*world[2]/depth)*height*.5);
            if(x<0||x>=width||y<0||y>=height){app.error="Terrain review sample outside viewport";return false;}
            const auto expected=terrainColor3D(terrainElevation3D(pos[2],p),app.visuals);
            for(int c=0;c<3;++c)if(std::abs(rgba[(static_cast<std::size_t>(y)*width+x)*4+c]/255.-expected[c])>.045) {
                app.error="Terrain screen corner or absolute-elevation color mismatch";return false;
            }
        }
    }
    app.visuals.colorTopography=false;
    if(!app.renderer.render(*app.simulation,app.displayFrame,app.modelFrame,app.visuals,app.slices,width,height,app.error))return false;
    glBindFramebuffer(GL_READ_FRAMEBUFFER,0);
    std::vector<unsigned char> rgba(static_cast<std::size_t>(width)*height*4);
    glBindTexture(GL_TEXTURE_2D,app.renderer.texture3D());glGetTexImage(GL_TEXTURE_2D,0,GL_RGBA,GL_UNSIGNED_BYTE,rgba.data());
    for(int c=0;c<3;++c)if(std::abs(rgba[(height/3*width+width/3)*4+c]/255.-app.visuals.canvas[c])>.01){app.error="Terrain off switch failed";return false;}
    if(app.simulation->revision()!=revision){app.error="Terrain display changed simulation state";return false;}
    std::cout<<"Terrain display PASS: "<<wavePaletteCount3D<<" shared palettes, "<<4*wavePaletteCount3D<<" screen corner samples, elevation sign and off switch\n";
    return glGetError()==GL_NO_ERROR;
}
#include "WorkflowCapture3D.inc"
#include "ViewportValidation3D.inc"
#include "ThemePanelValidation3D.inc"
}

// Runs a self-contained OpenGL/ImGui workspace; --smoke simulates, captures and exits for automated startup checks.
int runWorkbench3D(int argc,char** argv) {
    const std::string mode=argc>1?argv[1]:"";
    const bool routeSmoke=mode=="--smoke-route-3d";
    const bool sourceSmoke=mode=="--smoke-sources";
    const bool resizeSmoke=mode=="--smoke-resize";
    const bool deviceSmoke=mode=="--smoke-devices";
    const bool cudaSmoke=mode=="--smoke-cuda"||deviceSmoke;
    const bool navigationSmoke=mode=="--smoke-navigation";
    const bool boundarySmoke=mode=="--smoke-boundary";
    const bool colorbarSmoke=mode=="--smoke-colorbar";
    const bool cflSmoke=mode=="--smoke-cfl";
    const bool modelDisplaySmoke=mode=="--smoke-model-display";
    const bool terrainSmoke=mode=="--smoke-terrain";
    const bool commandSmoke=mode=="--smoke-commands";
    const bool gridConfigSmoke=mode=="--smoke-grid-config";
    const bool workflowCapture=mode=="--capture-workflow";
    const bool acquisitionSmoke=mode=="--smoke-acquisition";
    const bool layersSmoke=mode=="--smoke-layers";
    const bool workspaceSmoke=mode=="--smoke-workspace";
    const bool viewportSmoke=mode=="--smoke-viewports";
    const bool themeSmoke=mode=="--smoke-themes";
    const bool defaultTerrainSmoke=mode=="--smoke-default-terrain";
    const bool backgroundSmoke=mode=="--smoke-background";
    const bool recordsSmoke=mode=="--smoke-records";
    const bool uiSmoke=mode=="--smoke-ui" || sourceSmoke || recordsSmoke || workspaceSmoke || layersSmoke || acquisitionSmoke || navigationSmoke || boundarySmoke || colorbarSmoke || cflSmoke || modelDisplaySmoke || gridConfigSmoke;
    const bool smoke=mode=="--smoke" || terrainSmoke || resizeSmoke || cudaSmoke || uiSmoke || commandSmoke || workflowCapture || routeSmoke || viewportSmoke || themeSmoke || defaultTerrainSmoke || backgroundSmoke;
    std::string catalogError;
    if(!validateCatalog3D(catalogError)){std::cerr<<catalogError<<'\n';return 1;}
    std::error_code ec;std::filesystem::create_directories(outputRoot3D()/"ui",ec);
    if(ec){std::cerr<<ec.message()<<'\n';return 1;}
    if(smoke)SeisWave::Ui::setParameterDisplayMode(3,0);
    SeisWave::Ui::AppRuntime host;std::string hostError;
    if(!host.open("SeisWave Lab · Curvilinear | 3D | CPU / CUDA",smoke,hostError,!smoke||viewportSmoke)){std::cerr<<hostError<<'\n';return 2;}
    GLFWwindow* window=host.window();auto& io=ImGui::GetIO();
    const std::string ini=utf8Path3D(outputRoot3D()/"ui"/(smoke?"smoke.ini":"workspace.ini"));io.IniFilename=smoke?nullptr:ini.c_str();
    bool returnToHome=false;
    int result=0;
    {
        Workbench3D app;const int entryLanguage=language3D(),entryTheme=themeIndex3D();if(!smoke){app.loadPreferences();setLanguage3D(entryLanguage);app.theme=entryTheme;app.uiFontSize=SeisWave::Ui::interfaceFontSize();applyTheme3D(entryTheme);}app.resetLayout=smoke || !std::filesystem::exists(outputRoot3D()/"ui"/"workspace.ini");
        if(!smoke||defaultTerrainSmoke) {
            if(!app.stageDefaultSurface()){std::cerr<<"Default terrain: "<<app.error<<'\n';return 18;}
            if(defaultTerrainSmoke){app.edited.maxSteps=2;app.visuals.colorTopography=true;app.visuals.showGrid=false;}
        }
        if(cudaSmoke)app.selectedBackend=1;
        if(backgroundSmoke){app.selectedBackend=0;app.computeSettings.cpuThreads=1;app.edited.nx=64;app.edited.ny=48;app.edited.nz=48;app.edited.maxSteps=16;app.stepsPerFrame=1;}
        if(acquisitionSmoke)app.selectedBackend=0; // Deliberate CPU reference, then an identical CUDA run.
        if(acquisitionSmoke)app.edited.receiverArray={ReceiverArrayKind3D::GridXY,2,2,1,12,8,1,1};
        if(workspaceSmoke){app.edited=applyModelPreset3D(1,app.edited);app.edited.maxSteps=40;}
        if(boundarySmoke){app.edited=applyModelPreset3D(2,app.edited);app.edited.maxSteps=40;app.edited.absorbingBoundary.cells={10,3,12};app.visuals.showAbsorbingLayer=true;}
        if(colorbarSmoke){app.edited=applyModelPreset3D(0,app.edited);app.visuals.autoRange=true;app.visuals.colorTopography=true;app.visuals.terrainPalette=7;}
        if(layersSmoke||modelDisplaySmoke){app.edited=applyMaterialPreset3D(1,app.edited);app.materialPreset=1;app.edited.nx=100;app.edited.ny=20;app.edited.nz=100;app.edited.spacing=1;app.edited.topographyAmplitude=2;app.edited.topographyWavelength=60;app.edited.maxSteps=40;app.visuals.displayMode=1;}
        if(modelDisplaySmoke){app.edited.maxSteps=0;app.edited.duration=.12;app.edited.dominantFrequency=30;app.edited.sourceZ=20;app.visuals.autoRange=true;app.visuals.showGrid=false;}
        if(terrainSmoke) {
            auto& p=app.edited;p.nx=65;p.ny=49;p.nz=35;p.spacing=20;p.topographyAmplitude=0;p.dominantFrequency=2;p.maxSteps=2;
            auto& d=p.surfaceGrid;d.nx=p.nx;d.ny=p.ny;d.spacing=p.spacing;d.datum=1500;
            for(int j=0;j<d.ny;++j)for(int i=0;i<d.nx;++i)d.elevations.push_back(1470+.7*i+.2*j);
            if(argc>2){if(!readSurfaceGrid3D(fromUtf8Path3D(argv[2]),d,app.error)){std::cerr<<app.error<<'\n';return 12;}
                SimulationParameters next;p.spacing=d.spacing;p.nx=d.nx;p.ny=d.ny;
                const BackendMemory3D budget{512ull*1024*1024,512ull*1024*1024};
                if(!fitSurfaceModel3D(p,static_cast<BackendKind3D>(app.selectedBackend),budget,next,app.error)){std::cerr<<app.error<<'\n';return 12;}p=std::move(next);}
        }
        const auto sessionExportRoot=smoke?outputRoot3D()/"validation/three-d-smoke":outputRoot3D();
        if(smoke)std::filesystem::create_directories(sessionExportRoot);
        std::snprintf(app.modelPath.data(),app.modelPath.size(),"%s",utf8Path3D(sessionExportRoot/"curvilinear_model.vts").c_str());
        std::snprintf(app.vtsPath.data(),app.vtsPath.size(),"%s",utf8Path3D(sessionExportRoot/"curvilinear_snapshot.vts").c_str());
        std::snprintf(app.csvPath.data(),app.csvPath.size(),"%s",utf8Path3D(sessionExportRoot/"receiver_traces.csv").c_str());
        if(!app.rebuild(true)||!app.renderer.initialize(executableDirectory3D(argv[0])/"shaders",app.error)){std::cerr<<app.error<<'\n';result=6;}
        if(terrainSmoke){if(result==0&&!terrainReview3D(app)){std::cerr<<app.error<<'\n';return 12;}
            if(result)return result;app.visuals.colorTopography=true;app.visuals.terrainPalette=7;app.layoutPreset=1;app.resetLayout=true;app.requestedTab=2;setLanguage3D(1);}
        if(result==0&&smoke&&!acquisitionSmoke&&!cudaSmoke&&!backgroundSmoke&&app.simulation->backendInfo().kind!=defaultBackend3D()){std::cerr<<"Application default backend not selected\n";result=12;}
        if(result==0 && resizeSmoke && !app.verifyResizePolicy()){std::cerr<<app.error<<'\n';result=7;}
        if(result==0 && (resizeSmoke||cudaSmoke) && !app.verifyBackendPolicy()){std::cerr<<app.error<<'\n';result=7;}
        if(result==0 && cudaSmoke && !app.verifyDevicePolicy()){std::cerr<<app.error<<'\n';result=7;}
        if(deviceSmoke)app.requestedTab=7;
        if(result==0&&modelDisplaySmoke&&!app.verifyMaterialCache()){std::cerr<<app.error<<'\n';result=12;}
        if(result==0&&navigationSmoke) {
            VisualSettings3D v;v.range=1;const auto wave=waveColor3D(.37,v);cycleVisual3D(v,VisualShortcut3D::ModelPalette);
            if(waveColor3D(.37,v)!=wave||v.palette!=1||v.component!=0)result=12;
            v.modelLow=0;v.modelHigh=1;const auto material=modelColor3D(.37,v);cycleVisual3D(v,VisualShortcut3D::WavePalette);
            if(modelColor3D(.37,v)!=material)result=12;
            cycleVisual3D(v,VisualShortcut3D::ModelPalette);if(modelColor3D(.37,v)==material)result=12;
            for(auto action:{VisualShortcut3D::Component,VisualShortcut3D::WavePalette,VisualShortcut3D::ModelPalette}){const int c=v.component,w=v.palette,m=v.modelPalette;cycleVisual3D(v,action);cycleVisual3D(v,action,true);if(v.component!=c||v.palette!=w||v.modelPalette!=m)result=12;}
            if(result)std::cerr<<"Independent palettes / reverse cycle failed\n";
        }
        if(result==0&&colorbarSmoke) {
            if(!VisualSettings3D{}.normalizedColorbar){std::cerr<<"Colorbar must default to normalized\n";result=12;}
            for(int component=0;component<5;++component)for(bool normalized:{false,true})for(int palette=0;palette<wavePaletteCount3D;++palette) {
                VisualSettings3D v;v.component=component;v.palette=v.modelPalette=palette;v.range=8;v.gain=2;v.normalizedColorbar=normalized;
                const auto scale=colorbarScale3D(v);const bool expectedNormalized=normalized;
                if(scale.normalized!=expectedNormalized||scale.high!=(expectedNormalized?1:4)||scale.low!=(component==3?0:-scale.high))result=12;
                for(int stop=0;stop<5;++stop) {
                    const double tick=scale.low+(scale.high-scale.low)*stop/4;
                    const auto actual=waveColor3D(scale.physicalValue(tick),v);const auto& expected=RecordPalettes::wavePalette(palette)[stop];
                    for(int c=0;c<3;++c)if(std::abs(actual[c]-expected[c])>1e-6)result=12;
                }
                const auto before=waveColor3D(.27,v);v.normalizedColorbar=!normalized;if(before!=waveColor3D(.27,v))result=12;
            }
            if(result)std::cerr<<"Normalized/SI labels do not match field colors\n";
        }
        if(result==0&&workspaceSmoke) {
            for(int palette=0;palette<wavePaletteCount3D;++palette)for(int component:{0,3}) {
                VisualSettings3D v;v.palette=palette;v.component=component;v.range=8;v.gain=2;
                for(int stop=0;stop<5;++stop){const double value=component==3?stop:2.0*stop-4;const auto color=waveColor3D(value,v);const auto& expected=RecordPalettes::wavePalette(palette)[stop];for(int c=0;c<3;++c)if(std::abs(color[c]-expected[c])>1e-6){std::cerr<<"Wave colorbar mismatch\n";result=12;}}
            }
        }
        if(result==0&&acquisitionSmoke) {
            const int count=app.simulation->totalSteps()*3/4;
            if(!app.simulation->advance(count,app.error))result=7;app.rememberPerformance();
            if(app.cudaAvailability.available){app.selectedBackend=1;if(!app.rebuild(true)||!app.simulation->advance(count,app.error))result=7;app.rememberPerformance();if(app.performanceRuns.size()!=2||!comparableRuns3D(app.performanceRuns[0],app.performanceRuns[1])){std::cerr<<"GUI comparison rejected identical runs\n";result=12;}}
            if(!writePerformanceCsv3D(app.performanceRuns,outputRoot3D()/"validation"/"acquisition_gui_timing.csv",app.error))result=7;
        }
        if(result==0 && smoke && !workflowCapture && !acquisitionSmoke && !backgroundSmoke && !app.simulation->advance(colorbarSmoke?app.simulation->totalSteps():app.simulation->totalSteps()*3/4,app.error)){std::cerr<<app.error<<'\n';result=7;}
        if(result==0 && smoke && !workflowCapture && !defaultTerrainSmoke && !backgroundSmoke) {
            if(!colorbarSmoke)app.exportSnapshot();if(app.error.empty())app.exportTraces();
            if(!app.error.empty()){std::cerr<<app.error<<'\n';result=7;}
        }
        if(result==0&&viewportSmoke){const bool okay=verifyViewports3D(app,host);if(!okay)std::cerr<<app.error<<'\n';app.renderer.shutdown();return okay?0:16;}
        if(result==0&&themeSmoke){const bool okay=verifyThemePanels3D(app,host);if(!okay)std::cerr<<app.error<<'\n';app.renderer.shutdown();return okay?0:17;}
        int frame=0;
        int busyFrames=0,pausedStep=-1;ImGuiID originalDock=0;double pausedComputeMs{};
        const auto backgroundStart=std::chrono::steady_clock::now();
        if(backgroundSmoke)app.running=true;
        struct UiCase {int width,height,language,theme,layout;float font;};
        const UiCase uiCases[]{
            {1600,1000,1,3,0,20},{1600,1000,0,3,0,20},
            {1280,800,1,0,0,20},{1024,720,0,4,0,20},
            {1024,720,1,7,2,26},{1920,1080,0,5,0,22},
            {1600,1000,1,1,1,20},{1280,800,0,2,2,18},
            {1920,1080,1,6,0,26},{1280,800,1,8,0,20},
            {1600,1000,0,9,1,22},{1600,1000,1,11,1,22}
        };
        const auto smokeRevision=app.simulation->revision();const auto smokeCamera=app.renderer.cameraState();
        const float smokeGain=app.visuals.gain;const int smokePalette=app.visuals.palette,smokeModelPalette=app.visuals.modelPalette,smokeComponent=app.visuals.component;
        const auto rawFingerprint=[&]{std::uint64_t hash=1469598103934665603ULL;for(const auto& r:app.simulation->receivers())for(const auto* samples:{&r.samples,&r.samplesVx,&r.samplesVy})for(double v:*samples){hash^=std::bit_cast<std::uint64_t>(v);hash*=1099511628211ULL;}return hash;};
        const auto originalRaw=rawFingerprint();
        const double commandDt=app.simulation->dt();const auto commandParameters=app.simulation->parameters();const auto* commandCoordinates=app.simulation->grid().coordinates.data();
        const auto reviewDirectory=outputRoot3D()/"validation/ui"/(sourceSmoke?"source-review":gridConfigSmoke?"grid-config-review":modelDisplaySmoke?"model-display-review":cflSmoke?"cfl-review":colorbarSmoke?"colorbar-review":boundarySmoke?"boundary-review":navigationSmoke?"navigation-review":acquisitionSmoke?"acquisition-review":layersSmoke?"layers-review":workspaceSmoke?"workspace-review":recordsSmoke?"records-review":"ui-review");
        if(uiSmoke)std::filesystem::create_directories(reviewDirectory);
        if(result==0&&workflowCapture&&!captureWorkflow3D(app,window)){std::cerr<<app.error<<'\n';result=14;}
        while(result==0 && !workflowCapture && !glfwWindowShouldClose(window)) {
            if(defaultTerrainSmoke&&frame==2){std::size_t n=0;const auto* saved=ImGui::SaveIniSettingsToMemory(&n);app.workspace.restoreIni.assign(saved,n);app.workspace.focused=label3D("Spatial exploration");app.workspace.focusNew=true;app.renderer.topViewCamera();}
            if(backgroundSmoke) {
                auto* worker=dynamic_cast<AsyncSimulation3D*>(app.simulation.get());
                if(worker&&worker->busy())++busyFrames;
                if(frame==3){if(const auto* pane=ImGui::FindWindowByName(label3D("Spatial exploration")))originalDock=pane->DockId;std::size_t n=0;const auto* saved=ImGui::SaveIniSettingsToMemory(&n);app.workspace.restoreIni.assign(saved,n);app.workspace.focused=label3D("Spatial exploration");app.workspace.focusNew=true;}
                if(frame==6)app.running=false;
                if(frame==9){worker->synchronize();pausedStep=app.simulation->currentStep();pausedComputeMs=app.simulation->diagnostics().timing.totalStepMs;app.workspace.restoreRequested=true;}
                if(frame==13){if(pausedStep!=app.simulation->currentStep()||pausedComputeMs!=app.simulation->diagnostics().timing.totalStepMs){std::cerr<<"Background pause kept advancing state/timer\n";result=19;}app.running=true;}
                if(frame==16){const auto* pane=ImGui::FindWindowByName(label3D("Spatial exploration"));if(!pane||!originalDock||pane->DockId!=originalDock){std::cerr<<"Maximized view failed to restore dock\n";result=19;}}
                if(std::chrono::steady_clock::now()-backgroundStart>std::chrono::seconds(60)){std::cerr<<"Background GUI timeout\n";result=19;}
            }
            glfwPollEvents();
            if(uiSmoke&&frame%10==0) {
                const auto& c=uiCases[frame/10];
                const auto oldLanguage=language3D();setLanguage3D(0);const ImGuiID en=ImHashStr(label3D("Controls"));
                setLanguage3D(1);const ImGuiID zh=ImHashStr(label3D("Controls"));setLanguage3D(oldLanguage);
                if(en!=zh){std::cerr<<"Translated dock identity changed\n";result=11;break;}
                const bool changedLayout=app.layoutPreset!=c.layout;
                app.layoutPreset=c.layout;app.resetLayout=app.resetLayout||changedLayout;
                app.theme=c.theme;applyTheme3D(c.theme);setLanguage3D(c.language);app.uiFontSize=c.font;
                app.requestedTab=frame/10==11?4:(frame/10)%5;
                if(gridConfigSmoke){const int index=frame/10;app.edited=applyModelPreset3D(4+index%3,app.simulation->parameters());app.requestedTab=index%2==0?1:0;app.shortcutHelp=index>=8;}
                if(sourceSmoke){app.requestedTab=4;app.edited.source.kind=frame/10%2?SeisWave::SourceKind::IsotropicMoment:SeisWave::SourceKind::DirectionalForce;app.edited.source.direction=frame/10%3?std::array<double,3>{1,0,0}:std::array<double,3>{0,0,1};}
                if(cflSmoke)app.requestedTab=0;
                if(boundarySmoke){app.requestedTab=frame/10%3==2?2:8;app.visuals.showAbsorbingLayer=(frame/10)%2==0;app.visuals.displayMode=1;app.layoutPreset=1;app.resetLayout=true;app.groupsExpanded=true;++app.groupEpoch;}
                if(colorbarSmoke){const int components[]{0,0,3,3,4,4,5,6,7,1,2,0};const int scalar=components[frame/10];app.requestedTab=frame/10==11?1:2;app.visuals.displayMode=frame/10>=9?2:scalar>=5?1:0;if(scalar>=5)app.visuals.modelComponent=scalar-5;else app.visuals.component=scalar;app.visuals.normalizedColorbar=(frame/10)%2==0;app.visuals.showGrid=false;app.layoutPreset=1;app.resetLayout=true;}
                if(modelDisplaySmoke){const int index=frame/10;const int modes[]{0,1,2,1,2,2,0,1,2,2,2,2};app.requestedTab=index==11?1:2;app.visuals.displayMode=modes[index];app.visuals.component=index==5?4:0;app.visuals.modelComponent=index%3;app.visuals.normalizedColorbar=index!=6;app.visuals.showWaveColorbar=index!=8;app.visuals.showModelColorbar=index!=9;app.resetLayout=true;}
                if(recordsSmoke) {
                    const int index=frame/10;const int styles[]{3,1,0,2,4,5,0,0,3,3,5,3};const int palettes[]{0,0,13,3,0,1,4,5,2,7,8,13};
                    app.layoutPreset=3;app.resetLayout=true;
                    app.recordView.settings=RecordSettings3D{};auto& settings=app.recordView.settings;
                    settings.style=styles[index];settings.palette=palettes[index];settings.component=index%4;
                    settings.view=(index==6||index==7)?1:0;settings.agc.enabled=index==7||index==8;
                    settings.normalizeEach=index==9||index==10;
                    if(index==11){settings.showAll=false;settings.first=5;settings.visible=8;}
                    app.recordView.invalidate();
                }
                if(workspaceSmoke){app.visuals.showGrid=(frame/10)%2==0;app.visuals.showCrosshair=app.visuals.showGrid;app.visuals.palette=(frame/10)%wavePaletteCount3D;app.requestedTab=2;}
                if(layersSmoke){app.requestedTab=5;app.layoutPreset=1;app.resetLayout=true;app.layerEditorOpen=false;app.requestLayerEditor=frame/10==8;}
                if(acquisitionSmoke){const int tabs[]{6,7,0,6,7,1,5,4,7,2,3,6};app.requestedTab=tabs[frame/10];}
                glfwSetWindowSize(window,c.width,c.height);
            }
            if(uiSmoke&&frame%10==1&&!(layersSmoke&&frame/10==8))ImGui::SetWindowFocus(label3D(recordsSmoke?"Receiver records":(acquisitionSmoke||navigationSmoke||colorbarSmoke||cflSmoke||modelDisplaySmoke||gridConfigSmoke)?"Controls":(frame/10)%5==4?"Numerical diagnostics":"Controls"));
            if((workspaceSmoke||layersSmoke)&&frame%10==5&&!(layersSmoke&&frame/10==8))ImGui::SetWindowFocus(label3D("Inline X-Z"));
            if(workspaceSmoke&&frame==2)app.panels.fill(false);
            if(workspaceSmoke&&frame==4)app.restoreWorkspace();
            if(glfwGetWindowAttrib(window,GLFW_ICONIFIED)&&ImGui::GetPlatformIO().Viewports.Size<=1){glfwWaitEventsTimeout(0.05);continue;}
            if(auto* background=dynamic_cast<AsyncSimulation3D*>(app.simulation.get())) {
                if(!background->poll(app.error))app.running=false;
                if(app.running&&!app.simulation->completed())background->queue(app.stepsPerFrame,app.frameBudgetMs);else background->pause();
                if(app.simulation->completed()&&app.running){app.running=false;app.rememberPerformance();app.status="Simulation completed. Export the snapshot or receiver records.";}
            } else if(app.running) {
                const auto begin=std::chrono::steady_clock::now();
                const int startStep=app.simulation->currentStep();
                for(int n=0;n<app.stepsPerFrame && !app.simulation->completed();++n) {
                    if(!app.simulation->advance(1,app.error)){app.running=false;break;}
                    if(std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-begin).count()>=app.frameBudgetMs)break;
                }
                app.lastBatchMs=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-begin).count();
                const int advanced=app.simulation->currentStep()-startStep;
                if(advanced>0)app.meanStepMs=app.meanStepMs==0?app.lastBatchMs/advanced:0.85*app.meanStepMs+0.15*app.lastBatchMs/advanced;
                if(app.simulation->completed()){app.running=false;app.rememberPerformance();app.status="Simulation completed. Export the snapshot or receiver records.";}
            }
            ImGui_ImplOpenGL3_NewFrame();ImGui_ImplGlfw_NewFrame();
            if(routeSmoke){if(frame==0)io.AddFocusEvent(true);if(frame==2){const auto at=app.homeRect.GetCenter();io.AddMousePosEvent(at.x,at.y);}if(frame==3)io.AddMouseButtonEvent(0,true);if(frame==4)io.AddMouseButtonEvent(0,false);}
            if(commandSmoke) {
                if(frame==1){io.AddFocusEvent(true);io.AddKeyEvent(ImGuiKey_F1,true);}
                if(frame==2)io.AddKeyEvent(ImGuiKey_F1,false);
                if(frame==3)io.AddKeyEvent(ImGuiKey_Escape,true);
                if(frame==4){io.AddKeyEvent(ImGuiKey_Escape,false);app.edited.vp*=1.25;io.AddKeyEvent(ImGuiKey_C,true);}
                if(frame==5){io.AddKeyEvent(ImGuiKey_C,false);io.AddKeyEvent(ImGuiKey_G,true);}
                if(frame==6)io.AddKeyEvent(ImGuiKey_G,false);
                if(frame==7)io.AddKeyEvent(ImGuiKey_F1,true);
            }
            if((cflSmoke||modelDisplaySmoke)&&frame%10==3) {
                if(const auto* pane=ImGui::FindWindowByName(label3D("Controls"))){const auto at=pane->InnerRect.GetCenter();io.AddMousePosEvent(at.x,at.y);io.AddMouseWheelEvent(0,cflSmoke?-12.0f:frame/10%3==1?-6.0f:100.0f);}
            }
            if(navigationSmoke) {
                if(frame==0||frame==10)io.AddFocusEvent(true);
                if(frame==10)io.AddKeyEvent(ImGuiMod_Shift,true);
                if(frame<6||(frame>=10&&frame<16)){const int phase=frame%10;const ImGuiKey key=phase<2?ImGuiKey_Q:phase<4?ImGuiKey_W:ImGuiKey_E;io.AddKeyEvent(key,phase%2==0);}
                if(frame==15)io.AddKeyEvent(ImGuiMod_Shift,false);
                const int clickFrames[]{32,34,36,38,42};for(int i=0;i<5;++i)if(frame==clickFrames[i]||frame==clickFrames[i]+1){const auto at=app.workflowRects[i].GetCenter();io.AddMousePosEvent(at.x,at.y);io.AddMouseButtonEvent(0,frame==clickFrames[i]);}
            }
            ImGui::NewFrame();
            ImGui::PushFont(nullptr,app.uiFontSize);
            app.gridSegments.fill(0);app.toolbar();app.docking();app.controls();
            if(smoke){const auto* commands=ImGui::FindWindowByName("##Commands3D");if(!commands||!commands->InnerRect.Contains(app.elapsedRect)||app.elapsedRect.GetWidth()<=0){std::cerr<<"3D elapsed timer clipped\n";result=19;}}
            if(app.simulation&&app.simulation->initialized()){app.diagnosticsWindow();app.updateRange();app.spatialWindow();app.sliceWindow(0,"Inline X-Z");app.sliceWindow(1,"Crossline Y-Z");app.sliceWindow(2,"Depth X-Y");app.records();app.floatingControls();app.shortcuts();app.shortcutWindow();}
            else {app.running=false;app.homeRequested=true;}
            ImGui::PopFont();
            if(commandSmoke&&frame==2){const auto* help=ImGui::FindWindowByName(label3D("Keyboard shortcuts"));if(!app.shortcutHelp||!help||!help->Active){std::cerr<<"F1 failed to open shortcut help\n";result=12;}}
            if(commandSmoke&&frame==7) {
                if(app.simulation->currentStep()!=0||app.simulation->dt()!=commandDt||app.simulation->parameters()!=commandParameters||app.simulation->grid().coordinates.data()!=commandCoordinates||app.simulation->diagnostics().timing.measuredSteps!=0||app.edited.vp==commandParameters.vp||app.visuals.showGrid||!app.shortcutHelp){std::cerr<<"C/F1/G failed, applied staged edits, or rebuilt the geometry\n";result=12;}
                for(const auto& receiver:app.simulation->receivers())if(receiver.samples!=std::vector<double>{0})result=12;
                std::cout<<"Commands PASS: F1 help, Esc close, C in-place clear, G grid, staged Vp retained and live dt unchanged\n";
            }
            if(navigationSmoke) {
                if(frame<6||(frame>=10&&frame<16)) {
                    const int phase=frame%10,direction=frame<10?1:-1;
                    const int c=phase>=2?(smokeComponent+direction+5)%5:smokeComponent,w=(smokePalette+direction+14)%14,m=phase>=4?(smokeModelPalette+direction+14)%14:smokeModelPalette;
                    if(app.visuals.component!=c||app.visuals.palette!=w||app.visuals.modelPalette!=m){std::cerr<<"Q/W/E input mapping failed frame "<<frame<<'\n';result=12;}
                    if(phase==5){app.visuals.component=smokeComponent;app.visuals.palette=smokePalette;app.visuals.modelPalette=smokeModelPalette;}
                }
                const int released[]{33,35,37,39,43};for(int i=0;i<5;++i)if(frame==released[i]&&app.controlCategory!=i){std::cerr<<"Workflow click failed "<<i<<'\n';result=12;}
            }
            if(workspaceSmoke&&(frame==3||frame==6))for(const auto* name:Workbench3D::panelNames){const auto* pane=ImGui::FindWindowByName(label3D(name));if(!pane||(frame==3?pane->Active:!pane->Active||!pane->HasCloseButton)){std::cerr<<"Panel close/restore failed: "<<name<<'\n';result=12;}}
            ImGui::Render();int width=0,height=0;glfwGetFramebufferSize(window,&width,&height);glViewport(0,0,width,height);glClearColor(palette3D().background.x,palette3D().background.y,palette3D().background.z,1);glClear(GL_COLOR_BUFFER_BIT);
            ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
            if(uiSmoke&&frame%10==7) {
                const auto& c=uiCases[frame/10];
                if(app.simulation->revision()!=smokeRevision||app.renderer.cameraState()!=smokeCamera||app.visuals.gain!=smokeGain||(!workspaceSmoke&&app.visuals.palette!=smokePalette)){std::cerr<<"Presentation changed simulation/view state\n";result=11;}
                if(rawFingerprint()!=originalRaw){std::cerr<<"Record display processing modified raw receiver samples\n";result=11;}
                const auto* controls=ImGui::FindWindowByName(label3D("Controls"));
                if(!controls||controls->Pos.x<0||controls->Pos.y<0||controls->Pos.x+controls->Size.x>width+2||controls->Pos.y+controls->Size.y>height+2){std::cerr<<"Inspector escaped viewport\n";result=11;}
                if(navigationSmoke)for(int i=0;i<5;++i){const auto& r=app.workflowRects[i];if(std::abs(r.Min.y-app.workflowRects[0].Min.y)>.5f||r.Min.x<controls->Pos.x||r.Max.x>controls->Pos.x+controls->Size.x||r.GetWidth()<10){std::cerr<<"Horizontal workflow overflow\n";result=12;}}
                if(workspaceSmoke) {
                    const auto& p=app.simulation->parameters();const int u[]{p.nx,p.ny,p.nx},v[]{p.nz,p.nz,p.ny};
                    for(int axis=0;axis<3;++axis)if(app.gridSegments[axis]&&(!app.visuals.showGrid||app.gridSegments[axis]!=static_cast<std::size_t>(u[axis])*(v[axis]-1)+static_cast<std::size_t>(v[axis])*(u[axis]-1))){std::cerr<<"Incomplete slice mesh overlay\n";result=12;}
                }
                if(layersSmoke&&frame/10==8){const auto* modal=ImGui::FindWindowByName(label3D("Layer editor"));if(!modal||!modal->Active){std::cerr<<"Expanded layer editor failed to open\n";result=12;}}
                const auto base="case_"+std::to_string(frame/10)+"_"+std::to_string(c.width)+"x"+std::to_string(c.height)+(c.language?"_zh":"_en");
                if(!captureFrame3D(reviewDirectory/(base+".ppm"),width,height))result=8;
                if(glGetError()!=GL_NO_ERROR||!app.error.empty())result=9;
                std::cout<<"UI capture "<<base<<" theme="<<c.theme<<" font="<<c.font<<" layout="<<c.layout<<" panel="<<(frame/10)%5<<" | presentation invariant "<<(result==0?"PASS":"FAIL")<<'\n';
                if(frame/10==static_cast<int>(std::size(uiCases))-1)glfwSetWindowShouldClose(window,GLFW_TRUE);
            }
            host.renderPlatformWindows();
            if(backgroundSmoke&&app.simulation->completed()&&frame>22) {
                const int completedSteps=app.simulation->currentStep();
                const auto cache=app.renderer.cacheStatistics();const auto folder=outputRoot3D()/"validation/background";std::filesystem::create_directories(folder);
                std::ofstream(folder/"gui.json")<<"{\"frames\":"<<frame+1<<",\"steps\":"<<app.simulation->currentStep()<<",\"busy_frames\":"<<busyFrames<<",\"scene_reuse\":"<<cache[1]<<",\"pause_and_restore\":"<<(result==0?"true":"false")<<"}\n";
                if(!busyFrames||frame<=app.simulation->currentStep()||!cache[1]||glGetError()!=GL_NO_ERROR||!app.error.empty())result=19;
                if(!captureFrame3D(folder/"workspace.ppm",width,height))result=8;
                const double totalComputeMs=app.simulation->diagnostics().timing.totalStepMs;
                if(!(totalComputeMs>pausedComputeMs)||!app.clearFields()||app.simulation->diagnostics().timing.totalStepMs!=0||!app.simulation->advance(1,app.error)||!(app.simulation->diagnostics().timing.totalStepMs>0)||!app.rebuild(true)||app.simulation->diagnostics().timing.totalStepMs!=0){std::cerr<<"3D timer clear / step / rebuild failed\n";result=19;}
                std::cout<<"3D elapsed timer "<<(result==0?"PASS":"FAIL")<<": pause/resume, clear, single step, rebuild; completed compute "<<totalComputeMs<<" ms\n";
                std::cout<<"Background GUI "<<(result==0?"PASS":"FAIL")<<": "<<frame+1<<" frames / "<<completedSteps<<" steps, "<<busyFrames<<" busy frames; pause, maximize/restore, scene reuse\n";glfwSetWindowShouldClose(window,GLFW_TRUE);
            }
            if(smoke && !uiSmoke && !viewportSmoke && !backgroundSmoke && frame==7) {
                const auto screenshot=outputRoot3D()/(defaultTerrainSmoke?"validation/default-terrain/workspace.ppm":terrainSmoke?"validation/terrain-display/terrain_workspace.ppm":commandSmoke?"commands_smoke.ppm":"gui_smoke.ppm");
                std::filesystem::create_directories(screenshot.parent_path());
                if(!captureFrame3D(screenshot,width,height))result=8;
                if(defaultTerrainSmoke){GLint bound=0,tw=0,th=0;glGetIntegerv(GL_TEXTURE_BINDING_2D,&bound);glBindTexture(GL_TEXTURE_2D,app.renderer.texture3D());glGetTexLevelParameteriv(GL_TEXTURE_2D,0,GL_TEXTURE_WIDTH,&tw);glGetTexLevelParameteriv(GL_TEXTURE_2D,0,GL_TEXTURE_HEIGHT,&th);std::vector<unsigned char> pixels(static_cast<std::size_t>(tw)*th*4);glGetTexImage(GL_TEXTURE_2D,0,GL_RGBA,GL_UNSIGNED_BYTE,pixels.data());glBindTexture(GL_TEXTURE_2D,bound);std::ofstream raw(screenshot.parent_path()/"scene-texture.ppm",std::ios::binary);raw<<"P6\n"<<tw<<' '<<th<<"\n255\n";for(int y=th-1;y>=0;--y)for(int x=0;x<tw;++x)raw.write(reinterpret_cast<const char*>(pixels.data()+(static_cast<std::size_t>(y)*tw+x)*4),3);std::cout<<"Default terrain texture "<<tw<<'x'<<th<<'\n';if(tw<600||th<200){std::cerr<<"Default terrain framebuffer was undersized\n";result=18;}}
                if(defaultTerrainSmoke){const auto& p=app.simulation->parameters();const auto& d=p.surfaceGrid;std::ofstream(screenshot.parent_path()/"result.json")<<"{\"source_nx\":"<<d.nx<<",\"source_ny\":"<<d.ny<<",\"source_spacing_m\":"<<d.spacing<<",\"solver_nx\":"<<p.nx<<",\"solver_ny\":"<<p.ny<<",\"solver_nz\":"<<p.nz<<",\"solver_spacing_m\":"<<p.spacing<<",\"step\":"<<app.simulation->currentStep()<<",\"finite\":"<<(app.simulation->diagnostics().allFinite?"true":"false")<<"}\n";if(d.nx!=400||d.ny!=288||d.spacing!=25||d.elevations.size()!=115200||!app.simulation->diagnostics().allFinite)result=18;}
                const auto glError=glGetError();if(glError!=GL_NO_ERROR){std::cerr<<"OpenGL error: "<<glError<<'\n';result=9;}
                if(!app.error.empty()){std::cerr<<app.error<<'\n';result=10;}
                std::cout<<"GUI smoke: "<<(app.simulation->backendInfo().kind==BackendKind3D::Cuda?"CUDA":"CPU")<<" step "<<app.simulation->currentStep()<<" / "<<app.simulation->totalSteps()<<" | framebuffer "<<width<<'x'<<height<<" | traction-rate residual "<<app.simulation->diagnostics().maxTractionRateRelative<<'\n';
                glfwSetWindowShouldClose(window,GLFW_TRUE);
            }
            if(app.homeRequested){returnToHome=true;glfwSetWindowShouldClose(window,GLFW_TRUE);std::cout<<"3D Home route PASS\n";}
            if(routeSmoke&&frame>8&&!returnToHome){std::cerr<<"3D Home button failed\n";result=15;glfwSetWindowShouldClose(window,GLFW_TRUE);}
            glfwSwapBuffers(window);if(frame==0&&!smoke)glfwShowWindow(window);++frame;
        }
        if(!smoke){app.savePreferences();SeisWave::Ui::setInterfaceFontSize(app.uiFontSize);app.workspace.persistBeforeClose(ini.c_str());}app.renderer.shutdown();
    }
    return result?result:returnToHome?SeisWave::Ui::returnHome:0;
}
}

