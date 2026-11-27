// SPDX-FileCopyrightText: Copyright 2026 Vortex727 (FUKKO)
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <fstream>
#include <type_traits>

#include <fmt/format.h>

#include "common/hex_util.h"
#include "common/logging.h"
#include "common/scope_exit.h"
#include "common/swap.h"
#include "common/uuid.h"
#include "video_core/renderer_vulkan/nvn/nvn_debug.h"
#include "video_core/renderer_vulkan/nvn/nvn_pipeline_impl.h"

namespace Vulkan::Nvn {
namespace {

constexpr u32 MAGIC = 0x3143424e; // NBC1
constexpr size_t MAX_BINARIES = 8;
constexpr size_t MAX_FILE_SIZE = 2 * 1024 * 1024;
constexpr size_t MAX_COMPUTE_FILES = 256;
constexpr size_t MAX_GRAPHICS_FILES = 1024;
constexpr u32 PRUNE_INTERVAL = 16;

struct BinaryKey {
    u32_le size;
    std::array<u8, VK_MAX_PIPELINE_BINARY_KEY_SIZE_KHR> bytes;

    bool operator==(const BinaryKey&) const = default;
};
struct Header {
    u32_le magic;
    BinaryKey device;
    BinaryKey pipeline;
    u32_le count;
    u32_le crc;
};
struct Entry {
    BinaryKey key;
    u32_le size;
};
static_assert(sizeof(Header) == 84 && sizeof(Entry) == 40);

bool ValidKeySize(u32 size) {
    return size > 0 && size <= VK_MAX_PIPELINE_BINARY_KEY_SIZE_KHR;
}

struct BinaryTrace {
    using Clock = std::chrono::steady_clock;

    BinaryTrace(std::string_view kind_, const VkPipelineBinaryKeyKHR& key_)
        : kind{kind_}, key{key_}, enabled{Debug::Get().pipeline_trace},
          last{enabled ? Clock::now() : Clock::time_point{}} {}

    void Mark(const char* phase, std::string_view status, VkResult result = VK_SUCCESS,
              u64 value = 0) {
        if (!enabled) {
            return;
        }
        const auto now = Clock::now();
        const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(now - last);
        last = now;
        if (status == "ok" && elapsed < std::chrono::milliseconds(1)) {
            return;
        }
        const auto time =
            std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch());
        const auto key_name = ValidKeySize(key.keySize)
                                  ? Common::HexToString(std::span{key.key}.first(key.keySize), false)
                                  : "-";
        LOG_INFO(Render_Vulkan,
                 "[nvn] pipeline_binary_trace t_us={} kind={} khr={} phase={} status={} vk={} "
                 "value={} elapsed_us={}",
                 time.count(), kind, key_name, phase, status, static_cast<s32>(result), value,
                 elapsed.count());
    }

    std::string_view kind;
    const VkPipelineBinaryKeyKHR& key;
    bool enabled;
    Clock::time_point last;
};

BinaryKey StoreKey(const VkPipelineBinaryKeyKHR& key) {
    BinaryKey stored{.size = key.keySize, .bytes = {}};
    std::copy_n(key.key, key.keySize, stored.bytes.begin());
    return stored;
}

void Prune(const std::filesystem::path& directory, size_t max_files) {
    namespace fs = std::filesystem;
    std::error_code error;
    std::vector<std::pair<fs::file_time_type, fs::path>> files;
    for (fs::directory_iterator it{directory, error}, end; !error && it != end;
         it.increment(error)) {
        std::error_code entry_error;
        if (it->path().extension() == ".bin" && it->is_regular_file(entry_error)) {
            const auto time = it->last_write_time(entry_error);
            if (!entry_error) {
                files.emplace_back(time, it->path());
            }
        }
    }
    if (error || files.size() <= max_files) {
        return;
    }
    std::ranges::sort(files, std::greater<>{}, &decltype(files)::value_type::first);
    for (size_t i = max_files; i < files.size(); ++i) {
        fs::remove(files[i].second, error);
    }
}

struct BinaryHandles {
    const Device& device;
    std::array<VkPipelineBinaryKHR, MAX_BINARIES> handles{};

