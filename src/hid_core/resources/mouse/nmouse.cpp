// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "common/logging.h"
#include "core/core.h"
#include "core/hle/kernel/k_process.h"
#include "core/memory.h"
#include "hid_core/resources/mouse/nmouse.h"

namespace Service::HID {

Nmouse::Nmouse(Core::System& system_) : ControllerBase{system_.HIDCore()} {}

Nmouse::~Nmouse() = default;

void Nmouse::OnInit() {}
void Nmouse::OnRelease() {}

void Nmouse::OnUpdate(const Core::Timing::CoreTiming&) {}

void Nmouse::Initialize(Common::ProcessAddress t_mem, u64 size, Kernel::KProcess* process) {
    std::scoped_lock lock{mutex};
    if (size < MinimumSize) {
        LOG_ERROR(Service_HID, "Nmouse transfer memory too small: {:#x} (need {:#x})", size,
                  MinimumSize);
        return;
    }
    transfer_memory = t_mem;
    owner = process;
    WriteLayout();
    LOG_INFO(Service_HID, "Wrote the Switch 2 mouse state layout into the transfer memory "
                          "({} handles, {:#x} bytes)",
              Slots.size(), MinimumSize);
}

void Nmouse::Finalize() {
    std::scoped_lock lock{mutex};
    transfer_memory = {};
    owner = nullptr;
}

void Nmouse::WriteLayout() const {
    auto& memory = owner->GetMemory();
    for (std::size_t i = 0; i < Slots.size(); ++i) {
        const HandleEntry entry{.handle = 0x4e4d0000 + i, .is_valid = 1};
        memory.WriteBlock(transfer_memory + i * sizeof(HandleEntry), &entry, sizeof(entry));

        NmouseInfo info{};
        info.type_set = Slots[i].type_set;
        info.is_type_set_valid = 1;
        info.npad_id = 0; // Player 1
        info.is_npad_id_valid = Slots[i].has_npad ? 1 : 0;
        info.is_pointer_enabled = 1;
        memory.WriteBlock(transfer_memory + InfoOffset + i * InfoStride, &info, sizeof(info));

        // No mouse is fed, so the lifos stay empty: the application sees the handles but no
        // active mouse, as on a console with no Joy-Con in mouse mode.
        const NmouseLifo lifo{};
        memory.WriteBlock(transfer_memory + LifoOffset + i * LifoStride, &lifo, sizeof(lifo));
    }
    memory.Write8(transfer_memory + ShowCursorOffset, 1);
}

} // namespace Service::HID
