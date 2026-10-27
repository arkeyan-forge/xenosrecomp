#ifndef SHADER_COMMON_H_INCLUDED
#define SHADER_COMMON_H_INCLUDED

#define SPEC_CONSTANT_R11G11B10_NORMAL  (1 << 0)
#define SPEC_CONSTANT_ALPHA_TEST        (1 << 1)
#ifndef UNLEASHED_RECOMP
    // Skylanders: blend indices arrive as floats (FLOAT4), not UBYTE4: their 32-bit words are reinterpreted.
    #define SPEC_CONSTANT_BLEND_INDICES_FLOAT (1 << 2)
#endif

#ifdef UNLEASHED_RECOMP
    #define SPEC_CONSTANT_BICUBIC_GI_FILTER (1 << 2)
    #define SPEC_CONSTANT_ALPHA_TO_COVERAGE (1 << 3)
    #define SPEC_CONSTANT_REVERSE_Z         (1 << 4)
#endif

#if defined(__air__) || !defined(__cplusplus) || defined(__INTELLISENSE__)

#ifndef __air__
#define FLT_MIN asfloat(0xff7fffff)
#define FLT_MAX asfloat(0x7f7fffff)
#endif

#ifdef __spirv__

struct PushConstants
{
    uint64_t VertexShaderConstants;
    uint64_t PixelShaderConstants;
    uint64_t SharedConstants;
};

[[vk::push_constant]] ConstantBuffer<PushConstants> g_PushConstants;

#define g_Booleans                 vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 256)
#define g_SwappedTexcoords         vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 260)
#define g_HalfPixelOffset          vk::RawBufferLoad<float2>(g_PushConstants.SharedConstants + 264)
#define g_AlphaThreshold           vk::RawBufferLoad<float>(g_PushConstants.SharedConstants + 272)
#define g_PositionScale            vk::RawBufferLoad<float2>(g_PushConstants.SharedConstants + 280)
#define g_ResolutionScale          vk::RawBufferLoad<float>(g_PushConstants.SharedConstants + 300)
#define g_ParamGen                 vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 276)
#define g_ScaledTextures           vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 312)

[[vk::constant_id(0)]] const uint g_SpecConstants = 0;

#define g_SpecConstants() g_SpecConstants

#elif defined(__air__)

#include <metal_stdlib>

using namespace metal;

constant uint G_SPEC_CONSTANTS [[function_constant(0)]];
constant uint G_SPEC_CONSTANTS_VAL = is_function_constant_defined(G_SPEC_CONSTANTS) ? G_SPEC_CONSTANTS : 0;

uint g_SpecConstants()
{
    return G_SPEC_CONSTANTS_VAL;
}

struct PushConstants
{
    ulong VertexShaderConstants;
    ulong PixelShaderConstants;
    ulong SharedConstants;
};

#define g_Booleans (*(reinterpret_cast<device uint*>(g_PushConstants.SharedConstants + 256)))
#define g_SwappedTexcoords (*(reinterpret_cast<device uint*>(g_PushConstants.SharedConstants + 260)))
#define g_HalfPixelOffset (*(reinterpret_cast<device float2*>(g_PushConstants.SharedConstants + 264)))
#define g_AlphaThreshold (*(reinterpret_cast<device float*>(g_PushConstants.SharedConstants + 272)))
#define g_PositionScale (*(reinterpret_cast<device float2*>(g_PushConstants.SharedConstants + 280)))
#define g_ParamGen (*(reinterpret_cast<device uint*>(g_PushConstants.SharedConstants + 276)))
// Skylanders: the vertices a fetch with a computed index reads, gathered by the host as float4s (count 0: none).
#define g_PulledVertices (*(reinterpret_cast<device ulong*>(g_PushConstants.SharedConstants + 288)))
#define g_PulledCount (*(reinterpret_cast<device uint*>(g_PushConstants.SharedConstants + 296)))
// Skylanders: host render targets are this multiple of the guest's (point sizes and vPos are in guest pixels).
#define g_ResolutionScale (*(reinterpret_cast<device float*>(g_PushConstants.SharedConstants + 300)))
// Skylanders: sampler registers whose texture is g_ResolutionScale times its guest size (resolves), bit n = register n.
#define g_ScaledTextures (*(reinterpret_cast<device uint*>(g_PushConstants.SharedConstants + 312)))
#define PULLED_VERTEX(index) (g_PulledCount != 0 ? *(reinterpret_cast<device float4*>(g_PulledVertices + min(uint(index), g_PulledCount - 1) * 16)) : float4(0.0))

