// SPDX-FileCopyrightText: Copyright 2026 Dalia Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <zdict.h>
#include <zstd.h>

#include "common/common_types.h"
#include "common/lz4_compression.h"
#include "common/zstd_compression.h"
#include "core/file_sys/nup_container.h"
#include "core/file_sys/vfs/vfs_real.h"
#include "core/file_sys/vfs/vfs_types.h"
#include "shader_recompiler/frontend/ampere/lower_to_maxwell.h"
#include "shader_recompiler/frontend/ir/attribute.h"
#include "shader_recompiler/host_translate_info.h"
#include "shader_recompiler/profile.h"
#include "video_core/precompile/shader_container_scanner.h"
#include "video_core/precompile/shader_precompiler.h"

namespace {

using VideoCore::Precompile::ParseBnsh;
using VideoCore::Precompile::ParseNdsr;
using VideoCore::Precompile::ParseSarc;
using VideoCore::Precompile::ParseSharcb;
using VideoCore::Precompile::ScanContainerBlob;
using VideoCore::Precompile::ScanNso;
using VideoCore::Precompile::ScannedShader;
using VideoCore::Precompile::ScannedStage;
using VideoCore::Precompile::ScanOptions;
using VideoCore::Precompile::ScanStats;
using VideoCore::Precompile::ZstdDictionaryStore;

void AppendU16(std::vector<u8>& out, u16 v, bool big_endian) {
    if (big_endian) {
        out.push_back(static_cast<u8>((v >> 8) & 0xFF));
        out.push_back(static_cast<u8>(v & 0xFF));
    } else {
        out.push_back(static_cast<u8>(v & 0xFF));
        out.push_back(static_cast<u8>((v >> 8) & 0xFF));
    }
}

void AppendU32(std::vector<u8>& out, u32 v, bool big_endian) {
    if (big_endian) {
        out.push_back(static_cast<u8>((v >> 24) & 0xFF));
        out.push_back(static_cast<u8>((v >> 16) & 0xFF));
        out.push_back(static_cast<u8>((v >> 8) & 0xFF));
        out.push_back(static_cast<u8>(v & 0xFF));
    } else {
        out.push_back(static_cast<u8>(v & 0xFF));
        out.push_back(static_cast<u8>((v >> 8) & 0xFF));
        out.push_back(static_cast<u8>((v >> 16) & 0xFF));
        out.push_back(static_cast<u8>((v >> 24) & 0xFF));
    }
}

void WriteU32LE(std::vector<u8>& buf, std::size_t off, u32 v) {
    buf[off + 0] = static_cast<u8>(v & 0xFF);
    buf[off + 1] = static_cast<u8>((v >> 8) & 0xFF);
    buf[off + 2] = static_cast<u8>((v >> 16) & 0xFF);
    buf[off + 3] = static_cast<u8>((v >> 24) & 0xFF);
}

void WriteU64LE(std::vector<u8>& buf, std::size_t off, u64 v) {
    for (int i = 0; i < 8; ++i) {
        buf[off + i] = static_cast<u8>((v >> (i * 8)) & 0xFF);
    }
}

void AlignTo(std::vector<u8>& buf, std::size_t alignment) {
    while ((buf.size() % alignment) != 0) {
        buf.push_back(0);
    }
}

std::vector<u8> BuildTestSarc(const std::vector<std::pair<std::string, std::vector<u8>>>& items,
                              bool big_endian) {
    std::vector<u8> sfnt_strings;
    std::vector<u32> name_word_offsets;
    for (const auto& [name, _] : items) {
        AlignTo(sfnt_strings, 4);
        name_word_offsets.push_back(static_cast<u32>(sfnt_strings.size() / 4));
        sfnt_strings.insert(sfnt_strings.end(), name.begin(), name.end());
        sfnt_strings.push_back(0);
    }
    AlignTo(sfnt_strings, 4);

    const std::size_t header_size = 20;
    const std::size_t sfat_header = 12;
    const std::size_t sfat_entries = items.size() * 16;
    const std::size_t sfnt_header = 8;
    const std::size_t pre_data = header_size + sfat_header + sfat_entries + sfnt_header + sfnt_strings.size();
    const std::size_t data_start = (pre_data + 7U) & ~std::size_t(7U);

    std::vector<u8> blob;
    // SARC header
    blob.insert(blob.end(), {'S', 'A', 'R', 'C'});
    AppendU16(blob, static_cast<u16>(header_size), big_endian);
    AppendU16(blob, 0xFEFF, big_endian); // BOM -> FF FE (LE) or FE FF (BE)
    const std::size_t file_size_pos = blob.size();
    AppendU32(blob, 0, big_endian);
    AppendU32(blob, static_cast<u32>(data_start), big_endian);
    AppendU32(blob, 0x01000000, big_endian);

    // SFAT header
    blob.insert(blob.end(), {'S', 'F', 'A', 'T'});
    AppendU16(blob, 12, big_endian);
    AppendU16(blob, static_cast<u16>(items.size()), big_endian);
    AppendU32(blob, 0x00000065, big_endian);

    std::size_t cur_data = 0;
    for (std::size_t i = 0; i < items.size(); ++i) {
        const u32 lo = static_cast<u32>(cur_data);
        const u32 hi = static_cast<u32>(lo + items[i].second.size());
        cur_data = (static_cast<std::size_t>(hi) + 7U) & ~std::size_t(7U);

        AppendU32(blob, static_cast<u32>(i + 1), big_endian);
        AppendU32(blob, (1U << 24) | name_word_offsets[i], big_endian);
        AppendU32(blob, lo, big_endian);
        AppendU32(blob, hi, big_endian);
    }

    // SFNT header + string table
    blob.insert(blob.end(), {'S', 'F', 'N', 'T'});
    AppendU16(blob, 8, big_endian);
    AppendU16(blob, 0, big_endian);
    blob.insert(blob.end(), sfnt_strings.begin(), sfnt_strings.end());
    blob.resize(data_start, 0);

    for (const auto& [_, data] : items) {
        AlignTo(blob, 8);
        blob.insert(blob.end(), data.begin(), data.end());
    }

    const u32 total_size = static_cast<u32>(blob.size());
    if (big_endian) {
        blob[file_size_pos + 0] = static_cast<u8>((total_size >> 24) & 0xFF);
        blob[file_size_pos + 1] = static_cast<u8>((total_size >> 16) & 0xFF);
        blob[file_size_pos + 2] = static_cast<u8>((total_size >> 8) & 0xFF);
        blob[file_size_pos + 3] = static_cast<u8>(total_size & 0xFF);
    } else {
        WriteU32LE(blob, file_size_pos, total_size);
    }
    return blob;
}

std::vector<u8> MakeMinimalSm86ExitBytes() {
    const Shader::Ampere::Instruction exit_inst{{0x0000794d, 0, 0x03800000, 0}};
    std::vector<u8> out(sizeof(exit_inst));
    std::memcpy(out.data(), &exit_inst, sizeof(exit_inst));
    return out;
}

struct SyntheticSlot {
    u32 slot_rel{};
    std::vector<u8> control;
    std::vector<u8> code;
};

std::vector<u8> BuildNvn2Control(u16 stage, u32 code_size, std::span<const u8> c1 = {}) {
    std::vector<u8> ctrl(0xe0 + c1.size(), 0);
    WriteU32LE(ctrl, 0x00, 0x98761234U);
    WriteU32LE(ctrl, 0x38, code_size);
    WriteU32LE(ctrl, 0x54, static_cast<u32>(stage) << 16);
    if (!c1.empty()) {
        WriteU32LE(ctrl, 0x40, 0xe0U);
        WriteU32LE(ctrl, 0x44, static_cast<u32>(0xe0U + c1.size()));
        std::memcpy(ctrl.data() + 0xe0, c1.data(), c1.size());
    }
    return ctrl;
}

std::vector<u8> BuildNvn1Control(u32 stage, std::array<u32, 3> local_size = {1, 1, 1}) {
    std::vector<u8> ctrl(0x880, 0);
    WriteU32LE(ctrl, 0x00, 0x98761234U);
    WriteU32LE(ctrl, 0x714, stage);
    WriteU32LE(ctrl, 0x748, local_size[0]);
    WriteU32LE(ctrl, 0x74C, local_size[1]);
    WriteU32LE(ctrl, 0x750, local_size[2]);
    return ctrl;
}

std::vector<u8> BuildTestBnsh(const std::vector<SyntheticSlot>& slots) {
    // BNSH header (>= 140 bytes) + 1 variation (64 bytes) + 1 program (96 bytes)
    constexpr std::size_t VarsOff = 0x90;
    constexpr std::size_t ProgOff = VarsOff + 64;
    const std::size_t RecsOff = ProgOff + 96;

    std::vector<u8> blob(RecsOff + slots.size() * 32, 0);
    std::memcpy(blob.data(), "BNSH\0\0\0\0", 8);
    blob[12] = 0xFF;
    blob[13] = 0xFE;
    std::memcpy(blob.data() + 96, "grsc", 4);
    WriteU32LE(blob, 124, 1U);
    WriteU64LE(blob, 128, VarsOff);
    WriteU64LE(blob, VarsOff + 16, ProgOff);

    for (std::size_t i = 0; i < slots.size(); ++i) {
        const std::size_t rec_off = RecsOff + i * 32;
        WriteU64LE(blob, ProgOff + slots[i].slot_rel, rec_off);

        AlignTo(blob, 16);
        const std::size_t ctrl_off = blob.size();
        blob.insert(blob.end(), slots[i].control.begin(), slots[i].control.end());

        AlignTo(blob, 16);
        const std::size_t code_off = blob.size();
        blob.insert(blob.end(), slots[i].code.begin(), slots[i].code.end());

        WriteU64LE(blob, rec_off + 8, ctrl_off);
        WriteU64LE(blob, rec_off + 16, code_off);
        WriteU32LE(blob, rec_off + 24, static_cast<u32>(slots[i].code.size()));
        WriteU32LE(blob, rec_off + 28, static_cast<u32>(slots[i].control.size()));
    }

    WriteU32LE(blob, 28, static_cast<u32>(blob.size()));
    return blob;
}

} // Anonymous namespace

