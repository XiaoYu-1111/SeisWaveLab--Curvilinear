#include "SceneRenderer3D.hpp"
#include "SceneCoordinates3D.hpp"
#include <GL/glew.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <sstream>
#include <vector>

namespace Seis3D::Curvilinear::Ui {
namespace {
struct Vertex3D { float x,y,z,r,g,b,a; };
using Mat4=std::array<float,16>;
using Vec3=std::array<float,3>;
// Builds the minimal column-major camera matrices used by the independent viewport.
Mat4 multiply(const Mat4& a,const Mat4& b) {
    Mat4 out{};
    for(int c=0;c<4;++c) for(int r=0;r<4;++r) for(int k=0;k<4;++k) out[c*4+r]+=a[k*4+r]*b[c*4+k];
    return out;
}
Vec3 normalize(Vec3 a) { const float d=std::sqrt(a[0]*a[0]+a[1]*a[1]+a[2]*a[2]); for(float& x:a)x/=d; return a; }
Vec3 cross3(Vec3 a,Vec3 b) { return {a[1]*b[2]-a[2]*b[1],a[2]*b[0]-a[0]*b[2],a[0]*b[1]-a[1]*b[0]}; }
float dot3(Vec3 a,Vec3 b) { return a[0]*b[0]+a[1]*b[1]+a[2]*b[2]; }
Mat4 lookAt(Vec3 eye) {
    const Vec3 forward=normalize({-eye[0],-eye[1],-eye[2]});
    const Vec3 side=normalize(cross3(forward,{0,1,0})),up=cross3(side,forward);
    return {side[0],up[0],-forward[0],0,side[1],up[1],-forward[1],0,side[2],up[2],-forward[2],0,-dot3(side,eye),-dot3(up,eye),dot3(forward,eye),1};
}
// Compiles external shaders and returns driver diagnostics instead of silently drawing an empty scene.
unsigned compileShader(const std::filesystem::path& path,unsigned kind,std::string& error) {
    std::ifstream input(path);
    if(!input) { error="Missing shader: "+path.string(); return 0; }
    std::ostringstream content; content<<input.rdbuf(); const auto code=content.str(); const char* data=code.c_str();
    const unsigned shader=glCreateShader(kind); glShaderSource(shader,1,&data,nullptr); glCompileShader(shader);
    int okay=0; glGetShaderiv(shader,GL_COMPILE_STATUS,&okay);
    if(!okay) { char log[4096]{}; glGetShaderInfoLog(shader,4096,nullptr,log); error=log; glDeleteShader(shader); return 0; }
    return shader;
}
}

SceneRenderer3D::~SceneRenderer3D() { shutdown(); }

bool SceneRenderer3D::initialize(const std::filesystem::path& shaders,std::string& error) {
    const unsigned vert=compileShader(shaders/"curvilinear.vert",GL_VERTEX_SHADER,error);
    if(!vert)return false;
    const unsigned frag=compileShader(shaders/"curvilinear.frag",GL_FRAGMENT_SHADER,error);
    if(!frag) { glDeleteShader(vert); return false; }
    program_=glCreateProgram(); glAttachShader(program_,vert); glAttachShader(program_,frag); glLinkProgram(program_);
    glDeleteShader(vert); glDeleteShader(frag);
    int okay=0;glGetProgramiv(program_,GL_LINK_STATUS,&okay);
    if(!okay) { char log[4096]{}; glGetProgramInfoLog(program_,4096,nullptr,log);error=log;shutdown();return false; }
    glGenVertexArrays(1,&vao_); glGenBuffers(1,&vbo_);
    glBindVertexArray(vao_);glBindBuffer(GL_ARRAY_BUFFER,vbo_);
    glEnableVertexAttribArray(0);glVertexAttribPointer(0,3,GL_FLOAT,GL_FALSE,sizeof(Vertex3D),nullptr);
    glEnableVertexAttribArray(1);glVertexAttribPointer(1,4,GL_FLOAT,GL_FALSE,sizeof(Vertex3D),reinterpret_cast<void*>(3*sizeof(float)));
    glBindVertexArray(0);
    int maximum=1;glGetIntegerv(GL_MAX_SAMPLES,&maximum);samples_=std::min(4,maximum);
    return true;
}

bool SceneRenderer3D::render(const ISimulationBackend3D& s,const DisplayFrame3D& frame,const DisplayFrame3D& material,const VisualSettings3D& v,const SliceIndices3D& slice,int width,int height,std::string& error) {
    width=std::clamp(width,16,4096);height=std::clamp(height,16,4096);
    const auto geometry=s.grid().coordinates.data();const std::array<int,3> indices{slice.x,slice.y,slice.z};
    if(cached_&&width==width_&&height==height_&&geometry_==geometry&&cachedVisuals_==v&&cachedCamera_==cameraState()&&cachedSlices_==indices&&waveRevision_==frame.revision&&materialRevision_==material.revision){++hits_;return true;}
    if(width!=width_ || height!=height_) {
        if(texture_) { glDeleteTextures(1,&texture_);glDeleteFramebuffers(1,&framebuffer_);glDeleteFramebuffers(1,&msaaFramebuffer_);glDeleteRenderbuffers(1,&colorBuffer_);glDeleteRenderbuffers(1,&depthBuffer_); }
        width_=width;height_=height;
        glGenFramebuffers(1,&framebuffer_);glBindFramebuffer(GL_FRAMEBUFFER,framebuffer_);
        glGenTextures(1,&texture_);glBindTexture(GL_TEXTURE_2D,texture_);
        glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA8,width,height,0,GL_RGBA,GL_UNSIGNED_BYTE,nullptr);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
        glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,texture_,0);
        if(glCheckFramebufferStatus(GL_FRAMEBUFFER)!=GL_FRAMEBUFFER_COMPLETE) { error="Resolve framebuffer is incomplete.";glBindFramebuffer(GL_FRAMEBUFFER,0);return false; }
        glGenFramebuffers(1,&msaaFramebuffer_);glBindFramebuffer(GL_FRAMEBUFFER,msaaFramebuffer_);
        glGenRenderbuffers(1,&colorBuffer_);glBindRenderbuffer(GL_RENDERBUFFER,colorBuffer_);glRenderbufferStorageMultisample(GL_RENDERBUFFER,samples_,GL_RGBA8,width,height);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_RENDERBUFFER,colorBuffer_);
        glGenRenderbuffers(1,&depthBuffer_);glBindRenderbuffer(GL_RENDERBUFFER,depthBuffer_);glRenderbufferStorageMultisample(GL_RENDERBUFFER,samples_,GL_DEPTH_COMPONENT24,width,height);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER,GL_DEPTH_ATTACHMENT,GL_RENDERBUFFER,depthBuffer_);
        if(glCheckFramebufferStatus(GL_FRAMEBUFFER)!=GL_FRAMEBUFFER_COMPLETE) { error="Multisample framebuffer is incomplete.";glBindFramebuffer(GL_FRAMEBUFFER,0);return false; }
    }
    const auto& grid=s.grid();const int g=grid.ghost;
    const double lx=(grid.nx-2*g-1)*grid.dx,ly=(grid.ny-2*g-1)*grid.dy,lz=(grid.nz-2*g-1)*grid.dz;
    const double scale=std::max({lx,ly,lz+2.0*std::abs(grid.topographyAmplitude)});
    if(fitTopView_) {
        const double top=(lz*.5-((s.parameters().surfaceGrid.empty()?0:s.parameters().surfaceGrid.datum)-v.terrainHigh))/scale;
        distance_=static_cast<float>(std::max(.7,top+1.12/std::tan(.42)*.5*std::max(lx/scale*height/width,ly/scale)));
    }
    auto vertex=[&](int i,int j,int k,std::array<float,4> color) {
        const auto& p=grid.coordinates[grid.index(i,j,k)];
        const auto xyz=scenePosition3D(p,lx,ly,lz,scale);
        return Vertex3D{xyz[0],xyz[1],xyz[2],color[0],color[1],color[2],color[3]};
    };
    std::vector<Vertex3D> triangles,lines,points;
    auto plane=[&](int axis,int at) {
        const int nu=axis==0?grid.ny-2*g:grid.nx-2*g,nv=axis==2?grid.ny-2*g:grid.nz-2*g;
        auto sample=[&](int u,int w) {
            const auto color=absorbingColor3D(fieldColor3D(frame,material,axis==0?1:axis==1?0:2,u,w,v),s.parameters(),axis==0?at-g:u,axis==1?at-g:axis==0?u:w,axis==2?at-g:w,v);
            if(axis==0)return vertex(at,u+g,w+g,color);
            if(axis==1)return vertex(u+g,at,w+g,color);
            return vertex(u+g,w+g,at,color);
        };
        std::vector<Vertex3D> nodes(static_cast<std::size_t>(nu)*nv);
        for(int b=0;b<nv;++b)for(int a=0;a<nu;++a)nodes[static_cast<std::size_t>(b)*nu+a]=sample(a,b);
        auto node=[&](int a,int b){auto p=nodes[static_cast<std::size_t>(b)*nu+a];const auto color=gridColor3D(v);p.r=color[0];p.g=color[1];p.b=color[2];p.a=color[3];return p;};
        if(v.showGrid) {
            for(int a=0;a<nu;++a)for(int b=0;b<nv-1;++b){lines.push_back(node(a,b));lines.push_back(node(a,b+1));}
            for(int b=0;b<nv;++b)for(int a=0;a<nu-1;++a){lines.push_back(node(a,b));lines.push_back(node(a+1,b));}
        }
        for(int b=0;b<nv-1;++b)for(int a=0;a<nu-1;++a) {
            const auto n=static_cast<std::size_t>(b)*nu+a;const auto p0=nodes[n],p1=nodes[n+1],p2=nodes[n+nu+1],p3=nodes[n+nu];
            triangles.insert(triangles.end(),{p0,p1,p2,p0,p2,p3});
        }
    };
    if(v.showSlices) { plane(0,g+slice.x);plane(1,g+slice.y);if(!v.colorTopography||slice.z!=0)plane(2,g+slice.z); }
    if(v.colorTopography) {
        const int nx=grid.nx-2*g,ny=grid.ny-2*g;
        std::vector<Vertex3D> surface(static_cast<std::size_t>(nx)*ny);
        for(int j=0;j<ny;++j)for(int i=0;i<nx;++i) {
            const double elevation=terrainElevation3D(grid.coordinates[grid.index(i+g,j+g,g)][2],s.parameters());
            surface[static_cast<std::size_t>(j)*nx+i]=vertex(i+g,j+g,g,terrainColor3D(elevation,v));
        }
        for(int j=0;j<ny-1;++j)for(int i=0;i<nx-1;++i) {
            const auto n=static_cast<std::size_t>(j)*nx+i;
            triangles.insert(triangles.end(),{surface[n],surface[n+1],surface[n+nx+1],surface[n],surface[n+nx+1],surface[n+nx]});
        }
    }
    if(v.showTopography) {
        const std::array<float,4> color=v.colorTopography?std::array<float,4>{.12f,.12f,.12f,.25f}:std::array<float,4>{v.wire[0],v.wire[1],v.wire[2],1};
        const int stride=v.colorTopography?std::max(1,std::max(grid.nx-2*g,grid.ny-2*g)/60):1;
        for(int j=g;j<grid.ny-g;++j)if((j-g)%stride==0||j==grid.ny-g-1)for(int i=g;i<grid.nx-g-1;++i) { lines.push_back(vertex(i,j,g,color));lines.push_back(vertex(i+1,j,g,color)); }
        for(int i=g;i<grid.nx-g;++i)if((i-g)%stride==0||i==grid.nx-g-1)for(int j=g;j<grid.ny-g-1;++j) { lines.push_back(vertex(i,j,g,color));lines.push_back(vertex(i,j+1,g,color)); }
    }
    const std::array<float,4> border{0.35f,0.40f,0.43f,1};
    for(int k:{g,grid.nz-g-1}) {
        for(int j:{g,grid.ny-g-1}) {lines.push_back(vertex(g,j,k,border));lines.push_back(vertex(grid.nx-g-1,j,k,border));}
        for(int i:{g,grid.nx-g-1}) {lines.push_back(vertex(i,g,k,border));lines.push_back(vertex(i,grid.ny-g-1,k,border));}
    }
    for(int i:{g,grid.nx-g-1})for(int j:{g,grid.ny-g-1}) {lines.push_back(vertex(i,j,g,border));lines.push_back(vertex(i,j,grid.nz-g-1,border));}
    const auto source=sourceIndices3D(s.parameters());
    points.push_back(vertex(g+source[0],g+source[1],g+source[2],{0.94f,0.43f,0.08f,1}));
    if(v.showReceivers) for(const auto& receiver:s.receivers()) {
        const auto q=receiver.gridIndex;
        const int i=static_cast<int>(q%grid.nx),j=static_cast<int>((q/grid.nx)%grid.ny),k=static_cast<int>(q/(static_cast<std::size_t>(grid.nx)*grid.ny));
        points.push_back(vertex(i,j,k,{0.02f,0.48f,0.49f,1}));
    }
    Mat4 perspective{};const float cot=1.0f/std::tan(0.42f);perspective[0]=cot*height/width;perspective[5]=cot;perspective[10]=-1.002f;perspective[11]=-1.0f;perspective[14]=-0.02002f;
    auto view=lookAt({distance_*std::cos(pitch_)*std::sin(yaw_),distance_*std::sin(pitch_),distance_*std::cos(pitch_)*std::cos(yaw_)});
    view[12]+=panX_;view[13]+=panY_;
    const auto mvp=multiply(perspective,view);
    glBindFramebuffer(GL_FRAMEBUFFER,msaaFramebuffer_);glViewport(0,0,width,height);glEnable(GL_DEPTH_TEST);glEnable(GL_MULTISAMPLE);glEnable(GL_PROGRAM_POINT_SIZE);
    glClearColor(v.canvas[0],v.canvas[1],v.canvas[2],1);glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT);
    glUseProgram(program_);glUniformMatrix4fv(glGetUniformLocation(program_,"uMVP"),1,GL_FALSE,mvp.data());
    glBindVertexArray(vao_);glBindBuffer(GL_ARRAY_BUFFER,vbo_);
    auto draw=[&](const std::vector<Vertex3D>& data,GLenum primitive) {
        glBufferData(GL_ARRAY_BUFFER,static_cast<GLsizeiptr>(data.size()*sizeof(Vertex3D)),data.data(),GL_STREAM_DRAW);glDrawArrays(primitive,0,static_cast<GLsizei>(data.size()));
    };
    glEnable(GL_POLYGON_OFFSET_FILL);glPolygonOffset(1,1);draw(triangles,GL_TRIANGLES);glDisable(GL_POLYGON_OFFSET_FILL);
    glEnable(GL_BLEND);glBlendFunc(GL_SRC_ALPHA,GL_ONE_MINUS_SRC_ALPHA);draw(lines,GL_LINES);draw(points,GL_POINTS);glDisable(GL_BLEND);
    glBindFramebuffer(GL_READ_FRAMEBUFFER,msaaFramebuffer_);glBindFramebuffer(GL_DRAW_FRAMEBUFFER,framebuffer_);
    glBlitFramebuffer(0,0,width,height,0,0,width,height,GL_COLOR_BUFFER_BIT,GL_NEAREST);
    glBindFramebuffer(GL_FRAMEBUFFER,0);glBindVertexArray(0);glUseProgram(0);glDisable(GL_DEPTH_TEST);glDisable(GL_PROGRAM_POINT_SIZE);
    cached_=true;geometry_=geometry;cachedVisuals_=v;cachedCamera_=cameraState();cachedSlices_=indices;waveRevision_=frame.revision;materialRevision_=material.revision;++builds_;
    return true;
}

