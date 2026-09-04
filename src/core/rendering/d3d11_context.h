#pragma once

// Phase 17 — D3D11 GPU context for compute shader rendering.
// Wraps device, device context, and buffer/shader helpers.
// No window required — headless compute only.

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <vector>
#include <cstdint>
#include <string>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d3dcompiler.lib")

namespace Spectral {

class D3D11Context {
public:
    D3D11Context();
    ~D3D11Context();

    D3D11Context(const D3D11Context&) = delete;
    D3D11Context& operator=(const D3D11Context&) = delete;

    bool is_available() const { return device_ != nullptr; }
    ID3D11Device* device() const { return device_.Get(); }
    ID3D11DeviceContext* context() const { return context_.Get(); }

    // Buffer helpers
    Microsoft::WRL::ComPtr<ID3D11Buffer> create_buffer(
        const void* data, uint32_t byte_width,
        D3D11_USAGE usage = D3D11_USAGE_DEFAULT,
        uint32_t bind_flags = D3D11_BIND_SHADER_RESOURCE,
        uint32_t cpu_access = 0);

    Microsoft::WRL::ComPtr<ID3D11Buffer> create_staging_buffer(uint32_t byte_width);

    bool upload_buffer(ID3D11Buffer* dst, const void* data, uint32_t byte_width);
    bool readback_buffer(ID3D11Buffer* src, void* out, uint32_t byte_width);

    // Shader compilation
    Microsoft::WRL::ComPtr<ID3DBlob> compile_shader_source(
        const std::string& hlsl_source,
        const char* entry_point,
        const char* target);

    Microsoft::WRL::ComPtr<ID3D11ComputeShader> create_compute_shader(ID3DBlob* blob);

private:
    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
};

} // namespace Spectral