TEST_CASE("SARC parser handles LE, BE, and corrupt bounds safely", "[video_core][precompile]") {
    const std::vector<std::pair<std::string, std::vector<u8>>> items{
        {"shader/vs.bnsh", {1, 2, 3, 4, 5}},
        {"shader/fs.bnsh", {9, 8, 7}},
    };

    for (const bool be : {false, true}) {
        const auto sarc = BuildTestSarc(items, be);
        const auto parsed = ParseSarc(sarc);
        REQUIRE(parsed.has_value());
        REQUIRE(parsed->size() == 2);
        CHECK((*parsed)[0].name == "shader/vs.bnsh");
        CHECK(std::vector<u8>((*parsed)[0].data.begin(), (*parsed)[0].data.end()) ==
              items[0].second);
        CHECK((*parsed)[1].name == "shader/fs.bnsh");
        CHECK(std::vector<u8>((*parsed)[1].data.begin(), (*parsed)[1].data.end()) ==
              items[1].second);
    }

    // Corrupt SARC tests
    CHECK_FALSE(ParseSarc({}).has_value());
    auto bad_bom = BuildTestSarc(items, false);
    bad_bom[6] = 0x12;
    CHECK_FALSE(ParseSarc(bad_bom).has_value());

    auto truncated = BuildTestSarc(items, false);
    truncated.resize(truncated.size() - 2);
    CHECK_FALSE(ParseSarc(truncated).has_value());
}

