#pragma once
#include "Seis3D/CurvilinearSolver.hpp"
#include "PointKernels3D.hpp"
namespace Seis3D::Curvilinear {
// Owns device fields, RK buffers, mesh, compact diagnostics and staging buffers.
// Only slices, receiver values and block-reduced statistics cross PCIe during interactive stepping.
class CudaEngine3D {
public:
    CudaEngine3D(const Point::Mesh&,const std::vector<ReceiverTrace3D>&,int device);
    ~CudaEngine3D();
    CudaEngine3D(const CudaEngine3D&)=delete;
    CudaEngine3D& operator=(const CudaEngine3D&)=delete;
    void step(double time,double dt,double frequency,int step);
    void rollback();
    void resetFields();
    void diagnostics(Diagnostics3D&);
    void sample(std::vector<ReceiverTrace3D>&);
    void slices(const DisplayRequest3D&,DisplayFrame3D&);
    void download(std::array<std::vector<double>,9>&) const;
    BackendMemory3D residentMemory() const noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
std::uint64_t queryCudaMemory3D(int device);
}
