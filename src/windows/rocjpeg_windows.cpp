/* Copyright (c) 2026 rocJPEG Windows contributors.
 * SPDX-License-Identifier: MIT
 *
 * Windows VCN JPEG decoding through AMD AMF. Decoded pixels stay on the GPU:
 * AMF/D3D11 -> shared D3D11/D3D12 texture -> shared linear D3D12 buffer -> HIP.
 */

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d11_4.h>
#include <d3d10.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <core/Factory.h>
#include <core/Version.h>
#include <components/VideoDecoderUVD.h>

#include "rocjpeg_windows_kernels.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <thread>
#include <unordered_map>
#include <vector>

namespace {
using Microsoft::WRL::ComPtr;
using rocjpeg::windows::SurfaceFormat;
using rocjpeg::windows::SurfaceView;

struct Failure { RocJpegStatus status; };
void Check(HRESULT status) {
    if (FAILED(status)) throw Failure{status == E_OUTOFMEMORY ? ROCJPEG_STATUS_OUTOF_MEMORY : ROCJPEG_STATUS_EXECUTION_FAILED};
}
void CheckHip(hipError_t status) {
    if (status != hipSuccess) throw Failure{status == hipErrorOutOfMemory ? ROCJPEG_STATUS_OUTOF_MEMORY : ROCJPEG_STATUS_RUNTIME_ERROR};
}
void CheckAmf(AMF_RESULT status) {
    if (status != AMF_OK) throw Failure{status == AMF_OUT_OF_MEMORY ? ROCJPEG_STATUS_OUTOF_MEMORY : ROCJPEG_STATUS_EXECUTION_FAILED};
}

template <typename F> RocJpegStatus Guard(F&& fn) noexcept {
    try { return fn(); }
    catch (const Failure& failure) { return failure.status; }
    catch (const std::bad_alloc&) { return ROCJPEG_STATUS_OUTOF_MEMORY; }
    catch (...) { return ROCJPEG_STATUS_INTERNAL_ERROR; }
}

struct WinHandle {
    HANDLE value = nullptr;
    ~WinHandle() { if (value) CloseHandle(value); }
    WinHandle() = default;
    WinHandle(const WinHandle&) = delete;
    WinHandle& operator=(const WinHandle&) = delete;
};

struct Module {
    HMODULE value = nullptr;
    ~Module() { if (value) FreeLibrary(value); }
};

class DeviceScope {
public:
    explicit DeviceScope(int requested) {
        CheckHip(hipGetDevice(&previous_));
        if (previous_ != requested) { CheckHip(hipSetDevice(requested)); changed_ = true; }
    }
    ~DeviceScope() { if (changed_) (void)hipSetDevice(previous_); }
private:
    int previous_ = 0;
    bool changed_ = false;
};

struct JpegStream {
    std::mutex mutex;
    std::vector<uint8_t> bytes;
    uint32_t width = 0, height = 0;
    uint8_t components = 0;
    RocJpegChromaSubsampling subsampling = ROCJPEG_CSS_UNKNOWN;

