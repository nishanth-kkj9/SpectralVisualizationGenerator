#define NOMINMAX
#include "d3d11_context.h"

#include <algorithm>
#include <cstring>
#include <cstdio>

namespace Spectral {

D3D11Context::D3D11Context() {
    D3D_FEATURE_LEVEL feature_levels[] = { D3D_FEATURE_LEVEL_11_0 };
    D3D_FEATURE_LEVEL obtained_level;

    HRESULT hr = D3D11CreateDevice(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
        feature_levels, 1, D3D11_SDK_VERSION,
        device_.GetAddressOf(), &obtained_level,
        context_.GetAddressOf()
    );

    if (FAILED(hr)) {
        device_.Reset();
        context_.Reset();
        std::fprintf(stderr, "D3D11: hardware device failed (0x%08X), trying WARP\n", (unsigned)hr);
        hr = D3D11CreateDevice(
            nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0,
            feature_levels, 1, D3D11_SDK_VERSION,
            device_.GetAddressOf(), &obtained_level,
            context_.GetAddressOf()
        );
    }

    if (FAILED(hr)) {
        device_.Reset();
        context_.Reset();
        std::fprintf(stderr, "D3D11: all device creation attempts failed\n");
    }
}

D3D11Context::~D3D11Context() {
    context_.Reset();
    device_.Reset();
}

Microsoft::WRL::ComPtr<ID3D11Buffer> D3D11Context::create_buffer(
    const void* data, uint32_t byte_width,
    D3D11_USAGE usage, uint32_t bind_flags, uint32_t cpu_access)
{
    if (!device_) return nullptr;

    D3D11_BUFFER_DESC desc{};
    desc.ByteWidth = byte_width;
    desc.Usage = usage;
    desc.BindFlags = bind_flags;
    desc.CPUAccessFlags = cpu_access;
    desc.MiscFlags = 0;
    desc.StructureByteStride = 0;

    D3D11_SUBRESOURCE_DATA init_data{};
    init_data.pSysMem = data;

    Microsoft::WRL::ComPtr<ID3D11Buffer> buf;
    HRESULT hr = device_->CreateBuffer(&desc, data ? &init_data : nullptr, buf.GetAddressOf());
    return SUCCEEDED(hr) ? buf : nullptr;
}

Microsoft::WRL::ComPtr<ID3D11Buffer> D3D11Context::create_staging_buffer(uint32_t byte_width) {
    return create_buffer(nullptr, byte_width, D3D11_USAGE_STAGING,
                         0, D3D11_CPU_ACCESS_READ);
}

bool D3D11Context::upload_buffer(ID3D11Buffer* dst, const void* data, uint32_t byte_width) {
    if (!context_ || !dst || !data) return false;
    context_->UpdateSubresource(dst, 0, nullptr, data, byte_width, byte_width);
    return true;
}

bool D3D11Context::readback_buffer(ID3D11Buffer* src, void* out, uint32_t byte_width) {
    if (!context_ || !src || !out) return false;

    D3D11_MAPPED_SUBRESOURCE mapped{};
    HRESULT hr = context_->Map(src, 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hr)) return false;

    // ponytail: buffers map with RowPitch >= size; single copy suffices
    std::memcpy(out, mapped.pData, byte_width);

    context_->Unmap(src, 0);
    return true;
}

Microsoft::WRL::ComPtr<ID3DBlob> D3D11Context::compile_shader_source(
    const std::string& hlsl_source,
    const char* entry_point,
    const char* target)
{
    Microsoft::WRL::ComPtr<ID3DBlob> blob;
    Microsoft::WRL::ComPtr<ID3DBlob> error_blob;

    HRESULT hr = D3DCompile(
        hlsl_source.c_str(),
        hlsl_source.size(),
        nullptr,                // filename
        nullptr,                // defines
        nullptr,                // include
        entry_point,
        target,
        D3DCOMPILE_OPTIMIZATION_LEVEL3,
        0,
        blob.GetAddressOf(),
        error_blob.GetAddressOf()
    );

    if (FAILED(hr)) {
        if (error_blob) {
            std::fprintf(stderr, "HLSL compile error: %s\n",
                         static_cast<const char*>(error_blob->GetBufferPointer()));
        }
        return nullptr;
    }
    return blob;
}

Microsoft::WRL::ComPtr<ID3D11ComputeShader> D3D11Context::create_compute_shader(ID3DBlob* blob) {
    if (!device_ || !blob) return nullptr;

    Microsoft::WRL::ComPtr<ID3D11ComputeShader> shader;
    HRESULT hr = device_->CreateComputeShader(
        blob->GetBufferPointer(),
        blob->GetBufferSize(),
        nullptr,
        shader.GetAddressOf()
    );
    return SUCCEEDED(hr) ? shader : nullptr;
}

} // namespace Spectral
