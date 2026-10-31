#include "AcousticWorkbench2D.hpp"
#include "AppRuntime.hpp"
#include "HomeScreen.hpp"
#include "ImagingWorkbench2D.hpp"
#include "ModelTransfer2D.hpp"
#include "Presentation3D.hpp"
#include "Workbench2D.hpp"
#include "Workbench3D.hpp"
#include <fstream>
#include <iostream>
#include <string>
int main(int argc, char **argv) {
    try {
        using namespace SeisWave::Ui;
        using namespace Seis3D::Curvilinear::Ui;
        loadAppearancePreferences();
        const std::string mode = argc > 1 ? argv[1] : "";
        struct SaveAppearance {
            bool enabled;
            ~SaveAppearance() {
                if (enabled)
                    try {
                        saveAppearancePreferences();
                    } catch (...) {
                    }
            }
        } appearance{mode.empty() || mode == "--2d" || mode == "--3d" || mode == "--imaging-2d" ||
                     mode == "--acoustic-2d" || mode == "--viscoelastic-2d"};
        if (mode == "--smoke-acoustic-2d")
            return Seis2D::Acoustic::Ui::runWorkbench(true);
        if (mode == "--smoke-acoustic-capacity")
            return Seis2D::Acoustic::Ui::validateCapacityWorkflow();
        if (mode == "--smoke-imaging-2d")
            return Seis2D::Imaging::Ui::runWorkbench(true);
        if (mode == "--smoke-home") {
            runHomeScreen(true);
            return 0;
        }
        if (mode == "--smoke-window-state") {
            validateWindowState();
            std::cout
                << "Window routing PASS: normal size / position, maximized state and restore rectangle\n";
            return 0;
        }
        if (mode == "--capture-2d")
            return Seis2D::Curvilinear::Ui::captureWorkflow2D();
        if (mode == "--smoke-2d")
            return Seis2D::Curvilinear::Ui::runWorkbench2D(true);
        if (mode == "--smoke-2d-themes")
            return Seis2D::Curvilinear::Ui::validateThemePanels2D();
        if (mode == "--smoke-2d-model")
            return Seis2D::Curvilinear::Ui::validateModelWorkflow2D();
        if (mode == "--smoke-2d-pml")
            return Seis2D::Curvilinear::Ui::validateCfsPmlWorkflow2D();
        if (mode == "--smoke-2d-precision")
            return Seis2D::Curvilinear::Ui::validatePrecisionWorkflow2D();
        if (mode == "--smoke-2d-q")
            return Seis2D::Curvilinear::Ui::validateAttenuationWorkflow2D();
        if (mode == "--smoke-router") {
            if (runHomeScreen(true, 1) != Route::TwoD ||
                Seis2D::Curvilinear::Ui::runWorkbench2D(true, true) != returnHome)
                throw std::runtime_error("2D workflow route failed");
            if (runHomeScreen(true, 2) != Route::ThreeD)
                throw std::runtime_error("3D entry card failed");
            char flag[] = "--smoke-route-3d";
            char *args[]{argv[0], flag};
            if (runWorkbench3D(2, args) != returnHome)
                throw std::runtime_error("3D workflow Home route failed");
            if (runHomeScreen(true, 3) != Route::Imaging ||
                Seis2D::Curvilinear::Ui::runWorkbench2D(true, true, true) != routeRtm2D)
                throw std::runtime_error("2D imaging entry route failed");
            std::cout << "Router PASS: Home -> 2D -> Home -> 3D -> Home\n";
            std::cout << "Router PASS: grouped acoustic navigation; normal navigation does not copy models\n";
            if (runHomeScreen(true, 4) != Route::Acoustic)
                throw std::runtime_error("Acoustic home entry failed");
            std::cout << "Router PASS: Home -> Acoustic forward\n";
            return 0;
        }
        if (!mode.empty() && mode != "--2d" && mode != "--3d" && mode != "--imaging-2d" &&
            mode != "--acoustic-2d" && mode != "--viscoelastic-2d")
            return runWorkbench3D(argc, argv);
        Route route = mode == "--2d"                ? Route::TwoD
                      : mode == "--3d"              ? Route::ThreeD
                      : mode == "--imaging-2d"      ? Route::Imaging
                      : mode == "--acoustic-2d"     ? Route::Acoustic
                      : mode == "--viscoelastic-2d" ? Route::Viscoelastic
                                                    : Route::Home;
        while (route != Route::Quit) {
            if (route == Route::Home) {
                route = runHomeScreen();
                continue;
            }
            const int result = route == Route::Acoustic  ? Seis2D::Acoustic::Ui::runWorkbench()
                               : route == Route::Imaging ? Seis2D::Imaging::Ui::runWorkbench()
                               : route == Route::Viscoelastic
                                   ? Seis2D::Curvilinear::Ui::runWorkbench2D(false, false, false, true)
                               : route == Route::TwoD ? Seis2D::Curvilinear::Ui::runWorkbench2D()
                                                      : runWorkbench3D(argc, argv);
            if (result == routeRtm2D) {
                route = Route::Imaging;
                continue;
            }
            if (result == routeElastic2D) {
                route = Route::TwoD;
                continue;
            }
            if (result == routeAcoustic2D) {
                route = Route::Acoustic;
                continue;
            }
            if (result == routeViscoelastic2D) {
                route = Route::Viscoelastic;
                continue;
            }
            if (result != returnHome)
                return result;
            route = Route::Home;
        }
        return 0;
    } catch (const std::exception &ex) {
        std::cerr << ex.what() << '\n';
        return 1;
    }
}
