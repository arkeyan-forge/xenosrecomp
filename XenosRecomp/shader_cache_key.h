#pragma once

#include <cstdint>

// Skylanders: whether a shader cache entry written with `fileKey` serves a run whose compilers and arguments hash to
// `configuration` (low 12 bits clear) and that needs `streams`. A key is configuration | 0x100 | the entry's streams;
// older formats (1, or 0x100 | streams, with no configuration) never match.
inline bool CacheKeyCovers(uint64_t fileKey, uint64_t configuration, uint32_t streams)
{
    return (fileKey & ~0xFFFull) == configuration && (fileKey & 0xF00) == 0x100 && (fileKey & streams) == streams;
}