#else

#define DEFINE_SHARED_CONSTANTS() \
    uint g_Booleans : packoffset(c16.x); \
    uint g_SwappedTexcoords : packoffset(c16.y); \
    float2 g_HalfPixelOffset : packoffset(c16.z); \
    float g_AlphaThreshold : packoffset(c17.x); \
    uint g_ParamGen : packoffset(c17.y); \
    float2 g_PositionScale : packoffset(c17.z); \
    uint g_PulledCount : packoffset(c18.z); \
    float g_ResolutionScale : packoffset(c18.w); \
    uint g_ScaledTextures : packoffset(c19.z);

// Skylanders: D3D12 has no specialization constants. XenosRecomp compiles one DXIL shader per combination of the
// shader's specialization constants, each with SPEC_CONSTANTS defined to it.
#ifndef SPEC_CONSTANTS
#define SPEC_CONSTANTS 0
#endif
#define g_SpecConstants() uint(SPEC_CONSTANTS)

// Skylanders: the vertices a fetch with a computed index reads, gathered by the host as float4s (root SRV; count 0: none).
StructuredBuffer<float4> g_PulledVertexBuffer : register(t0, space5);
#define PULLED_VERTEX(index) (g_PulledCount != 0 ? g_PulledVertexBuffer[min(uint(index), max(g_PulledCount, 1u) - 1)] : float4(0.0, 0.0, 0.0, 0.0))

#endif

struct CubeMapData
{
    float3 cubeMapDirections[2];
    uint cubeMapIndex;
};

#ifdef __air__

struct Texture2DDescriptorHeap
{
    texture2d<float> tex;
};

struct Texture3DDescriptorHeap
{
    texture3d<float> tex;
};

struct TextureCubeDescriptorHeap
{
    texturecube<float> tex;
};

struct SamplerDescriptorHeap
{
    sampler samp;
};

uint2 getTexture2DDimensions(texture2d<float> texture)
{
    return uint2(texture.get_width(), texture.get_height());
}

// Skylanders: `scale` is g_ResolutionScale for a resolved texture (host size = guest size x scale), else 1: the offset is in
// guest texels.
float4 tfetch2D(constant Texture2DDescriptorHeap* textureHeap,
                constant SamplerDescriptorHeap* samplerHeap,
                uint resourceDescriptorIndex,
                uint samplerDescriptorIndex,
                float2 texCoord, float2 offset, float scale)
{
    texture2d<float> texture = textureHeap[resourceDescriptorIndex].tex;
    sampler sampler = samplerHeap[samplerDescriptorIndex].samp;
    return texture.sample(sampler, texCoord + offset * scale / (float2)getTexture2DDimensions(texture));
}

// Skylanders: a vertex shader's fetch, at level 0.
float4 tfetch2DVertex(constant Texture2DDescriptorHeap* textureHeap,
                      constant SamplerDescriptorHeap* samplerHeap,
                      uint resourceDescriptorIndex,
                      uint samplerDescriptorIndex,
                      float2 texCoord, float2 offset)
{
    texture2d<float> texture = textureHeap[resourceDescriptorIndex].tex;
    sampler sampler = samplerHeap[samplerDescriptorIndex].samp;
    return texture.sample(sampler, texCoord + offset / (float2)getTexture2DDimensions(texture), level(0));
}

// Skylanders: the bilinear weights on the guest texel grid (the host one divided by `scale`), to go with offsets in guest texels.
float2 getWeights2D(constant Texture2DDescriptorHeap* textureHeap,
                    constant SamplerDescriptorHeap* samplerHeap,
                    uint resourceDescriptorIndex,
                    uint samplerDescriptorIndex,
                    float2 texCoord, float2 offset, float scale)
{
    texture2d<float> texture = textureHeap[resourceDescriptorIndex].tex;
    return select(fract(texCoord * float2(getTexture2DDimensions(texture)) / scale + offset - 0.5), 0.0, isnan(texCoord));
}

// Skylanders: fetches with unnormalized coordinates (the instruction's texCoordDenorm bit): the coordinates are guest
// texels; `scale` is g_ResolutionScale for a resolved texture (host size = guest size x scale), else 1. Swap Force's
// screen-space passes fetch at the pixel position this way.
float4 tfetch2DDenorm(constant Texture2DDescriptorHeap* textureHeap,
                      constant SamplerDescriptorHeap* samplerHeap,
                      uint resourceDescriptorIndex,
                      uint samplerDescriptorIndex,
                      float2 texCoord, float2 offset, float scale)
{
    texture2d<float> texture = textureHeap[resourceDescriptorIndex].tex;
    sampler sampler = samplerHeap[samplerDescriptorIndex].samp;
    return texture.sample(sampler, (texCoord + offset) * scale / (float2)getTexture2DDimensions(texture));
}