TEST_CASE("BNSH parser extracts NVN1 and NVN2 shaders and lowers SM86 to SPIR-V",
          "[video_core][precompile]") {
    const auto sm86_exit = MakeMinimalSm86ExitBytes();
    const std::array<u8, 8> c1_sample{0x78, 0x56, 0x34, 0x12, 0xEF, 0xCD, 0xAB, 0x00};

    // NVN2 BNSH with Vertex + Fragment + Compute (with c1 data)
    const auto bnsh_nvn2 = BuildTestBnsh({
        {8, BuildNvn2Control(0, static_cast<u32>(sm86_exit.size())), sm86_exit},
        {40, BuildNvn2Control(1, static_cast<u32>(sm86_exit.size())), sm86_exit},
        {48, BuildNvn2Control(5, static_cast<u32>(sm86_exit.size()), c1_sample), sm86_exit},
    });

    std::vector<ScannedShader> shaders;
    ScanStats stats{};
    ParseBnsh(bnsh_nvn2, "test_nvn2.bnsh", shaders, &stats);
    REQUIRE(stats.bnsh_containers == 1);
    REQUIRE(shaders.size() == 3);

    CHECK(shaders[0].stage == ScannedStage::Vertex);
    CHECK(shaders[1].stage == ScannedStage::Fragment);
    CHECK(shaders[2].stage == ScannedStage::Compute);
    CHECK(shaders[2].is_native_sm86);
    CHECK(shaders[2].c1_bytes == std::vector<u8>(c1_sample.begin(), c1_sample.end()));

    // Translate extracted SM86 compute/vertex/fragment shaders into valid SPIR-V.
    VideoCore::Precompile::PrecompilePools pools;
    Shader::Profile profile{.supported_spirv = 0x00010300};
    Shader::HostTranslateInfo host_info{};
    for (const auto& s : shaders) {
        VideoCore::Precompile::TranslatedShader translated{};
        std::string err;
        const bool ok = VideoCore::Precompile::TranslateScannedShader(
            pools, s, profile, host_info, 49152, false, translated, &err);
        INFO("Shader translation error: " << err);
        REQUIRE(ok);
        REQUIRE(translated.spirv.size() >= 5);
        CHECK(translated.spirv[0] == 0x07230203U); // SPIR-V magic number
    }

    // NVN1 BNSH with Graphics Vertex (0x30 + 0x50 SPH + code) + Maxwell Compute (local 8x4x2)
    std::vector<u8> nvn1_vs_code(0x30 + 0x50 + 8, 0);
    WriteU32LE(nvn1_vs_code, 0x30, 0x00000461U); // sph_type=1, version=3
    std::vector<u8> nvn1_cs_code(16, 0);
    const auto bnsh_nvn1 = BuildTestBnsh({
        {8, BuildNvn1Control(0), nvn1_vs_code},
        {48, BuildNvn1Control(5, {8, 4, 2}), nvn1_cs_code},
    });
    shaders.clear();
    ParseBnsh(bnsh_nvn1, "test_nvn1.bnsh", shaders);
    REQUIRE(shaders.size() == 2);
    CHECK(shaders[0].stage == ScannedStage::Vertex);
    CHECK_FALSE(shaders[0].is_native_sm86);
    CHECK(shaders[0].sph_bytes.has_value());
    CHECK(shaders[1].stage == ScannedStage::Compute);
    CHECK_FALSE(shaders[1].is_native_sm86);
    CHECK(shaders[1].local_size == std::array<u32, 3>{8, 4, 2});
}

