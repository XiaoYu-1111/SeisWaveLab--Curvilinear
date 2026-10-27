bool verifyThemePanels3D(Workbench3D& app,SeisWave::Ui::AppRuntime& host) {
    SeisWave::Ui::validateThemePreferences();
    const auto folder=outputRoot3D()/"validation/themes/three-d";std::filesystem::create_directories(folder);
    const auto revision=app.simulation->revision();const auto step=app.simulation->currentStep();
    const auto* coordinates=app.simulation->grid().coordinates.data();const auto parameters=app.simulation->parameters();
    setLanguage3D(1);setGlassFinish3D(1);setGlassMotion3D(false);
    std::ofstream report(folder/"validation.tsv");report<<"theme\tmode\tglass_passes\tstatus\n";
    for(int theme=0;theme<themeCount3D;++theme)for(int mode=0;mode<3;++mode) {
        app.theme=theme;applyTheme3D(theme);SeisWave::Ui::setParameterDisplayMode(3,mode);
        app.parameterHost.centerNext=true;app.parameterHost.maximized=false;
        app.controlCategory=2;app.requestedTab=7;app.floatingCategory=0;app.floatingRequestedTab=1;
        const int passes=SeisWave::Ui::glassBackdropPasses();
        for(int frame=0;frame<8;++frame) {
            glfwPollEvents();ImGui_ImplOpenGL3_NewFrame();ImGui_ImplGlfw_NewFrame();ImGui::NewFrame();ImGui::PushFont(nullptr,20);
            app.toolbar();app.docking();app.controls();app.diagnosticsWindow();app.updateRange();app.spatialWindow();
            app.sliceWindow(0,"Inline X-Z");app.sliceWindow(1,"Crossline Y-Z");app.sliceWindow(2,"Depth X-Y");app.records();app.floatingControls();
            auto* floating=ImGui::FindWindowByName("###FloatingParameters3D");
            if(theme==10&&mode==2&&frame==2){app.parameterHost.savedPosition=floating->Pos;app.parameterHost.savedSize=floating->Size;app.parameterHost.maximized=true;}
            if(theme==10&&mode==2&&frame==4){if(floating->Size.x<ImGui::GetMainViewport()->WorkSize.x-15){app.error="Floating maximize failed.";return false;}app.parameterHost.maximized=false;app.parameterHost.restoreNext=true;}
            ImGui::PopFont();ImGui::Render();int w=0,h=0;glfwGetFramebufferSize(host.window(),&w,&h);
            glViewport(0,0,w,h);const auto p=palette3D().background;glClearColor(p.x,p.y,p.z,1);glClear(GL_COLOR_BUFFER_BIT);
            ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());host.renderPlatformWindows();
            if(frame==7) {
                const auto* left=ImGui::FindWindowByName(label3D("Controls"));
                if((mode>0)!=bool(floating&&floating->Active)||(mode!=1)!=bool(left&&left->Active)){app.error="Parameter host visibility mismatch.";return false;}
                if(mode>0&&(SeisWave::Ui::glassBackdropPasses()<=passes||app.floatingCategory!=0||app.controlCategory!=2)){app.error="Glass render or independent navigation failed.";return false;}
                if(mode==1||theme>=10)if(!captureFrame3D(folder/(std::string(themeKey3D(theme))+"_mode"+std::to_string(mode)+".ppm"),w,h))return false;
            }
            if(glGetError()!=GL_NO_ERROR||!SeisWave::Ui::glassBackdropError().empty()){app.error=SeisWave::Ui::glassBackdropError().empty()?"Theme frame OpenGL error.":SeisWave::Ui::glassBackdropError();return false;}
            glfwSwapBuffers(host.window());
        }
        if(app.simulation->revision()!=revision||app.simulation->currentStep()!=step||app.simulation->parameters()!=parameters||app.simulation->grid().coordinates.data()!=coordinates){app.error="Appearance changed the solver.";return false;}
        report<<themeKey3D(theme)<<'\t'<<mode<<'\t'<<SeisWave::Ui::glassBackdropPasses()-passes<<"\tPASS\n";
    }
    SeisWave::Ui::setParameterDisplayMode(3,2);
    for(int quality:{0,2,1}) {
        setGlassQuality3D(quality);const auto draws=SeisWave::Ui::glassBackdropPasses(),refreshes=SeisWave::Ui::glassBackdropRefreshes();
        for(int frame=0;frame<12;++frame){
            glfwPollEvents();ImGui_ImplOpenGL3_NewFrame();ImGui_ImplGlfw_NewFrame();ImGui::NewFrame();
            app.toolbar();app.docking();app.controls();app.spatialWindow();app.floatingControls();ImGui::Render();
            int w=0,h=0;glfwGetFramebufferSize(host.window(),&w,&h);glViewport(0,0,w,h);glClear(GL_COLOR_BUFFER_BIT);ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());host.renderPlatformWindows();
            if(glGetError()!=GL_NO_ERROR||!SeisWave::Ui::glassBackdropError().empty()){app.error="Material quality GL validation failed.";return false;}glfwSwapBuffers(host.window());
        }
        const int rendered=SeisWave::Ui::glassBackdropPasses()-draws,updated=SeisWave::Ui::glassBackdropRefreshes()-refreshes;
        if(quality==0?(rendered!=0||updated!=0):(rendered<1||updated<1||updated>=rendered)){app.error="Glass quality/cache did not reduce refresh work.";return false;}
        report<<"quality_"<<quality<<"\t2\t"<<rendered<<"\tPASS_refreshes_"<<updated<<'\n';
    }
    std::cout<<"Theme panels PASS: 12 themes x 3 modes, quality 0/1/2 and cached blur, live frosted GL passes, independent navigation, maximize/restore, unchanged solver and preference migration\n";
    return true;
}
