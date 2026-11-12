// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <type_traits>
#include <vector>

#include "common/common_types.h"

namespace Common {

template <class Traits>
class LeastRecentlyUsedCache {
    using ObjectType = typename Traits::ObjectType;
    using TickType = typename Traits::TickType;
    static constexpr u32 INVALID_INDEX = ~u32{0};

    struct Item {
        ObjectType obj;
        u32 next{INVALID_INDEX};
        u32 prev{INVALID_INDEX};
        TickType tick;
    };

public:
    LeastRecentlyUsedCache() = default;
    ~LeastRecentlyUsedCache() = default;

    size_t Insert(ObjectType obj, TickType tick) {
        const auto new_id = Build();
        auto& item = item_pool[new_id];
        item.obj = obj;
        item.tick = tick;
        Attach(new_id);
        return new_id;
    }

    void Touch(size_t id, TickType tick) {
        const u32 idx = static_cast<u32>(id);
        auto& item = item_pool[idx];
        if (item.tick >= tick) {
            return;
        }
        item.tick = tick;
        if (idx == last_item) {
            return;
        }
        Detach(idx);
        Attach(idx);
    }

    void Free(size_t id) {
        const u32 idx = static_cast<u32>(id);
        Detach(idx);
        auto& item = item_pool[idx];
        item.prev = INVALID_INDEX;
        item.next = INVALID_INDEX;
        free_items.push_back(idx);
    }

    template <typename Func>
    void ForEachItemBelow(TickType tick, Func&& func) {
        static constexpr bool RETURNS_BOOL =
            std::is_same_v<std::invoke_result_t<Func, ObjectType>, bool>;
        u32 iterator = first_item;
        while (iterator != INVALID_INDEX) {
            const auto& item = item_pool[iterator];
            if (static_cast<s64>(tick) - static_cast<s64>(item.tick) < 0) {
                return;
            }
            const u32 next = item.next;
            const ObjectType obj = item.obj;
            if constexpr (RETURNS_BOOL) {
                if (func(obj)) {
                    return;
                }
            } else {
                func(obj);
            }
            iterator = next;
        }
    }

private:
    u32 Build() {
        if (free_items.empty()) {
            const u32 item_id = static_cast<u32>(item_pool.size());
            item_pool.emplace_back();
            return item_id;
        }
        const u32 item_id = free_items.back();
        free_items.pop_back();
        auto& item = item_pool[item_id];
        item.next = INVALID_INDEX;
        item.prev = INVALID_INDEX;
        return item_id;
    }

    void Attach(u32 idx) {
        auto& item = item_pool[idx];
        if (first_item == INVALID_INDEX) {
            first_item = idx;
        }
        if (last_item == INVALID_INDEX) {
            last_item = idx;
        } else {
            item.prev = last_item;
            item_pool[last_item].next = idx;
            item.next = INVALID_INDEX;
            last_item = idx;
        }
    }

    void Detach(u32 idx) {
        const auto& item = item_pool[idx];
        if (item.prev != INVALID_INDEX) {
            item_pool[item.prev].next = item.next;
        }
        if (item.next != INVALID_INDEX) {
            item_pool[item.next].prev = item.prev;
        }
        if (idx == first_item) {
            first_item = item.next;
            if (first_item != INVALID_INDEX) {
                item_pool[first_item].prev = INVALID_INDEX;
            }
        }
        if (idx == last_item) {
            last_item = item.prev;
            if (last_item != INVALID_INDEX) {
                item_pool[last_item].next = INVALID_INDEX;
            }
        }
    }

    std::vector<Item> item_pool;
    std::vector<u32> free_items;
    u32 first_item{INVALID_INDEX};
    u32 last_item{INVALID_INDEX};
};

} // namespace Common
