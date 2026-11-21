// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <numeric>
#include <vector>

#include "common/logging.h"
#include "common/settings.h"
#include "core/core.h"
#include "core/file_sys/common_funcs.h"
#include "core/file_sys/content_archive.h"
#include "core/file_sys/control_metadata.h"
#include "core/file_sys/nca_metadata.h"
#include "core/file_sys/patch_manager.h"
#include "core/file_sys/registered_cache.h"
#include "core/hle/kernel/k_event.h"
#include "core/hle/service/aoc/addon_content_manager.h"
#include "core/hle/service/aoc/purchase_event_manager.h"
#include "core/hle/service/cmif_serialization.h"
#include "core/hle/service/filesystem/filesystem.h"
#include "core/hle/service/ipc_helpers.h"
#include "core/hle/service/server_manager.h"
#include "core/hle/service/am/am_results.h"
#include "core/loader/loader.h"

namespace Service::AOC {

static bool CheckAOCTitleIDMatchesBase(u64 title_id, u64 base) {
    return FileSys::GetBaseTitleID(title_id) == base;
}

static u64 GetCallerBaseTitleID(Core::System& system, const ClientProcessId& process_id) {
    return FileSys::GetBaseTitleID(system.ResolveCallerProgramId(*process_id));
}

static std::vector<u64> AccumulateAOCTitleIDs(Core::System& system) {
    std::vector<u64> add_on_content;
    const auto& rcu = system.GetContentProvider();
    const auto list =
        rcu.ListEntriesFilter(FileSys::TitleType::AOC, FileSys::ContentRecordType::Data);
    std::transform(list.begin(), list.end(), std::back_inserter(add_on_content),
                   [](const FileSys::ContentProviderEntry& rce) { return rce.title_id; });
    add_on_content.erase(
        std::remove_if(
            add_on_content.begin(), add_on_content.end(),
            [&rcu](u64 tid) {
                return rcu.GetEntry(tid, FileSys::ContentRecordType::Data)->GetStatus() !=
                       Loader::ResultStatus::Success;
            }),
        add_on_content.end());
    return add_on_content;
}

IAddOnContentManager::IAddOnContentManager(Core::System& system_)
    : ServiceFramework{system_, "aoc:u"}, add_on_content{AccumulateAOCTitleIDs(system)},
      service_context{system_, "aoc:u"} {
    // clang-format off
    static const FunctionInfo functions[] = {
        {0, nullptr, "CountAddOnContentByApplicationId"},
        {1, nullptr, "ListAddOnContentByApplicationId"},
        {2, D<&IAddOnContentManager::CountAddOnContent>, "CountAddOnContent"},
        {3, D<&IAddOnContentManager::ListAddOnContent>, "ListAddOnContent"},
        {4, nullptr, "GetAddOnContentBaseIdByApplicationId"},
        {5, D<&IAddOnContentManager::GetAddOnContentBaseId>, "GetAddOnContentBaseId"},
        {6, nullptr, "PrepareAddOnContentByApplicationId"},
        {7, D<&IAddOnContentManager::PrepareAddOnContent>, "PrepareAddOnContent"},
        {8, D<&IAddOnContentManager::GetAddOnContentListChangedEvent>, "GetAddOnContentListChangedEvent"},
        {9, &IAddOnContentManager::StubReplyZeroed, "GetAddOnContentLostErrorCode"},
        {10, D<&IAddOnContentManager::GetAddOnContentListChangedEventWithProcessId>, "GetAddOnContentListChangedEventWithProcessId"},
        {11, D<&IAddOnContentManager::NotifyMountAddOnContent>, "NotifyMountAddOnContent"},
        {12, D<&IAddOnContentManager::NotifyUnmountAddOnContent>, "NotifyUnmountAddOnContent"},
        {13, &IAddOnContentManager::StubReplyZeroed, "IsAddOnContentMountedForDebug"},
        {50, D<&IAddOnContentManager::CheckAddOnContentMountStatus>, "CheckAddOnContentMountStatus"},
        {100, D<&IAddOnContentManager::CreateEcPurchasedEventManager>, "CreateEcPurchasedEventManager"},
        {101, D<&IAddOnContentManager::CreatePermanentEcPurchasedEventManager>, "CreatePermanentEcPurchasedEventManager"},
        {110, &IAddOnContentManager::StubReplyInterface, "CreateContentsServiceManager"},
        {200, &IAddOnContentManager::StubReplyZeroed, "SetRequiredAddOnContentsOnContentsAvailabilityTransition"},
        {300, &IAddOnContentManager::StubReplyZeroed, "SetupHostAddOnContent"},
        {301, &IAddOnContentManager::StubReplyZeroed, "GetRegisteredAddOnContentPath"},
        {302, &IAddOnContentManager::StubReplyZeroed, "UpdateCachedList"},
    };
    // clang-format on

    RegisterHandlers(functions);

    aoc_change_event = service_context.CreateEvent("GetAddOnContentListChanged:Event");
}

IAddOnContentManager::~IAddOnContentManager() {
    service_context.CloseEvent(aoc_change_event);
}

std::vector<u64> IAddOnContentManager::GetAddOnContentTitleIds(
    const ClientProcessId& process_id) const {
    const auto current = GetCallerBaseTitleID(system, process_id);

    std::vector<u64> title_ids;
    for (u64 title_id : add_on_content) {
        if (CheckAOCTitleIDMatchesBase(title_id, current)) {
            title_ids.push_back(title_id);
        }
    }

    // Add-on content packaged in the loader's container is not installed content, so the content
    // provider cannot list it; the file system controller knows what the loader provided.
    const auto caller = system.ResolveCallerProgramId(*process_id);
    for (u64 title_id :
         system.GetFileSystemController().GetAddOnContentTitleIds(*process_id, caller)) {
        if (std::find(title_ids.begin(), title_ids.end(), title_id) == title_ids.end()) {
            title_ids.push_back(title_id);
        }
    }

    std::sort(title_ids.begin(), title_ids.end());
    return title_ids;
}

Result IAddOnContentManager::CountAddOnContent(Out<u32> out_count, ClientProcessId process_id) {
    LOG_DEBUG(Service_AOC, "called. process_id={}", process_id.pid);

    const auto current = GetCallerBaseTitleID(system, process_id);

    const auto& disabled = Settings::values.disabled_addons[current];
    if (std::find(disabled.begin(), disabled.end(), "DLC") != disabled.end()) {
        *out_count = 0;
        LOG_DEBUG(Service_AOC, "add-on content disabled, reporting 0");
        R_SUCCEED();
    }

    *out_count = static_cast<u32>(GetAddOnContentTitleIds(process_id).size());

    LOG_DEBUG(Service_AOC, "reporting {} add-on content title(s)", *out_count);
    R_SUCCEED();
}

Result IAddOnContentManager::ListAddOnContent(Out<u32> out_count,
                                              OutBuffer<BufferAttr_HipcMapAlias> out_addons,
                                              u32 offset, u32 count, ClientProcessId process_id) {
    LOG_DEBUG(Service_AOC, "called with offset={}, count={}, process_id={}", offset, count,
              process_id.pid);

    const auto current = GetCallerBaseTitleID(system, process_id);

    std::vector<u32> out;
    const auto& disabled = Settings::values.disabled_addons[current];
    if (std::find(disabled.begin(), disabled.end(), "DLC") == disabled.end()) {
        for (u64 content_id : GetAddOnContentTitleIds(process_id)) {
            out.push_back(static_cast<u32>(FileSys::GetAOCID(content_id)));
        }
    }

    R_UNLESS(out.size() >= offset, AM::ResultApplicationRecordNotFound);

    *out_count = static_cast<u32>(std::min<size_t>(out.size() - offset, count));
    std::rotate(out.begin(), out.begin() + offset, out.end());

    std::memcpy(out_addons.data(), out.data(), *out_count * sizeof(u32));

    LOG_DEBUG(Service_AOC, "listed {} add-on content id(s) from offset {}", *out_count, offset);
    R_SUCCEED();
}

Result IAddOnContentManager::GetAddOnContentBaseId(Out<u64> out_title_id,
                                                   ClientProcessId process_id) {
    LOG_DEBUG(Service_AOC, "called. process_id={}", process_id.pid);

    const auto title_id = system.ResolveCallerProgramId(*process_id);

    // Add-on content packaged in the container may carry a different title type byte than the
    // program (a Switch 2 edition program is 0x04, its DLC 0x01), so its own title id is what
    // defines the add-on content base.
    const auto container_addons =
        system.GetFileSystemController().GetAddOnContentTitleIds(*process_id, title_id);
    if (!container_addons.empty()) {
        *out_title_id = FileSys::GetAOCBaseTitleID(container_addons.front());
        LOG_DEBUG(Service_AOC, "reporting add-on content base id {:016X} from the container",
                  *out_title_id);
        R_SUCCEED();
    }

    const FileSys::PatchManager pm{title_id, system.GetFileSystemController(),
                                   system.GetContentProvider()};

    const auto res = pm.GetControlMetadata();
    if (res.first == nullptr) {
        *out_title_id = FileSys::GetAOCBaseTitleID(title_id);
        R_SUCCEED();
    }

    *out_title_id = res.first->GetDLCBaseTitleId();

    R_SUCCEED();
}

Result IAddOnContentManager::PrepareAddOnContent(s32 addon_index, ClientProcessId process_id) {
    // Nothing has to be staged: the loader already exposes every add-on content it carries to
    // nn::fs, and installed content is visible to it as soon as it is registered.
    const auto title_ids = GetAddOnContentTitleIds(process_id);
    const auto addon = std::find_if(title_ids.begin(), title_ids.end(), [addon_index](u64 id) {
        return static_cast<s32>(FileSys::GetAOCID(id)) == addon_index;
    });

    if (addon == title_ids.end()) {
        LOG_WARNING(Service_AOC,
                    "(STUBBED) called with addon_index={}, process_id={}: no such "
                    "add-on content",
                    addon_index, process_id.pid);
        R_SUCCEED();
    }

    LOG_DEBUG(Service_AOC, "called with addon_index={}, process_id={}: {:016X} prepared",
              addon_index, process_id.pid, *addon);
    R_SUCCEED();
}

Result IAddOnContentManager::GetAddOnContentListChangedEvent(
    OutCopyHandle<Kernel::KReadableEvent> out_event) {
    LOG_WARNING(Service_AOC, "(STUBBED) called");

    *out_event = &aoc_change_event->GetReadableEvent();

    R_SUCCEED();
}

Result IAddOnContentManager::GetAddOnContentListChangedEventWithProcessId(
    OutCopyHandle<Kernel::KReadableEvent> out_event, ClientProcessId process_id) {
    LOG_WARNING(Service_AOC, "(STUBBED) called");

    *out_event = &aoc_change_event->GetReadableEvent();

    R_SUCCEED();
}

Result IAddOnContentManager::NotifyMountAddOnContent() {
    // Mounting itself is nn::fs's job; this only tells aoc:u the game has mounted an add-on.
    LOG_DEBUG(Service_AOC, "called");

    R_SUCCEED();
}

Result IAddOnContentManager::NotifyUnmountAddOnContent() {
    LOG_DEBUG(Service_AOC, "called");

    R_SUCCEED();
}

Result IAddOnContentManager::CheckAddOnContentMountStatus() {
    LOG_WARNING(Service_AOC, "(STUBBED) called");

    R_SUCCEED();
}

Result IAddOnContentManager::CreateEcPurchasedEventManager(
    OutInterface<IPurchaseEventManager> out_interface) {
    LOG_WARNING(Service_AOC, "(STUBBED) called");

    *out_interface = std::make_shared<IPurchaseEventManager>(system);

    R_SUCCEED();
}

Result IAddOnContentManager::CreatePermanentEcPurchasedEventManager(
    OutInterface<IPurchaseEventManager> out_interface) {
    LOG_WARNING(Service_AOC, "(STUBBED) called");

    *out_interface = std::make_shared<IPurchaseEventManager>(system);

    R_SUCCEED();
}

void LoopProcess(Core::System& system) {
    auto server_manager = std::make_unique<ServerManager>(system);
    server_manager->RegisterNamedService("aoc:u", std::make_shared<IAddOnContentManager>(system));
    ServerManager::RunServer(std::move(server_manager));
}

} // namespace Service::AOC
