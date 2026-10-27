#include <deque>
#include <future>
#include <mutex>
#include <thread>

#include "shader.h"
#include "shader_recompiler.h"
#include "dxc_compiler.h"

#ifdef XENOS_RECOMP_AIR
#include "air_compiler.h"
#endif

static std::unique_ptr<uint8_t[]> readAllBytes(const char* filePath, size_t& fileSize)
{
    FILE* file = fopen(filePath, "rb");
    fseek(file, 0, SEEK_END);
    fileSize = ftell(file);
    fseek(file, 0, SEEK_SET);
    auto data = std::make_unique<uint8_t[]>(fileSize);
    fread(data.get(), 1, fileSize, file);
    fclose(file);
    return data;
}

static void writeAllBytes(const char* filePath, const void* data, size_t dataSize)
{
    FILE* file = fopen(filePath, "wb");
    fwrite(data, 1, dataSize, file);
    fclose(file);
}

struct RecompiledShader
{
    uint8_t* data = nullptr;
    std::vector<uint8_t> dxil; // see appendDxil
    std::vector<uint8_t> spirv;
    std::vector<uint8_t> air;
    uint32_t specConstantsMask = 0;
    std::string error; // set when the shader could not be translated; it is then left out of the cache
    std::string dxilError; // set when only the DXIL failed: the entry then has no DXIL (not drawn on D3D12)
};

// Skylanders: a compiled shader is kept in <cache dir>/<hash>.bin, keyed by the hash of the translated HLSL (and what
// else the compilers are given), not by the original shader: a second run (SkyBuild's movie pass, or the run after a
// game update whose translator changed) translates every shader again, which is fast, and compiles only those whose
// translated text is new (the text starts with shader_common.h, so that is covered). SkyBuild empties the folder when
// the compilers (dxc) change.
struct ShaderCache
{
    std::filesystem::path dir;
    uint64_t key = 0; // format marker

    static constexpr uint32_t MAGIC = 0x32435853; // "SXC2"

    std::filesystem::path pathOf(XXH64_hash_t hash) const { return dir / fmt::format("{:016X}.bin", hash); }

    bool load(XXH64_hash_t hash, RecompiledShader& shader) const
    {
        FILE* file = fopen(pathOf(hash).string().c_str(), "rb");
        if (file == nullptr)
            return false;
        std::vector<uint8_t> bytes;
        uint8_t chunk[1 << 16];
        for (size_t n; (n = fread(chunk, 1, sizeof(chunk), file)) > 0;)
            bytes.insert(bytes.end(), chunk, chunk + n);
        fclose(file);

        size_t at = 0;
        auto take = [&](void* out, size_t size) { if (at + size > bytes.size()) return false; memcpy(out, bytes.data() + at, size); at += size; return true; };
        uint32_t magic = 0, mask = 0;
        uint64_t fileKey = 0, sizes[5] = {};
        if (!take(&magic, 4) || magic != MAGIC || !take(&fileKey, 8) || fileKey != key || !take(&mask, 4) || !take(sizes, sizeof(sizes)))
            return false;
        uint64_t total = 0;
        for (uint64_t size : sizes)
            total += size;
        if (at + total != bytes.size())
            return false;

        shader.specConstantsMask = mask;
        auto bytesOf = [&](uint64_t size) { auto begin = bytes.begin() + at; at += size; return std::vector<uint8_t>(begin, begin + size); };
        shader.dxil = bytesOf(sizes[0]);
        shader.spirv = bytesOf(sizes[1]);
        shader.air = bytesOf(sizes[2]);
        auto errorOf = [&](uint64_t size) { auto begin = bytes.begin() + at; at += size; return std::string(begin, begin + size); };
        shader.error = errorOf(sizes[3]);
        shader.dxilError = errorOf(sizes[4]);
        return true;
    }

    void store(XXH64_hash_t hash, const RecompiledShader& shader) const
    {
        std::vector<uint8_t> bytes;
        auto put = [&](const void* data, size_t size) { auto begin = reinterpret_cast<const uint8_t*>(data); bytes.insert(bytes.end(), begin, begin + size); };
        uint32_t magic = MAGIC;
        uint64_t sizes[5] = { shader.dxil.size(), shader.spirv.size(), shader.air.size(), shader.error.size(), shader.dxilError.size() };
        put(&magic, 4);
        put(&key, 8);
        put(&shader.specConstantsMask, 4);
        put(sizes, sizeof(sizes));
        put(shader.dxil.data(), shader.dxil.size());
        put(shader.spirv.data(), shader.spirv.size());
        put(shader.air.data(), shader.air.size());
        put(shader.error.data(), shader.error.size());
        put(shader.dxilError.data(), shader.dxilError.size());

        // Written under another name and renamed: an interrupted run never leaves a half file.
        auto temporary = pathOf(hash).string() + ".tmp";
        writeAllBytes(temporary.c_str(), bytes.data(), bytes.size());
        std::error_code error;
        std::filesystem::rename(temporary, pathOf(hash), error);
    }
};

