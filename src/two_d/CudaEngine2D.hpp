#pragma once
#include "PointKernels2D.hpp"
#include "Seis2D/CurvilinearSolver2D.hpp"
namespace Seis2D::Curvilinear {
class CudaEngine {
  public:
    CudaEngine(const Point::Mesh &, const std::vector<Receiver> &, int);
    CudaEngine(const Point::MeshT<float> &, const std::vector<Receiver> &, int);
    ~CudaEngine();
    void step(double, double, double, double, int);
    void rollback();
    void reset();
    void diagnostics(Diagnostics &);
    void sample(std::vector<Receiver> &);
    void frame(int, std::vector<float> &,int nx,int nz,int offsetX) const;
    Snapshot snapshot(bool includeMemories=false) const;
    void probes(const std::vector<std::size_t>&,std::vector<std::array<double,5>>&)const;
    std::size_t residentBytes()const;

  public: // Type-erased backend interface; implementation remains private to the CUDA translation unit.
    struct Impl;
  private:
    std::unique_ptr<Impl> impl_;
};
} // namespace Seis2D::Curvilinear
