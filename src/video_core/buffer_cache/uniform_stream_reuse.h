// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <concepts>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <span>
#include <type_traits>

#include "common/common_types.h"

namespace VideoCommon {

/// What a buffer cache runtime offers for stream uniform reuse.
///  - StreamUniformRef: a small trivially copyable description of one stream allocation (the
///    host buffer, its offset and whatever else the runtime needs to add it to a descriptor
///    payload again, e.g. the device address of the descriptor buffer path).
///  - LastStreamUniformRef(): the allocation made by the latest BindMappedUniformBuffer call.
///  - RebindStreamUniform(ref, size): adds `ref` to the descriptor payload exactly as
///    BindMappedUniformBuffer added the allocation the first time.
template <class R>
concept StreamUniformReusable = requires(R& runtime, u32 size) {
    typename R::StreamUniformRef;
    requires std::is_trivially_copyable_v<typename R::StreamUniformRef>;
    { runtime.LastStreamUniformRef() } -> std::same_as<typename R::StreamUniformRef>;
    runtime.RebindStreamUniform(runtime.LastStreamUniformRef(), size);
    { runtime.CurrentTick() } -> std::convertible_to<u64>;
};

namespace detail {
inline const bool uniform_stream_reuse_enabled = [] {
    const char* const value = std::getenv("EDEN_NX2_UBOREUSE");
    return value == nullptr || value[0] != '0';
}();

[[nodiscard]] inline u64 QuickTag(const u8* src, u32 size) noexcept {
    if (size >= 8) {
        u64 head{};
        u64 tail{};
        std::memcpy(&head, src, sizeof(u64));
        std::memcpy(&tail, src + size - sizeof(u64), sizeof(u64));
        return head ^ (tail * 0x9e3779b97f4a7c15ULL);
    }
    u64 acc = size;
    for (u32 i = 0; i < size; ++i) {
        acc = (acc * 131U) ^ src[i];
    }
    return acc;
}

[[nodiscard]] inline bool FastBytesEqual(const u8* lhs, const u8* rhs, u32 size) noexcept {
    if (size > 64) {
        u64 a0{};
        u64 b0{};
        u64 a1{};
        u64 b1{};
        std::memcpy(&a0, lhs, sizeof(u64));
        std::memcpy(&b0, rhs, sizeof(u64));
        if (a0 != b0) {
            return false;
        }
        std::memcpy(&a1, lhs + size - sizeof(u64), sizeof(u64));
        std::memcpy(&b1, rhs + size - sizeof(u64), sizeof(u64));
        if (a1 != b1) {
            return false;
        }
        return std::memcmp(lhs + sizeof(u64), rhs + sizeof(u64), size - sizeof(u64) * 2) == 0;
    }
    if (size >= 8) {
        u64 a0{};
        u64 b0{};
        u64 a1{};
        u64 b1{};
        std::memcpy(&a0, lhs, sizeof(u64));
        std::memcpy(&b0, rhs, sizeof(u64));
        if (a0 != b0) {
            return false;
        }
        std::memcpy(&a1, lhs + size - sizeof(u64), sizeof(u64));
        std::memcpy(&b1, rhs + size - sizeof(u64), sizeof(u64));
        if (a1 != b1) {
            return false;
        }
        for (u32 off = 8; off + 8 < size; off += 8) {
            std::memcpy(&a0, lhs + off, sizeof(u64));
            std::memcpy(&b0, rhs + off, sizeof(u64));
            if (a0 != b0) {
                return false;
            }
        }
        return true;
    }
    if (size >= 4) {
        u32 a0{};
        u32 b0{};
        u32 a1{};
        u32 b1{};
        std::memcpy(&a0, lhs, sizeof(u32));
        std::memcpy(&b0, rhs, sizeof(u32));
        std::memcpy(&a1, lhs + size - sizeof(u32), sizeof(u32));
        std::memcpy(&b1, rhs + size - sizeof(u32), sizeof(u32));
        return a0 == b0 && a1 == b1;
    }
    for (u32 i = 0; i < size; ++i) {
        if (lhs[i] != rhs[i]) {
            return false;
        }
    }
    return true;
}
} // namespace detail

/// EDEN_NX2_UBOREUSE=0 turns the reuse off (every small uniform buffer is copied, as before).
[[nodiscard]] inline bool UniformStreamReuseEnabled() noexcept {
    return detail::uniform_stream_reuse_enabled;
}

/// Set-associative cache of small streamed uniform buffers owned by the current command-buffer
/// tick. When identical bytes are bound again within the same tick, the existing stream
/// allocation is rebound without calling BindMappedUniformBuffer or copying bytes.
template <class Ref, size_t NumStagesValue, size_t NumSlotsValue, size_t WayCountValue = 4>
class UniformReuseCache {
public:
    static constexpr u32 MaxBytes = 4096; // BufferCache's DEFAULT_SKIP_CACHE_SIZE
    static constexpr size_t NumStages = NumStagesValue;
    static constexpr size_t NumSlots = NumSlotsValue;
    static constexpr size_t WayCount = WayCountValue;

