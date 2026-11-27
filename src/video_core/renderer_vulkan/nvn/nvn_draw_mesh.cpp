// SPDX-FileCopyrightText: Copyright 2026 Vortex727 (FUKKO)
// SPDX-License-Identifier: GPL-3.0-or-later

// Direct mesh-task draws of the NVN replay (nvnCommandBufferDrawMeshTasks, ...3D): task/mesh
// pipelines drawn with VK_EXT_mesh_shader or compute emulation. Everything a vertex draw
// does besides vertex input (pipeline, dynamic state, descriptors, render pass, render
// conditions) is the same `DrawContext::PrepareDraw`. The indirect entries are in
// nvn_draw_indirect.cpp.
//
// What the NVN2 draw macros do (MME code captured on the native path, Ocarina of Time): every
// mesh-task draw is a one-dimensional launch of `count` workgroups starting at `first`. A 3D
// draw writes its {x, y, z} into the driver bank (VC::DriverMeshDimsOffset) and launches x*y*z
// workgroups; the task program linearizes with those words. So the host launches (n, 1, 1)
// workgroups too and leaves the dims in the driver bank.

#include <algorithm>
#include <array>
#include <limits>
#include <span>

#include <fmt/format.h>

#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "video_core/renderer_vulkan/nvn/nvn_draw.h"