    RocJpegStatus Parse(const uint8_t* data, size_t length) {
        bytes.clear(); width = height = components = 0; subsampling = ROCJPEG_CSS_UNKNOWN;
        if (!data || length < 4 || data[0] != 0xff || data[1] != 0xd8)
            return ROCJPEG_STATUS_BAD_JPEG;
        if (length > std::numeric_limits<uint32_t>::max()) return ROCJPEG_STATUS_JPEG_NOT_SUPPORTED;
        bool frame = false, scan = false, end = false;
        int adobe_transform = -1;
        std::array<uint8_t, 3> ids{};
        size_t pos = 2;
        while (pos < length) {
            if (data[pos++] != 0xff) return ROCJPEG_STATUS_BAD_JPEG;
            while (pos < length && data[pos] == 0xff) ++pos;
            if (pos >= length) return ROCJPEG_STATUS_BAD_JPEG;
            const uint8_t marker = data[pos++];
            if (marker == 0xd9) { end = true; break; }
            if (marker == 0 || marker == 0xd8 || (marker >= 0xd0 && marker <= 0xd7))
                return ROCJPEG_STATUS_BAD_JPEG;
            if (length - pos < 2) return ROCJPEG_STATUS_BAD_JPEG;
            const size_t size = (size_t(data[pos]) << 8) | data[pos + 1];
            if (size < 2 || size > length - pos) return ROCJPEG_STATUS_BAD_JPEG;
            const uint8_t* segment = data + pos + 2;
            if (marker == 0xee && size >= 14 && std::memcmp(segment, "Adobe", 5) == 0) {
                adobe_transform = segment[11];
            } else if ((marker >= 0xc0 && marker <= 0xcf) && marker != 0xc4 && marker != 0xc8 && marker != 0xcc) {
                if (marker != 0xc0) return ROCJPEG_STATUS_JPEG_NOT_SUPPORTED;
                if (frame || size < 8) return ROCJPEG_STATUS_BAD_JPEG;
                if (segment[0] != 8) return ROCJPEG_STATUS_JPEG_NOT_SUPPORTED;
                height = (uint32_t(segment[1]) << 8) | segment[2];
                width = (uint32_t(segment[3]) << 8) | segment[4];
                components = segment[5];
                if (!width || !height) return ROCJPEG_STATUS_BAD_JPEG;
                if (components != 1 && components != 3) return ROCJPEG_STATUS_JPEG_NOT_SUPPORTED;
                if (size != size_t(8 + 3 * components)) return ROCJPEG_STATUS_BAD_JPEG;
                for (unsigned c = 0; c < components; ++c) {
                    ids[c] = segment[6 + 3 * c];
                    const unsigned sample = segment[7 + 3 * c];
                    if (!(sample >> 4) || !(sample & 15) || (sample >> 4) > 4 || (sample & 15) > 4)
                        return ROCJPEG_STATUS_BAD_JPEG;
                    for (unsigned j = 0; j < c; ++j) if (ids[j] == ids[c]) return ROCJPEG_STATUS_BAD_JPEG;
                }
                if (components == 1) subsampling = ROCJPEG_CSS_400;
                else {
                    if ((ids[0] == 'R' && ids[1] == 'G' && ids[2] == 'B') || segment[10] != segment[13])
                        return ROCJPEG_STATUS_JPEG_NOT_SUPPORTED;
                    const unsigned yh = segment[7] >> 4, yv = segment[7] & 15;
                    const unsigned ch = segment[10] >> 4, cv = segment[10] & 15;
                    if (yh < ch || yv < cv || yh % ch || yv % cv)
                        return ROCJPEG_STATUS_JPEG_NOT_SUPPORTED;
                    // Subsampling is a ratio. For example Y=2x2/C=1x2 is
                    // legal 4:2:2, just like the more usual Y=2x1/C=1x1.
                    switch (((yh / ch) << 4) | (yv / cv)) {
                    case 0x11: subsampling = ROCJPEG_CSS_444; break;
                    case 0x12: subsampling = ROCJPEG_CSS_440; break;
                    case 0x21: subsampling = ROCJPEG_CSS_422; break;
                    case 0x22: subsampling = ROCJPEG_CSS_420; break;
                    default: return ROCJPEG_STATUS_JPEG_NOT_SUPPORTED;
                    }
                }
                frame = true;
            } else if (marker == 0xda) {
                if (!frame || scan || size < 6) return ROCJPEG_STATUS_BAD_JPEG;
                if (segment[0] != components) return ROCJPEG_STATUS_JPEG_NOT_SUPPORTED;
                if (size != size_t(6 + 2 * components)) return ROCJPEG_STATUS_BAD_JPEG;
                for (unsigned c = 0; c < components; ++c) if (segment[1 + 2 * c] != ids[c]) return ROCJPEG_STATUS_JPEG_NOT_SUPPORTED;
                if (segment[1 + 2 * components] != 0 || segment[2 + 2 * components] != 63 || segment[3 + 2 * components] != 0)
                    return ROCJPEG_STATUS_JPEG_NOT_SUPPORTED;
                pos += size; scan = true;
                while (pos < length) {
                    if (data[pos++] != 0xff) continue;
                    while (pos < length && data[pos] == 0xff) ++pos;
                    if (pos >= length) return ROCJPEG_STATUS_BAD_JPEG;
                    const uint8_t next = data[pos++];
                    if (!next || (next >= 0xd0 && next <= 0xd7)) continue;
                    if (next == 0xd9) { end = true; break; }
                    return ROCJPEG_STATUS_JPEG_NOT_SUPPORTED;
                }
                break;
            }
            pos += size;
        }
        if (!frame || !scan || !end) { width = height = components = 0; return ROCJPEG_STATUS_BAD_JPEG; }
        if (components == 3 && adobe_transform != -1 && adobe_transform != 1)
            return ROCJPEG_STATUS_JPEG_NOT_SUPPORTED;
        bytes.assign(data, data + pos);
        return ROCJPEG_STATUS_SUCCESS;
    }
};

// Imported HIP mappings must be released before their owning D3D12 allocations.
struct TransferBuffer {
    ComPtr<ID3D11Texture2D> texture11;
    ComPtr<ID3D12Resource> texture12;
    ComPtr<ID3D12Resource> buffer;
    hipExternalMemory_t external = nullptr;
    uint8_t* mapped = nullptr;
    std::array<D3D12_PLACED_SUBRESOURCE_FOOTPRINT, 2> layout{};
    D3D11_TEXTURE2D_DESC description{};
    unsigned planes = 1;
    ~TransferBuffer() {
        if (mapped) (void)hipFree(mapped);
        if (external) (void)hipDestroyExternalMemory(external);
    }
};

struct Submission {
    amf::AMFComponentPtr decoder;
    amf::AMFBufferPtr compressed;
    RocJpegDecodeParams params{};
    uint32_t width = 0, height = 0;
    RocJpegChromaSubsampling subsampling = ROCJPEG_CSS_UNKNOWN;
    amf::AMF_SURFACE_FORMAT format = amf::AMF_SURFACE_UNKNOWN;
    bool owns_decoder = false;
    ~Submission() { if (owns_decoder && decoder) (void)decoder->Terminate(); }
};

class Decoder {
public:
    explicit Decoder(int device) : device_(device) {}
    ~Decoder() {
        int previous = 0;
        const bool got_device = hipGetDevice(&previous) == hipSuccess;
        (void)hipSetDevice(device_);
        if (hip_stream_) (void)hipStreamSynchronize(hip_stream_);
        // In an error path the D3D submission might not yet have been waited by HIP.
        WaitForQueue();
        pending_.clear();
        transfer_.reset();
        if (hip_fence_) (void)hipDestroyExternalSemaphore(hip_fence_);
        if (hip_stream_) (void)hipStreamDestroy(hip_stream_);
        if (decoder_) { decoder_->Terminate(); decoder_ = nullptr; }
        if (context_) { context_->Terminate(); context_ = nullptr; }
        if (got_device && previous != device_) (void)hipSetDevice(previous);
    }