TEST_CASE("SHARCB parser validates control magic to reject false positives",
          "[video_core][precompile]") {
    const auto sm86_exit = MakeMinimalSm86ExitBytes();
    const auto valid_ctrl = BuildNvn2Control(5, static_cast<u32>(sm86_exit.size()));
    std::vector<u8> fake_ctrl(0xe0, 0xAA); // Lacks 0x98761234 magic!

    // Layout: [0..32) BAHS header | [32..pool) control + record table | [pool..end) code pool
    constexpr u32 Start = 32;
    const u32 valid_rec = Start;
    const u32 fake_rec = Start + 32;
    const u32 valid_ctrl_off = Start + 64;
    const u32 fake_ctrl_off = valid_ctrl_off + static_cast<u32>(valid_ctrl.size());
    const u32 pool_off = (fake_ctrl_off + static_cast<u32>(fake_ctrl.size()) + 15U) & ~15U;

    std::vector<u8> blob(pool_off, 0);
    std::memcpy(blob.data(), "BAHS", 4);
    WriteU32LE(blob, 20, Start);
    WriteU32LE(blob, 24, static_cast<u32>(sm86_exit.size()));
    WriteU32LE(blob, 28, pool_off);

    std::memcpy(blob.data() + valid_ctrl_off, valid_ctrl.data(), valid_ctrl.size());
    std::memcpy(blob.data() + fake_ctrl_off, fake_ctrl.data(), fake_ctrl.size());

    // Valid record at +8: {u64 code_off, u32 ctrl_size, i32 ctrl_rel_off}
    WriteU64LE(blob, valid_rec + 8, pool_off);
    WriteU32LE(blob, valid_rec + 16, 0xe0U);
    WriteU32LE(blob, valid_rec + 20, valid_ctrl_off - valid_rec);

    // False-positive record matching ctrl_size==0xe0 but wrong control magic
    WriteU64LE(blob, fake_rec + 8, pool_off);
    WriteU32LE(blob, fake_rec + 16, 0xe0U);
    WriteU32LE(blob, fake_rec + 20, fake_ctrl_off - fake_rec);

    blob.insert(blob.end(), sm86_exit.begin(), sm86_exit.end());

    std::vector<ScannedShader> shaders;
    ParseSharcb(blob, "test.sharcb", shaders);
    REQUIRE(shaders.size() == 1);
    CHECK(shaders[0].stage == ScannedStage::Compute);
    CHECK(shaders[0].is_native_sm86);
}

