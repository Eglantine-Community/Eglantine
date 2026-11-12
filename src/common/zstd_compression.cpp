// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2019 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <memory>
#include <zstd.h>

#include "common/zstd_compression.h"

namespace Common::Compression {

std::vector<u8> CompressDataZSTD(const u8* source, std::size_t source_size, s32 compression_level) {
    compression_level = std::clamp(compression_level, 1, ZSTD_maxCLevel());

    const std::size_t max_compressed_size = ZSTD_compressBound(source_size);
    std::vector<u8> compressed(max_compressed_size);

    const std::size_t compressed_size =
        ZSTD_compress(compressed.data(), compressed.size(), source, source_size, compression_level);

    if (ZSTD_isError(compressed_size)) {
        // Compression failed
        return {};
    }

    compressed.resize(compressed_size);

    return compressed;
}

std::vector<u8> CompressDataZSTDDefault(const u8* source, std::size_t source_size) {
    return CompressDataZSTD(source, source_size, ZSTD_CLEVEL_DEFAULT);
}

std::vector<u8> DecompressDataZSTD(std::span<const u8> compressed) {
    return TryDecompressDataZSTD(compressed, std::vector<u8>{}.max_size())
        .value_or(std::vector<u8>{});
}

std::optional<std::vector<u8>> TryDecompressDataZSTD(std::span<const u8> compressed,
                                                     std::size_t max_size) {
    // The size comes from the frame header, so check it before allocating
    const unsigned long long decompressed_size =
        ZSTD_getFrameContentSize(compressed.data(), compressed.size());
    if (decompressed_size == ZSTD_CONTENTSIZE_ERROR ||
        decompressed_size == ZSTD_CONTENTSIZE_UNKNOWN || decompressed_size > max_size) {
        return std::nullopt;
    }
    std::vector<u8> decompressed(static_cast<std::size_t>(decompressed_size));

    const std::size_t uncompressed_result_size = ZSTD_decompress(
        decompressed.data(), decompressed.size(), compressed.data(), compressed.size());

    if (ZSTD_isError(uncompressed_result_size) || uncompressed_result_size != decompressed_size) {
        return std::nullopt;
    }
    return decompressed;
}

std::size_t DecompressDataZSTD(std::span<const u8> compressed, std::span<u8> output) {
    const std::size_t result =
        ZSTD_decompress(output.data(), output.size(), compressed.data(), compressed.size());
    return ZSTD_isError(result) ? 0 : result;
}

std::optional<u32> GetZstdDictId(std::span<const u8> compressed) {
    if (compressed.empty()) {
        return std::nullopt;
    }
    const unsigned dict_id = ZSTD_getDictID_fromFrame(compressed.data(), compressed.size());
    if (dict_id == 0) {
        return std::nullopt;
    }
    return static_cast<u32>(dict_id);
}

std::optional<u32> GetZstdDictIdFromDict(std::span<const u8> dict) {
    if (dict.empty()) {
        return std::nullopt;
    }
    const unsigned dict_id = ZSTD_getDictID_fromDict(dict.data(), dict.size());
    if (dict_id == 0) {
        return std::nullopt;
    }
    return static_cast<u32>(dict_id);
}

std::optional<std::vector<u8>> TryDecompressDataZSTDStream(std::span<const u8> compressed,
                                                           std::size_t max_size,
                                                           std::span<const u8> dict) {
    if (compressed.empty()) {
        return std::nullopt;
    }
    const unsigned long long known_size =
        ZSTD_getFrameContentSize(compressed.data(), compressed.size());
    if (known_size != ZSTD_CONTENTSIZE_UNKNOWN) {
        if (known_size == ZSTD_CONTENTSIZE_ERROR || known_size > max_size) {
            return std::nullopt;
        }
        if (dict.empty()) {
            return TryDecompressDataZSTD(compressed, max_size);
        }
    }

    struct DCtxDeleter {
        void operator()(ZSTD_DCtx* ctx) const noexcept {
            ZSTD_freeDCtx(ctx);
        }
    };
    const std::unique_ptr<ZSTD_DCtx, DCtxDeleter> dctx(ZSTD_createDCtx());
    if (!dctx) {
        return std::nullopt;
    }
    if (!dict.empty()) {
        const std::size_t load_rc = ZSTD_DCtx_loadDictionary(dctx.get(), dict.data(), dict.size());
        if (ZSTD_isError(load_rc)) {
            return std::nullopt;
        }
    }

    std::vector<u8> output;
    if (known_size != ZSTD_CONTENTSIZE_UNKNOWN && known_size != ZSTD_CONTENTSIZE_ERROR) {
        output.reserve(static_cast<std::size_t>(known_size));
    }

    ZSTD_inBuffer in{compressed.data(), compressed.size(), 0};
    std::array<u8, 64 * 1024> chunk{};
    std::size_t last_rc = 0;
    while (in.pos < in.size || last_rc > 0) {
        ZSTD_outBuffer out{chunk.data(), chunk.size(), 0};
        const std::size_t rc = ZSTD_decompressStream(dctx.get(), &out, &in);
        if (ZSTD_isError(rc)) {
            return std::nullopt;
        }
        if (out.pos > max_size - output.size()) {
            return std::nullopt;
        }
        output.insert(output.end(), chunk.data(), chunk.data() + out.pos);
        last_rc = rc;
        if (in.pos == in.size && out.pos == 0 && rc == 0) {
            break;
        }
        if (in.pos == in.size && out.pos == 0 && rc > 0) {
            return std::nullopt;
        }
    }
    return output;
}

} // namespace Common::Compression
