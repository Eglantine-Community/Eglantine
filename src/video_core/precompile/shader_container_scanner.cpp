// SPDX-FileCopyrightText: Copyright 2026 Dalia Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <string>
#include <unordered_set>

#include <fmt/format.h>

#include "common/cityhash.h"
#include "common/lz4_compression.h"
#include "common/zstd_compression.h"
#include "core/core.h"
#include "core/file_sys/content_archive.h"
#include "core/file_sys/nca_metadata.h"
#include "core/file_sys/patch_manager.h"
#include "core/file_sys/registered_cache.h"
#include "core/file_sys/romfs.h"
#include "core/file_sys/vfs/vfs.h"
#include "core/loader/loader.h"
#include "shader_recompiler/program_header.h"
#include "video_core/precompile/shader_container_scanner.h"

namespace VideoCore::Precompile {

namespace {

constexpr u32 NvnControlMagic = 0x98761234U;
constexpr u32 ZstdFrameMagicLE = 0xFD2FB528U;

[[nodiscard]] bool HasMagic(std::span<const u8> data, std::size_t offset, std::string_view magic) {
    if (offset > data.size() || data.size() - offset < magic.size()) {
        return false;
    }
    return std::memcmp(data.data() + offset, magic.data(), magic.size()) == 0;
}

[[nodiscard]] bool IsKnownNonShaderExtension(std::string_view lower_name) {
    static constexpr std::array<std::string_view, 54> IgnoredSuffixes{{
        ".bwav",       ".bfwav",     ".bfstm",     ".bfsar",      ".bars",      ".extm",
        ".hght",       ".mate",      ".bgyml",     ".byml",       ".ainb",      ".bphcl",
        ".hkcl",       ".phsh",      ".tscb",      ".webm",       ".mc",        ".txt",
        ".bfres",      ".bntx",      ".bflan",     ".bflim",      ".bffnt",     ".bfgrp",
        ".bcul",       ".bkprb",     ".bkres",     ".bphsh",      ".bphsc",     ".bphnm",
        ".asb",        ".baev",      ".bfevfl",    ".bpvsts",     ".flnt",      ".bklight",
        ".bkprcb",     ".bkprmb",    ".bbox",      ".bfotf",      ".bfttf",     ".aamp",
        ".bgapkginfo", ".bgapkglist",".bptclconf", ".bgmsconf",   ".genvb",     ".rsizetable",
        ".png",        ".jpg",       ".dds",       ".astc",       ".npdm",      ".zsdic",
    }};
    std::string_view stem = lower_name;
    if (stem.ends_with(".zs")) {
        stem.remove_suffix(3);
    }
    for (const std::string_view suffix : IgnoredSuffixes) {
        if (stem.ends_with(suffix)) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] bool ReadU16(std::span<const u8> data, std::size_t offset, bool big_endian, u16& out) {
    if (offset > data.size() || data.size() - offset < sizeof(u16)) {
        return false;
    }
    const auto* p = data.data() + offset;
    if (big_endian) {
        out = static_cast<u16>((static_cast<u16>(p[0]) << 8) | static_cast<u16>(p[1]));
    } else {
        out = static_cast<u16>(static_cast<u16>(p[0]) | (static_cast<u16>(p[1]) << 8));
    }
    return true;
}

[[nodiscard]] bool ReadU32(std::span<const u8> data, std::size_t offset, bool big_endian, u32& out) {
    if (offset > data.size() || data.size() - offset < sizeof(u32)) {
        return false;
    }
    const auto* p = data.data() + offset;
    if (big_endian) {
        out = (static_cast<u32>(p[0]) << 24) | (static_cast<u32>(p[1]) << 16) |
              (static_cast<u32>(p[2]) << 8) | static_cast<u32>(p[3]);
    } else {
        out = static_cast<u32>(p[0]) | (static_cast<u32>(p[1]) << 8) |
              (static_cast<u32>(p[2]) << 16) | (static_cast<u32>(p[3]) << 24);
    }
    return true;
}

[[nodiscard]] bool ReadI32(std::span<const u8> data, std::size_t offset, bool big_endian, s32& out) {
    u32 raw = 0;
    if (!ReadU32(data, offset, big_endian, raw)) {
        return false;
    }
    out = static_cast<s32>(raw);
    return true;
}

[[nodiscard]] bool ReadU64(std::span<const u8> data, std::size_t offset, u64& out) {
    if (offset > data.size() || data.size() - offset < sizeof(u64)) {
        return false;
    }
    std::memcpy(&out, data.data() + offset, sizeof(u64));
    return true;
}

[[nodiscard]] std::string ToLowerAscii(std::string_view input) {
    std::string out(input);
    for (char& ch : out) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return out;
}

[[nodiscard]] u64 ComputeShaderKey(ScannedStage stage, bool is_native_sm86,
                                   std::span<const u8> program_bytes,
                                   std::span<const u8> c1_bytes,
                                   const std::optional<std::array<u8, 0x50>>& sph_bytes,
                                   std::array<u32, 3> local_size) {
    u64 seed = Common::CityHash64(reinterpret_cast<const char*>(program_bytes.data()),
                                  program_bytes.size());
    seed ^= static_cast<u64>(stage) * 0x9E3779B97F4A7C15ULL;
    if (is_native_sm86) {
        seed ^= 0xA5A5A5A55A5A5A5AULL;
    }
    if (!c1_bytes.empty()) {
        seed ^= Common::CityHash64(reinterpret_cast<const char*>(c1_bytes.data()),
                                   c1_bytes.size()) *
                0xBF58476D1CE4E5B9ULL;
    }
    if (sph_bytes) {
        seed ^= Common::CityHash64(reinterpret_cast<const char*>(sph_bytes->data()),
                                   sph_bytes->size()) *
                0x94D049BB133111EBULL;
    }
    if (stage == ScannedStage::Compute) {
        seed ^= (static_cast<u64>(local_size[0]) << 1) ^
                (static_cast<u64>(local_size[1]) << 21) ^
                (static_cast<u64>(local_size[2]) << 42);
    }
    return seed;
}

[[nodiscard]] std::optional<ScannedStage> MapNvnStage(u32 raw_stage) {
    switch (raw_stage) {
    case 0:
        return ScannedStage::Vertex;
    case 1:
        return ScannedStage::Fragment;
    case 2:
        return ScannedStage::Geometry;
    case 3:
        return ScannedStage::TessellationControl;
    case 4:
        return ScannedStage::TessellationEval;
    case 5:
        return ScannedStage::Compute;
    case 6:
        return ScannedStage::Task;
    case 7:
        return ScannedStage::Mesh;
    default:
        return std::nullopt;
    }
}

[[nodiscard]] bool LooksLikeSph(std::span<const u8> span) {
    if (span.size() < sizeof(Shader::ProgramHeader)) {
        return false;
    }
    Shader::ProgramHeader sph{};
    std::memcpy(&sph, span.data(), sizeof(sph));
    const u32 sph_type = sph.common0.sph_type.Value();
    const u32 version = sph.common0.version.Value();
    return (sph_type == 1 || sph_type == 2) && (version == 3 || version == 4);
}

[[nodiscard]] std::optional<ScannedShader> ExtractFromControlAndCode(
    std::span<const u8> control, std::span<const u8> code, std::string_view source_name,
    std::optional<ScannedStage> slot_hint = std::nullopt) {
    if (control.size() < 0xe0) {
        return std::nullopt;
    }
    u32 magic = 0;
    if (!ReadU32(control, 0, false, magic) || magic != NvnControlMagic) {
        return std::nullopt;
    }

    ScannedShader result;
    result.source_name = std::string(source_name);

    if (control.size() < 0x880) {
        // NVN2 (Switch 2 Ampere / Ada SM86 native binary)
        u32 raw_stage_word = 0;
        u32 reported_size = 0;
        u32 c1_start = 0;
        u32 c1_end = 0;
        if (!ReadU32(control, 0x54, false, raw_stage_word) ||
            !ReadU32(control, 0x38, false, reported_size) ||
            !ReadU32(control, 0x40, false, c1_start) ||
            !ReadU32(control, 0x44, false, c1_end)) {
            return std::nullopt;
        }
        const auto stage = MapNvnStage(raw_stage_word >> 16);
        if (!stage) {
            return std::nullopt;
        }
        result.stage = *stage;
        result.is_native_sm86 = true;

        std::span<const u8> instruction_span;
        if (*stage != ScannedStage::Compute) {
            const bool has_sph_at_0 = code.size() >= 0x50 && LooksLikeSph(code.subspan(0, 0x50));
            const bool has_sph_at_30 =
                code.size() >= 0x80 && LooksLikeSph(code.subspan(0x30, 0x50));
            if (code.size() >= 0x80 + 16U && (has_sph_at_0 || has_sph_at_30)) {
                std::array<u8, 0x50> sph{};
                const std::size_t sph_off = has_sph_at_0 ? 0 : 0x30;
                std::memcpy(sph.data(), code.data() + sph_off, sph.size());
                result.sph_bytes = sph;
                const std::size_t avail_inst = ((code.size() - 0x80) / 16U) * 16U;
                std::size_t inst_size = avail_inst;
                if (reported_size > 0 && (reported_size % 16U) == 0) {
                    if (reported_size == code.size() || reported_size > avail_inst) {
                        if (reported_size > 0x80 && (reported_size - 0x80) <= avail_inst) {
                            inst_size = reported_size - 0x80;
                        }
                    } else {
                        inst_size = reported_size;
                    }
                }
                instruction_span = code.subspan(0x80, inst_size);
            } else if (reported_size > 0 && reported_size <= code.size() &&
                       (reported_size % 16U) == 0) {
                instruction_span = code.subspan(0, reported_size);
            } else if (!code.empty() && (code.size() % 16U) == 0) {
                instruction_span = code;
            } else if (code.size() > 0x80 && ((code.size() - 0x80) % 16U) == 0) {
                instruction_span = code.subspan(0x80);
            }
        } else {
            if (reported_size > 0 && reported_size <= code.size() &&
                (reported_size % 16U) == 0) {
                instruction_span = code.subspan(0, reported_size);
            } else if (!code.empty() && (code.size() % 16U) == 0) {
                instruction_span = code;
            }
        }
        if (instruction_span.empty() || (instruction_span.size() % 16U) != 0) {
            return std::nullopt;
        }
        result.program_bytes.assign(instruction_span.begin(), instruction_span.end());
        if (c1_end > c1_start && c1_end <= control.size()) {
            const auto c1_span = control.subspan(c1_start, c1_end - c1_start);
            result.c1_bytes.assign(c1_span.begin(), c1_span.end());
        }
    } else {
        // NVN1 (Switch 1 Maxwell binary)
        u32 raw_stage = 0;
        u32 local_x = 1;
        u32 local_y = 1;
        u32 local_z = 1;
        if (!ReadU32(control, 0x714, false, raw_stage) ||
            !ReadU32(control, 0x748, false, local_x) ||
            !ReadU32(control, 0x74C, false, local_y) ||
            !ReadU32(control, 0x750, false, local_z)) {
            return std::nullopt;
        }
        auto stage = MapNvnStage(raw_stage);
        if (!stage) {
            stage = slot_hint;
        }
        if (!stage) {
            return std::nullopt;
        }
        result.stage = *stage;
        result.is_native_sm86 = false;
        if (*stage == ScannedStage::Compute) {
            result.local_size = {
                (std::max)(local_x, 1U),
                (std::max)(local_y, 1U),
                (std::max)(local_z, 1U),
            };
            if (code.size() < 8) {
                return std::nullopt;
            }
            const std::size_t aligned = (code.size() / 8U) * 8U;
            result.program_bytes.assign(code.data(), code.data() + aligned);
        } else {
            // Graphics NVN1 shaders prepend a 0x30-byte NVN header before the 0x50-byte SPH.
            if (code.size() < 0x30 + 0x50 + 8) {
                return std::nullopt;
            }
            const auto body = code.subspan(0x30);
            const std::size_t aligned = (body.size() / 8U) * 8U;
            if (aligned < 0x58) {
                return std::nullopt;
            }
            result.program_bytes.assign(body.data(), body.data() + aligned);
            std::array<u8, 0x50> sph{};
            std::memcpy(sph.data(), result.program_bytes.data(), sph.size());
            result.sph_bytes = sph;
        }
    }

    result.key = ComputeShaderKey(result.stage, result.is_native_sm86, result.program_bytes,
                                  result.c1_bytes, result.sph_bytes, result.local_size);
    return result;
}

void DeduplicateAndAppend(std::vector<ScannedShader>& dst, std::vector<ScannedShader>&& src,
                          std::unordered_set<u64>& seen_keys, ScanStats* stats) {
    for (auto& shader : src) {
        if (seen_keys.insert(shader.key).second) {
            dst.push_back(std::move(shader));
        } else if (stats) {
            ++stats->duplicates_skipped;
        }
    }
}

[[nodiscard]] FileSys::VirtualDir ResolvePatchedRomfs(Core::System& system, u64 title_id) {
    auto& loader = system.GetAppLoader();
    FileSys::VirtualFile romfs_file;
    if (loader.ReadRomFS(romfs_file) != Loader::ResultStatus::Success || !romfs_file) {
        return nullptr;
    }

    if (loader.GetFileType() != Loader::FileType::DeconstructedRomDirectory) {
        FileSys::VirtualFile packed_update_raw;
        (void)loader.ReadUpdateRaw(packed_update_raw);

        const auto& fsc = system.GetFileSystemController();
        const auto& content_provider = system.GetContentProvider();
        const FileSys::PatchManager patch_manager{title_id, fsc, content_provider};

        std::shared_ptr<FileSys::NCA> base_nca;
        if (loader.IsRomFSUpdatable()) {
            base_nca = content_provider.GetEntry(title_id, FileSys::ContentRecordType::Program);
        }
        auto patched = patch_manager.PatchRomFS(
            base_nca.get(), romfs_file, FileSys::ContentRecordType::Program,
            loader.IsRomFSUpdatable() ? packed_update_raw : nullptr);
        if (patched) {
            romfs_file = std::move(patched);
        }
    }

    return FileSys::ExtractRomFS(std::move(romfs_file));
}

[[nodiscard]] FileSys::VirtualDir ResolvePatchedExefs(Core::System& system, u64 title_id) {
    auto& loader = system.GetAppLoader();
    FileSys::VirtualDir exefs_dir;
    if (loader.ReadExeFS(exefs_dir) != Loader::ResultStatus::Success || !exefs_dir) {
        return nullptr;
    }

    if (loader.IsRomFSUpdatable() &&
        loader.GetFileType() != Loader::FileType::DeconstructedRomDirectory) {
        const FileSys::PatchManager patch_manager{title_id, system.GetFileSystemController(),
                                                  system.GetContentProvider()};
        if (auto patched = patch_manager.PatchExeFS(exefs_dir)) {
            exefs_dir = std::move(patched);
        }
    }
    return exefs_dir;
}

void UpdateFingerprintForDir(const FileSys::VirtualDir& dir, u64& hash, int max_depth) {
    if (!dir || max_depth < 0) {
        return;
    }
    std::vector<FileSys::VirtualFile> files = dir->GetFiles();
    std::ranges::sort(files, [](const auto& a, const auto& b) {
        return a->GetName() < b->GetName();
    });
    for (const auto& f : files) {
        if (!f) {
            continue;
        }
        const auto name = f->GetName();
        const u64 fhash = Common::CityHash64(name.data(), name.size());
        const u64 fsize = static_cast<u64>(f->GetSize());
        hash ^= fhash + 0x9E3779B97F4A7C15ULL + (hash << 6) + (hash >> 2);
        hash ^= fsize * 0xBF58476D1CE4E5B9ULL;
    }
    std::vector<FileSys::VirtualDir> subdirs = dir->GetSubdirectories();
    std::ranges::sort(subdirs, [](const auto& a, const auto& b) {
        return a->GetName() < b->GetName();
    });
    for (const auto& sub : subdirs) {
        if (!sub) {
            continue;
        }
        const auto name = sub->GetName();
        hash ^= Common::CityHash64(name.data(), name.size());
        UpdateFingerprintForDir(sub, hash, max_depth - 1);
    }
}

} // Anonymous namespace

std::optional<std::vector<SarcEntry>> ParseSarc(std::span<const u8> blob) {
    if (blob.size() < 20 || !HasMagic(blob, 0, "SARC")) {
        return std::nullopt;
    }
    const u8 bom0 = blob[6];
    const u8 bom1 = blob[7];
    bool big_endian = false;
    if (bom0 == 0xFF && bom1 == 0xFE) {
        big_endian = false;
    } else if (bom0 == 0xFE && bom1 == 0xFF) {
        big_endian = true;
    } else {
        return std::nullopt;
    }

    u16 header_size = 0;
    u32 file_size = 0;
    u32 data_start = 0;
    if (!ReadU16(blob, 4, big_endian, header_size) ||
        !ReadU32(blob, 8, big_endian, file_size) ||
        !ReadU32(blob, 12, big_endian, data_start)) {
        return std::nullopt;
    }
    if (header_size < 20 || file_size > blob.size() || data_start > file_size ||
        static_cast<std::size_t>(header_size) + 12 > data_start) {
        return std::nullopt;
    }
    if (!HasMagic(blob, header_size, "SFAT")) {
        return std::nullopt;
    }
    u16 node_count = 0;
    if (!ReadU16(blob, static_cast<std::size_t>(header_size) + 6, big_endian, node_count)) {
        return std::nullopt;
    }
    const std::size_t sfat_entries = static_cast<std::size_t>(header_size) + 12;
    const std::size_t sfat_bytes = static_cast<std::size_t>(node_count) * 16U;
    if (sfat_entries > data_start || sfat_bytes > data_start - sfat_entries) {
        return std::nullopt;
    }
    const std::size_t sfnt_start = sfat_entries + sfat_bytes;
    if (data_start - sfnt_start < 8 || !HasMagic(blob, sfnt_start, "SFNT")) {
        return std::nullopt;
    }
    const std::size_t str_table_start = sfnt_start + 8;

    std::vector<SarcEntry> entries;
    entries.reserve(node_count);
    for (u16 i = 0; i < node_count; ++i) {
        const std::size_t entry_off = sfat_entries + static_cast<std::size_t>(i) * 16U;
        u32 hash = 0;
        u32 attr = 0;
        u32 lo = 0;
        u32 hi = 0;
        if (!ReadU32(blob, entry_off + 0, big_endian, hash) ||
            !ReadU32(blob, entry_off + 4, big_endian, attr) ||
            !ReadU32(blob, entry_off + 8, big_endian, lo) ||
            !ReadU32(blob, entry_off + 12, big_endian, hi)) {
            return std::nullopt;
        }
        if (lo > hi || static_cast<u64>(data_start) + hi > file_size) {
            return std::nullopt;
        }
        std::string name;
        if ((attr >> 24) == 1U) {
            const std::size_t rel = static_cast<std::size_t>(attr & 0x00FFFFFFU) * 4U;
            if (rel <= data_start - str_table_start) {
                const std::size_t start = str_table_start + rel;
                std::size_t end = start;
                while (end < data_start && blob[end] != 0) {
                    ++end;
                }
                name.assign(reinterpret_cast<const char*>(blob.data() + start), end - start);
            }
        }
        if (name.empty()) {
            name = fmt::format("{:08x}", hash);
        }
        entries.push_back(SarcEntry{
            .name = std::move(name),
            .data = blob.subspan(static_cast<std::size_t>(data_start) + lo, hi - lo),
        });
    }
    return entries;
}

bool ZstdDictionaryStore::AddDictionary(std::span<const u8> dict_bytes) {
    const auto id = Common::Compression::GetZstdDictIdFromDict(dict_bytes);
    if (!id || *id == 0) {
        return false;
    }
    dicts[*id] = std::vector<u8>(dict_bytes.begin(), dict_bytes.end());
    return true;
}

std::size_t ZstdDictionaryStore::AddFromSarc(std::span<const u8> sarc_blob) {
    const auto entries = ParseSarc(sarc_blob);
    if (!entries) {
        return 0;
    }
    std::size_t loaded = 0;
    for (const auto& entry : *entries) {
        if (AddDictionary(entry.data)) {
            ++loaded;
        }
    }
    return loaded;
}

std::size_t ZstdDictionaryStore::AddFromDirectory(const FileSys::VirtualDir& romfs_root,
                                                  std::size_t max_pack_size) {
    if (!romfs_root) {
        return 0;
    }
    std::size_t loaded = 0;
    std::vector<FileSys::VirtualDir> stack{romfs_root};
    while (!stack.empty()) {
        auto dir = std::move(stack.back());
        stack.pop_back();
        if (!dir) {
            continue;
        }
        for (const auto& sub : dir->GetSubdirectories()) {
            if (sub) {
                stack.push_back(sub);
            }
        }
        for (const auto& file : dir->GetFiles()) {
            if (!file) {
                continue;
            }
            const auto lower = ToLowerAscii(file->GetName());
            if (lower.find("zsdic") == std::string::npos) {
                continue;
            }
            const std::size_t fsize = file->GetSize();
            if (fsize == 0 || fsize > max_pack_size) {
                continue;
            }
            auto bytes = file->ReadAllBytes();
            if (HasMagic(bytes, 0, "SARC")) {
                loaded += AddFromSarc(bytes);
                continue;
            }
            u32 magic = 0;
            if (ReadU32(bytes, 0, false, magic) && magic == ZstdFrameMagicLE) {
                const auto decompressed =
                    Common::Compression::TryDecompressDataZSTDStream(bytes, max_pack_size);
                if (decompressed) {
                    if (HasMagic(*decompressed, 0, "SARC")) {
                        loaded += AddFromSarc(*decompressed);
                    } else if (AddDictionary(*decompressed)) {
                        ++loaded;
                    }
                }
                continue;
            }
            if (AddDictionary(bytes)) {
                ++loaded;
            }
        }
    }
    return loaded;
}

std::optional<std::vector<u8>> ZstdDictionaryStore::TryDecompress(
    std::span<const u8> compressed, std::size_t max_output_size) const {
    u32 magic = 0;
    if (!ReadU32(compressed, 0, false, magic) || magic != ZstdFrameMagicLE) {
        return std::nullopt;
    }
    const auto dict_id = Common::Compression::GetZstdDictId(compressed);
    std::span<const u8> dict_span;
    if (dict_id) {
        const auto it = dicts.find(*dict_id);
        if (it == dicts.end()) {
            return std::nullopt;
        }
        dict_span = it->second;
    }
    return Common::Compression::TryDecompressDataZSTDStream(compressed, max_output_size, dict_span);
}

void ParseBnsh(std::span<const u8> blob, std::string_view source_name,
               std::vector<ScannedShader>& out, ScanStats* stats) {
    if (blob.size() < 140 || !HasMagic(blob, 0, "BNSH")) {
        return;
    }
    if (blob[12] != 0xFF || blob[13] != 0xFE) {
        return;
    }
    u32 declared_size = 0;
    if (!ReadU32(blob, 28, false, declared_size) || declared_size < 140 ||
        declared_size > blob.size()) {
        return;
    }
    if (!HasMagic(blob, 96, "grsc")) {
        return;
    }
    u32 variation_count = 0;
    u64 variations_off = 0;
    if (!ReadU32(blob, 124, false, variation_count) || !ReadU64(blob, 128, variations_off)) {
        return;
    }
    if (variations_off < 136 || variations_off > blob.size() ||
        variation_count > (blob.size() - variations_off) / 64U) {
        return;
    }

    if (stats) {
        ++stats->bnsh_containers;
    }

    static constexpr std::array<std::pair<u32, ScannedStage>, 11> Slots{{
        {8, ScannedStage::Vertex},
        {16, ScannedStage::TessellationControl},
        {24, ScannedStage::TessellationEval},
        {32, ScannedStage::Geometry},
        {40, ScannedStage::Fragment},
        {48, ScannedStage::Compute},
        {56, ScannedStage::Task},
        {64, ScannedStage::Mesh},
        {72, ScannedStage::Vertex},
        {80, ScannedStage::Vertex},
        {88, ScannedStage::Vertex},
    }};

    std::unordered_set<u64> seen_recs;
    for (u32 v = 0; v < variation_count; ++v) {
        const std::size_t var_off = static_cast<std::size_t>(variations_off) + static_cast<std::size_t>(v) * 64U;
        u64 program_off = 0;
        if (!ReadU64(blob, var_off + 16, program_off) || program_off == 0 ||
            program_off > blob.size() - 96) {
            continue;
        }
        for (const auto& [slot_rel, slot_stage] : Slots) {
            u64 rec_off = 0;
            if (!ReadU64(blob, static_cast<std::size_t>(program_off) + slot_rel, rec_off) ||
                rec_off == 0 || rec_off > blob.size() - 32) {
                continue;
            }
            if (!seen_recs.insert(rec_off).second) {
                continue;
            }
            u64 control_off = 0;
            u64 code_off = 0;
            u32 code_size = 0;
            u32 control_size = 0;
            if (!ReadU64(blob, static_cast<std::size_t>(rec_off) + 8, control_off) ||
                !ReadU64(blob, static_cast<std::size_t>(rec_off) + 16, code_off) ||
                !ReadU32(blob, static_cast<std::size_t>(rec_off) + 24, false, code_size) ||
                !ReadU32(blob, static_cast<std::size_t>(rec_off) + 28, false, control_size)) {
                continue;
            }
            if (control_off > blob.size() || control_size > blob.size() - control_off ||
                code_off > blob.size() || code_size > blob.size() - code_off) {
                continue;
            }
            const auto control_span =
                blob.subspan(static_cast<std::size_t>(control_off), control_size);
            const auto code_span = blob.subspan(static_cast<std::size_t>(code_off), code_size);
            if (auto shader = ExtractFromControlAndCode(control_span, code_span, source_name,
                                                        slot_stage)) {
                out.push_back(std::move(*shader));
            }
        }
    }
}

void ParseSharcb(std::span<const u8> blob, std::string_view source_name,
                 std::vector<ScannedShader>& out, ScanStats* stats) {
    if (blob.size() < 32 || !HasMagic(blob, 0, "BAHS")) {
        return;
    }
    u32 start = 0;
    u32 pool_size = 0;
    u32 pool = 0;
    if (!ReadU32(blob, 20, false, start) || !ReadU32(blob, 24, false, pool_size) ||
        !ReadU32(blob, 28, false, pool)) {
        return;
    }
    if (start < 32 || start > pool || static_cast<u64>(pool) + pool_size > blob.size()) {
        return;
    }

    if (stats) {
        ++stats->sharcb_containers;
    }

    std::unordered_set<u64> seen_controls;
    const std::size_t pool_end = static_cast<std::size_t>(pool) + pool_size;
    for (std::size_t rec = start; rec + 24 <= pool; rec += 4) {
        const std::size_t at = rec + 8;
        u64 code_off = 0;
        u32 ctrl_size = 0;
        s32 ctrl_rel_off = 0;
        if (!ReadU64(blob, at + 0, code_off) ||
            !ReadU32(blob, at + 8, false, ctrl_size) ||
            !ReadI32(blob, at + 12, false, ctrl_rel_off)) {
            continue;
        }
        if (ctrl_size != 0x880 && ctrl_size != 0x88c && ctrl_size != 0xe0) {
            continue;
        }
        // BAHS v0x0b stores ctrl_rel_off relative to `at` (= rec + 8); older/synthetic layouts
        // may store it relative to `rec`. Check `at` first and fall back to `rec`.
        std::size_t ctrl_at = 0;
        bool ctrl_valid = false;
        for (const std::size_t base : {at, rec}) {
            const s64 candidate = static_cast<s64>(base) + static_cast<s64>(ctrl_rel_off);
            if (candidate < static_cast<s64>(start) ||
                static_cast<u64>(candidate) + ctrl_size > pool) {
                continue;
            }
            const std::size_t off = static_cast<std::size_t>(candidate);
            u32 magic = 0;
            if (ReadU32(blob, off, false, magic) && magic == NvnControlMagic) {
                ctrl_at = off;
                ctrl_valid = true;
                break;
            }
        }
        if (!ctrl_valid) {
            continue;
        }
        if (!seen_controls.insert(ctrl_at).second) {
            continue;
        }

        std::size_t resolved_code_off = 0;
        // Real BAHS records store code_off relative to pool; check pool + code_off first before
        // falling back to file-relative code_off used by some synthetic test layouts.
        if (static_cast<u64>(pool) + code_off < pool_end) {
            resolved_code_off = static_cast<std::size_t>(pool + code_off);
        } else if (code_off >= pool && code_off < pool_end) {
            resolved_code_off = static_cast<std::size_t>(code_off);
        } else {
            continue;
        }

        const std::size_t avail = pool_end - resolved_code_off;
        const auto ctrl_span = blob.subspan(ctrl_at, ctrl_size);
        std::size_t code_len = avail;
        if (ctrl_size == 0xe0) {
            u32 nvn2_size = 0;
            if (!ReadU32(ctrl_span, 0x38, false, nvn2_size) || nvn2_size < 16 ||
                nvn2_size > avail) {
                continue;
            }
            code_len = nvn2_size;
        } else {
            u32 nvn1_len = 0;
            if (ReadU32(ctrl_span, 0x704, false, nvn1_len) && nvn1_len > 0) {
                code_len = (std::min<std::size_t>)(avail, static_cast<std::size_t>(nvn1_len) + 0x80U);
            }
        }
        const auto code_span = blob.subspan(resolved_code_off, code_len);
        if (auto shader = ExtractFromControlAndCode(ctrl_span, code_span, source_name)) {
            out.push_back(std::move(*shader));
        }
    }
}

void ParseNdsr(std::span<const u8> blob, std::string_view source_name, const ScanOptions& options,
               std::vector<ScannedShader>& out, ScanStats* stats) {
    if (blob.size() < 24 || !HasMagic(blob, 0, "NDSR")) {
        return;
    }
    if (stats) {
        ++stats->ndsr_containers;
    }

    std::unordered_set<u32> seen_ctrl;
    for (std::size_t pos = 8; pos + 16 <= blob.size(); pos += 4) {
        u32 ctrl_off = 0;
        u32 ctrl_word = 0;
        u32 code_off = 0;
        u32 code_word = 0;
        if (!ReadU32(blob, pos + 0, false, ctrl_off) ||
            !ReadU32(blob, pos + 4, false, ctrl_word) ||
            !ReadU32(blob, pos + 8, false, code_off) ||
            !ReadU32(blob, pos + 12, false, code_word)) {
            continue;
        }
        const u32 ctrl_tag = ctrl_word & 15U;
        const u32 code_tag = code_word & 15U;
        const bool tag_pair_ok =
            (ctrl_word >> 4) == 0xe0U &&
            ((ctrl_tag == 6U && code_tag == 7U) || (ctrl_tag == 0U && code_tag == 1U) ||
             (ctrl_tag == 3U && code_tag == 4U));
        if (!tag_pair_ok) {
            continue;
        }
        const u32 room = code_word >> 4;
        if (ctrl_off > blob.size() || blob.size() - ctrl_off < 0xe0 ||
            code_off > blob.size() || blob.size() - code_off < room || room < 16) {
            continue;
        }
        const auto ctrl_span = blob.subspan(ctrl_off, 0xe0);
        u32 ctrl_magic = 0;
        if (!ReadU32(ctrl_span, 0, false, ctrl_magic) || ctrl_magic != NvnControlMagic) {
            continue;
        }
        if (!seen_ctrl.insert(ctrl_off).second) {
            continue;
        }
        const auto code_span = blob.subspan(code_off, room);
        auto shader = ExtractFromControlAndCode(ctrl_span, code_span, source_name);
        if (!shader) {
            continue;
        }
        if (shader->stage == ScannedStage::Compute && !options.include_large_ndsr_compute &&
            shader->program_bytes.size() > options.max_ndsr_compute_bytes) {
            continue;
        }
        out.push_back(std::move(*shader));
    }
}

namespace {

void ScanEmbeddedBnsh(std::span<const u8> blob, std::string_view source_name,
                      std::vector<ScannedShader>& out, ScanStats* stats) {
    for (std::size_t pos = 0; pos + 140 <= blob.size(); pos += 4) {
        if (std::memcmp(blob.data() + pos, "BNSH\0\0\0\0", 8) != 0) {
            continue;
        }
        if (blob[pos + 12] != 0xFF || blob[pos + 13] != 0xFE) {
            continue;
        }
        u32 bnsh_size = 0;
        if (!ReadU32(blob, pos + 28, false, bnsh_size) || bnsh_size < 140 ||
            bnsh_size > blob.size() - pos) {
            continue;
        }
        if (!HasMagic(blob, pos + 96, "grsc")) {
            continue;
        }
        const std::size_t before = out.size();
        ParseBnsh(blob.subspan(pos, bnsh_size), source_name, out, stats);
        if (out.size() > before) {
            const std::size_t advance = ((static_cast<std::size_t>(bnsh_size) + 3U) / 4U) * 4U;
            if (advance >= 4) {
                pos += advance - 4;
            }
        }
    }
}

} // Anonymous namespace

void ParseFsha(std::span<const u8> blob, std::string_view source_name,
               std::vector<ScannedShader>& out, ScanStats* stats) {
    if (blob.size() < 160 || !HasMagic(blob, 0, "FSHA")) {
        return;
    }
    if (stats) {
        ++stats->fsha_containers;
    }
    ScanEmbeddedBnsh(blob, source_name, out, stats);
}

void ScanNso(std::span<const u8> blob, std::string_view source_name,
             std::vector<ScannedShader>& out, ScanStats* stats) {
    if (blob.size() < 0x100 || !HasMagic(blob, 0, "NSO0")) {
        return;
    }
    u32 flags = 0;
    if (!ReadU32(blob, 0x0C, false, flags)) {
        return;
    }
    if (stats) {
        ++stats->nso_modules;
    }

    constexpr std::size_t MaxSegmentBytes = 128ULL * 1024 * 1024;
    for (u32 seg = 0; seg < 3; ++seg) {
        u32 file_off = 0;
        u32 dec_size = 0;
        u32 comp_size = 0;
        if (!ReadU32(blob, 0x10U + seg * 0x10U, false, file_off) ||
            !ReadU32(blob, 0x18U + seg * 0x10U, false, dec_size) ||
            !ReadU32(blob, 0x60U + seg * 4U, false, comp_size)) {
            continue;
        }
        if (dec_size < 140 || dec_size > MaxSegmentBytes) {
            continue;
        }
        std::vector<u8> decompressed_owner;
        std::span<const u8> segment_span;
        if ((flags & (1U << seg)) != 0U) {
            if (file_off > blob.size() || comp_size > blob.size() - file_off || comp_size == 0) {
                continue;
            }
            decompressed_owner = Common::Compression::DecompressDataLZ4(
                blob.subspan(file_off, comp_size), dec_size);
            if (decompressed_owner.empty()) {
                continue;
            }
            segment_span = decompressed_owner;
        } else {
            if (file_off > blob.size() || dec_size > blob.size() - file_off) {
                continue;
            }
            segment_span = blob.subspan(file_off, dec_size);
        }

        if (segment_span.size() < 140) {
            continue;
        }
        ScanEmbeddedBnsh(segment_span, source_name, out, stats);
    }
}

void ScanContainerBlob(std::span<const u8> blob, std::string_view source_name,
                       const ZstdDictionaryStore& dict_store, const ScanOptions& options,
                       std::vector<ScannedShader>& out, ScanStats* stats, int max_depth) {
    if (max_depth < 0 || blob.size() < 4) {
        return;
    }
    if (options.should_stop && options.should_stop()) {
        return;
    }

    u32 magic_le = 0;
    if (ReadU32(blob, 0, false, magic_le) && magic_le == ZstdFrameMagicLE) {
        if (stats) {
            ++stats->zstd_entries;
        }
        const auto decompressed = dict_store.TryDecompress(blob, options.max_decompressed_size);
        if (!decompressed) {
            if (stats) {
                ++stats->errors_ignored;
            }
            return;
        }
        ScanContainerBlob(*decompressed, source_name, dict_store, options, out, stats,
                          max_depth - 1);
        return;
    }

    if (HasMagic(blob, 0, "SARC")) {
        const auto entries = ParseSarc(blob);
        if (!entries) {
            if (stats) {
                ++stats->errors_ignored;
            }
            return;
        }
        if (stats) {
            ++stats->sarc_archives;
        }
        for (const auto& entry : *entries) {
            if (options.should_stop && options.should_stop()) {
                return;
            }
            if (IsKnownNonShaderExtension(ToLowerAscii(entry.name))) {
                continue;
            }
            const auto child_name = fmt::format("{}/{}", source_name, entry.name);
            ScanContainerBlob(entry.data, child_name, dict_store, options, out, stats,
                              max_depth - 1);
        }
        return;
    }

    if (HasMagic(blob, 0, "FSHA")) {
        ParseFsha(blob, source_name, out, stats);
        return;
    }
    if (HasMagic(blob, 0, "BNSH")) {
        ParseBnsh(blob, source_name, out, stats);
        return;
    }
    if (HasMagic(blob, 0, "BAHS")) {
        ParseSharcb(blob, source_name, out, stats);
        return;
    }
    if (HasMagic(blob, 0, "NDSR")) {
        ParseNdsr(blob, source_name, options, out, stats);
        return;
    }
    if (HasMagic(blob, 0, "NSO0")) {
        ScanNso(blob, source_name, out, stats);
    }
}

u64 ComputeTitleContentFingerprint(Core::System& system, u64 title_id) {
    const auto exefs = ResolvePatchedExefs(system, title_id);
    const auto romfs = ResolvePatchedRomfs(system, title_id);
    u64 fp = title_id ^ 0x44414C4941505245ULL;
    UpdateFingerprintForDir(exefs, fp, 1);
    UpdateFingerprintForDir(romfs, fp, 4);
    return fp;
}

std::vector<ScannedShader> ScanDirectoryTrees(const FileSys::VirtualDir& exefs,
                                              const FileSys::VirtualDir& romfs, u64 title_id,
                                              const ScanOptions& options, ScanStats* out_stats,
                                              u64* out_content_fingerprint) {
    ScanStats local_stats;
    std::vector<ScannedShader> deduped;
    std::unordered_set<u64> seen_keys;

    if (out_content_fingerprint) {
        u64 fp = title_id ^ 0x44414C4941505245ULL;
        UpdateFingerprintForDir(exefs, fp, 1);
        UpdateFingerprintForDir(romfs, fp, 4);
        *out_content_fingerprint = fp;
    }

    ZstdDictionaryStore dict_store;
    dict_store.AddFromDirectory(romfs);
    local_stats.zstd_dicts_loaded = dict_store.Size();

    auto scan_file = [&](const FileSys::VirtualFile& file, std::string_view prefix) {
        if (!file || (options.should_stop && options.should_stop())) {
            return;
        }
        const std::size_t size = file->GetSize();
        if (size < 16 || size > options.max_decompressed_size) {
            return;
        }
        const auto name = file->GetName();
        const auto lower = ToLowerAscii(name);
        if (IsKnownNonShaderExtension(lower)) {
            return;
        }
        std::array<u8, 8> header{};
        if (file->Read(header.data(), header.size(), 0) < 4) {
            return;
        }
        u32 magic_le = 0;
        std::memcpy(&magic_le, header.data(), sizeof(magic_le));
        const bool magic_match =
            magic_le == ZstdFrameMagicLE ||
            std::memcmp(header.data(), "SARC", 4) == 0 ||
            std::memcmp(header.data(), "FSHA", 4) == 0 ||
            std::memcmp(header.data(), "BNSH", 4) == 0 ||
            std::memcmp(header.data(), "BAHS", 4) == 0 ||
            std::memcmp(header.data(), "NDSR", 4) == 0 ||
            std::memcmp(header.data(), "NSO0", 4) == 0;
        if (!magic_match && !lower.ends_with(".bfsha") && !lower.ends_with(".bnsh") &&
            !lower.ends_with(".sharcb") && !lower.ends_with(".sharc") &&
            !lower.ends_with(".ndsr") && !lower.ends_with(".sarc") &&
            !lower.ends_with(".pack.zs") && !lower.ends_with(".szs") &&
            !lower.ends_with(".zs")) {
            return;
        }

        ++local_stats.files_scanned;
        const auto bytes = file->ReadAllBytes();
        std::vector<ScannedShader> found;
        const auto full_name = fmt::format("{}/{}", prefix, name);
        ScanContainerBlob(bytes, full_name, dict_store, options, found, &local_stats);
        DeduplicateAndAppend(deduped, std::move(found), seen_keys, &local_stats);
    };

    if (exefs) {
        for (const auto& file : exefs->GetFiles()) {
            if (options.should_stop && options.should_stop()) {
                break;
            }
            scan_file(file, "exefs");
        }
    }

    if (romfs) {
        struct PendingDir {
            FileSys::VirtualDir dir;
            std::string path;
        };
        std::vector<PendingDir> stack{{romfs, "romfs"}};
        while (!stack.empty()) {
            if (options.should_stop && options.should_stop()) {
                break;
            }
            auto current = std::move(stack.back());
            stack.pop_back();
            if (!current.dir) {
                continue;
            }
            for (const auto& file : current.dir->GetFiles()) {
                scan_file(file, current.path);
                if (options.should_stop && options.should_stop()) {
                    break;
                }
            }
            for (const auto& sub : current.dir->GetSubdirectories()) {
                if (sub) {
                    auto sub_path = fmt::format("{}/{}", current.path, sub->GetName());
                    stack.push_back(PendingDir{sub, std::move(sub_path)});
                }
            }
        }
    }

    if (out_stats) {
        *out_stats = local_stats;
    }
    return deduped;
}

std::vector<ScannedShader> ScanTitleContainers(Core::System& system, u64 title_id,
                                               const ScanOptions& options, ScanStats* out_stats,
                                               u64* out_content_fingerprint) {
    const auto exefs = ResolvePatchedExefs(system, title_id);
    const auto romfs = ResolvePatchedRomfs(system, title_id);
    return ScanDirectoryTrees(exefs, romfs, title_id, options, out_stats, out_content_fingerprint);
}

} // namespace VideoCore::Precompile
