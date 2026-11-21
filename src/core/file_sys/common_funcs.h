// SPDX-FileCopyrightText: Copyright 2020 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "common/common_types.h"

namespace FileSys {

constexpr u64 AOC_TITLE_ID_MASK = 0x7FF;
constexpr u64 AOC_TITLE_ID_OFFSET = 0x1000;
constexpr u64 BASE_TITLE_ID_MASK = 0xFFFFFFFFFFFFE000;

/**
 * Gets the base title ID from a given title ID.
 *
 * @param title_id The title ID.
 * @returns The base title ID.
 */
[[nodiscard]] constexpr u64 GetBaseTitleID(u64 title_id) {
    return title_id & BASE_TITLE_ID_MASK;
}

/**
 * Gets the base title ID with a program index offset from a given title ID.
 *
 * @param title_id The title ID.
 * @param program_index The program index.
 * @returns The base title ID with a program index offset.
 */
[[nodiscard]] constexpr u64 GetBaseTitleIDWithProgramIndex(u64 title_id, u64 program_index) {
    return GetBaseTitleID(title_id) + program_index;
}

/**
 * Gets the AOC (Add-On Content) base title ID from a given title ID.
 *
 * @param title_id The title ID.
 * @returns The AOC base title ID.
 */
[[nodiscard]] constexpr u64 GetAOCBaseTitleID(u64 title_id) {
    return GetBaseTitleID(title_id) + AOC_TITLE_ID_OFFSET;
}

/**
 * Gets the AOC (Add-On Content) ID from a given AOC title ID.
 *
 * @param aoc_title_id The AOC title ID.
 * @returns The AOC ID.
 */
[[nodiscard]] constexpr u64 GetAOCID(u64 aoc_title_id) {
    return aoc_title_id & AOC_TITLE_ID_MASK;
}

/**
 * Whether `requested`, a data id nn::fs was asked to open, names the add-on content stored as
 * `addon_title_id`. The title type byte is ignored: a Switch 2 edition program carries 0x04 where
 * the add-on content it ships keeps 0x01, and the data id may be derived from either side.
 *
 * @param requested The title id being opened.
 * @param addon_title_id The add-on content title id the loader provides.
 * @returns True when both are the same add-on content title id.
 */
[[nodiscard]] constexpr bool IsMatchingAddOnContentTitle(u64 requested, u64 addon_title_id) {
    constexpr u64 title_type_mask = 0xFF00000000000000ULL;
    return (requested & AOC_TITLE_ID_OFFSET) != 0 && (addon_title_id & AOC_TITLE_ID_OFFSET) != 0 &&
           (requested & ~title_type_mask) == (addon_title_id & ~title_type_mask);
}

} // namespace FileSys