    struct WayMeta {
        Ref ref{};
        u64 tick{};
        u64 device_addr{};
        u64 quick_tag{};
        u32 size{}; // 0: nothing streamed yet
    };

    struct SlotSet {
        // Keep all 4 ways' metadata contiguous so misses stay inside ~3 cache lines total.
        std::array<WayMeta, WayCount> metas{};
        std::array<u8, WayCount> mru = [] {
            std::array<u8, WayCount> init{};
            for (size_t i = 0; i < WayCount; ++i) {
                init[i] = static_cast<u8>(i);
            }
            return init;
        }();

        void Reset() noexcept {
            for (size_t i = 0; i < WayCount; ++i) {
                metas[i].size = 0;
                metas[i].tick = 0;
                metas[i].device_addr = 0;
                metas[i].quick_tag = 0;
                mru[i] = static_cast<u8>(i);
            }
        }

        void Promote(size_t pos) noexcept {
            const u8 chosen = mru[pos];
            for (size_t i = pos; i > 0; --i) {
                mru[i] = mru[i - 1];
            }
            mru[0] = chosen;
        }
    };

    void Reset() noexcept {
        for (auto& stage : sets) {
            for (auto& set : stage) {
                set.Reset();
            }
        }
    }

    std::array<std::array<SlotSet, NumSlots>, NumStages> sets{};
    std::array<std::array<std::array<std::array<u8, MaxBytes>, WayCount>, NumSlots>, NumStages>
        payloads{};
    std::array<u8, MaxBytes> scratch{};
};

template <class Ref>
using UniformStreamReuse = UniformReuseCache<Ref, 5, 18, 4>;

template <class Ref>
using UniformComputeStreamReuse = UniformReuseCache<Ref, 1, 18, 4>;

template <class Ref>
[[nodiscard]] inline UniformStreamReuse<Ref>& UniformStreamReuseState() noexcept {
    static UniformStreamReuse<Ref> state;
    return state;
}

template <class Ref>
[[nodiscard]] inline UniformComputeStreamReuse<Ref>& UniformComputeStreamReuseState() noexcept {
    static UniformComputeStreamReuse<Ref> state;
    return state;
}

/// Forgets every slot. Call it when a buffer cache (and so its runtime, scheduler and stream
/// buffer) is created: ticks restart with a new scheduler and the old references are dead.
template <class Runtime>
inline void ResetUniformStreamReuse() noexcept {
    if constexpr (StreamUniformReusable<Runtime>) {
        UniformStreamReuseState<typename Runtime::StreamUniformRef>().Reset();
        UniformComputeStreamReuseState<typename Runtime::StreamUniformRef>().Reset();
    }
}

namespace detail {
template <class Reuse, class Runtime>
[[nodiscard]] inline bool TryRebindOrStreamUniform(Reuse& reuse, Runtime& runtime,
                                                   size_t cache_stage, size_t runtime_stage,
                                                   u32 index, u32 binding_index, u64 device_addr,
                                                   const u8* src, u32 size) {
    auto& set = reuse.sets[cache_stage][index];
    auto& set_payloads = reuse.payloads[cache_stage][index];
    const u64 current_tick = runtime.CurrentTick();
    const u64 tag = QuickTag(src, size);

    for (size_t pos = 0; pos < Reuse::WayCount; ++pos) {
        const u8 slot_idx = set.mru[pos];
        const auto& meta = set.metas[slot_idx];
        if (meta.size == size && meta.tick == current_tick && meta.quick_tag == tag &&
            FastBytesEqual(set_payloads[slot_idx].data(), src, size)) {
            runtime.RebindStreamUniform(meta.ref, size);
            set.Promote(pos);
            return true;
        }
    }

    const std::span<u8> span = runtime.BindMappedUniformBuffer(runtime_stage, binding_index, size);
    size_t target_pos = Reuse::WayCount - 1;
    for (size_t pos = 0; pos < Reuse::WayCount; ++pos) {
        const auto& meta = set.metas[set.mru[pos]];
        if (meta.size == 0 || meta.device_addr == device_addr) {
            target_pos = pos;
            break;
        }
    }
    const u8 victim_idx = set.mru[target_pos];
    auto& meta = set.metas[victim_idx];
    meta.ref = runtime.LastStreamUniformRef();
    meta.tick = current_tick;
    meta.device_addr = device_addr;
    meta.quick_tag = tag;
    meta.size = size;
    std::memcpy(set_payloads[victim_idx].data(), src, size);
    std::memcpy(span.data(), src, size);
    set.Promote(target_pos);
    return true;
}

template <class Reuse, class Runtime, class DeviceMemory>
[[nodiscard]] inline bool BindSmallUniformInCache(Reuse& reuse, Runtime& runtime,
                                                  DeviceMemory& device_memory, size_t cache_stage,
                                                  size_t runtime_stage, u32 index,
                                                  u32 binding_index, u64 device_addr, u32 size) {
    if constexpr (requires {
                      { device_memory.GetSpan(device_addr, size) } -> std::convertible_to<const u8*>;
                  }) {
        if (const u8* const direct = device_memory.GetSpan(device_addr, size); direct != nullptr)
            [[likely]] {
            return TryRebindOrStreamUniform(reuse, runtime, cache_stage, runtime_stage, index,
                                            binding_index, device_addr, direct, size);
        }
    }

    u8* const scratch = reuse.scratch.data();
    device_memory.ReadBlockUnsafe(device_addr, scratch, size);
    return TryRebindOrStreamUniform(reuse, runtime, cache_stage, runtime_stage, index, binding_index,
                                    device_addr, scratch, size);
}
} // namespace detail

/// Binds the small uniform buffer at `device_addr` for (stage, cbuf `index`) through the stream
/// buffer, reusing a prior allocation when the bytes and the command buffer are the
/// same. Returns false (nothing bound) when the buffer does not qualify, so the caller binds it
/// the classic way.
template <class Runtime, class DeviceMemory>
[[nodiscard]] bool BindSmallUniformReusing(Runtime& runtime, DeviceMemory& device_memory,
                                           size_t stage, u32 index, u32 binding_index,
                                           u64 device_addr, u32 size) {
    using Reuse = UniformStreamReuse<typename Runtime::StreamUniformRef>;
    if (size == 0 || size > Reuse::MaxBytes || stage >= Reuse::NumStages ||
        index >= Reuse::NumSlots) {
        return false;
    }
    Reuse& reuse = UniformStreamReuseState<typename Runtime::StreamUniformRef>();
    return detail::BindSmallUniformInCache(reuse, runtime, device_memory, stage, stage, index,
                                           binding_index, device_addr, size);
}

/// Same as BindSmallUniformReusing, dedicated to the compute constant buffer stage.
template <class Runtime, class DeviceMemory>
[[nodiscard]] bool BindSmallComputeUniformReusing(Runtime& runtime, DeviceMemory& device_memory,
                                                  u32 index, u32 binding_index, u64 device_addr,
                                                  u32 size) {
    using Reuse = UniformComputeStreamReuse<typename Runtime::StreamUniformRef>;
    if (size == 0 || size > Reuse::MaxBytes || index >= Reuse::NumSlots) {
        return false;
    }
    Reuse& reuse = UniformComputeStreamReuseState<typename Runtime::StreamUniformRef>();
    return detail::BindSmallUniformInCache(reuse, runtime, device_memory, 0, 0, index,
                                           binding_index, device_addr, size);
}

} // namespace VideoCommon
