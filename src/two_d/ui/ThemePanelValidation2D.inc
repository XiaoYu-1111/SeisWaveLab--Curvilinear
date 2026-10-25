int validateThemePanels2D() {
    SeisWave::Ui::AppRuntime runtime;std::string error;
    if(!runtime.open("2D theme / floating panel validation",true,error))throw std::runtime_error(error);
    SeisWave::Ui::validateThemePreferences();
    Workbench app;app.staged.nx=app.staged.nz=80;app.staged.spacing=5;app.staged.frequency=10;
    app.staged.relief=5;app.staged.sourceZ=20;app.staged.maxSteps=20;fitAcquisition(app.staged);
    if(!app.apply()||!app.simulation.advance(20,app.error))throw std::runtime_error(app.error);
    const auto parameters=app.simulation.parameters();const auto step=app.simulation.step();const auto dt=app.simulation.dt();
    const auto* coordinates=app.simulation.grid().coordinates.data();
    const auto fingerprint=[&]{double total=0;for(const auto& r:app.simulation.receivers())for(double v:r.vz)total+=v;return total;};
    const auto raw=fingerprint();const auto folder=SeisWave::Ui::outputRoot()/"validation/themes/two-d";
    std::filesystem::create_directories(folder);std::ofstream report(folder/"validation.tsv");report<<"theme\tmode\tstatus\n";
    setLanguage3D(1);setGlassFinish3D(1);setGlassMotion3D(false);
    for(int theme=0;theme<themeCount3D;++theme)for(int mode=0;mode<3;++mode) {
        applyTheme3D(theme);SeisWave::Ui::setParameterDisplayMode(2,mode);app.parameterHost.centerNext=true;
        app.category=2;app.floatingCategory=0;const auto passes=SeisWave::Ui::glassBackdropPasses();
        for(int frame=0;frame<8;++frame) {
            runtime.begin();app.toolbar();app.controls();app.activeParameters();app.wavefield();app.records();app.floatingControls();runtime.present();
            if(glGetError()!=GL_NO_ERROR||!SeisWave::Ui::glassBackdropError().empty())throw std::runtime_error("2D glass frame failed: "+SeisWave::Ui::glassBackdropError());
            if(frame==7) {
                const auto* left=ImGui::FindWindowByName(label("Parameters and run","参数与运行").c_str());
                const auto* floating=ImGui::FindWindowByName(label("Floating parameters 2D","悬浮参数 · 二维").c_str());
                if((mode>0)!=bool(floating&&floating->Active)||(mode!=1)!=bool(left&&left->Active))throw std::runtime_error("2D parameter host visibility mismatch.");
                if(mode>0&&(app.category!=2||app.floatingCategory!=0||SeisWave::Ui::glassBackdropPasses()<=passes))throw std::runtime_error("2D independent navigation or glass passes failed.");
                if(mode==1){int w=0,h=0;glfwGetFramebufferSize(runtime.window(),&w,&h);if(!SeisWave::Ui::capture(folder/(std::string(themeKey3D(theme))+".ppm"),w,h))return 4;}
            }
        }
        if(app.simulation.parameters()!=parameters||app.simulation.step()!=step||app.simulation.dt()!=dt||app.simulation.grid().coordinates.data()!=coordinates||fingerprint()!=raw||!app.error.empty())throw std::runtime_error("2D appearance changed the solver or raw samples.");
        report<<themeKey3D(theme)<<'\t'<<mode<<"\tPASS\n";
    }
    std::cout<<"2D theme panels PASS: 12 themes x 3 modes, independent navigation, real glass rendering, original solver / raw samples retained\n";
    return 0;
}
