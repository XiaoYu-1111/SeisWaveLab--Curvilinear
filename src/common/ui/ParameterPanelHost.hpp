#pragma once
#include "GlassMaterial.hpp"
#include "Presentation3D.hpp"
#include "imgui_internal.h"
#include <algorithm>
namespace SeisWave::Ui {
int parameterDisplayMode(int dimension);
void setParameterDisplayMode(int dimension,int mode);
// The content is reused by both hosts; only navigation and window geometry are separate.
struct ParameterPanelHost {
    int dimension{3};bool centerNext{true},maximized{},restoreNext{};
    ImVec2 savedPosition{},savedSize{};
    int mode()const{return parameterDisplayMode(dimension);}
    bool menu() {
        using namespace Seis3D::Curvilinear::Ui;
        const char* en[]{"Left dock","Centered floating","Dock + floating"};
        const char* zh[]{"左侧停靠","居中悬浮","停靠 + 悬浮并存"};
        bool changed=false;
        for(int i=0;i<3;++i)if(ImGui::MenuItem(language3D()?zh[i]:en[i],nullptr,mode()==i)){
            setParameterDisplayMode(dimension,i);centerNext=true;maximized=false;changed=true;
        }
        return changed;
    }
    void prepare(const char* title) {
        using namespace Seis3D::Curvilinear::Ui;
        const auto* vp=ImGui::GetMainViewport();
        if(centerNext) {
            ImGui::SetNextWindowViewport(vp->ID);ImGui::SetNextWindowPos(vp->GetWorkCenter(),ImGuiCond_Always,{.5f,.5f});
            ImGui::SetNextWindowSize({std::min(720.f,vp->WorkSize.x*.88f),std::min(850.f,vp->WorkSize.y*.86f)},ImGuiCond_Always);
            ImGui::SetNextWindowFocus();centerNext=false;
        }
        if(maximized) {
            ImGui::SetNextWindowViewport(vp->ID);ImGui::SetNextWindowPos({vp->WorkPos.x+6,vp->WorkPos.y+6});
            ImGui::SetNextWindowSize({vp->WorkSize.x-12,vp->WorkSize.y-12});
        }else if(restoreNext) {
            ImGui::SetNextWindowPos(savedPosition);ImGui::SetNextWindowSize(savedSize);restoreNext=false;
        }
        const auto* previous=ImGui::FindWindowByName(title);
        ImGui::SetNextWindowBgAlpha(glassQuality3D()>0&&(!previous||previous->Viewport==vp)&&glassBackdropError().empty()?.08f:.985f);
        ImGui::SetNextWindowSizeConstraints({360,std::min(vp->WorkSize.y-12,std::max(420.f,ImGui::GetFontSize()*18+80))},{FLT_MAX,FLT_MAX});
    }
    void tools() {
        using namespace Seis3D::Curvilinear::Ui;
        markGlassPanel();
        if(ImGui::SmallButton(language3D()?(maximized?"还原大小":"最大化"):(maximized?"Restore size":"Maximize"))) {
            if(!maximized){savedPosition=ImGui::GetWindowPos();savedSize=ImGui::GetWindowSize();}
            else restoreNext=true;maximized=!maximized;
        }
        ImGui::SameLine();if(ImGui::SmallButton(language3D()?"居中":"Center")){centerNext=true;maximized=false;}
        ImGui::SameLine();if(ImGui::SmallButton(language3D()?"返回左侧":"Return to dock"))setParameterDisplayMode(dimension,0);
        ImGui::Separator();
    }
    ImGuiWindowFlags flags()const {
        return ImGuiWindowFlags_NoDocking|ImGuiWindowFlags_NoSavedSettings|(maximized?(ImGuiWindowFlags_NoMove|ImGuiWindowFlags_NoResize):0);
    }
};
}
