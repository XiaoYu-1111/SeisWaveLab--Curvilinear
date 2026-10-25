#pragma once
#include "imgui.h"
#include <string>
namespace Seis3D::Curvilinear::Ui {
struct Theme3D {
    const char *name;
    ImVec4 background, panel, input, text, muted, accent, border;
    bool light;
    bool glass{};
};
constexpr int themeCount3D = 12;
enum class ThemeTone3D { Warm, Neutral, Cool, Special };
ThemeTone3D themeTone3D(int);
const char* themeToneName3D(ThemeTone3D);
int legacyThemeIndex3D(int);
const char* themeKey3D(int);
int findTheme3D(const std::string&);
enum class FunctionRole3D {
    Run,
    Model,
    Display,
    Export,
    Warning,
    Danger
};
ImVec4 functionColor3D(FunctionRole3D);
void pushFunctionStyle3D(FunctionRole3D);
void popFunctionStyle3D();
bool functionButton3D(const char *, FunctionRole3D, ImVec2 size = {0, 0});
const Theme3D &palette3D();
const Theme3D &themePalette3D(int);
const char *themeName3D(int);
void applyTheme3D(int);
bool glassTheme3D();
// Every palette uses the shared glass style; finish is independent of colour.
int glassFinish3D(); // 0 liquid, 1 frosted
void setGlassFinish3D(int);
float glassStrength3D();
void setGlassStrength3D(float);
bool glassMotion3D();
void setGlassMotion3D(bool);
void setLanguage3D(int); // 0 English, 1 Simplified Chinese; labels retain their original ImGui IDs.
int language3D();
int themeIndex3D();
const char *tr3D(const char *);
const char *label3D(const char *);
const char *choices3D(const char *);
bool validateCatalog3D(std::string &);
void glassAppearanceControls3D();
bool themeCombo3D(const char* id,int& theme,float width);
ImU32 color3D(ImVec4, float alpha = 1);
} // namespace Seis3D::Curvilinear::Ui
