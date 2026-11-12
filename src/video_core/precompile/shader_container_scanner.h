// SPDX-FileCopyrightText: Copyright 2026 Dalia Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <cstddef>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "common/common_types.h"
#include "core/file_sys/vfs/vfs_types.h"

namespace Core {
class System;
}

namespace VideoCore::Precompile {

enum class ScannedStage : u8 {
    Vertex = 0,
    TessellationControl = 1,
    TessellationEval = 2,
    Geometry = 3,
    Fragment = 4,
    Compute = 5,
    Task = 6,
    Mesh = 7,
};

struct ScannedShader {
    u64 key{};
    std::vector<u8> program_bytes;
    std::vector<u8> c1_bytes;
    std::optional<std::array<u8, 0x50>> sph_bytes;
    ScannedStage stage{ScannedStage::Vertex};
    bool is_native_sm86{};
    std::array<u32, 3> local_size{1, 1, 1};
    std::string source_name;
};

struct ScanOptions {
    bool include_large_ndsr_compute{false};
    u32 max_ndsr_compute_bytes{16384};
    std::size_t max_decompressed_size{512ULL * 1024 * 1024};
    std::function<bool()> should_stop;
};

struct ScanStats {
    std::size_t files_scanned{};
    std::size_t sarc_archives{};
    std::size_t zstd_entries{};
    std::size_t zstd_dicts_loaded{};
    std::size_t fsha_containers{};
    std::size_t bnsh_containers{};
    std::size_t sharcb_containers{};
    std::size_t ndsr_containers{};
    std::size_t nso_modules{};
    std::size_t duplicates_skipped{};
    std::size_t errors_ignored{};
};

struct SarcEntry {
    std::string name;
    std::span<const u8> data;
};

/// Parses an in-memory Nintendo SARC archive without throwing or reading out of bounds.
[[nodiscard]] std::optional<std::vector<SarcEntry>> ParseSarc(std::span<const u8> blob);

/// Holds Zstandard dictionaries extracted from RomFS dictionary packs (e.g. Pack/ZsDic.pack.zs).
class ZstdDictionaryStore {
public:
    /// Registers a raw Zstandard dictionary blob by its embedded dictionary ID.
    bool AddDictionary(std::span<const u8> dict_bytes);

    /// Registers all dictionary files stored inside an uncompressed SARC archive.
    std::size_t AddFromSarc(std::span<const u8> sarc_blob);

    /// Searches a RomFS directory tree for dictionary packs and loads all valid dictionaries.
    std::size_t AddFromDirectory(const FileSys::VirtualDir& romfs_root,
                                 std::size_t max_pack_size = 64ULL * 1024 * 1024);

    /// Attempts to decompress a single Zstandard frame using the matching dictionary if required.
    [[nodiscard]] std::optional<std::vector<u8>> TryDecompress(std::span<const u8> compressed,
                                                               std::size_t max_output_size) const;

    [[nodiscard]] std::size_t Size() const noexcept {
        return dicts.size();
    }

private:
    std::unordered_map<u32, std::vector<u8>> dicts;
};

/// Parses a `.bfsha` (`FSHA`) archive and all embedded `BNSH` shader programs.
void ParseFsha(std::span<const u8> blob, std::string_view source_name,
               std::vector<ScannedShader>& out, ScanStats* stats = nullptr);

/// Parses a BNSH shader container (supporting both NVN1 Maxwell and NVN2 SM86 programs).
void ParseBnsh(std::span<const u8> blob, std::string_view source_name,
               std::vector<ScannedShader>& out, ScanStats* stats = nullptr);

/// Parses a `.sharcb` (`BAHS`) shader archive.
void ParseSharcb(std::span<const u8> blob, std::string_view source_name,
                 std::vector<ScannedShader>& out, ScanStats* stats = nullptr);

/// Parses an `NDSR` native shader container.
void ParseNdsr(std::span<const u8> blob, std::string_view source_name, const ScanOptions& options,
               std::vector<ScannedShader>& out, ScanStats* stats = nullptr);

/// Decompresses NSO0 segments and scans them for embedded `BNSH` containers.
void ScanNso(std::span<const u8> blob, std::string_view source_name,
             std::vector<ScannedShader>& out, ScanStats* stats = nullptr);

/// Scans an arbitrary container blob (`BNSH`, `BAHS`, `NDSR`, `NSO0`, `SARC`, or Zstd).
void ScanContainerBlob(std::span<const u8> blob, std::string_view source_name,
                       const ZstdDictionaryStore& dict_store, const ScanOptions& options,
                       std::vector<ScannedShader>& out, ScanStats* stats = nullptr,
                       int max_depth = 4);

/// Computes a fast fingerprint of the title's patched ExeFS and RomFS layout without decompressing archives.
[[nodiscard]] u64 ComputeTitleContentFingerprint(Core::System& system, u64 title_id);

/// Scans explicit ExeFS and RomFS virtual directories for shaders and deduplicates results.
[[nodiscard]] std::vector<ScannedShader> ScanDirectoryTrees(const FileSys::VirtualDir& exefs,
                                                            const FileSys::VirtualDir& romfs,
                                                            u64 title_id = 0,
                                                            const ScanOptions& options = {},
                                                            ScanStats* out_stats = nullptr,
                                                            u64* out_content_fingerprint = nullptr);

/// Scans patched ExeFS and RomFS directories of the currently loaded title and deduplicates shaders.
[[nodiscard]] std::vector<ScannedShader> ScanTitleContainers(Core::System& system, u64 title_id,
                                                             const ScanOptions& options = {},
                                                             ScanStats* out_stats = nullptr,
                                                             u64* out_content_fingerprint = nullptr);

} // namespace VideoCore::Precompile
