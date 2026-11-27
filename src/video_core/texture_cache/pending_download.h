// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <memory>
#include <unordered_set>
#include <cstddef>
#include <utility>
#include "common/slot_vector.h"

namespace VideoCommon {

template <class Image>
struct PendingImageDownload {
    bool is_swizzle;
    size_t async_buffer_id;
    Common::SlotId object_id;
    std::shared_ptr<Image> retired_image{};
    u64 modification_tick = 0;

    Image& GetImage(Common::SlotVector<Image>& images) const {
        return retired_image ? *retired_image : images[object_id];
    }
};

// Whole-image copies are recorded together at commit time. Repeated requests
// for the same live image therefore observe the same contents. Keep the first
// occurrence to preserve the writeback order for aliases with equal modification ticks.
template <class Image, class Downloads>
size_t DeduplicatePendingImageDownloads(Downloads& downloads, Common::SlotVector<Image>& images) {
    const size_t previous_size = downloads.size();
    if (previous_size < 2) {
        return 0;
    }
    std::unordered_set<const Image*> seen;
    const auto end = std::remove_if(downloads.begin(), downloads.end(), [&](const auto& download) {
        return download.is_swizzle && !seen.insert(&download.GetImage(images)).second;
    });
    downloads.erase(end, downloads.end());
    return previous_size - downloads.size();
}

// Publish overlapping data in modification order. Before staging whole images, their buffer
// indices hold request order so same-version aliases retain their previous priority.
template <class Downloads>
void OrderPendingImageDownloads(Downloads& downloads) {
    const auto older = [](const auto& lhs, const auto& rhs) {
        if (lhs.modification_tick != rhs.modification_tick) {
            return lhs.modification_tick < rhs.modification_tick;
        }
        if (lhs.is_swizzle != rhs.is_swizzle) {
            return lhs.is_swizzle;
        }
        return lhs.is_swizzle ? lhs.async_buffer_id > rhs.async_buffer_id
                              : lhs.async_buffer_id < rhs.async_buffer_id;
    };
    if (downloads.size() > 1 && !std::ranges::is_sorted(downloads, older)) {
        std::ranges::sort(downloads, older);
    }
}

// Cache slots may be recycled before queued GPU downloads are submitted or
// consumed. Keep the actual image, including its metadata, until those finish.
template <class Image, class Downloads, class Batches>
bool RetainPendingImageDownloads(Common::SlotId id, Image& image, Downloads& pending,
                                 Batches& submitted) {
    std::shared_ptr<Image> retained;
    const auto retain = [&](auto& downloads) {
        for (auto& download : downloads) {
            if (!download.is_swizzle || download.object_id != id || download.retired_image) continue;
            if (!retained) retained = std::make_shared<Image>(std::move(image));
            download.retired_image = retained;
        }
    };
    retain(pending);
    for (auto& downloads : submitted) retain(downloads);
    return retained != nullptr;
}

} // namespace VideoCommon
