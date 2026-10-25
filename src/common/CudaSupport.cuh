#pragma once
#include <cuda_runtime.h>
#include <stdexcept>
#include <string>
namespace SeisWave::Cuda {
inline void checked(cudaError_t code, const char *action) {
    if (code != cudaSuccess)
        throw std::runtime_error(std::string("CUDA ") + action + ": " + cudaGetErrorString(code));
}
struct DeviceScope {
    int previous{-1}, device;
    explicit DeviceScope(int selected) : device(selected) {
        if (cudaGetDevice(&previous) != cudaSuccess) {
            cudaGetLastError();
            previous = -1;
        }
        checked(cudaSetDevice(device), "select device");
    }
    ~DeviceScope() {
        if (previous >= 0 && previous != device)
            cudaSetDevice(previous);
    }
    DeviceScope(const DeviceScope &) = delete;
};
struct DeviceBuffer {
    void *pointer{};
    std::size_t allocatedBytes{};
    int device{-1};
    DeviceBuffer() = default;
    DeviceBuffer(const DeviceBuffer &) = delete;
    ~DeviceBuffer() {
        if (pointer) {
            int previous = -1;
            cudaGetDevice(&previous);
            if (cudaSetDevice(device) == cudaSuccess)
                cudaFree(pointer);
            if (previous >= 0 && previous != device)
                cudaSetDevice(previous);
        }
    }
    void allocate(std::size_t bytes) {
        checked(cudaGetDevice(&device), "buffer device");
        checked(cudaMalloc(&pointer, bytes), "allocation");
        allocatedBytes=bytes;
        checked(cudaMemset(pointer, 0, bytes), "zero buffer");
    }
    double *doubles() const {
        return static_cast<double *>(pointer);
    }
    void upload(const void *data, std::size_t bytes) {
        allocate(bytes);
        checked(cudaMemcpy(pointer, data, bytes, cudaMemcpyHostToDevice), "upload");
    }
};
} // namespace SeisWave::Cuda