    void Initialize() {
        DeviceScope scope(device_);
        CheckHip(hipStreamCreateWithFlags(&hip_stream_, hipStreamNonBlocking));
        char luid[8]{}; unsigned node_mask = 0;
        CheckHip(hipDeviceGetLuid(luid, &node_mask, device_));
        ComPtr<IDXGIFactory1> dxgi;
        Check(CreateDXGIFactory1(IID_PPV_ARGS(&dxgi)));
        for (UINT i = 0; ; ++i) {
            ComPtr<IDXGIAdapter1> candidate;
            const HRESULT hr = dxgi->EnumAdapters1(i, &candidate);
            if (hr == DXGI_ERROR_NOT_FOUND) break;
            Check(hr);
            DXGI_ADAPTER_DESC1 desc{}; Check(candidate->GetDesc1(&desc));
            if (desc.VendorId == 0x1002 && std::memcmp(&desc.AdapterLuid, luid, sizeof(luid)) == 0) {
                adapter_ = candidate; break;
            }
        }
        if (!adapter_) throw Failure{ROCJPEG_STATUS_ARCH_MISMATCH};
        D3D_FEATURE_LEVEL level;
        Check(D3D11CreateDevice(adapter_.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                               D3D11_CREATE_DEVICE_VIDEO_SUPPORT, nullptr, 0, D3D11_SDK_VERSION,
                               &device11_, &level, &context11_));
        Check(device11_.As(&device11_5_));
        Check(context11_.As(&context11_4_));
        ComPtr<ID3D10Multithread> multithread;
        Check(context11_.As(&multithread));
        multithread->SetMultithreadProtected(TRUE);
        Check(D3D12CreateDevice(adapter_.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device12_)));
        D3D12_COMMAND_QUEUE_DESC queue_desc{};
        queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        Check(device12_->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue_)));
        Check(device12_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator_)));
        Check(device12_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator_.Get(), nullptr, IID_PPV_ARGS(&commands_)));
        Check(commands_->Close());
        Check(device12_->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&fence12_)));
        WinHandle fence_handle;
        Check(device12_->CreateSharedHandle(fence12_.Get(), nullptr, GENERIC_ALL, nullptr, &fence_handle.value));
        Check(device11_5_->OpenSharedFence(fence_handle.value, IID_PPV_ARGS(&fence11_)));
        hipExternalSemaphoreHandleDesc fence_desc{};
        fence_desc.type = hipExternalSemaphoreHandleTypeD3D12Fence;
        fence_desc.handle.win32.handle = fence_handle.value;
        CheckHip(hipImportExternalSemaphore(&hip_fence_, &fence_desc));

        module_.value = LoadLibraryExW(AMF_DLL_NAME, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!module_.value) throw Failure{ROCJPEG_STATUS_HW_JPEG_DECODER_NOT_SUPPORTED};
        const auto amf_init = reinterpret_cast<AMFInit_Fn>(GetProcAddress(module_.value, AMF_INIT_FUNCTION_NAME));
        if (!amf_init) throw Failure{ROCJPEG_STATUS_HW_JPEG_DECODER_NOT_SUPPORTED};
        CheckAmf(amf_init(AMF_FULL_VERSION, &factory_));
        CheckAmf(factory_->CreateContext(&context_));
        CheckAmf(context_->InitDX11(device11_.Get()));
        CheckAmf(factory_->CreateComponent(context_, AMFVideoDecoderUVD_MJPEG, &decoder_));
        amf::AMFCapsPtr caps;
        CheckAmf(decoder_->GetCaps(&caps));
        if (caps->GetAccelerationType() != amf::AMF_ACCEL_HARDWARE)
            throw Failure{ROCJPEG_STATUS_HW_JPEG_DECODER_NOT_SUPPORTED};
        amf::AMFIOCapsPtr input_caps;
        CheckAmf(caps->GetInputCaps(&input_caps));
        input_caps->GetWidthRange(&min_width_, &max_width_);
        input_caps->GetHeightRange(&min_height_, &max_height_);
    }

    RocJpegStatus Decode(const JpegStream& jpeg, const RocJpegDecodeParams& params, RocJpegImage& destination) {
        if (!pending_.empty()) return ROCJPEG_STATUS_EXECUTION_FAILED;
        DeviceScope scope(device_);
        try {
            auto job = Submit(jpeg, params, true);
            return Finish(*job, destination);
        } catch (...) {
            if (decoder_initialized_) (void)decoder_->Flush();
            throw;
        }
    }

    RocJpegStatus DecodeAsync(const JpegStream& jpeg, const RocJpegDecodeParams& params, RocJpegImage* destination) {
        if (pending_.count(destination)) return ROCJPEG_STATUS_INVALID_PARAMETER;
        DeviceScope scope(device_);
        auto job = Submit(jpeg, params, false);
        pending_.emplace(destination, std::move(job));
        return ROCJPEG_STATUS_SUCCESS;
    }

    RocJpegStatus DecodeSync(RocJpegImage* destination) {
        const auto it = pending_.find(destination);
        if (it == pending_.end()) return ROCJPEG_STATUS_INVALID_PARAMETER;
        DeviceScope scope(device_);
        auto job = std::move(it->second);
        pending_.erase(it);
        return Finish(*job, *destination);
    }

    bool IsPending(RocJpegImage* destination) const { return pending_.count(destination) != 0; }
    void DiscardPending(RocJpegImage* destination) { pending_.erase(destination); }

    std::mutex mutex;

