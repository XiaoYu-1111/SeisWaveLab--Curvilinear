// Exercises real GLFW windows, the shared scene texture and the live solver.
bool verifyViewports3D(Workbench3D& app,SeisWave::Ui::AppRuntime& host) {
    const auto folder=outputRoot3D()/"validation/multi-viewport";
    std::filesystem::create_directories(folder);
    auto* main=host.window();int monitorCount=0;auto** monitors=glfwGetMonitors(&monitorCount);
    if(monitorCount<1){app.error="No desktop monitor for viewport validation.";return false;}
    int mx=0,my=0,mw=0,mh=0;
    glfwGetMonitorWorkarea(monitors[0],&mx,&my,&mw,&mh);
    glfwSetWindowPos(main,mx,my);glfwSetWindowSize(main,960,700);
    // Prefer a real secondary monitor; a single monitor still exercises native windows.
    glfwGetMonitorWorkarea(monitors[monitorCount>1?1:0],&mx,&my,&mw,&mh);
    const ImVec2 origin{static_cast<float>(mx+mw-620),static_cast<float>(my+20)};
    const auto* solver=app.simulation.get();const auto* coordinates=solver->grid().coordinates.data();
    const int initialStep=solver->currentStep();const auto initialRevision=solver->revision();
    std::array<ImGuiID,7> targets{};int peak=0;
    setLanguage3D(0);app.visuals.colorTopography=true;
    const auto pane=[&](int i){return ImGui::FindWindowByName(label3D(Workbench3D::panelNames[i]));};
    const auto fail=[&](const char* message){app.error=message;glfwMakeContextCurrent(main);return false;};
    for(int frame=0;frame<38;++frame) {
        glfwPollEvents();
        if(frame>=8&&frame<18&&!app.simulation->advance(1,app.error))return false;
        if(frame==11) {
            auto* native=static_cast<GLFWwindow*>(pane(6)->Viewport->PlatformHandle);
            glfwSetWindowSize(native,650,430);glfwSetWindowPos(native,static_cast<int>(origin.x+25),static_cast<int>(origin.y+40));
        }
        if(frame==15)glfwSetWindowSize(main,1600,900);
        ImGui_ImplOpenGL3_NewFrame();ImGui_ImplGlfw_NewFrame();ImGui::NewFrame();ImGui::PushFont(nullptr,20);
        if(frame==3)for(int i=0;i<7;++i) {
            auto* w=pane(i);if(!w)return fail("Dock panel missing before detachment.");
            targets[i]=w->DockId;ImGui::SetWindowDock(w,0,ImGuiCond_Always);
            ImGui::SetWindowPos(w,{origin.x,origin.y+8*i},ImGuiCond_Always);
            ImGui::SetWindowSize(w,{600,500},ImGuiCond_Always);
        }
        // Native close is surfaced through PlatformRequestClose by the GLFW callback.
        if(frame==22)pane(6)->Viewport->PlatformRequestClose=true;
        if(frame==25)app.panels[6]=true;
        if(frame==29) {
            // Empty split nodes are pruned while all panels are detached; dock onto the surviving host.
            const auto dock=ImGui::FindWindowByName(label3D("Workspace##Host"))->GetID("CurvilinearDockspace3D.v5");
            targets.fill(dock);for(int i=0;i<7;++i)ImGui::SetWindowDock(pane(i),dock,ImGuiCond_Always);
        }
        app.toolbar();app.docking();app.controls();app.diagnosticsWindow();app.updateRange();
        app.spatialWindow();app.sliceWindow(0,"Inline X-Z");app.sliceWindow(1,"Crossline Y-Z");app.sliceWindow(2,"Depth X-Y");app.records();
        ImGui::PopFont();ImGui::Render();int width=0,height=0;glfwGetFramebufferSize(main,&width,&height);
        glViewport(0,0,width,height);glClearColor(.06f,.08f,.1f,1);glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());host.renderPlatformWindows();
        if(glfwGetCurrentContext()!=main)return fail("Platform rendering did not restore the main context.");
        peak=std::max(peak,ImGui::GetPlatformIO().Viewports.Size-1);
        if(frame==9||frame==19)for(int i=0;i<7;++i) {
            auto* w=pane(i);auto* vp=w->Viewport;
            if(w->DockId||vp==ImGui::GetMainViewport()||!vp->PlatformHandle||!vp->DrawData||vp->DrawData->TotalVtxCount==0)
                return fail("Detached panel has no native renderable viewport.");
            for(int j=0;j<i;++j)if(pane(j)->Viewport==vp)return fail("Independent panels share a platform window.");
            glfwMakeContextCurrent(static_cast<GLFWwindow*>(vp->PlatformHandle));
            if(i==2&&!glIsTexture(app.renderer.texture3D()))return fail("Detached 3D window cannot access the shared scene texture.");
            if(glGetError()!=GL_NO_ERROR)return fail("OpenGL error in detached viewport.");
            glfwMakeContextCurrent(main);
        }
        if(frame==19) {
            if(std::abs(pane(6)->Size.x-650)>=2||std::abs(pane(6)->Size.y-430)>=2)return fail("Native viewport resize did not reach the panel.");
            if(app.simulation.get()!=solver||solver->grid().coordinates.data()!=coordinates||solver->currentStep()!=initialStep+10||solver->revision()!=initialRevision+10||app.displayFrame.revision!=solver->revision())
                return fail("Detached views lost the shared live simulation/frame.");
            for(const auto& r:solver->receivers())if(r.samples.size()!=static_cast<std::size_t>(solver->currentStep()+1))return fail("Detached records lost current receiver samples.");
            for(int i=0;i<7;++i) {
                auto* native=static_cast<GLFWwindow*>(pane(i)->Viewport->PlatformHandle);glfwMakeContextCurrent(native);
                int w=0,h=0;glfwGetFramebufferSize(native,&w,&h);
                if(!SeisWave::Ui::capture(folder/("detached_"+std::to_string(i)+".ppm"),w,h))return fail("Could not capture detached native framebuffer.");
                glfwMakeContextCurrent(main);
            }
            std::size_t size=0;const auto* settings=ImGui::SaveIniSettingsToMemory(&size);
            std::ofstream layout(folder/"detached-layout.ini",std::ios::binary);layout.write(settings,static_cast<std::streamsize>(size));
        }
        if(frame==24&&app.panels[6])return fail("Closing detached receiver window did not close its panel.");
        if(frame==35) {
            for(int i=0;i<7;++i)if(pane(i)->DockId!=targets[i]||pane(i)->Viewport!=ImGui::GetMainViewport()) {
                std::cerr<<"Redock panel "<<i<<" dock="<<pane(i)->DockId<<" expected="<<targets[i]<<" viewport="<<pane(i)->Viewport->ID<<" main="<<ImGui::GetMainViewport()->ID<<'\n';
                return fail("Panel failed to dock back into the main workspace.");
            }
            if(ImGui::GetPlatformIO().Viewports.Size!=1)return fail("Native windows leaked after redocking.");
        }
        if(glGetError()!=GL_NO_ERROR||!app.error.empty())return fail("Viewport frame reported a render error.");
        glfwSwapBuffers(main);
    }
    std::ofstream report(folder/"result.json");
    report<<"{\n  \"desktop_monitors\": "<<monitorCount<<",\n  \"detached_native_windows\": "<<peak
          <<",\n  \"native_resize\": true,\n  \"shared_scene_texture\": true,\n  \"live_solver_steps\": 10,\n  \"close_reopen\": true,\n  \"redock_and_cleanup\": true,\n  \"main_context_restored\": true\n}\n";
    std::cout<<"Multi-viewport PASS: "<<peak<<" native windows, "<<monitorCount<<" desktop monitors, shared 3D texture, 10 live steps, native resize, close/reopen and redock\n";
    return true;
}