TEST_CASE("SHARCB parser prioritizes pool-relative code_off when >= pool",
          "[video_core][precompile]") {
    const auto sm86_exit = MakeMinimalSm86ExitBytes();
    std::vector<u8> expected_code;
    expected_code.insert(expected_code.end(), sm86_exit.begin(), sm86_exit.end());
    expected_code.insert(expected_code.end(), sm86_exit.begin(), sm86_exit.end());
    const auto valid_ctrl = BuildNvn2Control(5, static_cast<u32>(expected_code.size()));

    constexpr u32 Start = 32;
    const u32 rec_off = Start;
    const u32 ctrl_off = Start + 32;
    const u32 pool_off = (ctrl_off + static_cast<u32>(valid_ctrl.size()) + 15U) & ~15U;
    // code_off >= pool_off while pool_off + code_off + code_size <= pool_end
    const u32 code_rel_off = pool_off;
    const u32 pool_size = code_rel_off + static_cast<u32>(expected_code.size()) + 64U;

    std::vector<u8> blob(pool_off + pool_size, 0x11);
    std::memcpy(blob.data(), "BAHS", 4);
    WriteU32LE(blob, 20, Start);
    WriteU32LE(blob, 24, pool_size);
    WriteU32LE(blob, 28, pool_off);

    std::memcpy(blob.data() + ctrl_off, valid_ctrl.data(), valid_ctrl.size());
    WriteU64LE(blob, rec_off + 8, code_rel_off);
    WriteU32LE(blob, rec_off + 16, 0xe0U);
    WriteU32LE(blob, rec_off + 20, ctrl_off - rec_off);

    std::memcpy(blob.data() + pool_off + code_rel_off, expected_code.data(), expected_code.size());

    std::vector<ScannedShader> shaders;
    ParseSharcb(blob, "overlap.sharcb", shaders);
    REQUIRE(shaders.size() == 1);
    CHECK(shaders[0].stage == ScannedStage::Compute);
    CHECK(shaders[0].is_native_sm86);
    CHECK(shaders[0].program_bytes == expected_code);
}

TEST_CASE("NDSR parser respects large compute shader size threshold",
          "[video_core][precompile]") {
    std::vector<u8> small_cs(4096, 0);
    std::vector<u8> large_cs(32768, 0);
    std::vector<u8> small_fs(256, 0);

    const auto ctrl_small_cs = BuildNvn2Control(5, static_cast<u32>(small_cs.size()));
    const auto ctrl_large_cs = BuildNvn2Control(5, static_cast<u32>(large_cs.size()));
    const auto ctrl_small_fs = BuildNvn2Control(1, static_cast<u32>(small_fs.size()));

    // Header (8) + 6 items (3 pairs * 8 bytes = 48 bytes) => payloads start at 64
    constexpr std::size_t TableEnd = 8 + 6 * 8;
    std::vector<u8> blob(TableEnd, 0);
    std::memcpy(blob.data(), "NDSR", 4);

    auto append_pair = [&](std::size_t table_pos, std::span<const u8> ctrl,
                           std::span<const u8> code) {
        AlignTo(blob, 16);
        const u32 ctrl_off = static_cast<u32>(blob.size());
        blob.insert(blob.end(), ctrl.begin(), ctrl.end());

        AlignTo(blob, 16);
        const u32 code_off = static_cast<u32>(blob.size());
        blob.insert(blob.end(), code.begin(), code.end());

        WriteU32LE(blob, table_pos + 0, ctrl_off);
        WriteU32LE(blob, table_pos + 4, (0xe0U << 4) | 6U);
        WriteU32LE(blob, table_pos + 8, code_off);
        WriteU32LE(blob, table_pos + 12, (static_cast<u32>(code.size()) << 4) | 7U);
    };
    append_pair(8, ctrl_small_cs, small_cs);
    append_pair(24, ctrl_large_cs, large_cs);
    append_pair(40, ctrl_small_fs, small_fs);

    std::vector<ScannedShader> shaders;
    ScanOptions default_opts{};
    ParseNdsr(blob, "test.ndsr", default_opts, shaders);
    REQUIRE(shaders.size() == 2); // large compute skipped

    shaders.clear();
    ScanOptions all_opts{.include_large_ndsr_compute = true};
    ParseNdsr(blob, "test.ndsr", all_opts, shaders);
    REQUIRE(shaders.size() == 3);
}

