#pragma once
#include "AppRuntime.hpp"
#include "ParameterPanelHost.hpp"
#include <cmath>
#include <fstream>
#include <set>
#include <stdexcept>
namespace SeisWave::Ui {
inline void validateThemePreferences() {
    using namespace Seis3D::Curvilinear::Ui;
    const int original=themeIndex3D(),language=language3D(),finish=glassFinish3D();
    const int panel2=parameterDisplayMode(2),panel3=parameterDisplayMode(3);
    const float strength=glassStrength3D(),font=interfaceFontSize();const bool motion=glassMotion3D();
    const auto folder=outputRoot()/"validation/themes";std::filesystem::create_directories(folder);
    const auto file=folder/"test-preferences.cfg";std::ofstream report(folder/"catalog.tsv");
    report<<"key\tname\ttext_contrast\tmuted_contrast\n";std::set<std::string> keys;
    const auto require=[](bool okay,const char* message){if(!okay)throw std::runtime_error(message);};
    const auto luminance=[](ImVec4 c){const auto f=[](double v){return v<=.04045?v/12.92:std::pow((v+.055)/1.055,2.4);};return .2126*f(c.x)+.7152*f(c.y)+.0722*f(c.z);};
    const auto contrast=[&](ImVec4 a,ImVec4 b){const auto x=luminance(a),y=luminance(b);return (std::max(x,y)+.05)/(std::min(x,y)+.05);};
    require(themeCount3D==12,"Expected twelve themes.");
    for(int i=0;i<themeCount3D;++i) {
        require(keys.insert(themeKey3D(i)).second&&findTheme3D(themeKey3D(i))==i,"Theme keys are not unique.");
        applyTheme3D(i);const auto& p=palette3D();
        require(contrast(p.text,p.panel)>=7&&contrast(p.muted,p.panel)>=4.5,"Theme text contrast is insufficient.");
        for(int surface=0;surface<2;++surface) {
            setGlassFinish3D(surface);setGlassStrength3D(.37f);setGlassMotion3D(false);
            setParameterDisplayMode(2,i%3);setParameterDisplayMode(3,(i+1)%3);
            saveAppearancePreferences(file);applyTheme3D((i+1)%themeCount3D);setGlassFinish3D(1-surface);
            require(loadAppearancePreferences(file)&&themeIndex3D()==i&&glassFinish3D()==surface&&std::abs(glassStrength3D()-.37f)<1e-5f&&!glassMotion3D(),"Theme/material preference roundtrip failed.");
            require(parameterDisplayMode(2)==i%3&&parameterDisplayMode(3)==(i+1)%3,"Parameter display mode roundtrip failed.");
        }
        report<<themeKey3D(i)<<'\t'<<p.name<<'\t'<<contrast(p.text,p.panel)<<'\t'<<contrast(p.muted,p.panel)<<'\n';
    }
    constexpr int legacy[]{0,8,4,7,9,7,5,9,9,1};
    for(int i=0;i<10;++i){ {std::ofstream out(file);out<<"theme "<<i<<"\nfont 23\nlanguage 0\n";}
        require(loadAppearancePreferences(file)&&themeIndex3D()==legacy[i],"Old Curvilinear theme migration failed.");
        saveAppearancePreferences(file);applyTheme3D(0);require(loadAppearancePreferences(file)&&themeIndex3D()==legacy[i],"Theme migrated twice.");
    }
    {std::ofstream out(file);out<<"theme_key red-yellow-orange\ntheme_schema 2\ntheme 0\n";}
    require(loadAppearancePreferences(file)&&themeIndex3D()==11,"Stable key did not override numeric theme.");
    {std::ofstream out(file);out<<"theme_key bad\ntheme 999\nfont nan\nparameters_3d 999\n";}
    require(loadAppearancePreferences(file)&&themeIndex3D()==11,"Malformed preferences changed the theme.");
    applyTheme3D(original);setLanguage3D(language);setGlassFinish3D(finish);setGlassStrength3D(strength);setGlassMotion3D(motion);setInterfaceFontSize(font);
    setParameterDisplayMode(2,panel2);setParameterDisplayMode(3,panel3);
}
}
