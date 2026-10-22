#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace water {

inline constexpr char kWaterNormalMapRawfile[] = "WaterNormalMip.bin";
inline constexpr std::uint32_t kWaterNormalMapWidth = 256;
inline constexpr std::uint32_t kWaterNormalMapPeriodCells = 32;
inline constexpr std::uint32_t kWaterNormalMapMipLevels = 9;

// The offline baker writes a small header followed by a toroidally filtered
// RGBA8 mip chain. RG and BA each encode an independent periodic noise gradient.
inline bool DecodeWaterNormalMipChain(const void* encoded, std::size_t encodedSize,
    std::vector<std::vector<unsigned char>>& levels)
{
    constexpr std::size_t kHeaderSize = 20;
    const auto* bytes = static_cast<const unsigned char*>(encoded);
    if (bytes == nullptr || encodedSize < kHeaderSize ||
        std::memcmp(bytes, "WNRM", 4) != 0) {
        return false;
    }

    const auto readU32 = [](const unsigned char* value) {
        return static_cast<std::uint32_t>(value[0]) |
            (static_cast<std::uint32_t>(value[1]) << 8U) |
            (static_cast<std::uint32_t>(value[2]) << 16U) |
            (static_cast<std::uint32_t>(value[3]) << 24U);
    };
    if (readU32(bytes + 4) != 1 || readU32(bytes + 8) != kWaterNormalMapWidth ||
        readU32(bytes + 12) != kWaterNormalMapPeriodCells ||
        readU32(bytes + 16) != kWaterNormalMapMipLevels) {
        return false;
    }

    std::vector<std::vector<unsigned char>> decoded;
    decoded.reserve(kWaterNormalMapMipLevels);
    std::size_t offset = kHeaderSize;
    std::uint32_t width = kWaterNormalMapWidth;
    for (std::uint32_t level = 0; level < kWaterNormalMapMipLevels; ++level) {
        const std::size_t levelSize = static_cast<std::size_t>(width) * width * 4U;
        if (offset > encodedSize || levelSize > encodedSize - offset) {
            return false;
        }
        decoded.emplace_back(bytes + offset, bytes + offset + levelSize);
        offset += levelSize;
        width = width > 1 ? width / 2 : 1;
    }
    if (offset != encodedSize) {
        return false;
    }

    levels.swap(decoded);
    return true;
}

} // namespace water