// Releases render targets while the owning OpenGL context is still current.
void SceneRenderer3D::shutdown() {
    invalidate();
    if(texture_)glDeleteTextures(1,&texture_);if(framebuffer_)glDeleteFramebuffers(1,&framebuffer_);if(msaaFramebuffer_)glDeleteFramebuffers(1,&msaaFramebuffer_);
    if(colorBuffer_)glDeleteRenderbuffers(1,&colorBuffer_);if(depthBuffer_)glDeleteRenderbuffers(1,&depthBuffer_);
    if(vbo_)glDeleteBuffers(1,&vbo_);if(vao_)glDeleteVertexArrays(1,&vao_);if(program_)glDeleteProgram(program_);
    texture_=framebuffer_=msaaFramebuffer_=colorBuffer_=depthBuffer_=vbo_=vao_=program_=0;width_=height_=0;
}
void SceneRenderer3D::orbit(float dx,float dy) { fitTopView_=false;yaw_+=dx*0.007f;pitch_=std::clamp(pitch_+dy*0.007f,-1.45f,1.45f); }
void SceneRenderer3D::zoom(float delta) {if(delta!=0)fitTopView_=false;distance_=std::clamp(distance_*std::exp(-delta*0.12f),0.7f,8.0f); }
void SceneRenderer3D::pan(float dx,float dy) {fitTopView_=false;panX_+=dx*0.002f*distance_;panY_-=dy*0.002f*distance_;}
void SceneRenderer3D::resetCamera() {fitTopView_=false;yaw_=0.7f;pitch_=0.5f;distance_=2.0f;panX_=panY_=0;}
void SceneRenderer3D::topViewCamera() {fitTopView_=true;yaw_=0;pitch_=1.5706963f;distance_=2.0f;panX_=panY_=0;}
std::array<std::array<float,2>,3> SceneRenderer3D::orientationAxes() const {
    const auto m=lookAt({std::cos(pitch_)*std::sin(yaw_),std::sin(pitch_),std::cos(pitch_)*std::cos(yaw_)});
    return {{{m[0],-m[1]},{-m[8],m[9]},{m[4],-m[5]}}}; // X, Y, elevation up; screen Y down.
}
void SceneRenderer3D::fitCamera() {distance_=2.0f;panX_=panY_=0;}
}
