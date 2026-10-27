// Full native framebuffer captures from a separate, reproducible solver session.
int captureWorkflow2D() {
    SeisWave::Ui::AppRuntime runtime;std::string error;
    if(!runtime.open("SeisWave Lab · Curvilinear | 2D P-SV | CPU / CUDA",true,error))throw std::runtime_error(error);
    glfwSetWindowSize(runtime.window(),2560,1494);glfwPollEvents();
    setLanguage3D(1);applyTheme3D(11);SeisWave::Ui::setParameterDisplayMode(2,0);SeisWave::Ui::setInterfaceFontSize(20);
    Workbench app;app.staged.nx=app.staged.nz=200;app.staged.spacing=2.5;app.staged.frequency=30;
    app.staged.relief=5;app.staged.wavelength=300;app.staged.sourceX=100;app.staged.sourceZ=40;
    app.staged.duration=.25;app.staged.maxSteps=0;app.staged.sampleIntervalMs=.5;
    app.staged.receiverStart=2;app.staged.receiverCount=196;app.staged.receiverStride=1;app.staged.receiverDepth=1;
    if(!app.apply())throw std::runtime_error(app.error);
    const auto output=SeisWave::Ui::outputRoot()/"screenshots/two-d-workflow";std::filesystem::create_directories(output);
    std::ofstream manifest(output/"capture.tsv");
    manifest<<"image\twidth\theight\tstep\ttime_s\tdt_s\trecord_dt_s\tnx\tnz\tspacing_m\tf0_hz\tbackend\ttheme\tcapture_date\trecord_height\trecord_palette\trecord_style\trecord_gain\n"<<std::setprecision(17);
    const auto draw=[&](){runtime.begin();app.toolbar();app.controls();app.modelDialogs();app.activeParameters();app.wavefield();app.records();app.shortcuts();runtime.present();};
    bool publicationLayoutReady=false;
    const auto shot=[&](const char* name,int category){
        app.category=category;
        if(category!=4){app.groupsExpanded=true;++app.groupEpoch;}
        const auto step=app.simulation.step();
        draw();
        // Reorient the existing wave/record split; retain valid dock window ownership.
        if(!publicationLayoutReady){
            auto* pane=ImGui::FindWindowByName(label("Receiver records","接收记录").c_str());
            if(!pane||!pane->DockNode||!pane->DockNode->ParentNode)throw std::runtime_error("Publication dock layout unavailable");
            auto* node=pane->DockNode;auto* parent=node->ParentNode;
            auto* sibling=parent->ChildNodes[parent->ChildNodes[0]==node?1:0];
            parent->SplitAxis=ImGuiAxis_X;
            ImGui::DockBuilderSetNodeSize(node->ID,{parent->Size.x*.51f,parent->Size.y});
            ImGui::DockBuilderSetNodeSize(sibling->ID,{parent->Size.x*.49f,parent->Size.y});
            app.fit=true;publicationLayoutReady=true;
        }
        for(int i=0;i<6;++i)draw();
        if(step>0&&app.recordHeight<550)throw std::runtime_error("Record pane is too short for full-size publication");
        int w{},h{};glfwGetFramebufferSize(runtime.window(),&w,&h);
        if(w!=2560||h!=1494||!app.domainVisible||!app.error.empty()||app.simulation.step()!=step||!SeisWave::Ui::capture(output/(std::string(name)+".ppm"),w,h))throw std::runtime_error("Full-size 2D capture failed: "+app.error);
        const auto &p=app.simulation.parameters();
        manifest<<name<<'\t'<<w<<'\t'<<h<<'\t'<<step<<'\t'<<step*app.simulation.dt()<<'\t'<<app.simulation.dt()<<'\t'<<app.simulation.recordDt()<<'\t'<<p.nx<<'\t'<<p.nz<<'\t'<<p.spacing<<'\t'<<p.frequency<<'\t'<<(app.selectedBackend?"CUDA":"CPU")<<"\tRed Yellow Orange\t2026-10-27\t"<<app.recordHeight<<'\t'<<app.recordView.settings.palette<<'\t'<<app.recordView.settings.style<<'\t'<<app.recordView.settings.gain<<'\n';
        std::cout<<"Captured 2D "<<name<<" | "<<w<<'x'<<h<<" | "<<step<<" steps\n"<<std::flush;
    };
    const auto exportGroup=[&](const char* title){
        app.category=4;++app.groupEpoch;app.groupsExpanded=false;draw();draw();
        for(auto* window:ImGui::GetCurrentContext()->Windows)
            if(std::string_view(window->Name).find("##2DParameterFields")!=std::string_view::npos){
                window->StateStorage.SetInt(window->GetID(title),1);ImGui::SetScrollY(window,0);
            }
    };
    shot("model",0);
    ++app.groupEpoch;app.groupsExpanded=true;
    if(!app.simulation.advance(static_cast<int>(.1/app.simulation.dt()),app.error))throw std::runtime_error(app.error);
    shot("workbench",2);
    if(!app.simulation.advance(app.simulation.totalSteps()-app.simulation.step(),app.error))throw std::runtime_error(app.error);
    ++app.recordGeneration;app.recordView.invalidate();shot("completed",2);
    app.snapshotFollowDisplay=false;app.snapshotComponents={true,true,false,false,false,false,false};
    exportGroup("模型保存与复现（.vts）");shot("model-io",4);
    exportGroup("波场快照与 CSV");shot("export",4);
    app.segyProfile=2;app.segyGain=1;app.segyIntervalMs=.5;
    exportGroup("SEG-Y 导出");shot("segy",4);
    app.help=true;shot("help",2);app.help=false;
    app.category=3;shot("display",3);
    const auto bounds=app.recordView.optionsBounds;auto& io=ImGui::GetIO();
    io.AddMousePosEvent((bounds[0]+bounds[2])*.5f,(bounds[1]+bounds[3])*.5f);io.AddMouseButtonEvent(0,true);draw();io.AddMouseButtonEvent(0,false);draw();shot("record-options",3);
    io.AddKeyEvent(ImGuiKey_Escape,true);draw();io.AddKeyEvent(ImGuiKey_Escape,false);draw();
    app.staged.layers=materialPresets()[2].layers;app.staged.layered=true;app.openLayers=true;shot("layers",0);
    io.AddKeyEvent(ImGuiKey_Escape,true);draw();io.AddKeyEvent(ImGuiKey_Escape,false);draw();
    // Dismiss the modal without applying its draft by using the real cancel control.
    if(!ImGui::GetCurrentContext()->OpenPopupStack.empty()){
        runtime.begin();ImGui::ClosePopupToLevel(0,true);runtime.present();
    }
    app.staged.layers.clear();app.staged.layered=false;
    const auto path=(SeisWave::Ui::elevationExampleRoot()/"ridge_2km.csv").u8string();
    std::snprintf(app.surfacePath.data(),app.surfacePath.size(),"%s",reinterpret_cast<const char*>(path.c_str()));
    if(!app.loadTerrain())throw std::runtime_error(app.error);app.openTerrain=true;shot("terrain",0);
    runtime.begin();ImGui::ClosePopupToLevel(0,true);runtime.present();
    app.staged.importedSurface=false;app.staged.surfaceSamples.clear();
    app.staged.source.kind=SeisWave::SourceKind::IsotropicMoment;
    if(!app.apply()||!app.simulation.advance(static_cast<int>(.1/app.simulation.dt()),app.error))throw std::runtime_error(app.error);
    app.component=3;app.groupsExpanded=true;++app.groupEpoch;shot("source-explosion",1);
    const auto example=(SeisWave::Ui::elevationExampleRoot()/"hill_valley_1km.csv").u8string();
    std::snprintf(app.surfacePath.data(),app.surfacePath.size(),"%s",reinterpret_cast<const char*>(example.c_str()));
    if(!app.loadTerrain())throw std::runtime_error(app.error);app.openTerrain=true;shot("elevation-examples",0);
    return 0;
}
