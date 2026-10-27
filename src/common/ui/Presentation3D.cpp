#include "Presentation3D.hpp"
#include "implot.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <string_view>
#include <unordered_map>
namespace Seis3D::Curvilinear::Ui {
namespace {
int selectedLanguage = 1, selectedTheme = 1;
int glassFinish=1;float glassStrength=.8f;bool glassMotion=true;int glassQuality=1;
// Process catalogue v2 plus MATLAB-inspired and petroleum red/yellow/orange themes.
const Theme3D themes[]{
    {"Light Sandpaper",{.945f,.925f,.880f,1},{.995f,.985f,.960f,1},{.920f,.905f,.865f,1},{.16f,.20f,.19f,1},{.32f,.37f,.33f,1},{.10f,.40f,.32f,1},{.68f,.70f,.63f,1},true,true},
    {"Warm Ivory",{.970f,.950f,.920f,1},{1,.990f,.970f,1},{.960f,.935f,.890f,1},{.22f,.18f,.15f,1},{.40f,.34f,.28f,1},{.64f,.31f,.12f,1},{.76f,.69f,.59f,1},true,true},
    {"Apricot Glow",{.985f,.925f,.860f,1},{1,.976f,.938f,1},{.980f,.930f,.860f,1},{.25f,.17f,.12f,1},{.44f,.33f,.25f,1},{.76f,.40f,.22f,1},{.83f,.68f,.54f,1},true,true},
    {"Rose Quartz",{.985f,.935f,.940f,1},{1,.983f,.983f,1},{1,.957f,.960f,1},{.26f,.13f,.17f,1},{.45f,.31f,.35f,1},{.66f,.24f,.38f,1},{.84f,.66f,.70f,1},true,true},
    {"Soft Jade",{.940f,.970f,.940f,1},{.980f,.995f,.970f,1},{.870f,.930f,.870f,1},{.13f,.23f,.17f,1},{.29f,.40f,.32f,1},{.12f,.43f,.28f,1},{.65f,.76f,.67f,1},true,true},
    {"Scientific Light",{.930f,.950f,.970f,1},{.990f,.995f,1,1},{.880f,.920f,.960f,1},{.12f,.18f,.24f,1},{.30f,.38f,.46f,1},{.035f,.36f,.64f,1},{.65f,.74f,.82f,1},true,true},
    {"Silver Mist",{.900f,.910f,.920f,1},{.980f,.985f,.990f,1},{.930f,.945f,.950f,1},{.16f,.19f,.21f,1},{.34f,.39f,.42f,1},{.36f,.43f,.48f,1},{.66f,.70f,.73f,1},true,true},
    {"Graphite",{.065f,.070f,.080f,1},{.105f,.115f,.130f,1},{.170f,.180f,.200f,1},{.93f,.94f,.96f,1},{.65f,.68f,.73f,1},{.72f,.77f,.84f,1},{.31f,.34f,.39f,1},false,true},
    {"Glacier Blue",{.890f,.945f,.985f,1},{.975f,.990f,1,1},{.960f,.980f,1,1},{.09f,.15f,.23f,1},{.28f,.36f,.45f,1},{.025f,.38f,.82f,1},{.61f,.73f,.85f,1},true,true},
    {"Abyss",{.025f,.045f,.080f,1},{.055f,.095f,.150f,1},{.090f,.150f,.220f,1},{.91f,.95f,.99f,1},{.58f,.69f,.80f,1},{.28f,.77f,.94f,1},{.25f,.43f,.59f,1},false,true},
    {"MATLAB Light",{.940f,.940f,.940f,1},{.985f,.985f,.985f,1},{1,1,1,1},{.12f,.12f,.14f,1},{.36f,.37f,.39f,1},{0,.447f,.741f,1},{.69f,.70f,.72f,1},true,true},
    {"Red Yellow Orange",{1,.950f,.865f,1},{1,.990f,.965f,1},{1,.962f,.895f,1},{.28f,.15f,.10f,1},{.44f,.30f,.21f,1},{.76f,.10f,.08f,1},{.84f,.61f,.30f,1},true,true}
};
static_assert(sizeof(themes)/sizeof(themes[0])==themeCount3D);
const char* themeKeys[]={"paper","ivory","apricot","rose","jade","scientific","mist","graphite","glacier","abyss","matlab","red-yellow-orange"};
const char* themeChinese[]={"浅色砂纸","暖象牙白","杏桃暖光","玫瑰晶","柔和浅玉","科研浅色","雾银灰","石墨灰","冰川蓝","深海曜","MATLAB 浅灰蓝","红黄橙"};
constexpr int legacyThemes[]={0,8,4,7,9,7,5,9,9,1,8,4,6,3,9};
struct ThemeAlias{const char* key;int index;};
const ThemeAlias themeAliases[]{{"arctic",8},{"cuda-green",7},{"ocean",9},{"violet",9},{"teal",9},{"glass-glacier",8},{"glass-jade",4},{"glass-violet",6},{"glass-rose",3},{"glass-abyss",9}};
struct Entry {
    const char *english;
    const char *chinese;
};
const Entry entries[]{
#include "Translations3D.inc"
};
const std::unordered_map<std::string_view, const char *> &dictionary() {
    static const auto values = [] {
        std::unordered_map<std::string_view, const char *> m;
        for (const auto &e : entries)
            m.emplace(e.english, e.chinese);
        return m;
    }();
    return values;
}
ImVec4 blend(ImVec4 a, ImVec4 b, float t) {
    return {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, 1};
}
} // namespace
const Theme3D &palette3D() {
    return themes[selectedTheme];
}
const Theme3D &themePalette3D(int index){return themes[std::clamp(index,0,themeCount3D-1)];}
const char *themeName3D(int i) {
    const int index=std::clamp(i,0,themeCount3D-1);return selectedLanguage?themeChinese[index]:themes[index].name;
}
void setLanguage3D(int language) {
    selectedLanguage = std::clamp(language, 0, 1);
}
int language3D() {
    return selectedLanguage;
}
int themeIndex3D() {
    return selectedTheme;
}
const char *tr3D(const char *source) {
    if (!source || selectedLanguage == 0)
        return source;
    const auto found = dictionary().find(source);
    return found == dictionary().end() ? source : found->second;
}
const char *label3D(const char *source) {
    if (!source || source[0] == '#')
        return source;
    static std::unordered_map<std::string, std::string> labels[2];
    auto &cache = labels[selectedLanguage];
    auto found = cache.find(source);
    if (found != cache.end())
        return found->second.c_str();
    return cache.emplace(source, std::string(tr3D(source)) + "###" + source).first->second.c_str();
}
const char *choices3D(const char *source) {
    static std::unordered_map<std::string, std::string> choices[2];
    std::string key;
    for (const char *p = source; *p; p += std::strlen(p) + 1) {
        key += p;
        key += '\n';
    }
    auto &cache = choices[selectedLanguage];
    auto found = cache.find(key);
    if (found != cache.end())
        return found->second.c_str();
    std::string result;
    for (const char *p = source; *p; p += std::strlen(p) + 1) {
        result += tr3D(p);
        result += '\0';
    }
    result += '\0';
    return cache.emplace(key, std::move(result)).first->second.c_str();
}
bool validateCatalog3D(std::string &error) {
    std::unordered_map<std::string, std::string> seen;
    for (const auto &e : entries) {
        if (!*e.english || !*e.chinese || !seen.emplace(e.english, e.chinese).second) {
            error = "Empty or duplicate translation entry";
            return false;
        }
        // Format tokens must be byte-for-byte equal, including argument order and length modifiers.
        const auto formats = [](const char *p) {
            std::string out;
            while (*p) {
                if (*p++ != '%')
                    continue;
                out += '%';
                while (*p) {
                    const char c = *p++;
                    out += c;
                    if (std::strchr("diuoxXfFeEgGaAcsp%", c))
                        break;
                }
                out += '|';
            }
            return out;
        };
        if (formats(e.english) != formats(e.chinese)) {
            error = std::string("Translation format mismatch: ") + e.english;
            return false;
        }
    }
    return true;
}
ImU32 color3D(ImVec4 c, float alpha) {
    c.w *= alpha;
    return ImGui::ColorConvertFloat4ToU32(c);
}
ImVec4 functionColor3D(FunctionRole3D role) {
    const bool light = palette3D().light;
    switch (role) {
    case FunctionRole3D::Run:
        return light ? ImVec4(.20f, .46f, .23f, 1) : ImVec4(.40f, .78f, .43f, 1);
    case FunctionRole3D::Model:
        return light ? ImVec4(.24f, .39f, .64f, 1) : ImVec4(.42f, .63f, .93f, 1);
    case FunctionRole3D::Display:
        return light ? ImVec4(.12f, .46f, .45f, 1) : ImVec4(.25f, .76f, .73f, 1);
    case FunctionRole3D::Export:
        return light ? ImVec4(.48f, .30f, .63f, 1) : ImVec4(.76f, .55f, .94f, 1);
    case FunctionRole3D::Warning:
        return light ? ImVec4(.66f, .42f, .12f, 1) : ImVec4(.95f, .70f, .28f, 1);
    case FunctionRole3D::Danger:
        return light ? ImVec4(.70f, .25f, .19f, 1) : ImVec4(.95f, .42f, .35f, 1);
    }
    return palette3D().accent;
}
void pushFunctionStyle3D(FunctionRole3D role) {
    const auto c = functionColor3D(role), base = palette3D().panel;
    ImGui::PushStyleColor(ImGuiCol_Button, blend(base, c, .19f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, blend(base, c, .33f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, blend(base, c, .48f));
    ImGui::PushStyleColor(ImGuiCol_Header, blend(base, c, .16f));
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, blend(base, c, .29f));
    ImGui::PushStyleColor(ImGuiCol_HeaderActive, blend(base, c, .42f));
    ImGui::PushStyleColor(ImGuiCol_Border, blend(base, c, .60f));
}
void popFunctionStyle3D() {
    ImGui::PopStyleColor(7);
}
bool functionButton3D(const char *text, FunctionRole3D role, ImVec2 size) {
    pushFunctionStyle3D(role);
    const bool clicked = ImGui::Button(label3D(text), size);
    popFunctionStyle3D();
    return clicked;
}
void applyGlassStyle();
void applyTheme3D(int theme) {
    if (ImPlot::GetCurrentContext())
        ImPlot::StyleColorsAuto();
    selectedTheme = std::clamp(theme, 0, themeCount3D - 1);
    if (!ImGui::GetCurrentContext())
        return;
    const auto &p = palette3D();
    if (p.light)
        ImGui::StyleColorsLight();
    else
        ImGui::StyleColorsDark();
    auto &s = ImGui::GetStyle();
    s.WindowPadding = {12, 10};
    s.FramePadding = {8, 5};
    s.ItemSpacing = {8, 7};
    s.FrameRounding = 4;
    s.TabRounding = 4;
    s.GrabRounding = 4;
    s.ChildRounding = 5;
    s.PopupRounding = 6;
    s.FrameBorderSize = 1;
    s.WindowRounding = 0;
    s.WindowBorderSize = 1;
    s.PopupBorderSize = 1;
    s.DisabledAlpha = .65f;
    auto *c = s.Colors;
    c[ImGuiCol_Text] = p.text;
    c[ImGuiCol_TextDisabled] = p.muted;
    c[ImGuiCol_WindowBg] = p.panel;
    c[ImGuiCol_ChildBg] = p.panel;
    c[ImGuiCol_PopupBg] = p.panel;
    c[ImGuiCol_Border] = p.border;
    c[ImGuiCol_FrameBg] = p.input;
    c[ImGuiCol_FrameBgHovered] = blend(p.input, p.accent, .2f);
    c[ImGuiCol_FrameBgActive] = blend(p.input, p.accent, .35f);
    c[ImGuiCol_Button] = blend(p.input, p.accent, .12f);
    c[ImGuiCol_ButtonHovered] = blend(p.input, p.accent, .3f);
    c[ImGuiCol_ButtonActive] = blend(p.input, p.accent, .5f);
    c[ImGuiCol_Header] = blend(p.panel, p.accent, .15f);
    c[ImGuiCol_HeaderHovered] = blend(p.panel, p.accent, .3f);
    c[ImGuiCol_HeaderActive] = blend(p.panel, p.accent, .45f);
    c[ImGuiCol_Tab] = p.input;
    c[ImGuiCol_TabSelected] = blend(p.panel, p.accent, .22f);
    c[ImGuiCol_TabHovered] = blend(p.panel, p.accent, .4f);
    c[ImGuiCol_TabDimmed] = p.panel;
    c[ImGuiCol_TabDimmedSelected] = p.input;
    c[ImGuiCol_TabSelectedOverline] = p.accent;
    c[ImGuiCol_TitleBg] = p.background;
    c[ImGuiCol_TitleBgActive] = p.input;
    c[ImGuiCol_MenuBarBg] = p.background;
    c[ImGuiCol_CheckMark] = p.accent;
    c[ImGuiCol_SliderGrab] = p.accent;
    c[ImGuiCol_SliderGrabActive] = blend(p.accent, p.text, .2f);
    c[ImGuiCol_PlotHistogram] = p.accent;
    c[ImGuiCol_Separator] = p.border;
    c[ImGuiCol_DockingPreview] = {p.accent.x, p.accent.y, p.accent.z, .35f};
    c[ImGuiCol_DockingEmptyBg] = p.background;
    c[ImGuiCol_ScrollbarBg] = p.background;
    c[ImGuiCol_ScrollbarGrab] = p.border;
    c[ImGuiCol_TextSelectedBg] = {p.accent.x, p.accent.y, p.accent.z, .3f};
    if(p.glass)applyGlassStyle();
}
bool glassTheme3D(){return palette3D().glass;}
int glassFinish3D(){return glassFinish;}
void setGlassFinish3D(int finish){if(finish<0||finish>1)return;glassFinish=finish;if(glassTheme3D()&&ImGui::GetCurrentContext())applyGlassStyle();}
float glassStrength3D(){return glassStrength;}
void setGlassStrength3D(float strength){if(std::isfinite(strength))glassStrength=std::clamp(strength,0.f,1.f);}
bool glassMotion3D(){return glassMotion;}
void setGlassMotion3D(bool enabled){glassMotion=enabled;}
int glassQuality3D(){return glassQuality;}
void setGlassQuality3D(int value){if(value>=0&&value<=2)glassQuality=value;}
ThemeTone3D themeTone3D(int i){i=std::clamp(i,0,themeCount3D-1);return i<4?ThemeTone3D::Warm:i<8?ThemeTone3D::Neutral:i<10?ThemeTone3D::Cool:ThemeTone3D::Special;}
const char* themeToneName3D(ThemeTone3D tone){const int i=static_cast<int>(tone);static const char* en[]={"Warm (4)","Neutral (4)","Cool (2)","Application styles (2)"};static const char* zh[]={"暖色调（4）","中性色调（4）","冷色调（2）","应用风格（2）"};return selectedLanguage?zh[i]:en[i];}
int legacyThemeIndex3D(int i){return i>=0&&i<15?legacyThemes[i]:-1;}
const char* themeKey3D(int i){return themeKeys[std::clamp(i,0,themeCount3D-1)];}
int findTheme3D(const std::string& key){for(int i=0;i<themeCount3D;++i)if(key==themeKeys[i])return i;for(const auto& alias:themeAliases)if(key==alias.key)return alias.index;return -1;}
void applyGlassStyle() {
    const auto& p=palette3D();const bool light=p.light;
    auto alpha=[](ImVec4 color,float a){color.w=a;return color;};
    auto& s=ImGui::GetStyle();auto* c=s.Colors;
    s.FrameRounding=7;s.GrabRounding=7;s.TabRounding=8;s.ChildRounding=12;s.PopupRounding=16;s.WindowRounding=14;
    // Docked scientific views stay opaque; floating controls opt into the sampled backdrop.
    c[ImGuiCol_WindowBg]=alpha(p.panel,.985f);
    c[ImGuiCol_ChildBg]=alpha(p.panel,.08f);c[ImGuiCol_PopupBg]=alpha(p.panel,.985f);
    c[ImGuiCol_Border]=alpha(p.border,light?.40f:.48f);c[ImGuiCol_BorderShadow]=alpha(p.text,light?.035f:.08f);
    c[ImGuiCol_FrameBg]=alpha(p.input,.82f);c[ImGuiCol_FrameBgHovered]=alpha(blend(p.input,p.accent,.09f),.94f);c[ImGuiCol_FrameBgActive]=alpha(blend(p.input,p.accent,.16f),1);
    c[ImGuiCol_Button]=alpha(p.input,light?.54f:.64f);c[ImGuiCol_ButtonHovered]=alpha(blend(p.input,p.accent,.13f),.94f);c[ImGuiCol_ButtonActive]=blend(p.input,p.accent,.26f);
    c[ImGuiCol_Header]=alpha(blend(p.panel,p.accent,.12f),.74f);c[ImGuiCol_HeaderHovered]=alpha(blend(p.panel,p.accent,.20f),.92f);c[ImGuiCol_HeaderActive]=blend(p.panel,p.accent,.29f);
    c[ImGuiCol_Tab]=alpha(p.panel,.25f);c[ImGuiCol_TabSelected]=alpha(p.panel,.96f);c[ImGuiCol_TabDimmed]=alpha(p.panel,.18f);c[ImGuiCol_TabDimmedSelected]=alpha(p.panel,.67f);c[ImGuiCol_TabHovered]=alpha(blend(p.panel,p.accent,.12f),.96f);
    c[ImGuiCol_TitleBg]=alpha(p.panel,.20f);c[ImGuiCol_TitleBgActive]=alpha(p.panel,.55f);
    c[ImGuiCol_DockingEmptyBg]={0,0,0,0};c[ImGuiCol_Separator]=alpha(p.border,.23f);
    c[ImGuiCol_ScrollbarBg]=alpha(p.panel,.10f);c[ImGuiCol_ScrollbarGrab]=alpha(p.border,.58f);
    if(ImPlot::GetCurrentContext()){auto& plot=ImPlot::GetStyle();plot.Colors[ImPlotCol_FrameBg]=alpha(p.panel,.94f);plot.Colors[ImPlotCol_PlotBg]=alpha(p.panel,1);plot.Colors[ImPlotCol_PlotBorder]=alpha(p.border,.60f);}
    if(selectedTheme==10){c[ImGuiCol_PlotHistogram]={.850f,.325f,.098f,1};s.FrameRounding=3;s.TabRounding=3;}
    if(selectedTheme==11){c[ImGuiCol_TitleBgActive]={.82f,.20f,.075f,.18f};c[ImGuiCol_TabSelectedOverline]={1,.62f,.08f,1};c[ImGuiCol_SliderGrab]={.94f,.42f,.06f,1};}
}
bool themeCombo3D(const char* id,int& theme,float width) {
    ImGui::SetNextItemWidth(width);bool changed=false;
    if(ImGui::BeginCombo(id,themeName3D(theme),ImGuiComboFlags_HeightLarge)) {
        for(int i=0;i<themeCount3D;++i) {
            if(i==0||themeTone3D(i)!=themeTone3D(i-1))ImGui::SeparatorText(themeToneName3D(themeTone3D(i)));
            ImGui::PushID(i);ImGui::ColorButton("##swatch",themePalette3D(i).accent,ImGuiColorEditFlags_NoTooltip,{12,12});ImGui::SameLine();
            if(ImGui::Selectable(themeName3D(i),theme==i)){theme=i;applyTheme3D(i);changed=true;}
            if(theme==i)ImGui::SetItemDefaultFocus();ImGui::PopID();
        }
        ImGui::EndCombo();
    }
    return changed;
}
void glassAppearanceControls3D() {
    ImGui::SeparatorText(language3D()?"玻璃材质":"Glass material");
    int quality=glassQuality3D();ImGui::SetNextItemWidth(240);
    if(ImGui::Combo(language3D()?"材质质量":"Material quality",&quality,language3D()?"清晰实底\0节能磨砂\0完整磨砂\0":"Opaque\0Economical glass\0Full glass\0"))setGlassQuality3D(quality);
    int finish=glassFinish3D();ImGui::SetNextItemWidth(240);
    if(ImGui::Combo(language3D()?"表面":"Finish",&finish,language3D()?"液态 · 通透\0磨砂 · 柔雾\0":"Liquid\0Frosted\0"))setGlassFinish3D(finish);
    float strength=glassStrength3D();ImGui::SetNextItemWidth(240);
    if(ImGui::SliderFloat(language3D()?"效果强度":"Effect strength",&strength,0,1,"%.2f"))setGlassStrength3D(strength);
    bool motion=glassMotion3D();if(ImGui::Checkbox(language3D()?"悬停高光":"Hover highlight",&motion))setGlassMotion3D(motion);
    ImGui::TextWrapped("%s",language3D()?"悬浮面板与菜单模糊后方界面；波场和图表保持清晰。":"Floating controls and menus blur their backdrop; scientific plots stay sharp.");
}
} // namespace Seis3D::Curvilinear::Ui