static ShaderCache* g_shaderCache = nullptr;

void recompileShader(RecompiledShader& shader, const std::string_view include)
{
    XXH64_hash_t contentHash = 0; // Skylanders: of the translated text, 0 until it exists
    try
    {
    thread_local ShaderRecompiler recompiler;
    recompiler = {};
    recompiler.recompile(shader.data, include);

    shader.specConstantsMask = recompiler.specConstantsMask;

    // Skylanders: the compiled outputs depend on the translated text and these three values only.
    if (g_shaderCache != nullptr)
    {
        contentHash = XXH3_64bits(recompiler.out.data(), recompiler.out.size());
        uint32_t extra[3] = { recompiler.isPixelShader ? 1u : 0u, recompiler.exportsPointSize ? 1u : 0u, recompiler.specConstantsMask };
        contentHash ^= XXH3_64bits(extra, sizeof(extra)) * 0x9E3779B97F4A7C15ull;
        if (g_shaderCache->load(contentHash, shader))
            return;
    }

    thread_local DxcCompiler dxcCompiler;

#ifdef XENOS_RECOMP_DXIL
    // Skylanders: D3D12 has no specialization constants, so the shader is compiled once per combination of the ones it
    // uses (at most 2 here). Its DXIL entry is: flags (bit 0: the vertex shader outputs a point size), the variant
    // count, each variant's size, then the signed containers, in increasing order of their g_SpecConstants() value.
    try
    {
    std::vector<uint32_t> variants;
    for (uint32_t value = 0; value <= recompiler.specConstantsMask; value++)
    {
        if ((value & ~recompiler.specConstantsMask) == 0)
            variants.push_back(value);
    }

    auto appendWord = [&](uint32_t value) { shader.dxil.insert(shader.dxil.end(), reinterpret_cast<uint8_t*>(&value), reinterpret_cast<uint8_t*>(&value) + 4); };
    appendWord(!recompiler.isPixelShader && recompiler.exportsPointSize ? 1 : 0);
    appendWord(uint32_t(variants.size()));
    size_t sizesOffset = shader.dxil.size();
    for (size_t i = 0; i < variants.size(); i++)
        appendWord(0);

    for (size_t i = 0; i < variants.size(); i++)
    {
        IDxcBlob* dxil = dxcCompiler.compile(recompiler.out, recompiler.isPixelShader, false, false, variants[i]);
        if (dxil == nullptr)
            throw std::runtime_error(fmt::format("DXIL compilation failed (specialization constants {})", variants[i]));

        size_t offset = shader.dxil.size();
        uint32_t size = uint32_t(dxil->GetBufferSize());
        auto bytes = reinterpret_cast<uint8_t*>(dxil->GetBufferPointer());
        shader.dxil.insert(shader.dxil.end(), bytes, bytes + size);
        dxil->Release();

        signDxil(shader.dxil.data() + offset, size);
        memcpy(shader.dxil.data() + sizesOffset + i * 4, &size, 4);
    }
    }
    catch (const std::exception& exception)
    {
        shader.dxil.clear();
        shader.dxilError = exception.what();
    }
#endif

#ifdef XENOS_RECOMP_AIR
    shader.air = AirCompiler::compile(recompiler.out);
#endif

    IDxcBlob* spirv = dxcCompiler.compile(recompiler.out, recompiler.isPixelShader, false, true);
    // Skylanders: skipped like a shader Metal rejects (on macOS AIR fails first; elsewhere this was a crash).
    if (spirv == nullptr)
        throw std::runtime_error("SPIR-V compilation failed");

    bool result = smolv::Encode(spirv->GetBufferPointer(), spirv->GetBufferSize(), shader.spirv, smolv::kEncodeFlagStripDebugInfo);
    assert(result);

    spirv->Release();
    }
    catch (const std::exception& exception)
    {
        shader.error = exception.what();
    }
    if (g_shaderCache != nullptr && contentHash != 0)
        g_shaderCache->store(contentHash, shader);
}