    ~BinaryHandles() {
        const auto& dispatch = device.GetDispatchLoader();
        for (const VkPipelineBinaryKHR handle : handles) {
            if (handle) {
                dispatch.vkDestroyPipelineBinaryKHR(*device.GetLogical(), handle, nullptr);
            }
        }
    }
};

template <typename CreateInfo>
vk::Pipeline Build(const Device& device, const CreateInfo& ci) {
    if constexpr (std::is_same_v<CreateInfo, VkComputePipelineCreateInfo>) {
        return device.GetLogical().CreateComputePipeline(ci, VK_NULL_HANDLE);
    } else {
        return device.GetLogical().CreateGraphicsPipeline(ci, VK_NULL_HANDLE);
    }
}

template <typename CreateInfo>
vk::Pipeline Restore(const Device& device, CreateInfo ci, const std::filesystem::path& path,
                     const BinaryKey& device_key, const BinaryKey& pipeline_key,
                     std::string_view kind, u64 hash, BinaryTrace& trace) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        if (trace.enabled) {
            std::error_code error;
            const bool exists = std::filesystem::exists(path, error);
            trace.Mark("restore_file", exists || error ? "open_error" : "missing");
        }
        return {};
    }
    const auto file_size = static_cast<std::streamoff>(file.tellg());
    if (file_size <= static_cast<std::streamoff>(sizeof(Header)) ||
        file_size > static_cast<std::streamoff>(MAX_FILE_SIZE)) {
        trace.Mark("restore_file", "invalid_size", VK_SUCCESS,
                   file_size > 0 ? static_cast<u64>(file_size) : 0);
        return {};
    }
    trace.Mark("restore_file", "ok", VK_SUCCESS, static_cast<u64>(file_size));
    file.seekg(0);
    Header header{};
    file.read(reinterpret_cast<char*>(&header), sizeof(header));
    if (!file || header.magic != MAGIC || header.count == 0 || header.count > MAX_BINARIES) {
        trace.Mark("restore_format", "invalid_header", VK_SUCCESS, header.count);
        return {};
    }
    if (header.device != device_key || header.pipeline != pipeline_key) {
        trace.Mark("restore_format", "key_mismatch");
        if (Debug::Get().cache_log) {
            LOG_INFO(Render_Vulkan, "[nvn] {} binary key mismatch {:016x}: {}", kind, hash,
                     path.filename().string());
        }
        return {};
    }
    std::vector<u8> bytes(static_cast<size_t>(file_size) - sizeof(header));
    file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!file || Disk::Crc32(bytes) != header.crc) {
        trace.Mark("restore_format", file ? "crc_mismatch" : "read_error");
        return {};
    }

    std::array<VkPipelineBinaryKeyKHR, MAX_BINARIES> keys{};
    std::array<VkPipelineBinaryDataKHR, MAX_BINARIES> data{};
    size_t offset = 0;
    for (size_t i = 0; i < header.count; ++i) {
        Entry entry{};
        if (bytes.size() - offset < sizeof(entry)) {
            trace.Mark("restore_format", "truncated_entry", VK_SUCCESS, i);
            return {};
        }
        std::memcpy(&entry, bytes.data() + offset, sizeof(entry));
        offset += sizeof(entry);
        if (!ValidKeySize(entry.key.size) || entry.size == 0 || entry.size > bytes.size() - offset) {
            trace.Mark("restore_format", "invalid_entry", VK_SUCCESS, i);
            return {};
        }
        keys[i].sType = VK_STRUCTURE_TYPE_PIPELINE_BINARY_KEY_KHR;
        keys[i].keySize = entry.key.size;
        std::copy(entry.key.bytes.begin(), entry.key.bytes.end(), keys[i].key);
        data[i] = {.dataSize = entry.size, .pData = bytes.data() + offset};
        offset += entry.size;
    }
    if (offset != bytes.size()) {
        trace.Mark("restore_format", "trailing_bytes", VK_SUCCESS, bytes.size() - offset);
        return {};
    }
    trace.Mark("restore_format", "ok", VK_SUCCESS, header.count);

    const VkPipelineBinaryKeysAndDataKHR keys_and_data{
        .binaryCount = header.count,
        .pPipelineBinaryKeys = keys.data(),
        .pPipelineBinaryData = data.data(),
    };
    const VkPipelineBinaryCreateInfoKHR create{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_BINARY_CREATE_INFO_KHR,
        .pKeysAndDataInfo = &keys_and_data,
    };
    BinaryHandles binaries{device};
    VkPipelineBinaryHandlesInfoKHR output{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_BINARY_HANDLES_INFO_KHR,
        .pipelineBinaryCount = header.count,
        .pPipelineBinaries = binaries.handles.data(),
    };
    const auto& dispatch = device.GetDispatchLoader();
    const VkResult result =
        dispatch.vkCreatePipelineBinariesKHR(*device.GetLogical(), &create, nullptr, &output);
    if (result != VK_SUCCESS || output.pipelineBinaryCount != header.count ||
        std::ranges::any_of(std::span{binaries.handles}.first(header.count),
                            [](VkPipelineBinaryKHR binary) { return binary == VK_NULL_HANDLE; })) {
        trace.Mark("restore_import", "invalid_handles", result, output.pipelineBinaryCount);
        if (Debug::Get().cache_log) {
            LOG_INFO(Render_Vulkan, "[nvn] {} binary import failed {:016x}: {} ({} of {})", kind,
                     hash, static_cast<s32>(result), output.pipelineBinaryCount, u32{header.count});
        }
        return {};
    }
    trace.Mark("restore_import", "ok", result, output.pipelineBinaryCount);

    const VkPipelineBinaryInfoKHR binary_info{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_BINARY_INFO_KHR,
        .pNext = ci.pNext,
        .binaryCount = output.pipelineBinaryCount,
        .pPipelineBinaries = binaries.handles.data(),
    };
    ci.pNext = &binary_info;
    auto pipeline = Build(device, ci);
    trace.Mark("restore_build", pipeline ? "ok" : "empty_pipeline");
    return pipeline;
}

