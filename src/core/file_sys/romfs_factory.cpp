// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <memory>
#include "common/assert.h"
#include "common/common_types.h"
#include "common/logging.h"
#include "core/file_sys/common_funcs.h"
#include "core/file_sys/content_archive.h"
#include "core/file_sys/nca_metadata.h"
#include "core/file_sys/patch_manager.h"
#include "core/file_sys/registered_cache.h"
#include "core/file_sys/romfs.h"
#include "core/file_sys/romfs_factory.h"
#include "core/hle/kernel/k_process.h"
#include "core/hle/service/filesystem/filesystem.h"
#include "core/loader/loader.h"

namespace FileSys {

RomFSFactory::RomFSFactory(Loader::AppLoader& app_loader, ContentProvider& provider,
                           Service::FileSystem::FileSystemController& controller)
    : content_provider{provider}, filesystem_controller{controller} {
    // Load the RomFS from the app
    if (app_loader.ReadRomFS(file) != Loader::ResultStatus::Success) {
        LOG_WARNING(Service_FS, "Unable to read base RomFS");
    }

    updatable = app_loader.IsRomFSUpdatable();
}

RomFSFactory::~RomFSFactory() = default;

void RomFSFactory::SetPackedUpdate(VirtualFile update_raw_file) {
    packed_update_raw = std::move(update_raw_file);
}

void RomFSFactory::AddAddOnContent(u64 title_id, VirtualDir romfs_dir) {
    std::scoped_lock lock{addon_mutex};
    add_on_contents.insert_or_assign(title_id, AddOnContent{std::move(romfs_dir), nullptr});
}

std::vector<u64> RomFSFactory::GetAddOnContentTitleIds() const {
    std::scoped_lock lock{addon_mutex};

    std::vector<u64> title_ids;
    title_ids.reserve(add_on_contents.size());
    for (const auto& [title_id, entry] : add_on_contents) {
        title_ids.push_back(title_id);
    }
    // aoc:u indexes add-ons, so the order has to be stable between calls.
    std::sort(title_ids.begin(), title_ids.end());
    return title_ids;
}

VirtualFile RomFSFactory::OpenAddOnContent(u64 title_id) const {
    std::scoped_lock lock{addon_mutex};

    auto it = add_on_contents.find(title_id);
    if (it == add_on_contents.end()) {
        it = std::find_if(add_on_contents.begin(), add_on_contents.end(),
                          [title_id](const auto& entry) {
                              return IsMatchingAddOnContentTitle(title_id, entry.first);
                          });
    }
    if (it == add_on_contents.end()) {
        return nullptr;
    }
    if (it->second.romfs == nullptr) {
        it->second.romfs = CreateRomFS(it->second.dir);
        if (it->second.romfs != nullptr) {
            LOG_INFO(Service_FS, "Built add-on content RomFS for title_id={:016X}", it->first);
        }
    }
    if (it->first != title_id) {
        LOG_DEBUG(Service_FS,
                  "Data id {:016X} resolved to add-on content {:016X} (title type differs)",
                  title_id, it->first);
    }
    return it->second.romfs;
}

VirtualFile RomFSFactory::OpenCurrentProcess(u64 current_process_title_id) const {
    if (!updatable) {
        // LOCAL TEST (Zelda): a RomFS with the update already merged (NUP containers) still takes
        // LayeredFS mods (romfs folders of the title's mods); without, they were never applied.
        // No update is looked up (no base NCA). Built once.
        std::scoped_lock lock{layered_mutex};
        if (!layered_file) {
            const PatchManager patch_manager{current_process_title_id, filesystem_controller,
                                             content_provider};
            layered_file = patch_manager.PatchRomFS(nullptr, file, ContentRecordType::Program,
                                                    nullptr);
            if (!layered_file) {
                layered_file = file;
            }
        }
        return layered_file;
    }

    const auto type = ContentRecordType::Program;
    const auto nca = content_provider.GetEntry(current_process_title_id, type);
    const PatchManager patch_manager{current_process_title_id, filesystem_controller,
                                     content_provider};
    return patch_manager.PatchRomFS(nca.get(), file, ContentRecordType::Program, packed_update_raw);
}

VirtualFile RomFSFactory::OpenPatchedRomFS(u64 title_id, ContentRecordType type) const {
    auto nca = content_provider.GetEntry(title_id, type);

    if (nca == nullptr) {
        return nullptr;
    }

    const PatchManager patch_manager{title_id, filesystem_controller, content_provider};

    return patch_manager.PatchRomFS(nca.get(), nca->GetRomFS(), type);
}

VirtualFile RomFSFactory::OpenPatchedRomFSWithProgramIndex(u64 title_id, u8 program_index,
                                                           ContentRecordType type) const {
    const auto res_title_id = GetBaseTitleIDWithProgramIndex(title_id, program_index);

    return OpenPatchedRomFS(res_title_id, type);
}

VirtualFile RomFSFactory::Open(u64 title_id, StorageId storage, ContentRecordType type) const {
    // Add-on content packaged in the loader's container is a data storage of its own.
    if (const auto addon = OpenAddOnContent(title_id); addon != nullptr) {
        return addon;
    }

    const std::shared_ptr<NCA> res = GetEntry(title_id, storage, type);
    if (res == nullptr) {
        return nullptr;
    }

    return res->GetRomFS();
}

std::shared_ptr<NCA> RomFSFactory::GetEntry(u64 title_id, StorageId storage,
                                            ContentRecordType type) const {
    switch (storage) {
    case StorageId::None:
        return content_provider.GetEntry(title_id, type);
    case StorageId::NandSystem:
        return filesystem_controller.GetSystemNANDContents()->GetEntry(title_id, type);
    case StorageId::NandUser:
        return filesystem_controller.GetUserNANDContents()->GetEntry(title_id, type);
    case StorageId::SdCard:
        return filesystem_controller.GetSDMCContents()->GetEntry(title_id, type);
    case StorageId::Host:
    case StorageId::GameCard:
    default:
        UNIMPLEMENTED_MSG("Unimplemented storage_id={:02X}", static_cast<u8>(storage));
        return nullptr;
    }
}

} // namespace FileSys
