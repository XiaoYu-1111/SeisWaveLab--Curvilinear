#pragma once
#include "VisualSettings3D.hpp"
#include <filesystem>
#include <string>

namespace Seis3D::Curvilinear::Ui {
// Renders physical curvilinear slices, elevation-colored terrain and wireframe.
class SceneRenderer3D {
public:
    ~SceneRenderer3D();
    bool initialize(const std::filesystem::path& shaders,std::string& error);
    bool render(const ISimulationBackend3D& simulation,const DisplayFrame3D& frame,const DisplayFrame3D& material,const VisualSettings3D& visuals,const SliceIndices3D& slices,int width,int height,std::string& error);
    void shutdown();
    void orbit(float dx,float dy);
    void zoom(float delta);
    void pan(float dx,float dy);
    void resetCamera();
    void topViewCamera();
    [[nodiscard]] std::array<std::array<float,2>,3> orientationAxes() const;
    void fitCamera();
    [[nodiscard]] std::array<float,5> cameraState() const {return {yaw_,pitch_,distance_,panX_,panY_};}
    [[nodiscard]] unsigned texture3D() const { return texture_; }
private:
    unsigned program_{},vao_{},vbo_{},framebuffer_{},texture_{},msaaFramebuffer_{},colorBuffer_{},depthBuffer_{};
    int width_{},height_{},samples_{1};
    bool fitTopView_{};
    float yaw_{0.7f},pitch_{0.5f},distance_{2.0f},panX_{},panY_{};
};
}