private:
    std::unique_ptr<Submission> Submit(const JpegStream& jpeg, const RocJpegDecodeParams& params, bool reuse) {
        if (jpeg.bytes.empty()) throw Failure{ROCJPEG_STATUS_BAD_JPEG};
        if (jpeg.width < unsigned(min_width_) || jpeg.height < unsigned(min_height_) ||
            jpeg.width > unsigned(max_width_) || jpeg.height > unsigned(max_height_))
            throw Failure{ROCJPEG_STATUS_JPEG_NOT_SUPPORTED};
        if (params.output_format < ROCJPEG_OUTPUT_NATIVE || params.output_format >= ROCJPEG_OUTPUT_FORMAT_MAX)
            throw Failure{ROCJPEG_STATUS_INVALID_PARAMETER};
        // AMF advertises BGRA conversion, but the Windows driver can fault while
        // initializing that path for 4:4:4 JPEG. Reject before entering the driver;
        // silently reducing 4:4:4/4:4:0 chroma to NV12 would also lose image detail.
        if (jpeg.subsampling == ROCJPEG_CSS_444 || jpeg.subsampling == ROCJPEG_CSS_440)
            throw Failure{ROCJPEG_STATUS_JPEG_NOT_SUPPORTED};
        const amf::AMF_SURFACE_FORMAT format = jpeg.subsampling == ROCJPEG_CSS_422 ?
            amf::AMF_SURFACE_YUY2 : amf::AMF_SURFACE_NV12;
        auto job = std::make_unique<Submission>();
        job->params = params;
        job->width = jpeg.width; job->height = jpeg.height;
        job->subsampling = jpeg.subsampling; job->format = format;
        if (!reuse) {
            job->owns_decoder = true;
            CheckAmf(factory_->CreateComponent(context_, AMFVideoDecoderUVD_MJPEG, &job->decoder));
            CheckAmf(job->decoder->SetProperty(AMF_VIDEO_DECODER_REORDER_MODE, amf_int64(AMF_VIDEO_DECODER_MODE_LOW_LATENCY)));
            CheckAmf(job->decoder->Init(format, jpeg.width, jpeg.height));
        } else if (!decoder_initialized_ || width_ != jpeg.width || height_ != jpeg.height || format_ != format) {
            if (decoder_initialized_) CheckAmf(decoder_->Terminate());
            decoder_initialized_ = false;
            CheckAmf(decoder_->SetProperty(AMF_VIDEO_DECODER_REORDER_MODE, amf_int64(AMF_VIDEO_DECODER_MODE_LOW_LATENCY)));
            CheckAmf(decoder_->Init(format, jpeg.width, jpeg.height));
            decoder_initialized_ = true;
            width_ = jpeg.width; height_ = jpeg.height; format_ = format;
        }
        if (reuse) job->decoder = decoder_;
        CheckAmf(context_->AllocBuffer(amf::AMF_MEMORY_HOST, jpeg.bytes.size(), &job->compressed));
        std::memcpy(job->compressed->GetNative(), jpeg.bytes.data(), jpeg.bytes.size());
        job->compressed->SetPts(0);
        job->compressed->SetDuration(333333);
        CheckAmf(job->decoder->SubmitInput(job->compressed));
        return job;
    }

    RocJpegStatus Finish(const Submission& job, RocJpegImage& destination) {
        // A previous failed conversion still owns work on these queues. Never reset
        // the command allocator or overwrite its imported buffer before it finishes.
        CheckHip(hipStreamSynchronize(hip_stream_));
        if (!WaitForQueue()) return ROCJPEG_STATUS_EXECUTION_FAILED;
        amf::AMFDataPtr output;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (std::chrono::steady_clock::now() < deadline) {
            const auto status = job.decoder->QueryOutput(&output);
            if (output) { CheckAmf(status); break; }
            if (status != AMF_REPEAT && status != AMF_OK) CheckAmf(status);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        if (!output) {
            (void)job.decoder->Flush();
            return ROCJPEG_STATUS_EXECUTION_FAILED;
        }
        amf::AMFSurfacePtr surface(output);
        if (!surface || surface->GetMemoryType() != amf::AMF_MEMORY_DX11 || surface->GetFormat() != job.format)
            return ROCJPEG_STATUS_IMPLEMENTATION_NOT_SUPPORTED;
        auto* texture = static_cast<ID3D11Texture2D*>(surface->GetPlaneAt(0)->GetNative());
        if (!texture) return ROCJPEG_STATUS_EXECUTION_FAILED;
        PrepareTransfer(texture);
        Check(allocator_->Reset());
        Check(commands_->Reset(allocator_.Get(), nullptr));
        context11_->CopyResource(transfer_->texture11.Get(), texture);
        const uint64_t source_ready = ++fence_value_;
        Check(context11_4_->Signal(fence11_.Get(), source_ready));
        context11_->Flush();
        Check(queue_->Wait(fence12_.Get(), source_ready));
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = transfer_->texture12.Get();
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        commands_->ResourceBarrier(1, &barrier);
        D3D12_RESOURCE_BARRIER buffer_barrier{};
        buffer_barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        buffer_barrier.Transition.pResource = transfer_->buffer.Get();
        buffer_barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        buffer_barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
        buffer_barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
        commands_->ResourceBarrier(1, &buffer_barrier);
        for (unsigned p = 0; p < transfer_->planes; ++p) {
            D3D12_TEXTURE_COPY_LOCATION source{};
            source.pResource = transfer_->texture12.Get();
            source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            source.SubresourceIndex = p;
            D3D12_TEXTURE_COPY_LOCATION target{};
            target.pResource = transfer_->buffer.Get();
            target.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            target.PlacedFootprint = transfer_->layout[p];
            commands_->CopyTextureRegion(&target, 0, 0, 0, &source, nullptr);
        }
        std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
        commands_->ResourceBarrier(1, &barrier);
        std::swap(buffer_barrier.Transition.StateBefore, buffer_barrier.Transition.StateAfter);
        commands_->ResourceBarrier(1, &buffer_barrier);
        Check(commands_->Close());
        ID3D12CommandList* lists[] = {commands_.Get()};
        queue_->ExecuteCommandLists(1, lists);
        const uint64_t hip_ready = ++fence_value_;
        Check(queue_->Signal(fence12_.Get(), hip_ready));
        last_copy_ = hip_ready;
        hipExternalSemaphoreWaitParams wait{};
        wait.params.fence.value = hip_ready;
        CheckHip(hipWaitExternalSemaphoresAsync(&hip_fence_, &wait, 1, hip_stream_));
        SurfaceView view{};
        view.format = job.format == amf::AMF_SURFACE_NV12 ? SurfaceFormat::NV12 : SurfaceFormat::YUY2;
        view.width = job.width; view.height = job.height; view.chromaSubsampling = job.subsampling;
        for (unsigned p = 0; p < transfer_->planes; ++p) {
            view.planes[p] = transfer_->mapped + transfer_->layout[p].Offset;
            view.pitches[p] = transfer_->layout[p].Footprint.RowPitch;
        }
        const RocJpegStatus status = rocjpeg::windows::Convert(view, job.params, destination, hip_stream_);
        // Also drain the fence wait when Convert rejects an output layout. This keeps
        // the allocator and imported mapping safe for the next call and destruction.
        CheckHip(hipStreamSynchronize(hip_stream_));
        return status;
    }

    void PrepareTransfer(ID3D11Texture2D* texture) {
        D3D11_TEXTURE2D_DESC source{}; texture->GetDesc(&source);
        if (source.ArraySize != 1 || source.MipLevels != 1 || source.SampleDesc.Count != 1)
            throw Failure{ROCJPEG_STATUS_IMPLEMENTATION_NOT_SUPPORTED};
        if (transfer_ && transfer_->description.Width == source.Width && transfer_->description.Height == source.Height &&
            transfer_->description.Format == source.Format) return;
        auto next = std::make_unique<TransferBuffer>();
        next->description = source;
        next->planes = source.Format == DXGI_FORMAT_NV12 ? 2 : 1;
        auto shared_desc = source;
        shared_desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED;
        // Starting with D3D11 is deliberate: AMF's NV12 allocation cannot be opened
        // by D3D11 when the shared texture is instead created on D3D12.
        Check(device11_->CreateTexture2D(&shared_desc, nullptr, &next->texture11));
        ComPtr<IDXGIResource1> shared_resource;
        Check(next->texture11.As(&shared_resource));
        WinHandle texture_handle;
        Check(shared_resource->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE,
                                                   nullptr, &texture_handle.value));
        Check(device12_->OpenSharedHandle(texture_handle.value, IID_PPV_ARGS(&next->texture12)));
        const auto texture_desc = next->texture12->GetDesc();
        UINT64 total = 0;
        device12_->GetCopyableFootprints(&texture_desc, 0, next->planes, 0, next->layout.data(), nullptr, nullptr, &total);
        if (!total || total == std::numeric_limits<UINT64>::max()) throw Failure{ROCJPEG_STATUS_EXECUTION_FAILED};
        D3D12_RESOURCE_DESC buffer_desc{};
        buffer_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        buffer_desc.Width = total;
        buffer_desc.Height = 1;
        buffer_desc.DepthOrArraySize = 1;
        buffer_desc.MipLevels = 1;
        buffer_desc.SampleDesc.Count = 1;
        buffer_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        Check(device12_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_SHARED, &buffer_desc,
              D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&next->buffer)));
        WinHandle buffer_handle;
        Check(device12_->CreateSharedHandle(next->buffer.Get(), nullptr, GENERIC_ALL, nullptr, &buffer_handle.value));
        hipExternalMemoryHandleDesc import{};
        import.type = hipExternalMemoryHandleTypeD3D12Resource;
        import.handle.win32.handle = buffer_handle.value;
        import.size = device12_->GetResourceAllocationInfo(0, 1, &buffer_desc).SizeInBytes;
        import.flags = hipExternalMemoryDedicated;
        CheckHip(hipImportExternalMemory(&next->external, &import));
        hipExternalMemoryBufferDesc mapping{};
        mapping.size = total;
        CheckHip(hipExternalMemoryGetMappedBuffer(reinterpret_cast<void**>(&next->mapped), next->external, &mapping));
        transfer_ = std::move(next);
    }

    bool WaitForQueue() noexcept {
        if (!queue_ || !fence12_ || !last_copy_ || fence12_->GetCompletedValue() >= last_copy_) return true;
        WinHandle event;
        event.value = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (event.value && SUCCEEDED(fence12_->SetEventOnCompletion(last_copy_, event.value)))
            return WaitForSingleObject(event.value, 10000) == WAIT_OBJECT_0;
        return false;
    }

    int device_;
    hipStream_t hip_stream_ = nullptr;
    hipExternalSemaphore_t hip_fence_ = nullptr;
    Module module_;
    amf::AMFFactory* factory_ = nullptr;
    amf::AMFContextPtr context_;
    amf::AMFComponentPtr decoder_;
    ComPtr<IDXGIAdapter1> adapter_;
    ComPtr<ID3D11Device> device11_;
    ComPtr<ID3D11Device5> device11_5_;
    ComPtr<ID3D11DeviceContext> context11_;
    ComPtr<ID3D11DeviceContext4> context11_4_;
    ComPtr<ID3D12Device> device12_;
    ComPtr<ID3D12CommandQueue> queue_;
    ComPtr<ID3D12CommandAllocator> allocator_;
    ComPtr<ID3D12GraphicsCommandList> commands_;
    ComPtr<ID3D12Fence> fence12_;
    ComPtr<ID3D11Fence> fence11_;
    std::unique_ptr<TransferBuffer> transfer_;
    std::unordered_map<RocJpegImage*, std::unique_ptr<Submission>> pending_;
    uint64_t fence_value_ = 0, last_copy_ = 0;
    amf_int32 min_width_ = 0, max_width_ = 0, min_height_ = 0, max_height_ = 0;
    uint32_t width_ = 0, height_ = 0;
    amf::AMF_SURFACE_FORMAT format_ = amf::AMF_SURFACE_UNKNOWN;
    bool decoder_initialized_ = false;
};
} // namespace

