#pragma once
#include "MenuWidgets.hpp"
#include "AppRuntime.hpp"
#include "ParameterPanelHost.hpp"
#include <fstream>
#include <array>
#include <span>
namespace SeisWave::Ui {
// Window-only state: these operations never touch physical parameters or fields.
struct WorkspaceTools {
    std::string focused,restoreIni,message;
    bool focusNew{},restoreRequested{};
    std::array<std::string,3> modeLayouts;
    void persistBeforeClose(const char* path) {
        if(focused.empty())return;
        const auto* utf8=reinterpret_cast<const char8_t*>(path);
        std::ofstream out(std::filesystem::path(std::u8string(utf8)),std::ios::binary);out.write(restoreIni.data(),static_cast<std::streamsize>(restoreIni.size()));
        ImGui::GetIO().IniFilename=nullptr; // Avoid persisting the temporary maximized geometry at context teardown.
    }
    void forgetFocus(){focused.clear();restoreIni.clear();restoreRequested=false;}
    bool beginFrame() {
        if(!focused.empty()&&ImGui::IsKeyPressed(ImGuiKey_Escape,false)&&!ImGui::GetIO().WantTextInput&&!ImGui::IsAnyItemActive()&&!ImGui::IsPopupOpen(nullptr,ImGuiPopupFlags_AnyPopupId|ImGuiPopupFlags_AnyPopupLevel))restoreRequested=true;
        if(!restoreRequested)return false;
        ImGui::LoadIniSettingsFromMemory(restoreIni.c_str(),restoreIni.size());focused.clear();restoreIni.clear();restoreRequested=false;return true;
    }
    bool changeMode(int previous,int current) {
        std::size_t n=0;const auto* ini=ImGui::SaveIniSettingsToMemory(&n);modeLayouts[previous].assign(ini,n);
        if(!modeLayouts[current].empty()){ImGui::LoadIniSettingsFromMemory(modeLayouts[current].c_str(),modeLayouts[current].size());return true;}
        return (previous==1)==(current==1); // Dock + floating retains the existing sidebar geometry.
    }
    ImGuiWindowFlags prepare(const char* name) {
        if(focused.empty()||ImHashStr(focused.c_str())!=ImHashStr(name))return 0;
        const auto* v=ImGui::GetMainViewport();
        ImGui::SetNextWindowDockID(0,ImGuiCond_Always);ImGui::SetNextWindowViewport(v->ID);
        ImGui::SetNextWindowPos({v->WorkPos.x+3,v->WorkPos.y+3});ImGui::SetNextWindowSize({v->WorkSize.x-6,v->WorkSize.y-6});
        ImGui::SetNextWindowBgAlpha(1);
        if(focusNew){ImGui::SetNextWindowFocus();focusNew=false;}
        return ImGuiWindowFlags_NoDocking|ImGuiWindowFlags_NoMove|ImGuiWindowFlags_NoResize|ImGuiWindowFlags_NoCollapse;
    }
    void tools(const char* name) {
        using namespace Seis3D::Curvilinear::Ui;
        const bool active=!focused.empty()&&ImHashStr(focused.c_str())==ImHashStr(name);
        if(ImGui::SmallButton(language3D()?(active?"还原视图 [Esc]":"放大视图"):(active?"Restore view [Esc]":"Maximize view"))) {
            if(active)restoreRequested=true;
            else {std::size_t n=0;const auto* ini=ImGui::SaveIniSettingsToMemory(&n);restoreIni.assign(ini,n);focused=name;focusNew=true;}
        }
    }
    bool menu(int dimension,std::span<bool> panels) {
        using namespace Seis3D::Curvilinear::Ui;bool loaded=false;
        ImGui::BeginDisabled(!focused.empty());
        for(bool save:{true,false})if(SeisWave::Ui::beginDropdownMenu(language3D()?(save?"保存自定义布局":"载入自定义布局"):(save?"Save custom layout":"Load custom layout"))) {
            for(int slot=1;slot<=3;++slot) {
                const auto folder=outputRoot()/"ui";
                const auto file=folder/(std::to_string(dimension)+"d-workspace-"+std::to_string(slot)+".ini");
                auto meta=file;meta+=".cfg";
                const std::string label=(language3D()?"工作区 ":"Workspace ")+std::to_string(slot);
                if(ImGui::MenuItem(label.c_str(),nullptr,false,save||std::filesystem::is_regular_file(file))) {
                    if(save){std::filesystem::create_directories(folder);std::size_t size=0;const auto* data=ImGui::SaveIniSettingsToMemory(&size);std::ofstream out(file,std::ios::binary);out.write(data,static_cast<std::streamsize>(size));
                        std::ofstream config(meta);config<<parameterDisplayMode(dimension)<<' ';for(bool value:panels)config<<value<<' ';message=language3D()?"工作区布局已保存。":"Workspace saved.";
                    }else{
                        std::ifstream config(meta);int mode=0;if(config>>mode)setParameterDisplayMode(dimension,mode);
                        for(auto& value:panels){int v;if(config>>v)value=v!=0;}
                        std::ifstream in(file,std::ios::binary);const std::string data{std::istreambuf_iterator<char>(in),{}};ImGui::LoadIniSettingsFromMemory(data.c_str(),data.size());
                        loaded=true;message=language3D()?"工作区布局已恢复。":"Workspace restored.";
                    }
                }
            }
            ImGui::EndMenu();
        }
        ImGui::EndDisabled();return loaded;
    }
};
}
