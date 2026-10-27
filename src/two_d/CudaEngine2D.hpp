#pragma once
#include "PointKernels2D.hpp"
#include "Seis2D/CurvilinearSolver2D.hpp"
namespace Seis2D::Curvilinear {
class CudaEngine {
  public:
    CudaEngine(const Point::Mesh &, const std::vector<Receiver> &, int);
    ~CudaEngine();
    void step(double, double, double, double, int);
    void rollback();
    void reset();
    void diagnostics(Diagnostics &);
    void sample(std::vector<Receiver> &);
    void frame(int, std::vector<float> &) const;
    Snapshot snapshot(bool includeMemories=false) const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace Seis2D::Curvilinear