extern "C" {
RocJpegStatus ROCJPEGAPI rocJpegStreamCreate(RocJpegStreamHandle* stream) {
    if (!stream) return ROCJPEG_STATUS_INVALID_PARAMETER;
    *stream = nullptr;
    return Guard([&] { *stream = new JpegStream; return ROCJPEG_STATUS_SUCCESS; });
}

RocJpegStatus ROCJPEGAPI rocJpegStreamDestroy(RocJpegStreamHandle stream) {
    if (!stream) return ROCJPEG_STATUS_INVALID_PARAMETER;
    delete static_cast<JpegStream*>(stream);
    return ROCJPEG_STATUS_SUCCESS;
}

RocJpegStatus ROCJPEGAPI rocJpegStreamParse(const unsigned char* data, size_t length, RocJpegStreamHandle stream) {
    if (!data || !stream) return ROCJPEG_STATUS_INVALID_PARAMETER;
    return Guard([&] {
        auto& jpeg = *static_cast<JpegStream*>(stream);
        std::lock_guard<std::mutex> lock(jpeg.mutex);
        return jpeg.Parse(data, length);
    });
}

RocJpegStatus ROCJPEGAPI rocJpegCreate(RocJpegBackend backend, int device, RocJpegHandle* handle) {
    if (!handle) return ROCJPEG_STATUS_INVALID_PARAMETER;
    *handle = nullptr;
    if (backend == ROCJPEG_BACKEND_HYBRID) return ROCJPEG_STATUS_NOT_IMPLEMENTED;
    if (backend != ROCJPEG_BACKEND_HARDWARE || device < 0) return ROCJPEG_STATUS_INVALID_PARAMETER;
    return Guard([&] {
        int count = 0;
        CheckHip(hipGetDeviceCount(&count));
        if (device >= count) return ROCJPEG_STATUS_INVALID_PARAMETER;
        auto decoder = std::make_unique<Decoder>(device);
        decoder->Initialize();
        *handle = decoder.release();
        return ROCJPEG_STATUS_SUCCESS;
    });
}

RocJpegStatus ROCJPEGAPI rocJpegDestroy(RocJpegHandle handle) {
    if (!handle) return ROCJPEG_STATUS_INVALID_PARAMETER;
    delete static_cast<Decoder*>(handle);
    return ROCJPEG_STATUS_SUCCESS;
}

RocJpegStatus ROCJPEGAPI rocJpegGetImageInfo(RocJpegHandle handle, RocJpegStreamHandle stream, uint8_t* components,
                                         RocJpegChromaSubsampling* subsampling, uint32_t* widths, uint32_t* heights) {
    if (!handle || !stream || !components || !subsampling || !widths || !heights) return ROCJPEG_STATUS_INVALID_PARAMETER;
    return Guard([&] {
        auto& jpeg = *static_cast<JpegStream*>(stream);
        std::lock_guard<std::mutex> lock(jpeg.mutex);
        if (jpeg.bytes.empty()) return ROCJPEG_STATUS_BAD_JPEG;
        *components = jpeg.components;
        *subsampling = jpeg.subsampling;
        std::fill_n(widths, ROCJPEG_MAX_COMPONENT, 0);
        std::fill_n(heights, ROCJPEG_MAX_COMPONENT, 0);
        widths[0] = jpeg.width; heights[0] = jpeg.height;
        if (jpeg.components == 3) {
            const bool half_x = jpeg.subsampling == ROCJPEG_CSS_420 || jpeg.subsampling == ROCJPEG_CSS_422;
            const bool half_y = jpeg.subsampling == ROCJPEG_CSS_420 || jpeg.subsampling == ROCJPEG_CSS_440;
            widths[1] = widths[2] = half_x ? (jpeg.width + 1) / 2 : jpeg.width;
            heights[1] = heights[2] = half_y ? (jpeg.height + 1) / 2 : jpeg.height;
        }
        return ROCJPEG_STATUS_SUCCESS;
    });
}

RocJpegStatus ROCJPEGAPI rocJpegDecode(RocJpegHandle handle, RocJpegStreamHandle stream,
                                    const RocJpegDecodeParams* params, RocJpegImage* destination) {
    if (!handle || !stream || !params || !destination) return ROCJPEG_STATUS_INVALID_PARAMETER;
    return Guard([&] {
        auto& decoder = *static_cast<Decoder*>(handle);
        auto& jpeg = *static_cast<JpegStream*>(stream);
        std::scoped_lock lock(decoder.mutex, jpeg.mutex);
        return decoder.Decode(jpeg, *params, *destination);
    });
}

RocJpegStatus ROCJPEGAPI rocJpegDecodeBatched(RocJpegHandle handle, RocJpegStreamHandle* streams, int batch_size,
                                           const RocJpegDecodeParams* params, RocJpegImage* destinations) {
    if (!handle || !streams || batch_size <= 0 || !params || !destinations) return ROCJPEG_STATUS_INVALID_PARAMETER;
    for (int i = 0; i < batch_size; ++i) {
        const auto status = rocJpegDecode(handle, streams[i], params + i, destinations + i);
        if (status != ROCJPEG_STATUS_SUCCESS) return status;
    }
    return ROCJPEG_STATUS_SUCCESS;
}

RocJpegStatus ROCJPEGAPI rocJpegDecodeAsync(RocJpegHandle handle, RocJpegStreamHandle stream,
                                         const RocJpegDecodeParams* params, RocJpegImage* destination) {
    if (!handle || !stream || !params || !destination) return ROCJPEG_STATUS_INVALID_PARAMETER;
    return Guard([&] {
        auto& decoder = *static_cast<Decoder*>(handle);
        auto& jpeg = *static_cast<JpegStream*>(stream);
        std::scoped_lock lock(decoder.mutex, jpeg.mutex);
        return decoder.DecodeAsync(jpeg, *params, destination);
    });
}

RocJpegStatus ROCJPEGAPI rocJpegDecodeSync(RocJpegHandle handle, RocJpegImage* destination) {
    if (!handle || !destination) return ROCJPEG_STATUS_INVALID_PARAMETER;
    return Guard([&] {
        auto& decoder = *static_cast<Decoder*>(handle);
        std::lock_guard<std::mutex> lock(decoder.mutex);
        return decoder.DecodeSync(destination);
    });
}

RocJpegStatus ROCJPEGAPI rocJpegDecodeBatchedAsync(RocJpegHandle handle, RocJpegStreamHandle* streams, int batch_size,
                                                const RocJpegDecodeParams* params, RocJpegImage* destinations) {
    if (!handle || !streams || batch_size <= 0 || !params || !destinations) return ROCJPEG_STATUS_INVALID_PARAMETER;
    return Guard([&] {
        auto& decoder = *static_cast<Decoder*>(handle);
        std::lock_guard<std::mutex> lock(decoder.mutex);
        for (int i = 0; i < batch_size; ++i) {
            if (!streams[i] || decoder.IsPending(destinations + i)) return ROCJPEG_STATUS_INVALID_PARAMETER;
        }
        for (int i = 0; i < batch_size; ++i) {
            const auto status = Guard([&] {
                auto& jpeg = *static_cast<JpegStream*>(streams[i]);
                std::lock_guard<std::mutex> stream_lock(jpeg.mutex);
                return decoder.DecodeAsync(jpeg, params[i], destinations + i);
            });
            if (status != ROCJPEG_STATUS_SUCCESS) {
                for (int j = 0; j < i; ++j) decoder.DiscardPending(destinations + j);
                return status;
            }
        }
        return ROCJPEG_STATUS_SUCCESS;
    });
}

RocJpegStatus ROCJPEGAPI rocJpegDecodeBatchedSync(RocJpegHandle handle, RocJpegImage* destinations, int batch_size) {
    if (!handle || !destinations || batch_size <= 0) return ROCJPEG_STATUS_INVALID_PARAMETER;
    return Guard([&] {
        auto& decoder = *static_cast<Decoder*>(handle);
        std::lock_guard<std::mutex> lock(decoder.mutex);
        for (int i = 0; i < batch_size; ++i) {
            if (!decoder.IsPending(destinations + i)) return ROCJPEG_STATUS_INVALID_PARAMETER;
        }
        RocJpegStatus first_error = ROCJPEG_STATUS_SUCCESS;
        for (int i = 0; i < batch_size; ++i) {
            const auto status = Guard([&] { return decoder.DecodeSync(destinations + i); });
            if (first_error == ROCJPEG_STATUS_SUCCESS && status != ROCJPEG_STATUS_SUCCESS) first_error = status;
        }
        return first_error;
    });
}

ROCJPEGAPI const char* rocJpegGetErrorName(RocJpegStatus status) {
#define STATUS_NAME(name) case name: return #name
    switch (status) {
        STATUS_NAME(ROCJPEG_STATUS_SUCCESS);
        STATUS_NAME(ROCJPEG_STATUS_NOT_INITIALIZED);
        STATUS_NAME(ROCJPEG_STATUS_INVALID_PARAMETER);
        STATUS_NAME(ROCJPEG_STATUS_BAD_JPEG);
        STATUS_NAME(ROCJPEG_STATUS_JPEG_NOT_SUPPORTED);
        STATUS_NAME(ROCJPEG_STATUS_OUTOF_MEMORY);
        STATUS_NAME(ROCJPEG_STATUS_EXECUTION_FAILED);
        STATUS_NAME(ROCJPEG_STATUS_ARCH_MISMATCH);
        STATUS_NAME(ROCJPEG_STATUS_INTERNAL_ERROR);
        STATUS_NAME(ROCJPEG_STATUS_IMPLEMENTATION_NOT_SUPPORTED);
        STATUS_NAME(ROCJPEG_STATUS_HW_JPEG_DECODER_NOT_SUPPORTED);
        STATUS_NAME(ROCJPEG_STATUS_RUNTIME_ERROR);
        STATUS_NAME(ROCJPEG_STATUS_NOT_IMPLEMENTED);
        STATUS_NAME(ROCJPEG_STATUS_MAX_VALUE);
        default: return "ROCJPEG_STATUS_UNKNOWN";
    }
#undef STATUS_NAME
}
} // extern "C"
