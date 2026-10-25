#include "AppRuntime.hpp"
#include "Presentation3D.hpp"
#include "GlassMaterial.hpp"
#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#include "implot.h"
#include <GL/glew.h>
#include <GLFW/glfw3.h>
#include <algorithm>
#include <cmath>
#include <fstream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#define GLFW_EXPOSE_NATIVE_WIN32
#define GLFW_NATIVE_INCLUDE_NONE
#include <GLFW/glfw3native.h>
#endif
namespace SeisWave::Ui {
namespace {
float interfaceFont = 20;
int parameterModes[2]{};
struct WindowState {
    int width{1600}, height{1000}, x{}, y{};
    bool saved{}, maximized{};
} windowState;
void (*platformRenderer)(ImGuiViewport*,void*){};
void renderThemedViewport(ImGuiViewport* viewport,void* argument) {
    const auto background=Seis3D::Curvilinear::Ui::palette3D().background;
    glClearColor(background.x,background.y,background.z,1);glClear(GL_COLOR_BUFFER_BIT);
    const auto flags=viewport->Flags;viewport->Flags|=ImGuiViewportFlags_NoRendererClear;
    platformRenderer(viewport,argument);viewport->Flags=flags;
}
} // namespace
int parameterDisplayMode(int dimension){return parameterModes[dimension==2?0:1];}
void setParameterDisplayMode(int dimension,int mode){if(mode>=0&&mode<=2)parameterModes[dimension==2?0:1]=mode;}
float interfaceFontSize() {
    return interfaceFont;
}
void saveAppearancePreferences(const std::filesystem::path& destination) {
    const auto file = destination.empty()?outputRoot()/"ui/preferences.cfg":destination;
    const auto folder = file.parent_path();
    std::error_code ec;
    if(!folder.empty())std::filesystem::create_directories(folder, ec);
    if (ec)
        return;
    std::map<std::string, std::string> values;
    std::ifstream in(file);
    std::string key, value;
    while (in >> key >> value)
        values[key] = value;
    in.close();
    values["font"] = std::to_string(interfaceFontSize());
    values["language"] = std::to_string(Seis3D::Curvilinear::Ui::language3D());
    values["theme"] = std::to_string(Seis3D::Curvilinear::Ui::themeIndex3D());
    values["theme_key"] = Seis3D::Curvilinear::Ui::themeKey3D(Seis3D::Curvilinear::Ui::themeIndex3D());
    values["theme_schema"] = "2";
    values["parameters_2d"] = std::to_string(parameterModes[0]);
    values["parameters_3d"] = std::to_string(parameterModes[1]);
    values["glass_finish"] = std::to_string(Seis3D::Curvilinear::Ui::glassFinish3D());
    values["glass_strength"] = std::to_string(Seis3D::Curvilinear::Ui::glassStrength3D());
    values["glass_motion"] = std::to_string(Seis3D::Curvilinear::Ui::glassMotion3D()?1:0);
    std::ofstream out(file);
    for (const auto &[name, setting] : values)
        out << name << ' ' << setting << '\n';
}
bool loadAppearancePreferences(const std::filesystem::path& source) {
    std::ifstream in(source.empty()?outputRoot()/"ui/preferences.cfg":source);if(!in)return false;
    std::string line;std::map<std::string,std::string> settings;
    while(std::getline(in,line)) {
        std::istringstream row(line);std::string key,value;if(row>>key>>value)settings[key]=value;
    }
    using namespace Seis3D::Curvilinear::Ui;
    const auto number=[&](const char* key,double& value){auto it=settings.find(key);if(it==settings.end())return false;std::istringstream row(it->second);return bool(row>>value)&&std::isfinite(value)&&row.peek()==std::char_traits<char>::eof();};
    int resolved=-1;if(const auto it=settings.find("theme_key");it!=settings.end())resolved=findTheme3D(it->second);
    double schema{},index{};
    if(resolved<0&&number("theme",index)&&index==std::floor(index)&&index>=0&&index<15) {
        if(number("theme_schema",schema)&&schema==2) {if(index<themeCount3D)resolved=static_cast<int>(index);}
        else resolved=legacyThemeIndex3D(static_cast<int>(index));
    }
    if(resolved>=0)applyTheme3D(resolved);
    for(const auto& [key,text]:settings) {
        double value{};if(!number(key.c_str(),value))continue;(void)text;
        if((key=="parameters_2d"||key=="parameters_3d")&&value==std::floor(value)&&value>=0&&value<=2)setParameterDisplayMode(key=="parameters_2d"?2:3,static_cast<int>(value));
        if(key=="language"&&(value==0||value==1))setLanguage3D(static_cast<int>(value));
        if(key=="font")setInterfaceFontSize(static_cast<float>(value));
        if(key=="glass_finish"&&(value==0||value==1))setGlassFinish3D(static_cast<int>(value));
        if(key=="glass_strength"&&value>=0&&value<=1)setGlassStrength3D(static_cast<float>(value));
        if(key=="glass_motion"&&(value==0||value==1))setGlassMotion3D(value!=0);
    }
    return true;
}
void setInterfaceFontSize(float size) {
    if (std::isfinite(size))
        interfaceFont = std::clamp(size, 16.f, 28.f);
}
std::filesystem::path outputRoot() {
    // A portable marker wins even when the package is being tested inside out.
    // Build-tree programs keep the workspace out; moved programs use local userdata.
    static const auto root=[] {
        std::filesystem::path executable;
#ifdef _WIN32
        std::vector<wchar_t> name(32768);
        const auto length=GetModuleFileNameW(nullptr,name.data(),static_cast<DWORD>(name.size()));
        if(length&&length<name.size())executable=std::filesystem::path(name.data());
#else
        std::error_code error;executable=std::filesystem::read_symlink("/proc/self/exe",error);
#endif
        if(!executable.empty()) {
            const auto folder=executable.parent_path();std::error_code error;
            if(std::filesystem::is_regular_file(folder/"portable.flag",error))return folder/"userdata";
            for(auto at=folder;!at.empty()&&at!=at.parent_path();at=at.parent_path())
                if(at.filename()=="bin"&&at.parent_path().filename()=="out")return at.parent_path();
            return folder/"userdata";
        }
        return std::filesystem::path(SEIS3D_OUTPUT_ROOT);
    }();
    return root;
}
std::filesystem::path elevationExampleRoot() {
#ifdef _WIN32
    std::vector<wchar_t> name(32768);
    const auto length = GetModuleFileNameW(nullptr, name.data(), static_cast<DWORD>(name.size()));
    if (length && length < name.size())
        return std::filesystem::path(name.data()).parent_path() / "examples/elevation";
#else
    std::error_code error;
    const auto executable = std::filesystem::read_symlink("/proc/self/exe", error);
    if (!error)
        return executable.parent_path() / "examples/elevation";
#endif
    return std::filesystem::current_path() / "examples/elevation";
}
bool AppRuntime::open(const char *title, bool hidden, std::string &error, bool multiViewport) {
    if (!glfwInit()) {
        error = "GLFW initialization failed.";
        return false;
    }
    glfw_ = true;
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    glfwWindowHint(GLFW_SAMPLES, 4);
    const bool maximize = windowState.saved ? windowState.maximized : !hidden;
    glfwWindowHint(GLFW_MAXIMIZED, maximize ? GLFW_TRUE : GLFW_FALSE);
    window_ = glfwCreateWindow(windowState.width, windowState.height, title, nullptr, nullptr);
    if (!window_) {
        error = "OpenGL 4.3 window creation failed.";
        return false;
    }
    glfwSetWindowSizeLimits(window_, 960, 640, GLFW_DONT_CARE, GLFW_DONT_CARE);
    if (windowState.saved && !windowState.maximized)
        glfwSetWindowPos(window_, windowState.x, windowState.y);
#ifdef _WIN32
    if (windowState.saved) {
        // rcNormalPosition survives maximization, unlike glfwGetWindowSize/Pos.
        WINDOWPLACEMENT placement{sizeof(WINDOWPLACEMENT)};
        if (GetWindowPlacement(glfwGetWin32Window(window_), &placement)) {
            placement.rcNormalPosition = {windowState.x, windowState.y, windowState.x + windowState.width,
                                          windowState.y + windowState.height};
            placement.showCmd = SW_HIDE;
            SetWindowPlacement(glfwGetWin32Window(window_), &placement);
        }
    }
#endif
    glfwMakeContextCurrent(window_);
    glfwSwapInterval(hidden ? 0 : 1);
    glewExperimental = GL_TRUE;
    if (glewInit() != GLEW_OK) {
        error = "GLEW initialization failed.";
        return false;
    }
    while (glGetError() != GL_NO_ERROR) {
    }
#ifdef _WIN32
    for (int size : {ICON_BIG, ICON_SMALL}) {
        const auto icon = static_cast<HICON>(
            LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(101), IMAGE_ICON,
                       GetSystemMetrics(size == ICON_BIG ? SM_CXICON : SM_CXSMICON),
                       GetSystemMetrics(size == ICON_BIG ? SM_CYICON : SM_CYSMICON), LR_SHARED));
        if (icon)
            SendMessageW(glfwGetWin32Window(window_), WM_SETICON, size, reinterpret_cast<LPARAM>(icon));
    }
#endif
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    imgui_ = true;
    ImPlot::CreateContext();
    implot_ = true;
    auto &io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    if (multiViewport) {
        io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;
        io.ConfigViewportsNoTaskBarIcon = false;
        io.ConfigViewportsNoDefaultParent = true;
    }
    io.IniFilename = nullptr;
    bool loaded = false;
#ifdef _WIN32
    std::vector<wchar_t> directory(32768);
    if (GetWindowsDirectoryW(directory.data(), static_cast<UINT>(directory.size()))) {
        const auto font = std::filesystem::path(directory.data()) / "Fonts/msyh.ttc";
        const auto name = font.u8string();
        if (std::filesystem::exists(font))
            loaded =
                io.Fonts->AddFontFromFileTTF(std::string(name.begin(), name.end()).c_str(), 20) != nullptr;
    }
#endif
    if (!loaded) {
        ImFontConfig config;
        config.SizePixels = 20;
        io.Fonts->AddFontDefault(&config);
    }
    Seis3D::Curvilinear::Ui::applyTheme3D(Seis3D::Curvilinear::Ui::themeIndex3D());
    glfwBackend_ = ImGui_ImplGlfw_InitForOpenGL(window_, true);
    glBackend_ = ImGui_ImplOpenGL3_Init("#version 430");
    if (!glfwBackend_ || !glBackend_) {
        error = "ImGui backend initialization failed.";
        return false;
    }
    if (!hidden) {
        glfwShowWindow(window_);
        if (maximize)
            glfwMaximizeWindow(window_);
    }
    platformRenderer=ImGui::GetPlatformIO().Renderer_RenderWindow;
    ImGui::GetPlatformIO().Renderer_RenderWindow=renderThemedViewport;
    installGlassPresentation();
    error.clear();
    return true;
}
AppRuntime::~AppRuntime() {
    // Platform rendering uses other shared contexts; GL owners are released on the main one.
    if (window_)
        glfwMakeContextCurrent(window_);
    if (window_) {
        windowState.maximized = glfwGetWindowAttrib(window_, GLFW_MAXIMIZED) == GLFW_TRUE;
#ifdef _WIN32
        WINDOWPLACEMENT placement{sizeof(WINDOWPLACEMENT)};
        if (GetWindowPlacement(glfwGetWin32Window(window_), &placement)) {
            const auto r = placement.rcNormalPosition;
            windowState.x = r.left;
            windowState.y = r.top;
            windowState.width = std::max(960L, r.right - r.left);
            windowState.height = std::max(640L, r.bottom - r.top);
            windowState.maximized |=
                placement.showCmd == SW_SHOWMAXIMIZED ||
                ((placement.showCmd == SW_SHOWMINIMIZED || placement.showCmd == SW_MINIMIZE ||
                  placement.showCmd == SW_SHOWMINNOACTIVE) &&
                 (placement.flags & WPF_RESTORETOMAXIMIZED) != 0);
            windowState.saved = true;
        }
#else
        if (!windowState.maximized && !glfwGetWindowAttrib(window_, GLFW_ICONIFIED)) {
            glfwGetWindowPos(window_, &windowState.x, &windowState.y);
            glfwGetWindowSize(window_, &windowState.width, &windowState.height);
            windowState.saved = true;
        }
#endif
    }
    if (glBackend_)
        ImGui_ImplOpenGL3_Shutdown();
    if (glfwBackend_)
        ImGui_ImplGlfw_Shutdown();
    if (implot_)
        ImPlot::DestroyContext();
    if (imgui_)
        ImGui::DestroyContext();
    if (window_)
        glfwDestroyWindow(window_);
    if (glfw_)
        glfwTerminate();
}
void validateWindowState() {
    std::string error;
    {
        AppRuntime first;
        if (!first.open("Window state test", true, error))
            throw std::runtime_error(error);
        glfwSetWindowSize(first.window(), 1280, 820);
        glfwSetWindowPos(first.window(), 120, 90);
        glfwPollEvents();
    }
    const auto normal = windowState;
    {
        AppRuntime second;
        if (!second.open("Window state test", true, error))
            throw std::runtime_error(error);
        int w{}, h{};
        glfwGetWindowSize(second.window(), &w, &h);
        if (w != 1280 || h != 820)
            throw std::runtime_error("Normal window size changed during routing");
#ifdef _WIN32
        const auto hwnd = glfwGetWin32Window(second.window());
        // Emulate maximized native state without showing a window on the desktop.
        SetWindowLongPtrW(hwnd, GWL_STYLE, GetWindowLongPtrW(hwnd, GWL_STYLE) | WS_MAXIMIZE);
#endif
    }
#ifdef _WIN32
    if (!windowState.maximized)
        throw std::runtime_error("Maximized window state was lost");
    for (int route = 0; route < 3; ++route) {
        AppRuntime next;
        if (!next.open("Window state test", true, error))
            throw std::runtime_error(error);
        if (!glfwGetWindowAttrib(next.window(), GLFW_MAXIMIZED))
            throw std::runtime_error("Reopened window is not maximized");
    }
    if (!windowState.maximized || windowState.width != normal.width || windowState.height != normal.height ||
        windowState.x != normal.x || windowState.y != normal.y)
        throw std::runtime_error("Maximized routing changed the restore rectangle");
#endif
}
void AppRuntime::begin() {
    glfwPollEvents();
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();
    ImGui::PushFont(nullptr, interfaceFontSize());
}
void AppRuntime::present() {
    ImGui::PopFont();
    ImGui::Render();
    int w, h;
    glfwGetFramebufferSize(window_, &w, &h);
    glViewport(0, 0, w, h);
    const auto p = Seis3D::Curvilinear::Ui::palette3D().background;
    glClearColor(p.x, p.y, p.z, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    renderPlatformWindows();
    glfwSwapBuffers(window_);
}
void AppRuntime::renderPlatformWindows() {
    if (!(ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_ViewportsEnable))
        return;
    GLFWwindow *previous = glfwGetCurrentContext();
    // Submit the scene texture before another context samples it in a detached viewport.
    glFlush();
    ImGui::UpdatePlatformWindows();
    ImGui::RenderPlatformWindowsDefault();
    glfwMakeContextCurrent(previous);
}
bool capture(const std::filesystem::path &path, int width, int height) {
    std::filesystem::create_directories(path.parent_path());
    std::vector<unsigned char> data(static_cast<std::size_t>(width) * height * 3);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadBuffer(GL_FRONT);
    glReadPixels(0, 0, width, height, GL_RGB, GL_UNSIGNED_BYTE, data.data());
    std::ofstream out(path, std::ios::binary);
    out << "P6\n" << width << ' ' << height << "\n255\n";
    for (int y = height - 1; y >= 0; --y)
        out.write(reinterpret_cast<const char *>(data.data() + static_cast<std::size_t>(y) * width * 3),
                  width * 3);
    return out.good();
}
} // namespace SeisWave::Ui
