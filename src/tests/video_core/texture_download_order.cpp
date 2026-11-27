// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "video_core/texture_cache/pending_download.h"

namespace {

struct TestImage {};
using Download = VideoCommon::PendingImageDownload<TestImage>;

TEST_CASE("Texture downloads publish overlapping DMA and image data in modification order",
          "[video_core][texture_download]") {
    std::array downloads{
        Download{false, 0, Common::SlotId{0}, {}, 2},
        Download{true, 1, Common::SlotId{1}, {}, 4},
        Download{true, 2, Common::SlotId{2}, {}, 1},
        Download{false, 1, Common::SlotId{3}, {}, 3},
    };

    VideoCommon::OrderPendingImageDownloads(downloads);

    CHECK(downloads[0].object_id == Common::SlotId{2});
    CHECK(downloads[1].object_id == Common::SlotId{0});
    CHECK(downloads[2].object_id == Common::SlotId{3});
    CHECK(downloads[3].object_id == Common::SlotId{1});
}

TEST_CASE("Same-version whole-image downloads preserve first-request alias priority",
          "[video_core][texture_download]") {
    Common::SlotVector<TestImage> images;
    const auto first = images.insert();
    const auto second = images.insert();
    std::vector downloads{
        Download{true, 0, first, {}, 5},
        Download{true, 1, second, {}, 5},
        Download{true, 2, first, {}, 5},
    };

    REQUIRE(VideoCommon::DeduplicatePendingImageDownloads(downloads, images) == 1);
    VideoCommon::OrderPendingImageDownloads(downloads);

    REQUIRE(downloads.size() == 2);
    CHECK(downloads[0].object_id == second);
    CHECK(downloads[1].object_id == first);
}

} // namespace
