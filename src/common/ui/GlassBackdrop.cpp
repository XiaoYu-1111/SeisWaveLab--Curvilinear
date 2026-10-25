#include <GL/glew.h>
#include "GlassMaterial.hpp"
#include "Presentation3D.hpp"
#include "imgui_internal.h"
#include <GLFW/glfw3.h>
#include <unordered_set>
#include <algorithm>
#include <cmath>
#include <string>
namespace SeisWave::Ui {
using namespace Seis3D::Curvilinear::Ui;
namespace {
GLFWwindow* owner{};std::unordered_set<ImGuiID> glassWindows;
GLuint program{},blurProgram{},texture{},blurTextures[2]{},framebuffer{},vao{};int textureWidth{},textureHeight{},passes{};bool attempted{};std::string error;
const char* vertexSource=R"(#version 330 core
uniform vec4 rect; uniform vec2 viewportSize;
void main(){vec2 uv[6]=vec2[6](vec2(0,0),vec2(1,0),vec2(1,1),vec2(0,0),vec2(1,1),vec2(0,1));
gl_Position=vec4((rect.xy+uv[gl_VertexID]*rect.zw)/viewportSize*2.0-1.0,0,1);})";
const char* fragmentSource=R"(#version 330 core
uniform sampler2D backdrop;uniform vec4 rect;uniform vec2 copyOrigin,copySize;
uniform vec3 tint;uniform vec2 mousePoint;uniform float radius,roughness,strength,tintAmount,targetLuma,pixelScale;uniform int lightMode,hoverHighlight;
out vec4 outputColor;
vec3 linearRGB(vec3 c){return mix(c/12.92,pow((c+.055)/1.055,vec3(2.4)),step(vec3(.04045),c));}
vec3 srgb(vec3 c){return mix(c*12.92,1.055*pow(max(c,vec3(0)),vec3(1.0/2.4))-.055,step(vec3(.0031308),c));}
vec3 fetch(vec2 p){return texture(backdrop,clamp(p/copySize,vec2(0),vec2(1))).rgb;}
void main(){
 vec2 p=gl_FragCoord.xy-rect.xy,q=abs(p-rect.zw*.5)-(rect.zw*.5-radius);
 float sd=length(max(q,0.0))+min(max(q.x,q.y),0.0)-radius;
 if(sd>1.0)discard;
 vec2 normal=normalize(p-rect.zw*.5+vec2(.001));
 vec2 lens=normal*(1.0-roughness)*strength*9.0*pixelScale*exp(-max(0.0,-sd)/(10.0*pixelScale));
 vec2 at=gl_FragCoord.xy-copyOrigin+lens;vec3 c=fetch(at);
 c=mix(c,tint,tintAmount);vec3 lin=linearRGB(c);float lum=dot(lin,vec3(.2126,.7152,.0722));
 if(lightMode==1&&lum<targetLuma)lin=mix(lin,vec3(1),clamp((targetLuma-lum)/max(.001,1.0-lum),0.0,1.0));
 if(lightMode==0&&lum>targetLuma)lin*=clamp(targetLuma/max(.001,lum),0.0,1.0);
 c=srgb(lin);float rim=exp(-abs(sd+1.0)*1.8)*(1.0-roughness*.5)*strength;
 if(hoverHighlight==1){vec2 delta=(gl_FragCoord.xy-mousePoint)/(110.0*pixelScale);c=mix(c,vec3(1),exp(-dot(delta,delta))*.035*strength);}
 c=mix(c,vec3(1),rim*(lightMode==1?.35:.15));
 float grain=fract(sin(dot(floor(gl_FragCoord.xy),vec2(12.9898,78.233)))*43758.5453)-.5;
 c+=grain*.012*roughness;
 outputColor=vec4(c,1.0-smoothstep(-.6,.8,sd));
})";
const char* blurVertex=R"(#version 330 core
out vec2 uv;
void main(){vec2 p[6]=vec2[6](vec2(0,0),vec2(1,0),vec2(1,1),vec2(0,0),vec2(1,1),vec2(0,1));uv=p[gl_VertexID];gl_Position=vec4(uv*2.0-1.0,0,1);})";
const char* blurFragment=R"(#version 330 core
in vec2 uv;uniform sampler2D source;uniform vec2 direction;out vec4 outputColor;
void main(){float w[5]=float[5](.227027027,.194594595,.121621622,.054054054,.016216216);
vec3 c=texture(source,uv).rgb*w[0];for(int i=1;i<5;++i)c+=(texture(source,uv+direction*float(i)).rgb+texture(source,uv-direction*float(i)).rgb)*w[i];outputColor=vec4(c,1);})";
GLuint shader(GLenum type,const char* source) {
    const GLuint handle=glCreateShader(type);glShaderSource(handle,1,&source,nullptr);glCompileShader(handle);GLint ok{};glGetShaderiv(handle,GL_COMPILE_STATUS,&ok);
    if(!ok){char log[1024]{};glGetShaderInfoLog(handle,sizeof(log),nullptr,log);error=log;glDeleteShader(handle);return 0;}return handle;
}
GLuint link(const char* vertex,const char* fragment) {
    const GLuint vs=shader(GL_VERTEX_SHADER,vertex),fs=shader(GL_FRAGMENT_SHADER,fragment);
    if(!vs||!fs){if(vs)glDeleteShader(vs);if(fs)glDeleteShader(fs);return false;}
    const GLuint handle=glCreateProgram();glAttachShader(handle,vs);glAttachShader(handle,fs);glLinkProgram(handle);glDeleteShader(vs);glDeleteShader(fs);
    GLint ok{};glGetProgramiv(handle,GL_LINK_STATUS,&ok);if(!ok){char log[1024]{};glGetProgramInfoLog(handle,sizeof(log),nullptr,log);error=log;glDeleteProgram(handle);return 0;}return handle;
}
bool initialize() {
    if(attempted)return program&&blurProgram;attempted=true;
    program=link(vertexSource,fragmentSource);blurProgram=link(blurVertex,blurFragment);if(!program||!blurProgram)return false;
    glGenTextures(1,&texture);glGenTextures(2,blurTextures);glGenFramebuffers(1,&framebuffer);glGenVertexArrays(1,&vao);return true;
}
void allocateTexture(GLuint handle,int width,int height) {
    glBindTexture(GL_TEXTURE_2D,handle);glTexImage2D(GL_TEXTURE_2D,0,GL_RGB8,width,height,0,GL_RGB,GL_UNSIGNED_BYTE,nullptr);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
}
struct Payload {ImVec2 pos,size,displayPos,displaySize;ImVec4 tint;float strength,roughness,luma;bool light;};
void drawBackdrop(const ImDrawList*,const ImDrawCmd* cmd) {
    if(!program||glfwGetCurrentContext()!=owner)return;
    const auto& p=*static_cast<const Payload*>(cmd->UserCallbackData);GLint viewport[4]{};glGetIntegerv(GL_VIEWPORT,viewport);
    if(viewport[2]<1||viewport[3]<1||p.displaySize.x<=0||p.displaySize.y<=0)return;
    const float sx=viewport[2]/p.displaySize.x,sy=viewport[3]/p.displaySize.y;
    const float x=(p.pos.x-p.displayPos.x)*sx,y=viewport[3]-(p.pos.y-p.displayPos.y+p.size.y)*sy,w=p.size.x*sx,h=p.size.y*sy;
    const int pad=static_cast<int>(std::ceil(48*std::max(sx,sy)));
    const int left=std::clamp(static_cast<int>(std::floor(x))-pad,0,viewport[2]),bottom=std::clamp(static_cast<int>(std::floor(y))-pad,0,viewport[3]);
    const int right=std::clamp(static_cast<int>(std::ceil(x+w))+pad,0,viewport[2]),top=std::clamp(static_cast<int>(std::ceil(y+h))+pad,0,viewport[3]);
    const int tw=right-left,th=top-bottom;if(tw<1||th<1)return;
    GLint oldProgram{},oldVao{},active{},oldTexture{},oldSampler{},oldRead{},readBuffer{},drawBuffer{},scissor[4]{};
    glGetIntegerv(GL_CURRENT_PROGRAM,&oldProgram);glGetIntegerv(GL_VERTEX_ARRAY_BINDING,&oldVao);glGetIntegerv(GL_ACTIVE_TEXTURE,&active);glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D,&oldTexture);glGetIntegerv(GL_SAMPLER_BINDING,&oldSampler);glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING,&oldRead);glGetIntegerv(GL_READ_BUFFER,&readBuffer);glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING,&drawBuffer);glGetIntegerv(GL_SCISSOR_BOX,scissor);
    const bool hadScissor=glIsEnabled(GL_SCISSOR_TEST)!=0,hadBlend=glIsEnabled(GL_BLEND)!=0;
    glBindFramebuffer(GL_READ_FRAMEBUFFER,drawBuffer);glReadBuffer(drawBuffer?GL_COLOR_ATTACHMENT0:GL_BACK);
    glBindSampler(0,0);const int bw=std::max(1,(tw+1)/2),bh=std::max(1,(th+1)/2);
    if(textureWidth!=tw||textureHeight!=th){allocateTexture(texture,tw,th);for(auto handle:blurTextures)allocateTexture(handle,bw,bh);textureWidth=tw;textureHeight=th;}
    glBindTexture(GL_TEXTURE_2D,texture);
    glCopyTexSubImage2D(GL_TEXTURE_2D,0,0,0,left,bottom,tw,th);
    bool copied=glGetError()==GL_NO_ERROR;if(!copied)error="OpenGL backdrop copy failed; using opaque popup fallback.";
    // Two separable Gaussian passes at half resolution, rather than sparse 2-D
    // taps that leave recognizable copies of background text in frosted menus.
    if(copied) {
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER,framebuffer);glViewport(0,0,bw,bh);glDisable(GL_SCISSOR_TEST);glDisable(GL_BLEND);glUseProgram(blurProgram);glBindVertexArray(vao);
        glUniform1i(glGetUniformLocation(blurProgram,"source"),0);
        const float sigma=(p.roughness>.5f?13.f:5.f)*(.35f+.65f*p.strength)*std::max(sx,sy);
        for(int axis=0;axis<2;++axis) {
            glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,blurTextures[axis],0);
            if(glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER)!=GL_FRAMEBUFFER_COMPLETE){error="Glass blur framebuffer is incomplete; using opaque popup fallback.";copied=false;break;}
            glBindTexture(GL_TEXTURE_2D,axis?blurTextures[0]:texture);glUniform2f(glGetUniformLocation(blurProgram,"direction"),axis?0:sigma/(1.689f*tw),axis?sigma/(1.689f*th):0);glDrawArrays(GL_TRIANGLES,0,6);
        }
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER,drawBuffer);glViewport(viewport[0],viewport[1],viewport[2],viewport[3]);if(hadBlend)glEnable(GL_BLEND);
    }
    glBindTexture(GL_TEXTURE_2D,copied?blurTextures[1]:texture);
    glUseProgram(program);glBindVertexArray(vao);
    const auto location=[](const char* name){return glGetUniformLocation(program,name);};
    glUniform1i(location("backdrop"),0);glUniform4f(location("rect"),x,y,w,h);glUniform2f(location("viewportSize"),static_cast<float>(viewport[2]),static_cast<float>(viewport[3]));glUniform2f(location("copyOrigin"),static_cast<float>(left),static_cast<float>(bottom));glUniform2f(location("copySize"),static_cast<float>(tw),static_cast<float>(th));
    glUniform3f(location("tint"),p.tint.x,p.tint.y,p.tint.z);glUniform1f(location("radius"),std::min({16*std::max(sx,sy),w*.5f,h*.5f}));glUniform1f(location("roughness"),p.roughness);glUniform1f(location("strength"),p.strength);glUniform1f(location("tintAmount"),!copied?1.f:p.roughness>.5f?.76f:.56f);glUniform1f(location("targetLuma"),p.luma);glUniform1f(location("pixelScale"),std::max(sx,sy));glUniform1i(location("lightMode"),p.light?1:0);
    const auto mouse=ImGui::GetIO().MousePos;glUniform2f(location("mousePoint"),(mouse.x-p.displayPos.x)*sx,viewport[3]-(mouse.y-p.displayPos.y)*sy);glUniform1i(location("hoverHighlight"),glassMotion3D()?1:0);
    glEnable(GL_SCISSOR_TEST);glScissor(std::max(0,static_cast<int>(x)),std::max(0,static_cast<int>(y)),std::max(0,std::min(static_cast<int>(w+1),viewport[2]-std::max(0,static_cast<int>(x)))),std::max(0,std::min(static_cast<int>(h+1),viewport[3]-std::max(0,static_cast<int>(y)))));
    glDrawArrays(GL_TRIANGLES,0,6);++passes;
    if(glGetError()!=GL_NO_ERROR)error="OpenGL backdrop draw failed; using opaque popup fallback.";
    glScissor(scissor[0],scissor[1],scissor[2],scissor[3]);if(!hadScissor)glDisable(GL_SCISSOR_TEST);
    glUseProgram(oldProgram);glBindVertexArray(oldVao);glBindTexture(GL_TEXTURE_2D,oldTexture);glBindSampler(0,oldSampler);glActiveTexture(active);glBindFramebuffer(GL_READ_FRAMEBUFFER,oldRead);glReadBuffer(readBuffer);
}
}
void glassPrepareBackdrop() {
    if(!owner)owner=glfwGetCurrentContext();glassWindows.clear();
    if(glassTheme3D()){const auto& p=palette3D();auto color=p.panel;color.w=initialize()&&error.empty()?.08f:.985f;ImGui::GetStyle().Colors[ImGuiCol_PopupBg]=color;}
}
void glassPreparePopups() {
    if(!glassTheme3D()||!program||!error.empty())return;
    auto* context=ImGui::GetCurrentContext();const auto& p=palette3D();
    const auto linear=[](float a){return a<=.04045f?a/12.92f:std::pow((a+.055f)/1.055f,2.4f);};const float ink=.2126f*linear(p.muted.x)+.7152f*linear(p.muted.y)+.0722f*linear(p.muted.z);
    for(auto* window:context->Windows) {
        if(!window->Active||window->Hidden||(!(window->Flags&(ImGuiWindowFlags_Popup|ImGuiWindowFlags_Tooltip))&&!glassWindows.contains(window->ID)))continue;
        const auto* v=window->Viewport;
        auto* d=window->DrawList;if(!d||d->VtxBuffer.empty())continue;
        if(v!=ImGui::GetMainViewport()) {
            // Native popups use a readable theme surface. Main-context VAO/FBOs
            // cannot be used on a second GLFW context; no desktop blur is implied.
            d->PushClipRect(window->Pos,{window->Pos.x+window->Size.x,window->Pos.y+window->Size.y},false);
            d->AddDrawCmd();const int material=d->CmdBuffer.Size-1;
            d->AddRectFilled(window->Pos,{window->Pos.x+window->Size.x,window->Pos.y+window->Size.y},color3D(p.panel),ImGui::GetStyle().PopupRounding);
            d->AddDrawCmd();d->PopClipRect();
            std::rotate(d->CmdBuffer.begin(),d->CmdBuffer.begin()+material,d->CmdBuffer.begin()+material+1);
            continue;
        }
        Payload payload{window->Pos,window->Size,v->Pos,v->Size,p.panel,glassStrength3D(),glassFinish3D()?1.f:0.f,p.light?4.6f*(ink+.05f)-.05f:(ink+.05f)/4.6f-.05f,p.light};
        d->AddCallback(drawBackdrop,&payload,sizeof(payload));const int start=d->CmdBuffer.Size-2;
        d->AddCallback(ImDrawCallback_ResetRenderState,nullptr);
        // Paint sampled material before this popup's own text/widgets. Existing
        // index offsets stay unchanged; copied callback payloads survive Render().
        std::rotate(d->CmdBuffer.begin(),d->CmdBuffer.begin()+start,d->CmdBuffer.begin()+start+2);
    }
}
void markGlassPanel() {
    auto* window=ImGui::GetCurrentWindow();
    if(window->Viewport==ImGui::GetMainViewport())glassWindows.insert(window->ID);
}
void installGlassPresentation() {
    ImGuiContextHook start;start.Type=ImGuiContextHookType_NewFramePost;start.Callback=[](ImGuiContext*,ImGuiContextHook*){glassPrepareBackdrop();};ImGui::AddContextHook(ImGui::GetCurrentContext(),&start);
    ImGuiContextHook finish;finish.Type=ImGuiContextHookType_EndFramePre;finish.Callback=[](ImGuiContext*,ImGuiContextHook*){glassPreparePopups();};ImGui::AddContextHook(ImGui::GetCurrentContext(),&finish);
    ImGuiContextHook stop;stop.Type=ImGuiContextHookType_Shutdown;stop.Callback=[](ImGuiContext*,ImGuiContextHook*){destroyGlassBackdrop();};ImGui::AddContextHook(ImGui::GetCurrentContext(),&stop);
}
int glassBackdropPasses(){return passes;}
const std::string& glassBackdropError(){return error;}
void destroyGlassBackdrop() {
    if(program)glDeleteProgram(program);if(blurProgram)glDeleteProgram(blurProgram);if(texture)glDeleteTextures(1,&texture);glDeleteTextures(2,blurTextures);if(framebuffer)glDeleteFramebuffers(1,&framebuffer);if(vao)glDeleteVertexArrays(1,&vao);
    owner=nullptr;glassWindows.clear();program=blurProgram=texture=framebuffer=vao=0;blurTextures[0]=blurTextures[1]=0;textureWidth=textureHeight=passes=0;attempted=false;error.clear();
}
}
