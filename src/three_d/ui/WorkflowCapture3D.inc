// Real solver and native framebuffer. This independent session never saves user preferences.
bool captureWorkflow3D(Workbench3D& app,GLFWwindow* window) {
    const auto output=outputRoot3D()/"screenshots"/"curvilinear-workflow";std::filesystem::create_directories(output);
    glfwSetWindowSize(window,2560,1494);glfwPollEvents();setLanguage3D(1);app.theme=0;applyTheme3D(0);app.uiFontSize=20;
    std::ofstream manifest(output/"capture.tsv");
    manifest<<"image\tdescription\twidth\theight\tbackend\tstep\ttime_s\tdt_s\tnx\tny\tnz\tspacing_m\tf0_hz\tppw\treceivers\tsamples\tconfiguration_id\tconfiguration\n"<<std::setprecision(17);
    const auto rawHash=[&]{std::uint64_t hash=1469598103934665603ULL;for(const auto& r:app.simulation->receivers())for(const auto* samples:{&r.samples,&r.samplesVx,&r.samplesVy})for(double v:*samples){hash^=std::bit_cast<std::uint64_t>(v);hash*=1099511628211ULL;}return hash;};
    const auto stage=[&](const SimulationParameters& p){app.edited=p;app.shortcutHelp=false;app.error.clear();if(!app.rebuild(true))return false;const auto s=sourceIndices3D(p);app.slices={s[0],s[1],s[2]};app.resetViews();app.recordView.settings={};app.visuals={};app.visuals.autoRange=true;app.visuals.showGrid=true;app.visuals.modelPalette=1;app.visuals.palette=1;app.visuals.displayMode=0;return true;};
    const auto advanceTo=[&](double time){const int goal=std::min(app.simulation->totalSteps(),static_cast<int>(std::lround(time/app.simulation->dt())));return app.simulation->advance(std::max(0,goal-app.simulation->currentStep()),app.error);};
    const auto shot=[&](const char* name,const char* description,int tab,int layout=1){
        app.running=false;app.requestedTab=tab;app.layoutPreset=layout;app.resetLayout=true;++app.groupEpoch;app.groupsExpanded=true;
        const auto revision=app.simulation->revision(),hash=rawHash();
        for(int frame=0;frame<7;++frame) {
            glfwPollEvents();ImGui_ImplOpenGL3_NewFrame();ImGui_ImplGlfw_NewFrame();ImGui::NewFrame();ImGui::PushFont(nullptr,20);
            if(frame==1)ImGui::SetWindowFocus(label3D("Controls"));
            if(frame==2){if(const auto* pane=ImGui::FindWindowByName(label3D("Controls"))){const auto at=pane->InnerRect.GetCenter();ImGui::GetIO().AddMousePosEvent(at.x,at.y);ImGui::GetIO().AddMouseWheelEvent(0,100);}}
            app.gridSegments.fill(0);app.toolbar();app.docking();app.controls();app.diagnosticsWindow();app.updateRange();app.spatialWindow();app.sliceWindow(0,"Inline X-Z");app.sliceWindow(1,"Crossline Y-Z");app.sliceWindow(2,"Depth X-Y");app.records();app.floatingControls();app.shortcutWindow();
            ImGui::PopFont();ImGui::Render();int width=0,height=0;glfwGetFramebufferSize(window,&width,&height);glViewport(0,0,width,height);glClearColor(palette3D().background.x,palette3D().background.y,palette3D().background.z,1);glClear(GL_COLOR_BUFFER_BIT);ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
            if(frame==6&& (width!=2560||height!=1494||!captureFrame3D(output/(std::string(name)+".ppm"),width,height))){app.error="Native workflow framebuffer capture failed.";return false;}
            glfwSwapBuffers(window);
        }
        if(glGetError()!=GL_NO_ERROR||!app.error.empty()||app.simulation->revision()!=revision||rawHash()!=hash){if(app.error.empty())app.error="Workflow presentation changed raw simulation data.";return false;}
        const auto& p=app.simulation->parameters();manifest<<name<<'\t'<<description<<"\t2560\t1494\t"<<(app.simulation->backendInfo().kind==BackendKind3D::Cuda?"CUDA":"CPU")<<'\t'<<app.simulation->currentStep()<<'\t'<<app.simulation->currentStep()*app.simulation->dt()<<'\t'<<app.simulation->dt()<<'\t'<<p.nx<<'\t'<<p.ny<<'\t'<<p.nz<<'\t'<<p.spacing<<'\t'<<p.dominantFrequency<<'\t'<<app.simulation->diagnostics().pointsPerShortestWavelength<<'\t'<<app.simulation->receivers().size()<<'\t'<<app.simulation->receivers().front().samples.size()<<'\t'<<configurationId3D(p)<<'\t'<<configurationText3D(p)<<'\n';
        std::cout<<"Captured "<<name<<" | "<<app.simulation->currentStep()<<" steps | "<<p.dominantFrequency<<" Hz\n"<<std::flush;return true;
    };
    auto base=applyModelPreset3D(0,{});base.receiverArray={ReceiverArrayKind3D::XLine,2,-1,1,196,1,1,1};
    if(!stage(base)||!shot("01-model","Curved-grid model and physical dimensions",1)||!shot("02-source","Vertical-force Ricker source and spatial sampling advice",4)||!shot("03-receivers","196-channel line and inclusive capacity",6)||!shot("04-run-start","Applied time step and stop conditions at t=0",0))return false;
    if(!advanceTo(.10)||!shot("05-wave-010","Wave propagation near 0.10 s",0)||!advanceTo(.25)||!shot("06-wave-025","Wave propagation near 0.25 s",0)||!advanceTo(.40)||!shot("07-wave-040","Wave propagation near 0.40 s",0)||!advanceTo(.5)||!shot("08-records-raw","Completed 0.5 s native records without AGC",6,3))return false;
    app.recordView.settings.agc.enabled=true;app.recordView.invalidate();if(!shot("09-records-agc","AGC preview of the same immutable raw records",6,3))return false;
    app.recordView.settings.agc.enabled=false;app.recordView.settings.view=1;app.selectedReceiver=98;app.recordView.invalidate();if(!shot("10-single-3c","Same receiver: Vz Vx Vy raw single-channel records",6,3))return false;
    app.recordView.settings={};app.visuals.showGrid=true;app.visuals.showAbsorbingLayer=true;if(!shot("11-grid-absorber","Actual cell grid and numerical sponge overlay",8))return false;
    app.visuals.showGrid=app.visuals.showAbsorbingLayer=false;app.shortcutHelp=true;if(!shot("12-shortcuts","Regular-grid-compatible shortcut help",2))return false;app.shortcutHelp=false;
    const auto modelPath=output/"workflow_model.vts";
    std::snprintf(app.modelPath.data(),app.modelPath.size(),"%s",utf8Path3D(modelPath).c_str());
    app.saveModel();if(!app.error.empty())return false;
    const auto saved=app.simulation->parameters();const double savedDt=app.simulation->dt();
    app.edited.spacing=7;app.dirty=true;
    if(!app.loadModel()||app.simulation->parameters()!=saved||app.simulation->currentStep()!=0||app.simulation->dt()!=savedDt||!advanceTo(.5))return false;
    std::snprintf(app.vtsPath.data(),app.vtsPath.size(),"%s",utf8Path3D(output/"workflow_vz.vts").c_str());
    app.visuals.component=0;app.snapshotFollowDisplay=true;app.exportSnapshot();if(!app.error.empty())return false;
    std::cout<<"3D UI model restore / pending synchronization / selected Vz export PASS\n"<<std::flush;
    std::snprintf(app.csvPath.data(),app.csvPath.size(),"%s",utf8Path3D(output/"benchmark_3c.csv").c_str());app.receiverExportComponents={true,true,true};app.exportTraces();if(!app.error.empty()||!shot("13-export","Reproducible model save/import and selected wavefield export",3))return false;
    auto layers=applyMaterialPreset3D(1,base);layers.nx=100;layers.ny=20;layers.nz=100;layers.spacing=1;layers.topographyAmplitude=2;layers.topographyWavelength=60;layers.sourceX=50;layers.sourceY=10;layers.sourceZ=20;layers.receiverArray={};layers.duration=.12;
    if(!stage(layers))return false;app.materialPreset=1;app.visuals.displayMode=1;app.visuals.showGrid=true;if(!shot("14-layered-model","Provided five-layer loess table aligned by k",5))return false;
    for(int frequency:{30,50,60}) {
        auto high=base;high.nx=128;high.ny=64;high.nz=144;high.spacing=frequency==30?2.5:frequency==50?1.25:1;high.sourceX=64;high.sourceY=32;high.sourceZ=24;high.receiverArray={};high.topographyAmplitude=2;high.topographyWavelength=100;high.duration=.12;high.maxSteps=0;high.dominantFrequency=frequency;
        if(!stage(high)||!advanceTo(.12))return false;
        const auto name=std::string(frequency==30?"15-high-30hz":frequency==50?"16-high-50hz":"17-high-60hz");if(!shot(name.c_str(),"Resolved high-frequency run; finite/source-window check, not an accuracy proof",4))return false;
    }
    auto plane=base;plane.nx=plane.ny=64;plane.nz=80;plane.sourceX=plane.sourceY=32;plane.sourceZ=20;plane.topographyAmplitude=2;plane.receiverArray={ReceiverArrayKind3D::GridXY,0,0,1,32,32,2,2};
    if(!stage(plane)||!shot("18-receivers-1024","1024-position X-fast grid, coupled capacities and three components",6))return false;
    if(!manifest.good()){app.error="Workflow manifest write failed.";return false;}return true;
}
