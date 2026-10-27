#include "dxc_compiler.h"

DxcCompiler::DxcCompiler()
{
    HRESULT hr = DxcCreateInstance(CLSID_DxcCompiler, IID_PPV_ARGS(&dxcCompiler));
    assert(SUCCEEDED(hr));
}

DxcCompiler::~DxcCompiler()
{
    dxcCompiler->Release();
}

IDxcBlob* DxcCompiler::compile(const std::string& shaderSource, bool compilePixelShader, bool compileLibrary, bool compileSpirv, uint32_t specConstants)
{
    DxcBuffer source{};
    source.Ptr = shaderSource.c_str();
    source.Size = shaderSource.size();

    const wchar_t* args[32]{};
    uint32_t argCount = 0;

    const wchar_t* target = nullptr;
    if (compileLibrary)
    {
        assert(!compileSpirv);
        target = L"-T lib_6_3";
    }
    else
    {
        if (compilePixelShader)
            target = L"-T ps_6_0";
        else
            target = L"-T vs_6_0";
    }

    if (!compileLibrary)
    {
        args[argCount++] = L"-E shaderMain";
    }

    args[argCount++] = target;
    args[argCount++] = L"-HV 2021";
    args[argCount++] = L"-all-resources-bound";

    if (compileSpirv)
    {
        args[argCount++] = L"-spirv";
        args[argCount++] = L"-fvk-use-dx-layout";

        if (!compilePixelShader)
            args[argCount++] = L"-fvk-invert-y";
    }
    else
    {
        args[argCount++] = L"-Wno-ignored-attributes";
        args[argCount++] = L"-Qstrip_reflect";
    }

    args[argCount++] = L"-Qstrip_debug";

    std::wstring specConstantsDefine = L"-DSPEC_CONSTANTS=" + std::to_wstring(specConstants);
    if (!compileSpirv)
        args[argCount++] = specConstantsDefine.c_str();

#ifdef UNLEASHED_RECOMP
    args[argCount++] = L"-DUNLEASHED_RECOMP";
#endif

    IDxcResult* result = nullptr;
    HRESULT hr = dxcCompiler->Compile(&source, args, argCount, nullptr, IID_PPV_ARGS(&result));

    IDxcBlob* object = nullptr;
    if (SUCCEEDED(hr))
    {
        assert(result != nullptr);

        HRESULT status;
        hr = result->GetStatus(&status);
        assert(SUCCEEDED(hr));

        if (FAILED(status))
        {
            if (result->HasOutput(DXC_OUT_ERRORS))
            {
                IDxcBlobUtf8* errors = nullptr;
                hr = result->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(&errors), nullptr);
                assert(SUCCEEDED(hr) && errors != nullptr);

                fputs(errors->GetStringPointer(), stderr);

                errors->Release();
            }
        }
        else
        {
            hr = result->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&object), nullptr);
            assert(SUCCEEDED(hr) && object != nullptr);
        }

        result->Release();
    }
    else
    {
        assert(result == nullptr);
    }

    return object;
}

// The container hash is MD5 over the bytes after the hash field, with a different final block: the bit count first and
// (byte count * 2) | 1 last, where MD5 puts the 64-bit bit count at the end.
void signDxil(uint8_t* container, size_t size)
{
    static constexpr uint32_t K[64] =
    {
        0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
        0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
        0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
        0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
        0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
        0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
        0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
        0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391,
    };
    static constexpr uint32_t S[16] = { 7, 12, 17, 22, 5, 9, 14, 20, 4, 11, 16, 23, 6, 10, 15, 21 };

    uint32_t state[4] = { 0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476 };
    auto block = [&](const uint8_t* data)
    {
        uint32_t x[16];
        for (uint32_t i = 0; i < 16; i++)
            x[i] = data[i * 4] | (data[i * 4 + 1] << 8) | (data[i * 4 + 2] << 16) | (uint32_t(data[i * 4 + 3]) << 24);
        uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
        for (uint32_t i = 0; i < 64; i++)
        {
            uint32_t f, g;
            if (i < 16) { f = (b & c) | (~b & d); g = i; }
            else if (i < 32) { f = (b & d) | (c & ~d); g = (5 * i + 1) % 16; }
            else if (i < 48) { f = b ^ c ^ d; g = (3 * i + 5) % 16; }
            else { f = c ^ (b | ~d); g = (7 * i) % 16; }
            f += a + K[i] + x[g];
            uint32_t s = S[(i / 16) * 4 + i % 4];
            a = d;
            d = c;
            c = b;
            b += (f << s) | (f >> (32 - s));
        }
        state[0] += a;
        state[1] += b;
        state[2] += c;
        state[3] += d;
    };
    auto put32 = [](uint8_t* out, uint32_t value)
    {
        for (uint32_t i = 0; i < 4; i++)
            out[i] = uint8_t(value >> (i * 8));
    };

    constexpr size_t HashedFrom = 20; // "DXBC", then the 16-byte hash
    const uint8_t* data = container + HashedFrom;
    size_t length = size - HashedFrom;
    size_t full = length / 64 * 64, rest = length % 64;
    for (size_t offset = 0; offset < full; offset += 64)
        block(data + offset);

    uint8_t tail[128] = {};
    uint32_t bitCount = uint32_t(length * 8), lastWord = uint32_t(length * 2) | 1;
    if (rest < 56)
    {
        put32(tail, bitCount);
        memcpy(tail + 4, data + full, rest);
        tail[4 + rest] = 0x80;
        put32(tail + 60, lastWord);
        block(tail);
    }
    else
    {
        memcpy(tail, data + full, rest);
        tail[rest] = 0x80;
        put32(tail + 64, bitCount);
        put32(tail + 124, lastWord);
        block(tail);
        block(tail + 64);
    }

    for (uint32_t i = 0; i < 4; i++)
        put32(container + 4 + i * 4, state[i]);
}