float2 getWeights2DDenorm(constant Texture2DDescriptorHeap* textureHeap,
                          constant SamplerDescriptorHeap* samplerHeap,
                          uint resourceDescriptorIndex,
                          uint samplerDescriptorIndex,
                          float2 texCoord, float2 offset, float scale)
{
    return select(fract(texCoord + offset - 0.5), 0.0, isnan(texCoord));
}

float4 tfetch3D(constant Texture3DDescriptorHeap* textureHeap,
                constant SamplerDescriptorHeap* samplerHeap,
                uint resourceDescriptorIndex,
                uint samplerDescriptorIndex,
                float3 texCoord)
{
    texture3d<float> texture = textureHeap[resourceDescriptorIndex].tex;
    sampler sampler = samplerHeap[samplerDescriptorIndex].samp;
    return texture.sample(sampler, texCoord);
}

float4 tfetchCube(constant TextureCubeDescriptorHeap* textureHeap,
                  constant SamplerDescriptorHeap* samplerHeap,
                  uint resourceDescriptorIndex,
                  uint samplerDescriptorIndex,
                  float3 texCoord, thread CubeMapData* cubeMapData)
{
    texturecube<float> texture = textureHeap[resourceDescriptorIndex].tex;
    sampler sampler = samplerHeap[samplerDescriptorIndex].samp;
    return texture.sample(sampler, cubeMapData->cubeMapDirections[(uint)texCoord.z]);
}

float4 cube(float4 value, thread CubeMapData* cubeMapData)
{
    uint index = cubeMapData->cubeMapIndex;
    cubeMapData->cubeMapDirections[index] = value.xyz;
    ++cubeMapData->cubeMapIndex;

    return float4(0.0, 0.0, 0.0, index);
}

#else

Texture2D<float4> g_Texture2DDescriptorHeap[] : register(t0, space0);
Texture3D<float4> g_Texture3DDescriptorHeap[] : register(t0, space1);
TextureCube<float4> g_TextureCubeDescriptorHeap[] : register(t0, space2);
SamplerState g_SamplerDescriptorHeap[] : register(s0, space3);

uint2 getTexture2DDimensions(Texture2D<float4> texture)
{
    uint2 dimensions;
    texture.GetDimensions(dimensions.x, dimensions.y);
    return dimensions;
}

// Skylanders: `scale` as above (offsets in guest texels).
float4 tfetch2D(uint resourceDescriptorIndex, uint samplerDescriptorIndex, float2 texCoord, float2 offset, float scale)
{
    Texture2D<float4> texture = g_Texture2DDescriptorHeap[resourceDescriptorIndex];
    return texture.Sample(g_SamplerDescriptorHeap[samplerDescriptorIndex], texCoord + offset * scale / getTexture2DDimensions(texture));
}

// Skylanders: a vertex shader's fetch, at level 0.
float4 tfetch2DVertex(uint resourceDescriptorIndex, uint samplerDescriptorIndex, float2 texCoord, float2 offset)
{
    Texture2D<float4> texture = g_Texture2DDescriptorHeap[resourceDescriptorIndex];
    return texture.SampleLevel(g_SamplerDescriptorHeap[samplerDescriptorIndex], texCoord + offset / getTexture2DDimensions(texture), 0);
}

float2 getWeights2D(uint resourceDescriptorIndex, uint samplerDescriptorIndex, float2 texCoord, float2 offset, float scale)
{
    Texture2D<float4> texture = g_Texture2DDescriptorHeap[resourceDescriptorIndex];
    return select(isnan(texCoord), 0.0, frac(texCoord * getTexture2DDimensions(texture) / scale + offset - 0.5));
}

// Skylanders: unnormalized coordinates (texCoordDenorm), in texels.
float4 tfetch2DDenorm(uint resourceDescriptorIndex, uint samplerDescriptorIndex, float2 texCoord, float2 offset, float scale)
{
    Texture2D<float4> texture = g_Texture2DDescriptorHeap[resourceDescriptorIndex];
    return texture.Sample(g_SamplerDescriptorHeap[samplerDescriptorIndex], (texCoord + offset) * scale / getTexture2DDimensions(texture));
}