TEST_CASE("NSO0 LZ4 segment scan and dictionary-compressed SARC/Zstd integration",
          "[video_core][precompile]") {
    const auto sm86_exit = MakeMinimalSm86ExitBytes();
    const auto bnsh = BuildTestBnsh({
        {48, BuildNvn2Control(5, static_cast<u32>(sm86_exit.size())), sm86_exit},
    });

    // 1. NSO0 with LZ4-compressed .rodata holding embedded BNSH
    std::vector<u8> rodata(64, 0xCC);
    rodata.insert(rodata.end(), bnsh.begin(), bnsh.end());
    const auto comp_rodata = Common::Compression::CompressDataLZ4(rodata.data(), rodata.size());
    REQUIRE_FALSE(comp_rodata.empty());

    std::vector<u8> nso(0x100, 0);
    std::memcpy(nso.data(), "NSO0", 4);
    WriteU32LE(nso, 0x0C, 2U); // bit 1 => segment 1 (rodata) is LZ4 compressed
    const u32 seg_off = static_cast<u32>(nso.size());
    nso.insert(nso.end(), comp_rodata.begin(), comp_rodata.end());
    WriteU32LE(nso, 0x20, seg_off);
    WriteU32LE(nso, 0x28, static_cast<u32>(rodata.size()));
    WriteU32LE(nso, 0x64, static_cast<u32>(comp_rodata.size()));

    std::vector<ScannedShader> nso_shaders;
    ScanNso(nso, "main", nso_shaders);
    REQUIRE(nso_shaders.size() == 1);
    CHECK(nso_shaders[0].is_native_sm86);
    CHECK(nso_shaders[0].stage == ScannedStage::Compute);

    // 2. Zstd Dictionary Store + Zstd-compressed SARC containing dict + dict-compressed BNSH
    // Build a real formatted Zstd dictionary using ZDICT_trainFromBuffer over structured shader samples.
    std::vector<u8> samples;
    std::vector<std::size_t> sample_sizes;
    samples.reserve(128 * 1800);
    for (int i = 0; i < 128; ++i) {
        std::vector<u8> copy;
        copy.reserve(1600);
        // Structured prefix + varied BNSH copies so FASTCOVER trains a high-coverage dictionary.
        for (int r = 0; r < 3; ++r) {
            std::vector<u8> variant = bnsh;
            if (variant.size() > 0x20) {
                variant[0x18] = static_cast<u8>((i * 3 + r) & 0xFF);
                variant[0x19] = static_cast<u8>((i >> 2) & 0xFF);
            }
            copy.insert(copy.end(), variant.begin(), variant.end());
            const u32 marker = static_cast<u32>((i << 8) | r);
            copy.push_back(static_cast<u8>(marker & 0xFF));
            copy.push_back(static_cast<u8>((marker >> 8) & 0xFF));
        }
        // Pad with structured repeating 16-byte shader constant records (varying slightly per sample).
        for (int j = 0; j < 24; ++j) {
            const u8 tag = static_cast<u8>((i + j) & 0x1F);
            copy.insert(copy.end(), {'N', 'V', 'N', '2', 0x4D, 0x79, 0x00, tag,
                                     0x00, 0x00, 0x80, 0x03, 0x00, 0x00, 0x00, 0x00});
        }
        samples.insert(samples.end(), copy.begin(), copy.end());
        sample_sizes.push_back(copy.size());
    }
    std::vector<u8> dict_buf(2048);
    const std::size_t dict_size =
        ZDICT_trainFromBuffer(dict_buf.data(), dict_buf.size(), samples.data(),
                              sample_sizes.data(), static_cast<unsigned>(sample_sizes.size()));
    REQUIRE_FALSE(ZDICT_isError(dict_size));
    dict_buf.resize(dict_size);

    const u32 dict_id = ZSTD_getDictID_fromDict(dict_buf.data(), dict_buf.size());
    REQUIRE(dict_id != 0);

    const auto dict_pack = BuildTestSarc({{"zs.zsdic", dict_buf}}, false);
    ZstdDictionaryStore dict_store;
    REQUIRE(dict_store.AddFromSarc(dict_pack) == 1);

    // Compress BNSH with the dictionary and verify the embedded dictionary ID is preserved.
    std::unique_ptr<ZSTD_CCtx, decltype(&ZSTD_freeCCtx)> cctx(ZSTD_createCCtx(), &ZSTD_freeCCtx);
    REQUIRE(cctx != nullptr);
    std::unique_ptr<ZSTD_CDict, decltype(&ZSTD_freeCDict)> cdict(
        ZSTD_createCDict(dict_buf.data(), dict_buf.size(), 3), &ZSTD_freeCDict);
    REQUIRE(cdict != nullptr);
    std::vector<u8> zstd_bnsh(ZSTD_compressBound(bnsh.size()) + 128);
    const std::size_t z_sz = ZSTD_compress_usingCDict(
        cctx.get(), zstd_bnsh.data(), zstd_bnsh.size(), bnsh.data(), bnsh.size(), cdict.get());
    REQUIRE_FALSE(ZSTD_isError(z_sz));
    zstd_bnsh.resize(z_sz);
    REQUIRE(ZSTD_getDictID_fromFrame(zstd_bnsh.data(), zstd_bnsh.size()) == dict_id);

    // Wrap dict-compressed BNSH inside SARC, compressed again with standard Zstd (nested container)
    const auto outer_sarc = BuildTestSarc({{"nested/shader.bnsh.zs", zstd_bnsh}}, false);
    const auto outer_zs =
        Common::Compression::CompressDataZSTDDefault(outer_sarc.data(), outer_sarc.size());
    REQUIRE_FALSE(outer_zs.empty());

    std::vector<ScannedShader> nested_shaders;
    ScanStats nested_stats{};
    ScanContainerBlob(outer_zs, "romfs/Pack/Shaders.pack.zs", dict_store, ScanOptions{},
                      nested_shaders, &nested_stats);
    CHECK(nested_stats.zstd_entries >= 2);
    CHECK(nested_stats.sarc_archives == 1);
    CHECK(nested_stats.bnsh_containers == 1);
    REQUIRE(nested_shaders.size() == 1);
    CHECK(nested_shaders[0].is_native_sm86);
}