std::vector<u8> Capture(const Device& device, VkPipeline pipeline, const BinaryKey& device_key,
                        const BinaryKey& pipeline_key, BinaryTrace& trace) {
    const VkPipelineBinaryCreateInfoKHR create{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_BINARY_CREATE_INFO_KHR,
        .pipeline = pipeline,
    };
    const auto& dispatch = device.GetDispatchLoader();
    const VkDevice logical = *device.GetLogical();
    VkPipelineBinaryHandlesInfoKHR output{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_BINARY_HANDLES_INFO_KHR,
    };
    const VkResult count_result =
        dispatch.vkCreatePipelineBinariesKHR(logical, &create, nullptr, &output);
    if (count_result != VK_SUCCESS ||
        output.pipelineBinaryCount == 0 || output.pipelineBinaryCount > MAX_BINARIES) {
        trace.Mark("capture_count", "invalid_count", count_result, output.pipelineBinaryCount);
        return {};
    }
    trace.Mark("capture_count", "ok", count_result, output.pipelineBinaryCount);
    BinaryHandles binaries{device};
    output.pPipelineBinaries = binaries.handles.data();
    const VkResult handles_result =
        dispatch.vkCreatePipelineBinariesKHR(logical, &create, nullptr, &output);
    if (handles_result != VK_SUCCESS ||
        output.pipelineBinaryCount == 0 || output.pipelineBinaryCount > MAX_BINARIES) {
        trace.Mark("capture_handles", "invalid_count", handles_result, output.pipelineBinaryCount);
        return {};
    }
    trace.Mark("capture_handles", "ok", handles_result, output.pipelineBinaryCount);

    std::vector<u8> bytes(sizeof(Header));
    for (const auto handle : std::span{binaries.handles}.first(output.pipelineBinaryCount)) {
        if (!handle) {
            trace.Mark("capture_handles", "empty_handle");
            return {};
        }
        const VkPipelineBinaryDataInfoKHR info{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_BINARY_DATA_INFO_KHR,
            .pipelineBinary = handle,
        };
        VkPipelineBinaryKeyKHR key{.sType = VK_STRUCTURE_TYPE_PIPELINE_BINARY_KEY_KHR};
        const size_t entry_offset = bytes.size();
        size_t size = 0;
        const VkResult size_result =
            dispatch.vkGetPipelineBinaryDataKHR(logical, &info, &key, &size, nullptr);
        if (size_result != VK_SUCCESS ||
            size == 0 || MAX_FILE_SIZE - entry_offset < sizeof(Entry) ||
            size > MAX_FILE_SIZE - entry_offset - sizeof(Entry)) {
            trace.Mark("capture_size", "invalid_size", size_result, size);
            return {};
        }
        trace.Mark("capture_size", "ok", size_result, size);
        const size_t data_offset = entry_offset + sizeof(Entry);
        bytes.resize(data_offset + size);
        const VkResult data_result = dispatch.vkGetPipelineBinaryDataKHR(
            logical, &info, &key, &size, bytes.data() + data_offset);
        if (data_result != VK_SUCCESS ||
            !ValidKeySize(key.keySize) || size == 0 || size > bytes.size() - data_offset) {
            trace.Mark("capture_data", "invalid_key_or_size", data_result, size);
            return {};
        }
        trace.Mark("capture_data", "ok", data_result, size);
        bytes.resize(data_offset + size);
        const Entry entry{StoreKey(key), static_cast<u32>(size)};
        std::memcpy(bytes.data() + entry_offset, &entry, sizeof(entry));
    }
    const Header header{
        MAGIC, device_key, pipeline_key, output.pipelineBinaryCount,
        Disk::Crc32(std::span{bytes}.subspan(sizeof(Header))),
    };
    std::memcpy(bytes.data(), &header, sizeof(header));
    return bytes;
}