float2 getWeights2DDenorm(uint resourceDescriptorIndex, uint samplerDescriptorIndex, float2 texCoord, float2 offset, float scale)
{
    return select(isnan(texCoord), 0.0, frac(texCoord + offset - 0.5));
}

float4 tfetch3D(uint resourceDescriptorIndex, uint samplerDescriptorIndex, float3 texCoord)
{
    return g_Texture3DDescriptorHeap[resourceDescriptorIndex].Sample(g_SamplerDescriptorHeap[samplerDescriptorIndex], texCoord);
}

float4 tfetchCube(uint resourceDescriptorIndex, uint samplerDescriptorIndex, float3 texCoord, inout CubeMapData cubeMapData)
{
    return g_TextureCubeDescriptorHeap[resourceDescriptorIndex].Sample(g_SamplerDescriptorHeap[samplerDescriptorIndex], cubeMapData.cubeMapDirections[texCoord.z]);
}

float4 cube(float4 value, inout CubeMapData cubeMapData)
{
    uint index = cubeMapData.cubeMapIndex;
    cubeMapData.cubeMapDirections[index] = value.xyz;
    ++cubeMapData.cubeMapIndex;

    return float4(0.0, 0.0, 0.0, index);
}

#endif

float4 tfetchR11G11B10(uint4 value)
{
    if (g_SpecConstants() & SPEC_CONSTANT_R11G11B10_NORMAL)
    {
        // Skylanders packs normals as DEC3N: three signed normalized 10-bit components, X in the low bits.
        float3 magnitude = float3(value.x & 0x1FF, (value.x >> 10) & 0x1FF, (value.x >> 20) & 0x1FF);
        float3 sign = float3(value.x & 0x200 ? 512.0 : 0.0, (value.x >> 10) & 0x200 ? 512.0 : 0.0, (value.x >> 20) & 0x200 ? 512.0 : 0.0);
        return float4(max((magnitude - sign) / 511.0, -1.0), 0.0);
    }
    else
    {
#ifdef __air__
        return as_type<float4>(value);
#else
        return asfloat(value);
#endif
    }
}

#ifndef UNLEASHED_RECOMP
// BLENDINDICES are declared uint4: integer formats (UBYTE4) convert by value; float formats are fed as their bits.
float4 tfetchBlendIndices(uint4 value)
{
    if (g_SpecConstants() & SPEC_CONSTANT_BLEND_INDICES_FLOAT)
    {
#ifdef __air__
        return as_type<float4>(value);
#else
        return asfloat(value);
#endif
    }
    return float4(value);
}
#endif

#ifdef __air__
#define selectWrapper(a, b, c) select(c, b, a)
#else
#define selectWrapper(a, b, c) select(a, b, c)
#endif

#ifdef __air__
#define frac(X) fract(X)

template<typename T>
void clip(T a)
{
    if (a < 0.0) {
        discard_fragment();
    }
}

template<typename T>
float rcp(T a)
{
    return 1.0 / a;
}

template<typename T>
float4x4 mul(T a, T b)
{
    return b * a;
}
#endif

// Skylanders: shaders with jumps run their blocks in a loop from block `pc` on, falling through to the next block. Metal
// and SPIR-V dispatch with a switch whose cases fall through; DXIL uses a chain of `if (pc <= N)` blocks instead, the same
// order without fallthrough, which compilers that translate DXIL (dxil-spirv) structure correctly.
#if defined(__air__) || defined(__spirv__)
#define PC_DISPATCH switch (pc)
#define PC_CASE(N) case N:
#else
#define PC_DISPATCH
#define PC_CASE(N) } if (pc <= N) {
#endif

#ifdef __air__
#define UNROLL
#define BRANCH
#else
#define UNROLL [unroll]
#define BRANCH [branch]
#endif

float w0(float a)
{
    return (1.0f / 6.0f) * (a * (a * (-a + 3.0f) - 3.0f) + 1.0f);
}

float w1(float a)
{
    return (1.0f / 6.0f) * (a * a * (3.0f * a - 6.0f) + 4.0f);
}

float w2(float a)
{
    return (1.0f / 6.0f) * (a * (a * (-3.0f * a + 3.0f) + 3.0f) + 1.0f);
}

float w3(float a)
{
    return (1.0f / 6.0f) * (a * a * a);
}

float g0(float a)
{
    return w0(a) + w1(a);
}

float g1(float a)
{
    return w2(a) + w3(a);
}

float h0(float a)
{
    return -1.0f + w1(a) / (w0(a) + w1(a)) + 0.5f;
}

float h1(float a)
{
    return 1.0f + w3(a) / (w2(a) + w3(a)) + 0.5f;
}