TEST_CASE("FSHA container parser extracts embedded BNSH shaders", "[video_core][precompile]") {
    const auto code = MakeMinimalSm86ExitBytes();
    const auto ctrl = BuildNvn2Control(1, static_cast<u32>(code.size()));
    const auto bnsh = BuildTestBnsh({{.slot_rel = 40, .control = ctrl, .code = code}});

    std::vector<u8> fsha(0x100, 0);
    std::memcpy(fsha.data(), "FSHA    ", 8);
    AlignTo(fsha, 16);
    fsha.insert(fsha.end(), bnsh.begin(), bnsh.end());
    const auto fsha_zs = Common::Compression::CompressDataZSTDDefault(fsha.data(), fsha.size());
    REQUIRE_FALSE(fsha_zs.empty());

    ZstdDictionaryStore empty_dicts;
    std::vector<ScannedShader> shaders;
    ScanStats stats{};
    ScanContainerBlob(fsha_zs, "romfs/Shader/Test.bfsha.zs", empty_dicts, ScanOptions{}, shaders,
                      &stats);

    CHECK(stats.zstd_entries == 1);
    CHECK(stats.fsha_containers == 1);
    CHECK(stats.bnsh_containers == 1);
    REQUIRE(shaders.size() == 1);
    CHECK(shaders[0].stage == ScannedStage::Fragment);
    CHECK(shaders[0].is_native_sm86);
}

TEST_CASE("Precompiler translates standalone SM86 compute shader without QMD",
          "[video_core][precompile]") {
    ScannedShader compute_shader{};
    compute_shader.stage = ScannedStage::Compute;
    compute_shader.is_native_sm86 = true;
    compute_shader.local_size = {8, 8, 1};
    compute_shader.program_bytes = MakeMinimalSm86ExitBytes();
    compute_shader.source_name = "synthetic/compute";

    Shader::Profile profile{
        .supported_spirv = 0x00010600,
        .unified_descriptor_binding = true,
    };
    Shader::HostTranslateInfo host_info{};
    VideoCore::Precompile::PrecompilePools pools;
    VideoCore::Precompile::TranslatedShader translated{};
    std::string err;

    REQUIRE(VideoCore::Precompile::TranslateScannedShader(pools, compute_shader, profile, host_info,
                                                          49152U, false, translated, &err));
    CHECK(translated.stage == Shader::Stage::Compute);
    REQUIRE_FALSE(translated.spirv.empty());
    CHECK(translated.spirv[0] == 0x07230203U);
}