std::filesystem::path PrepareDirectory(const std::filesystem::path& base, std::string_view name,
                                       size_t max_files) {
    try {
        auto dir = base / name;
        std::error_code error;
        std::filesystem::create_directories(dir, error);
        if (error) {
            return {};
        }
        Prune(dir, max_files);
        return dir;
    } catch (const std::exception&) {
        return {};
    }
}

template <typename CreateInfo>
vk::Pipeline CreateBinary(const Device& device, CreateInfo ci, const std::filesystem::path& directory,
                          const VkPipelineBinaryKeyKHR& global_key, std::string_view kind, u64 hash,
                          bool save_binary, size_t max_files) {
    const auto& dispatch = device.GetDispatchLoader();
    const VkDevice logical = *device.GetLogical();
    VkPipelineCreateFlags2CreateInfoKHR flags{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_CREATE_FLAGS_2_CREATE_INFO_KHR,
        .pNext = ci.pNext,
        .flags = ci.flags,
    };
    if constexpr (std::is_same_v<CreateInfo, VkGraphicsPipelineCreateInfo>) {
        ci.pNext = &flags;
        ci.flags = 0;
    }
    const VkPipelineCreateInfoKHR info{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_CREATE_INFO_KHR,
        .pNext = &ci,
    };
    VkPipelineBinaryKeyKHR vk_key{.sType = VK_STRUCTURE_TYPE_PIPELINE_BINARY_KEY_KHR};
    BinaryTrace trace{kind, vk_key};
    const VkResult key_result = dispatch.vkGetPipelineKeyKHR(logical, &info, &vk_key);
    if (key_result != VK_SUCCESS || !ValidKeySize(vk_key.keySize)) {
        trace.Mark("query", "invalid_key", key_result, vk_key.keySize);
        return {};
    }
    trace.Mark("query", "ok", key_result, vk_key.keySize);
    const BinaryKey device_key = StoreKey(global_key);
    const BinaryKey key = StoreKey(vk_key);
    const bool logging = Debug::Get().cache_log;
    if (logging && hash == 0) {
        hash = Common::CityHash64(reinterpret_cast<const char*>(key.bytes.data()), key.size);
    }
    const auto start = logging ? std::chrono::steady_clock::now()
                               : std::chrono::steady_clock::time_point{};
    const auto log = [&](std::string_view mode, size_t size) {
        if (logging) {
            const double ms =
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
                    .count();
            LOG_INFO(Render_Vulkan, "[nvn] {} binary {} {:016x}: {:.3f} ms, {} bytes", kind,
                     mode, hash, ms, size);
        }
    };

    std::filesystem::path path;
    try {
        const auto device_name =
            Common::HexToString(std::span{device_key.bytes}.first(device_key.size), false);
        const auto pipeline_name =
            Common::HexToString(std::span{key.bytes}.first(key.size), false);
        path = directory / fmt::format("{}-{}.bin", device_name, pipeline_name);
        if (auto pipeline = Restore(device, ci, path, device_key, key, kind, hash, trace)) {
            log("hit", 0);
            return pipeline;
        }
    } catch (const vk::Exception& error) {
        trace.Mark("restore_build", "vulkan_error", error.GetResult());
        if (logging) {
            LOG_INFO(Render_Vulkan, "[nvn] {} binary restore failed {:016x}: {}", kind, hash,
                     error.what());
        }
    } catch (const std::exception&) {
        trace.Mark("restore", "host_exception");
        return {};
    }

    if (!save_binary) {
        trace.Mark("capture_build", "disabled");
        return {};
    }

    flags.flags |= VK_PIPELINE_CREATE_2_CAPTURE_DATA_BIT_KHR;
    ci.pNext = &flags;
    vk::Pipeline pipeline;
    try {
        pipeline = Build(device, ci);
    } catch (const vk::Exception& error) {
        trace.Mark("capture_build", "vulkan_error", error.GetResult());
        LOG_DEBUG(Render_Vulkan, "[nvn] {} binary capture pipeline failed: {}", kind, error.what());
        return {};
    } catch (const std::exception& error) {
        trace.Mark("capture_build", "host_exception");
        LOG_DEBUG(Render_Vulkan, "[nvn] {} binary capture pipeline failed: {}", kind,
                  error.what());
        return {};
    }
    if (!pipeline) {
        trace.Mark("capture_build", "empty_pipeline");
        return {};
    }
    trace.Mark("capture_build", "ok");
    const VkReleaseCapturedPipelineDataInfoKHR release{
        .sType = VK_STRUCTURE_TYPE_RELEASE_CAPTURED_PIPELINE_DATA_INFO_KHR,
        .pipeline = *pipeline,
    };
    SCOPE_EXIT {
        dispatch.vkReleaseCapturedPipelineDataKHR(logical, &release, nullptr);
    };
    try {
        const auto bytes = Capture(device, *pipeline, device_key, key, trace);
        if (bytes.empty()) {
            return pipeline;
        }
        const bool written =
            Disk::WriteFileAtomic(path, bytes, Common::UUID::MakeRandom().RawString());
        trace.Mark("write", written ? "ok" : "failed", VK_SUCCESS, bytes.size());
        if (written) {
            log("saved", bytes.size());
            static std::atomic<u32> writes{};
            if (writes.fetch_add(1, std::memory_order_relaxed) % PRUNE_INTERVAL ==
                PRUNE_INTERVAL - 1) {
                Prune(directory, max_files);
            }
        }
    } catch (const std::exception& error) {
        trace.Mark("save", "host_exception");
        LOG_DEBUG(Render_Vulkan, "[nvn] {} binary save failed: {}", kind, error.what());
    }
    return pipeline;
}

} // Anonymous namespace

