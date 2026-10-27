#pragma once

struct DxcCompiler
{
    IDxcCompiler3* dxcCompiler = nullptr;

    DxcCompiler();
    ~DxcCompiler();

    // `specConstants`: for DXIL, the value g_SpecConstants() returns (shader_common.h).
    IDxcBlob* compile(const std::string& shaderSource, bool compilePixelShader, bool compileLibrary, bool compileSpirv, uint32_t specConstants = 0);
};

// Skylanders: fills in the hash of a DXIL container, which D3D12 checks ("signed"). dxc only writes it when it finds
// the validator library (dxil.dll, libdxil.so), which macOS lacks.
void signDxil(uint8_t* container, size_t size);