TEST_CASE("Real Zelda OoT NUP shader extraction and translation", "[.][nup]") {
    constexpr const char* NupPath =
        "C:\\Users\\azese\\Documents\\Switch Games\\The Legend of Zelda Ocarina of Time "
        "[0400486024dec000][v131072].nup";
    if (!std::filesystem::exists(NupPath)) {
        SUCCEED("Real Zelda OoT NUP not present on this machine");
        return;
    }

    auto real_vfs = std::make_shared<FileSys::RealVfsFilesystem>();
    auto file = real_vfs->OpenFile(NupPath, FileSys::OpenMode::Read);
    REQUIRE(file != nullptr);

    FileSys::NupContainer nup{file};
    REQUIRE(nup.GetStatus() == Loader::ResultStatus::Success);

    const auto base_idx = nup.FindBasePackage();
    const auto upd_idx = nup.FindBestUpdatePackage();
    auto merged_exefs = FileSys::NupContainer::MergeShadowDirs(
        base_idx ? nup.GetExeFSDir(*base_idx) : FileSys::VirtualDir{},
        upd_idx ? nup.GetExeFSDir(*upd_idx) : FileSys::VirtualDir{});
    auto merged_romfs = FileSys::NupContainer::MergeShadowDirs(
        base_idx ? nup.GetRomFSDir(*base_idx) : FileSys::VirtualDir{},
        upd_idx ? nup.GetRomFSDir(*upd_idx) : FileSys::VirtualDir{});
    REQUIRE(merged_exefs != nullptr);
    REQUIRE(merged_romfs != nullptr);

    ScanStats stats{};
    u64 content_fp = 0;
    const auto all_shaders = VideoCore::Precompile::ScanDirectoryTrees(
        merged_exefs, merged_romfs, 0x0400486024DEC000ULL, ScanOptions{}, &stats, &content_fp);

    CHECK(content_fp != 0);
    CHECK(stats.zstd_dicts_loaded == 4);
    CHECK(stats.fsha_containers >= 10);
    CHECK(stats.bnsh_containers >= 50);
    CHECK(stats.sharcb_containers >= 10);
    CHECK(stats.ndsr_containers >= 3);
    CHECK(stats.errors_ignored == 0);
    REQUIRE(all_shaders.size() > 20000);

    bool has_vertex = false;
    bool has_fragment = false;
    bool has_compute = false;
    for (const auto& s : all_shaders) {
        has_vertex |= (s.stage == ScannedStage::Vertex);
        has_fragment |= (s.stage == ScannedStage::Fragment);
        has_compute |= (s.stage == ScannedStage::Compute);
    }
    CHECK(has_vertex);
    CHECK(has_fragment);
    CHECK(has_compute);

    // Sample across every container family plus AglShader.sharcb compute shaders without hanging CI.
    Shader::Profile profile{
        .supported_spirv = 0x00010600,
        .unified_descriptor_binding = true,
    };
    Shader::HostTranslateInfo host_info{};
    VideoCore::Precompile::PrecompilePools pools;

    std::size_t sampled = 0;
    std::size_t translated_ok = 0;
    std::size_t translated_compute = 0;
    std::size_t vertex_pos_inputs = 0;
    for (std::size_t i = 0; i < all_shaders.size(); ++i) {
        const auto& sh = all_shaders[i];
        if (sh.stage != ScannedStage::Vertex && sh.stage != ScannedStage::Fragment &&
            sh.stage != ScannedStage::Compute) {
            continue;
        }
        const bool is_agl = sh.source_name.find("AglShader.sharcb") != std::string::npos ||
                            sh.source_name.find("agl_ndsr") != std::string::npos;
        if (!is_agl && (i % 100) != 0) {
            continue;
        }
        ++sampled;
        VideoCore::Precompile::TranslatedShader tr;
        std::string err;
        if (VideoCore::Precompile::TranslateScannedShader(pools, sh, profile, host_info, 49152U,
                                                          false, tr, &err)) {
            REQUIRE_FALSE(tr.spirv.empty());
            CHECK(tr.spirv[0] == 0x07230203U);
            ++translated_ok;
            if (tr.stage == Shader::Stage::Compute) {
                ++translated_compute;
            }
            if (tr.stage == Shader::Stage::VertexB &&
                tr.info.loads.AnyComponent(Shader::IR::Attribute::PositionX)) {
                ++vertex_pos_inputs;
            }
        }
    }

    CHECK(sampled >= 200);
    CHECK(translated_compute >= 50);
    CHECK(translated_ok * 10 > sampled * 7);
    CHECK(vertex_pos_inputs == 0);
}