static std::string label_lower(const char* name)
{
    std::string text(name);
    text[0] = char(tolower(text[0]));
    return text;
}

// SkyBuild reads these lines while the program runs (the launcher's progress bar): flushed, as stdout is a file.
static void reportProgress(size_t done, size_t total)
{
    if ((done % 10) == 0 || done == total)
    {
        fmt::println("Recompiling shaders... {}%", done / float(total) * 100.0f);
        fmt::println("Progress {} {}", done, total);
        fflush(stdout);
    }
}

int main(int argc, char** argv)
{
#ifndef XENOS_RECOMP_INPUT
    if (argc < 4)
    {
        printf("Usage: XenosRecomp [input path] [output path] [shader common header file path]");
        return 0;
    }
#endif

    const char* input =
#ifdef XENOS_RECOMP_INPUT 
        XENOS_RECOMP_INPUT
#else
        argv[1]
#endif
    ;

    const char* output =
#ifdef XENOS_RECOMP_OUTPUT 
        XENOS_RECOMP_OUTPUT
#else
        argv[2]
#endif
        ;
    
    const char* includeInput =
#ifdef XENOS_RECOMP_INCLUDE_INPUT
        XENOS_RECOMP_INCLUDE_INPUT
#else
        argv[3]
#endif
        ;

    size_t includeSize = 0;
    auto includeData = readAllBytes(includeInput, includeSize);
    std::string_view include(reinterpret_cast<const char*>(includeData.get()), includeSize);

    // Optional 4th argument: the folder of the translated shaders (ShaderCache).
    ShaderCache cache;
#ifndef XENOS_RECOMP_INPUT
    if (argc > 4)
    {
        cache.dir = argv[4];
        cache.key = 1; // the translated text starts with shader_common.h, so its hash covers it
        std::filesystem::create_directories(cache.dir);
        g_shaderCache = &cache;
    }
#endif

    if (std::filesystem::is_directory(input))
    {
        std::vector<std::unique_ptr<uint8_t[]>> files;
        std::map<XXH64_hash_t, RecompiledShader> shaders;
        std::map<XXH64_hash_t, std::string> shaderFilenames;

        for (auto& file : std::filesystem::recursive_directory_iterator(input))
        {
            if (std::filesystem::is_directory(file))
            {
                continue;
            }
            
            size_t fileSize = 0;
            auto fileData = readAllBytes(file.path().string().c_str(), fileSize);
            bool foundAny = false;

            for (size_t i = 0; fileSize > sizeof(ShaderContainer) && i < fileSize - sizeof(ShaderContainer) - 1;)
            {
                auto shaderContainer = reinterpret_cast<const ShaderContainer*>(fileData.get() + i);
                size_t dataSize = shaderContainer->virtualSize + shaderContainer->physicalSize;

                if ((shaderContainer->flags & 0xFFFFFF00) == 0x102A1100 &&
                    dataSize <= (fileSize - i) &&
                    shaderContainer->field1C == 0 &&
                    shaderContainer->field20 == 0)
                {
                    XXH64_hash_t hash = XXH3_64bits(shaderContainer, dataSize);
                    auto shader = shaders.try_emplace(hash);
                    if (shader.second)
                    {
                        shader.first->second.data = fileData.get() + i;
                        foundAny = true;
                        shaderFilenames[hash] = file.path().string();
                    }

                    i += dataSize;
                }
                else
                {
                    i += sizeof(uint32_t);
                }
            }

            if (foundAny)
                files.emplace_back(std::move(fileData));
        }

        std::mutex shaderQueueMutex;
        std::deque<XXH64_hash_t> shaderQueue;
        for (const auto& [hash, _] : shaders)
        {
            shaderQueue.emplace_back(hash);
        }

        const uint32_t numThreads = std::max(std::thread::hardware_concurrency(), 1u);
        fmt::println("Recompiling shaders with {} threads", numThreads);

        std::atomic<uint32_t> progress = 0;
        std::vector<std::thread> threads;
        threads.reserve(numThreads);
        for (uint32_t i = 0; i < numThreads; i++)
        {
            threads.emplace_back([&]
            {
                while (true)
                {
                    XXH64_hash_t shaderHash;
                    {
                        std::lock_guard lock(shaderQueueMutex);
                        if (shaderQueue.empty()) {
                            return;
                        }
                        shaderHash = shaderQueue.front();
                        shaderQueue.pop_front();
                    }
                    RecompiledShader& shader = shaders[shaderHash];
                    recompileShader(shader, include);
                    reportProgress(++progress, shaders.size());
                }
            });
        }
        for (auto& thread : threads)
        {
            thread.join();
        }

        size_t failedCount = 0;
        for (auto& [hash, shader] : shaders)
        {
            if (!shader.error.empty())
            {
                fmt::println("Skipped {:016X} ({}): {}", hash, shaderFilenames[hash], shader.error);
                failedCount++;
            }
        }
        fmt::println("{} of {} shaders translated", shaders.size() - failedCount, shaders.size());

        size_t dxilFailedCount = 0;
        for (auto& [hash, shader] : shaders)
        {
            if (shader.error.empty() && !shader.dxilError.empty())
            {
                fmt::println("No DXIL for {:016X} ({}): {}", hash, shaderFilenames[hash], shader.dxilError);
                dxilFailedCount++;
            }
        }
#ifdef XENOS_RECOMP_DXIL
        fmt::println("{} of them without DXIL", dxilFailedCount);
#endif

        fmt::println("Creating shader cache...");

        StringBuffer f;
        f.println("#include \"shader_cache.h\"");
        f.println("ShaderCacheEntry g_shaderCacheEntries[] = {{");

        std::vector<uint8_t> dxil;
        std::vector<uint8_t> spirv;
        std::vector<uint8_t> air;

        for (auto& [hash, shader] : shaders)
        {
            if (!shader.error.empty())
                continue;

            const std::string& fullFilename = shaderFilenames[hash];
            std::string filename = fullFilename;
            size_t shaderPos = filename.find("shader");
            if (shaderPos != std::string::npos) {
                filename = filename.substr(shaderPos);
                // Prevent bad escape sequences in Windows shader path.
                std::replace(filename.begin(), filename.end(), '\\', '/');
            }
            f.println("\t{{ 0x{:X}, {}, {}, {}, {}, {}, {}, {}, \"{}\" }},",
                hash, dxil.size(), shader.dxil.size(),
                spirv.size(), shader.spirv.size(), air.size(), shader.air.size(), shader.specConstantsMask, filename);

            dxil.insert(dxil.end(), shader.dxil.begin(), shader.dxil.end());

#ifdef XENOS_RECOMP_AIR
            air.insert(air.end(), shader.air.begin(), shader.air.end());
#endif

            spirv.insert(spirv.end(), shader.spirv.begin(), shader.spirv.end());
        }

        f.println("}};");

        int level = ZSTD_maxCLevel();

        // Skylanders: the three caches are compressed (zstd at its highest level, one core each) and written as C
        // arrays side by side; the text is the same as when this ran one after the other.
        auto block = [level](const std::vector<uint8_t>& data, const char* name, const char* label)
        {
            fmt::println("Compressing {} cache...", label);
            fflush(stdout);
            std::vector<uint8_t> compressed(ZSTD_compressBound(data.size()));
            compressed.resize(ZSTD_compress(compressed.data(), compressed.size(), data.data(), data.size(), level));

            StringBuffer text;
            text.print("const uint8_t g_compressed{}Cache[] = {{", name);
            for (auto byte : compressed)
                text.print("{},", byte);
            text.println("}};");
            text.println("const size_t g_{}CacheCompressedSize = {};", label_lower(name), compressed.size());
            text.println("const size_t g_{}CacheDecompressedSize = {};", label_lower(name), data.size());
            return text.out;
        };

#ifdef XENOS_RECOMP_DXIL
        auto dxilText = std::async(std::launch::async, block, std::cref(dxil), "Dxil", "DXIL");
#endif
#ifdef XENOS_RECOMP_AIR
        auto airText = std::async(std::launch::async, block, std::cref(air), "Air", "AIR");
#endif
        auto spirvText = std::async(std::launch::async, block, std::cref(spirv), "Spirv", "SPIRV");

#ifdef XENOS_RECOMP_DXIL
        // Skylanders: only Windows builds use the DXIL (D3D12); the others would carry it for nothing.
        f.println("#if defined(_WIN32)");
        f.out += dxilText.get();
        f.println("#endif");
#endif
#ifdef XENOS_RECOMP_AIR
        f.out += airText.get();
#endif
        f.out += spirvText.get();
        f.println("const size_t g_shaderCacheEntryCount = {};", shaders.size() - failedCount);

        writeAllBytes(output, f.out.data(), f.out.size());
    }
    else
    {
        ShaderRecompiler recompiler;
        size_t fileSize;
        recompiler.recompile(readAllBytes(input, fileSize).get(), include);
        writeAllBytes(output, recompiler.out.data(), recompiler.out.size());
    }

    return 0;
}