namespace Vulkan::Nvn {

void DrawContext::DrawMeshTasks(const VC::DrawMeshTasksPayload& p) {
    if (p.indirect != 0) {
        return DrawMeshTasksIndirect(p);
    }
    if (p.dims == 3) {
        // The macro writes the dims whatever the count (the words stay for later draws).
        const std::array<u32, 3> dims{p.x, p.y, p.z};
        if (dims != mesh_dims) {
            mesh_dims = dims;
            ++mesh_dims_gen;
        }
    }
    if (p.x == 0 || p.y == 0 || p.z == 0) {
        return; // no workgroups: the GPU launches nothing
    }
    constexpr u64 MAX_GROUPS = std::numeric_limits<u32>::max();
    const u64 xy = u64{p.x} * p.y;
    if (xy > MAX_GROUPS / p.z) {
        ++counters.draws;
        ++counters.draws_skipped;
        ++counters.mesh_draws_skipped;
        host->HostUnsupported("DrawMeshTasks", "the workgroup count exceeds 32 bits");
        return;
    }
    const u32 groups = static_cast<u32>(xy * p.z);
    if (p.first != 0) {
        // VK_EXT_mesh_shader numbers the task workgroups of a draw from 0; there is no first one.
        ++counters.draws;
        ++counters.draws_skipped;
        host->HostUnsupported("DrawMeshTasks",
                              "a first task workgroup other than 0 (VK_EXT_mesh_shader has none)");
        return;
    }
    if (!host->HostBeginRenderCondition()) {
        return;
    }
    PreparedDraw prepared;
    HostIndex host_index;
    dbg_call = {groups, 1, 0, false};
    if (impl.gpu_profiler) {
        prof_units = groups;
    }
    const MeshDraw mesh_draw{.groups = groups};
    if (PrepareDraw(MESH_DRAW_MODE, 0, nullptr, prepared, host_index, &mesh_draw)) {
        if (prepared.pipeline->mesh_emulation) {
            DrawEmulatedMesh(prepared);
            host->HostEndRenderCondition();
            return;
        }
        // The workgroups the device can launch in one dimension: of the task stage, or of the
        // mesh stage when the pipeline has no task program.
        const VkPhysicalDeviceMeshShaderPropertiesEXT& limits = impl.device.MeshShaderProperties();
        const bool task = prepared.pipeline->stages[STAGE_TASK].present;
        const u32 limit =
            task ? (std::min)(limits.maxTaskWorkGroupCount[0], limits.maxTaskWorkGroupTotalCount)
                 : (std::min)(limits.maxMeshWorkGroupCount[0], limits.maxMeshWorkGroupTotalCount);
        if (groups > limit) {
            ++counters.draws_skipped;
            ++counters.mesh_draws_skipped;
            host->HostUnsupported("DrawMeshTasks",
                                  fmt::format("{} {} workgroups exceed the device's {}", groups,
                                              task ? "task" : "mesh", limit));
        } else {
            host->HostCmd().DrawMeshTasksEXT(static_cast<u32>(groups), 1, 1);
            impl.stats.mesh_task_draws.fetch_add(1, std::memory_order_relaxed);
        }
    }
    host->HostEndRenderCondition();
}

bool DrawContext::PrepareEmulatedMesh(GraphicsPipeline& graphics, DrawPlan& plan,
                                      const MeshDraw& draw, PreparedDraw& prepared) {
    using namespace Shader::Backend::SPIRV;
    const auto& emulation = *graphics.mesh_emulation;
    const bool indirect = draw.arguments.buffer != VK_NULL_HANDLE;
    const bool has_task = emulation.has_task;
    const u64 block_bytes = u64{emulation.block_words} * sizeof(u32);
    const u64 max_range = impl.device.GetMaxStorageBufferRange();
    const u32 max_groups = impl.device.GetMaxComputeWorkGroupCount()[0];
    const u64 alignment = (std::max<u64>)(impl.device.GetStorageBufferAlignment(), sizeof(u32));
    const auto align = [alignment](u64 offset) {
        return (offset + alignment - 1) / alignment * alignment;
    };
    if (block_bytes == 0 || emulation.vertices_per_group == 0) {
        return false;
    }

    // The generated vertices are consumed before the next emulated draw reuses this scratch.
    constexpr u64 OUTPUT_BUDGET = 32ULL << 20;
    constexpr u64 INDIRECT_PAYLOAD_BUDGET = 4ULL << 20;
    const u64 output_limit =
        (std::min)(max_range, has_task || indirect ? OUTPUT_BUDGET : max_range);
    const u32 max_mesh_groups = static_cast<u32>(
        (std::min)({u64{max_groups}, output_limit / block_bytes,
                    u64{std::numeric_limits<u32>::max()} / emulation.vertices_per_group}));
    const u64 payload_stride = u64{emulation.payload_stride} * sizeof(u32);
    const u64 payload_limit = indirect ? (std::min)(max_range, INDIRECT_PAYLOAD_BUDGET) : max_range;
    const u32 max_task_groups =
        has_task ? static_cast<u32>((std::min)(u64{max_groups}, payload_limit / payload_stride))
                 : 0;
    const u32 group_limit = has_task ? max_task_groups : max_mesh_groups;
    const u32 groups = indirect ? group_limit : draw.groups;
    if (max_mesh_groups == 0 || groups == 0 || groups > group_limit) {
        host->HostUnsupported("DrawMeshTasks",
                              "mesh emulation exceeds the device's buffer or workgroup limits");
        return false;
    }
    const MeshEmulationHelpers* helpers =
        has_task || indirect ? resources.Cache().GetMeshEmulationHelpers() : nullptr;
    if ((has_task || indirect) && helpers == nullptr) {
        return false;
    }

    const u64 output_size = block_bytes * (has_task ? max_mesh_groups : groups);
    const u64 payload_offset = align(output_size);
    const u64 offsets_offset = align(payload_offset + (has_task ? groups * payload_stride : 0));
    const u64 scratch_size = has_task ? offsets_offset + groups * sizeof(u32) : output_size;
    if (!mesh_scratch || mesh_scratch->size < scratch_size) {
        auto replacement = resources.AcquireShadow(scratch_size);
        if (!replacement) {
            return false;
        }
        mesh_scratch = std::move(replacement);
    }
    if (mesh_scratch_kept != mesh_scratch.get()) {
        host->HostFlight().keepalive.push_back(mesh_scratch);
        mesh_scratch_kept = mesh_scratch.get();
    }
    const VkBuffer scratch = *mesh_scratch->buffer;
    const BufferRef distributed =
        has_task ? AllocArena(MESH_EMULATION_ARGS_WORDS * sizeof(u32), alignment, nullptr)
                 : BufferRef{};
    const BufferRef clamped =
        indirect ? AllocArena(MESH_EMULATION_CLAMP_WORDS * sizeof(u32), alignment, nullptr)
                 : BufferRef{};
    if ((has_task && !distributed.buffer) || (indirect && !clamped.buffer)) {
        return false;
    }
    const std::array<VkDescriptorBufferInfo, 4> program_buffers{{
        {.buffer = scratch, .range = output_size},
        {.buffer = scratch, .offset = payload_offset, .range = groups * payload_stride},
        {.buffer = scratch, .offset = offsets_offset, .range = groups * sizeof(u32)},
        {.buffer = distributed.buffer, .offset = distributed.offset, .range = distributed.range},
    }};
    const auto allocate_helper = [&](VkDescriptorSetLayout layout,
                                     std::span<const VkDescriptorBufferInfo> buffers) {
        const VkDescriptorSet set = AllocateSet(layout);
        if (set == VK_NULL_HANDLE) {
            return set;
        }
        std::array<VkWriteDescriptorSet, 4> writes{};
        for (u32 i = 0; i < buffers.size(); ++i) {
            writes[i] = {
                .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                .dstSet = set,
                .dstBinding = i,
                .descriptorCount = 1,
                .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                .pBufferInfo = &buffers[i],
            };
        }
        impl.dld.vkUpdateDescriptorSets(impl.logical, static_cast<u32>(buffers.size()),
                                        writes.data(), 0, nullptr);
        return set;
    };
    VkDescriptorSet clamp_set = VK_NULL_HANDLE;
    MeshEmulationIndirectConstants clamp_constants{};
    if (indirect) {
        const auto descriptor = [alignment](const BufferRef& ref) {
            const u64 skew = ref.offset % alignment;
            return VkDescriptorBufferInfo{ref.buffer, ref.offset - skew, ref.range + skew};
        };
        const std::array<VkDescriptorBufferInfo, 3> buffers{{
            descriptor(draw.arguments),
            {.buffer = clamped.buffer, .offset = clamped.offset, .range = clamped.range},
            descriptor(draw.count.buffer ? draw.count : draw.arguments),
        }};
        clamp_constants = {
            .max_groups = group_limit,
            .vertices_per_group = has_task ? 0u : emulation.vertices_per_group,
            .argument_word = static_cast<u32>((draw.arguments.offset % alignment) / sizeof(u32)),
            .count_word = static_cast<u32>(
                ((draw.count.buffer ? draw.count.offset : draw.arguments.offset) % alignment) /
                sizeof(u32)),
            .draw_index = draw.draw_index,
            .counted = draw.count.buffer != VK_NULL_HANDLE ? 1u : 0u,
        };
        clamp_set = allocate_helper(*helpers->indirect_clamp_set_layout, buffers);
        if (clamp_set == VK_NULL_HANDLE) {
            return false;
        }
    }
    VkDescriptorSet distribution_set = VK_NULL_HANDLE;
    if (has_task) {
        const std::array<VkDescriptorBufferInfo, 4> buffers{{
            program_buffers[1],
            program_buffers[2],
            program_buffers[3],
            {.buffer = clamped.buffer, .offset = clamped.offset, .range = clamped.range},
        }};
        distribution_set = allocate_helper(indirect ? *helpers->indirect_dist_set_layout
                                                    : *helpers->distribution_set_layout,
                                           std::span{buffers}.first(indirect ? 4 : 3));
        if (distribution_set == VK_NULL_HANDLE) {
            return false;
        }
    }

    for (u32 i = 0; i < (has_task ? 4u : 1u); ++i) {
        scratch_buffers.push_back(program_buffers[i]);
        scratch_writes.push_back({
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstBinding = emulation.output_binding + i,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            .pBufferInfo = &scratch_buffers.back(),
        });
    }
    // Order the scratch writes after shader reads of the previous draw, including across
    // submissions on this replay queue. Track the dispatch writes for later guest commands.
    host->HostMarkWritten();
    host->HostFlushBarrier();
    const auto barrier = [&](bool uses_indirect) {
        VkPipelineStageFlags stages = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
        VkAccessFlags access = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        if (uses_indirect) {
            stages |= VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT;
            access |= VK_ACCESS_INDIRECT_COMMAND_READ_BIT;
        }
        host->HostCmd().PipelineBarrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, stages, 0,
                                       VkMemoryBarrier{
                                           .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                                           .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
                                           .dstAccessMask = access,
                                       });
    };
    const auto dispatch_helper = [&](VkPipeline handle, VkPipelineLayout layout,
                                     VkDescriptorSet set, const auto& constants) {
        auto command = host->HostCmd();
        command.BindPipeline(VK_PIPELINE_BIND_POINT_COMPUTE, handle);
        command.BindDescriptorSets(VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, std::span{&set, 1},
                                   {});
        command.PushConstants(layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(constants),
                              &constants);
        command.Dispatch(1, 1, 1);
    };
    if (indirect) {
        dispatch_helper(*helpers->indirect_clamp_pipeline, *helpers->indirect_clamp_layout,
                        clamp_set, clamp_constants);
        barrier(true);
    }
    const auto dispatch_program = [&](VkPipeline handle, const BufferRef& arguments) {
        auto command = host->HostCmd();
        command.BindPipeline(VK_PIPELINE_BIND_POINT_COMPUTE, handle);
        if (!PushPlan(graphics, plan, VK_PIPELINE_BIND_POINT_COMPUTE)) {
            return false;
        }
        if (arguments.buffer) {
            command.DispatchIndirect(arguments.buffer, arguments.offset);
        } else {
            command.Dispatch(groups, 1, 1);
        }
        return true;
    };
    if (has_task) {
        if (!dispatch_program(*emulation.task_pipeline, clamped)) {
            return false;
        }
        host->HostMarkWritten();
        barrier(false);
        const MeshEmulationDistributionConstants constants{
            .tasks = groups,
            .payload_words = emulation.payload_stride,
            .capacity_words = static_cast<u32>(output_size / sizeof(u32)),
            .block_words = emulation.block_words,
            .vertices_per_group = emulation.vertices_per_group,
            .max_groups = max_mesh_groups,
        };
        dispatch_helper(indirect ? *helpers->indirect_dist_pipeline
                                 : *helpers->distribution_pipeline,
                        indirect ? *helpers->indirect_dist_layout : *helpers->distribution_layout,
                        distribution_set, constants);
        barrier(true);
    }
    if (!dispatch_program(*emulation.mesh_pipeline, has_task ? distributed : clamped)) {
        return false;
    }
    // The next graphics pass can consume guest buffers and images written by the programs.
    host->HostMarkWritten();
    host->HostFlushBarrier();
    if (has_task || indirect) {
        prepared.mesh_draw_args = has_task ? distributed : clamped;
        prepared.mesh_draw_args.offset +=
            has_task ? MESH_EMULATION_DRAW_ARGS_OFFSET : MESH_EMULATION_CLAMP_DRAW_OFFSET;
    } else {
        prepared.mesh_vertex_count = groups * emulation.vertices_per_group;
    }
    return true;
}

void DrawContext::DrawEmulatedMesh(const PreparedDraw& prepared) {
    auto cmd = host->HostCmd();
    if (prepared.mesh_draw_args.buffer) {
        cmd.DrawIndirect(prepared.mesh_draw_args.buffer, prepared.mesh_draw_args.offset, 1,
                         sizeof(VkDrawIndirectCommand));
    } else {
        cmd.Draw(prepared.mesh_vertex_count, 1, 0, 0);
    }
    impl.stats.mesh_task_draws.fetch_add(1, std::memory_order_relaxed);
}

} // namespace Vulkan::Nvn