void PipelineCache::Impl::InitPipelineBinaries() {
    const auto& debug = Debug::Get();
    if (!debug.compute_binaries && !debug.graphics_binaries) {
        return;
    }
    if (!device.IsKhrPipelineBinarySupported()) {
        if (debug.pipeline_trace) {
            LOG_INFO(Render_Vulkan, "[nvn] pipeline_binary_trace init status=unsupported");
        }
        return;
    }
    const auto& dispatch = device.GetDispatchLoader();
    const VkResult result = dispatch.vkGetPipelineKeyKHR(*device.GetLogical(), nullptr,
                                                        &pipeline_binary_device_key);
    if (result != VK_SUCCESS || !ValidKeySize(pipeline_binary_device_key.keySize)) {
        if (debug.pipeline_trace) {
            LOG_INFO(Render_Vulkan,
                     "[nvn] pipeline_binary_trace init status=invalid_device_key vk={} size={}",
                     static_cast<s32>(result), pipeline_binary_device_key.keySize);
        }
        return;
    }
    if (debug.compute_binaries) {
        compute_binary_directory =
            PrepareDirectory(options.directory, "compute-binaries-v1", MAX_COMPUTE_FILES);
    }
    if (debug.graphics_binaries && options.graphics_pipeline_library &&
        device.IsExtGraphicsPipelineLibrarySupported()) {
        graphics_binary_directory = PrepareDirectory(
            options.directory, "graphics-library-binaries-v1", MAX_GRAPHICS_FILES);
    }
}

vk::Pipeline PipelineCache::Impl::CreateComputeBinary(VkComputePipelineCreateInfo ci, u64 hash,
                                                      bool save_binary) {
    if (compute_binary_directory.empty() || !DiskEnabled()) {
        return {};
    }
    return CreateBinary(device, ci, compute_binary_directory, pipeline_binary_device_key,
                        "compute", hash, save_binary, MAX_COMPUTE_FILES);
}

std::unique_ptr<GraphicsPipelineLibraryCache> PipelineCache::Impl::MakeLibraryCache() {
    GraphicsPipelineLibraryCache::ShaderPartFactory shader_part;
    if (!graphics_binary_directory.empty()) {
        shader_part = [this](const VkGraphicsPipelineCreateInfo& info) {
            return CreateGraphicsLibraryBinary(info);
        };
    }
    return std::make_unique<GraphicsPipelineLibraryCache>(device, std::move(shader_part));
}

vk::Pipeline PipelineCache::Impl::CreateGraphicsLibraryBinary(VkGraphicsPipelineCreateInfo ci) {
    if (!DiskEnabled()) {
        return {};
    }
    const auto& part = *static_cast<const VkGraphicsPipelineLibraryCreateInfoEXT*>(ci.pNext);
    const auto kind = part.flags == VK_GRAPHICS_PIPELINE_LIBRARY_PRE_RASTERIZATION_SHADERS_BIT_EXT
                          ? "pre-raster"
                          : "fragment";
    return CreateBinary(device, ci, graphics_binary_directory, pipeline_binary_device_key,
                        kind, 0, true, MAX_GRAPHICS_FILES);
}

} // namespace Vulkan::Nvn
