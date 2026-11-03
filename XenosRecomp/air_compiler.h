#pragma once

#include <string>
#include <vector>

class AirCompiler
{
public:
    [[nodiscard]] static std::vector<uint8_t> compile(const std::string& shaderSource);

    // Skylanders: Metal's options (the shader cache's key).
    static std::string signature();

    // Skylanders: shader_common.h, which every translated shader starts with (see compile).
    static inline std::string commonHeader;
};