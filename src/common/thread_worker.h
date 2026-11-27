// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2020 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

#include "common/polyfill_thread.h"
#include "common/thread.h"
#include "common/unique_function.h"

namespace Common {

template <class StateType = void>
class StatefulThreadWorker {
    static constexpr bool with_state = !std::is_same_v<StateType, void>;

    struct DummyCallable {
        int operator()() const noexcept {
            return 0;
        }
    };

    using Task =
        std::conditional_t<with_state, UniqueFunction<void, StateType*>, UniqueFunction<void>>;
    using StateMaker = std::conditional_t<with_state, std::function<StateType()>, DummyCallable>;

    struct Request {
        Task task;
        const void* key;
    };

public:
    explicit StatefulThreadWorker(size_t num_workers, std::string name, StateMaker func = {},
                                  ThreadPlacement placement = ThreadPlacement::Default)
        : workers_queued{num_workers}, thread_name{std::move(name)} {
        const auto lambda = [this, func, placement](std::stop_token stop_token) {
            Common::SetCurrentThreadName(thread_name.c_str());
            if (placement != ThreadPlacement::Default) {
                Common::SetCurrentThreadPriority(ThreadPriority::Low);
            }
            switch (placement) {
            case ThreadPlacement::Efficiency:
                Common::SetCurrentThreadToEfficiencyCores();
                break;
            case ThreadPlacement::Background:
                Common::SetCurrentThreadToBackgroundWork();
                break;
            default:
                Common::SetCurrentThreadToAllCores();
                break;
            }
            {
                [[maybe_unused]] std::conditional_t<with_state, StateType, int> state{func()};
                while (!stop_token.stop_requested()) {
                    Task task;
                    {
                        std::unique_lock lock{queue_mutex};
                        if (requests.empty()) {
                            wait_condition.notify_all();
                        }
                        condition.wait(lock, stop_token,
                                       [this] { return !requests.empty(); });
                        if (stop_token.stop_requested()) {
                            break;
                        }
                        task = std::move(requests.front().task);
                        requests.pop_front();
                    }
                    if constexpr (with_state) {
                        task(&state);
                    } else {
                        task();
                    }
                    ++work_done;
                }
            }
            {
                std::scoped_lock lock{queue_mutex};
                ++workers_stopped;
            }
            wait_condition.notify_all();
        };
        threads.reserve(num_workers);
        for (size_t i = 0; i < num_workers; ++i) {
            threads.emplace_back(lambda);
        }
    }

    StatefulThreadWorker& operator=(const StatefulThreadWorker&) = delete;
    StatefulThreadWorker(const StatefulThreadWorker&) = delete;

    StatefulThreadWorker& operator=(StatefulThreadWorker&&) = delete;
    StatefulThreadWorker(StatefulThreadWorker&&) = delete;

    void QueueWork(Task work, const void* key = nullptr) {
        {
            std::unique_lock lock{queue_mutex};
            requests.emplace_back(std::move(work), key);
            ++work_scheduled;
        }
        condition.notify_one();
    }

    void WaitForRequests(std::stop_token stop_token = {}) {
        std::stop_callback callback(stop_token, [this] {
            for (auto& thread : threads) {
                thread.request_stop();
            }
        });
        std::unique_lock lock{queue_mutex};
        wait_condition.wait(lock, [this] {
            return workers_stopped >= workers_queued || work_done >= work_scheduled;
        });
    }

    /// Tasks queued or running.
    [[nodiscard]] size_t PendingWork() const noexcept {
        return work_scheduled.load(std::memory_order_relaxed) -
               work_done.load(std::memory_order_relaxed);
    }

    /// Runs the first queued task matching key, or the front task if no key is supplied.
    /// Stateless workers only.
    bool TryRunOne(const void* key = nullptr) {
        static_assert(!with_state, "TryRunOne has no state to give the task");
        Task task;
        {
            std::scoped_lock lock{queue_mutex};
            const auto it = key == nullptr
                                ? requests.begin()
                                : std::ranges::find(requests, key, &Request::key);
            if (it == requests.end()) {
                return false;
            }
            task = std::move(it->task);
            requests.erase(it);
        }
        task();
        {
            std::scoped_lock lock{queue_mutex};
            ++work_done;
        }
        wait_condition.notify_all();
        return true;
    }

    // Drops tasks no worker has started. Running tasks still finish.
    void ClearRequests() {
        std::scoped_lock lock{queue_mutex};
        work_scheduled -= requests.size();
        requests.clear();
    }

private:
    std::deque<Request> requests;
    std::mutex queue_mutex;
    std::condition_variable_any condition;
    std::condition_variable wait_condition;
    std::atomic<size_t> work_scheduled{};
    std::atomic<size_t> work_done{};
    std::atomic<size_t> workers_stopped{};
    std::atomic<size_t> workers_queued{};
    std::string thread_name;
    std::vector<std::jthread> threads;
};

using ThreadWorker = StatefulThreadWorker<>;

} // namespace Common