#ifdef __air__

float4 tfetch2DBicubic(constant Texture2DDescriptorHeap* textureHeap,
                       constant SamplerDescriptorHeap* samplerHeap,
                       uint resourceDescriptorIndex,
                       uint samplerDescriptorIndex,
                       float2 texCoord, float2 offset)
{
    texture2d<float> texture = textureHeap[resourceDescriptorIndex].tex;
    sampler sampler = samplerHeap[samplerDescriptorIndex].samp;
    uint2 dimensions = getTexture2DDimensions(texture);

    float x = texCoord.x * dimensions.x + offset.x;
    float y = texCoord.y * dimensions.y + offset.y;

    x -= 0.5f;
    y -= 0.5f;
    float px = floor(x);
    float py = floor(y);
    float fx = x - px;
    float fy = y - py;

    float g0x = g0(fx);
    float g1x = g1(fx);
    float h0x = h0(fx);
    float h1x = h1(fx);
    float h0y = h0(fy);
    float h1y = h1(fy);

    float4 r =
        g0(fy) * (g0x * texture.sample(sampler, float2(px + h0x, py + h0y) / float2(dimensions)) +
              g1x * texture.sample(sampler, float2(px + h1x, py + h0y) / float2(dimensions))) +
        g1(fy) * (g0x * texture.sample(sampler, float2(px + h0x, py + h1y) / float2(dimensions)) +
              g1x * texture.sample(sampler, float2(px + h1x, py + h1y) / float2(dimensions)));

    return r;
}

#else

float4 tfetch2DBicubic(uint resourceDescriptorIndex, uint samplerDescriptorIndex, float2 texCoord, float2 offset)
{
    Texture2D<float4> texture = g_Texture2DDescriptorHeap[resourceDescriptorIndex];
    SamplerState samplerState = g_SamplerDescriptorHeap[samplerDescriptorIndex];
    uint2 dimensions = getTexture2DDimensions(texture);

    float x = texCoord.x * dimensions.x + offset.x;
    float y = texCoord.y * dimensions.y + offset.y;

    x -= 0.5f;
    y -= 0.5f;
    float px = floor(x);
    float py = floor(y);
    float fx = x - px;
    float fy = y - py;

    float g0x = g0(fx);
    float g1x = g1(fx);
    float h0x = h0(fx);
    float h1x = h1(fx);
    float h0y = h0(fy);
    float h1y = h1(fy);

    float4 r =
        g0(fy) * (g0x * texture.Sample(samplerState, float2(px + h0x, py + h0y) / float2(dimensions)) +
            g1x * texture.Sample(samplerState, float2(px + h1x, py + h0y) / float2(dimensions))) +
        g1(fy) * (g0x * texture.Sample(samplerState, float2(px + h0x, py + h1y) / float2(dimensions)) +
            g1x * texture.Sample(samplerState, float2(px + h1x, py + h1y) / float2(dimensions)));

    return r;
}

#endif

float4 swapFloats(uint swappedFloats, float4 value, uint semanticIndex)
{
    return (swappedFloats & (1ull << semanticIndex)) != 0 ? value.yxwz : value;
}

float4 dst(float4 src0, float4 src1)
{
    float4 dest;
    dest.x = 1.0;
    dest.y = src0.y * src1.y;
    dest.z = src0.z;
    dest.w = src1.w;
    return dest;
}

float4 max4(float4 src0)
{
    return max(max(src0.x, src0.y), max(src0.z, src0.w));
}

#ifdef __air__

float2 getPixelCoord(constant Texture2DDescriptorHeap* textureHeap,
                     uint resourceDescriptorIndex,
                     float2 texCoord)
{
    texture2d<float> texture = textureHeap[resourceDescriptorIndex].tex;
    return (float2)getTexture2DDimensions(texture) * texCoord;
}

#else

float2 getPixelCoord(uint resourceDescriptorIndex, float2 texCoord)
{
    return getTexture2DDimensions(g_Texture2DDescriptorHeap[resourceDescriptorIndex]) * texCoord;
}

#endif

float computeMipLevel(float2 pixelCoord)
{
#ifdef __air__
    float2 dx = dfdx(pixelCoord);
    float2 dy = dfdy(pixelCoord);
#else
    float2 dx = ddx(pixelCoord);
    float2 dy = ddy(pixelCoord);
#endif
    float deltaMaxSqr = max(dot(dx, dx), dot(dy, dy));
    return max(0.0, 0.5 * log2(deltaMaxSqr));
}

#endif

#endif
