/* Copyright (c) 2026 rocJPEG Windows contributors.
 * SPDX-License-Identifier: MIT
 *
 * Windows VCN JPEG decoding through the AMD D3D11 MJPEG profile. No AMF is used.
 * Decoded pixels stay on the GPU: D3D11 -> shared D3D12 buffer -> HIP.
 */

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d11_4.h>
#include <d3d10.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <initguid.h>
#include <dxva.h>
#include <wrl/client.h>

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

// AMD's profile predates the standard Windows 11 MJPEG profiles. Its picture
// buffer is DXVA_PictureParameters, not DXVA_PicParams_MJPEG. See docs/windows.md.
constexpr GUID kAmdMjpeg = {0xd1c20509, 0xae7b, 0x4e72, {0xae, 0x3b, 0x49, 0xf8, 0x8d, 0x58, 0x99, 0x2f}};
static_assert(sizeof(DXVA_PictureParameters) == 44);

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
    bool complete_tables = false;

    RocJpegStatus Parse(const uint8_t* data, size_t length) {
        bytes.clear(); width = height = components = 0; subsampling = ROCJPEG_CSS_UNKNOWN;
        complete_tables = false;
        if (!data || length < 4 || data[0] != 0xff || data[1] != 0xd8)
            return ROCJPEG_STATUS_BAD_JPEG;
        if (length > std::numeric_limits<uint32_t>::max()) return ROCJPEG_STATUS_JPEG_NOT_SUPPORTED;
        bool frame = false, scan = false, end = false;
        int adobe_transform = -1;
        std::array<uint8_t, 3> ids{};
        std::array<uint8_t, 3> quantizers{};
        unsigned quant_mask = 0, dc_mask = 0, ac_mask = 0;
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
            if (marker == 0xdb) {
                size_t offset = 0;
                while (offset < size - 2) {
                    const unsigned table = segment[offset++];
                    if ((table & 15) > 3) return ROCJPEG_STATUS_BAD_JPEG;
                    if (table >> 4) return ROCJPEG_STATUS_JPEG_NOT_SUPPORTED;
                    if (size - 2 - offset < 64) return ROCJPEG_STATUS_BAD_JPEG;
                    for (unsigned i = 0; i < 64; ++i) if (!segment[offset + i]) return ROCJPEG_STATUS_BAD_JPEG;
                    quant_mask |= 1u << table;
                    offset += 64;
                }
            } else if (marker == 0xc4) {
                size_t offset = 0;
                while (offset < size - 2) {
                    const unsigned table = segment[offset++];
                    if ((table & 15) > 3 || (table >> 4) > 1 || size - 2 - offset < 16)
                        return ROCJPEG_STATUS_BAD_JPEG;
                    unsigned symbols = 0;
                    int codes = 1;
                    for (unsigned i = 0; i < 16; ++i) {
                        const unsigned count = segment[offset++];
                        symbols += count;
                        codes = 2 * codes - static_cast<int>(count);
                        if (codes <= 0) return ROCJPEG_STATUS_BAD_JPEG;
                    }
                    if (!symbols || symbols > 256 || symbols > size - 2 - offset)
                        return ROCJPEG_STATUS_BAD_JPEG;
                    for (unsigned i = 0; i < symbols; ++i) {
                        const unsigned value = segment[offset + i];
                        if ((table >> 4) == 0 ? value > 11 : (value & 15) > 10 || (!(value & 15) && value != 0 && value != 0xf0))
                            return ROCJPEG_STATUS_BAD_JPEG;
                    }
                    (table >> 4 ? ac_mask : dc_mask) |= 1u << (table & 15);
                    offset += symbols;
                }
            } else if (marker == 0xdd) {
                if (size != 4) return ROCJPEG_STATUS_BAD_JPEG;
            } else if (marker == 0xee && size >= 14 && std::memcmp(segment, "Adobe", 5) == 0) {
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
                    quantizers[c] = segment[8 + 3 * c];
                    if (quantizers[c] > 3) return ROCJPEG_STATUS_BAD_JPEG;
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
                complete_tables = true;
                for (unsigned c = 0; c < components; ++c) {
                    const unsigned selectors = segment[2 + 2 * c];
                    if ((selectors >> 4) > 3 || (selectors & 15) > 3) return ROCJPEG_STATUS_BAD_JPEG;
                    complete_tables = complete_tables && (quant_mask & (1u << quantizers[c])) &&
                        (dc_mask & (1u << (selectors >> 4))) && (ac_mask & (1u << (selectors & 15)));
                }
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

struct DecodeSession {
    ComPtr<ID3D11VideoDecoder> decoder;
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<ID3D11VideoDecoderOutputView> output;
    uint32_t width = 0, height = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
};

struct Submission {
    std::shared_ptr<DecodeSession> session;
    ComPtr<ID3D11Query> completion;
    RocJpegDecodeParams params{};
    uint32_t width = 0, height = 0;
    RocJpegChromaSubsampling subsampling = ROCJPEG_CSS_UNKNOWN;
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
        for (const auto& [destination, job] : pending_) (void)WaitForDecode(*job);
        pending_.clear();
        reusable_.reset();
        transfer_.reset();
        if (hip_fence_) (void)hipDestroyExternalSemaphore(hip_fence_);
        if (hip_stream_) (void)hipStreamDestroy(hip_stream_);
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
        Check(device11_.As(&video_device_));
        Check(context11_.As(&video_context_));
        bool found_profile = false;
        for (UINT i = 0; i < video_device_->GetVideoDecoderProfileCount(); ++i) {
            GUID profile{};
            Check(video_device_->GetVideoDecoderProfile(i, &profile));
            if (profile == kAmdMjpeg) { found_profile = true; break; }
        }
        BOOL nv12 = FALSE;
        if (!found_profile || FAILED(video_device_->CheckVideoDecoderFormat(&kAmdMjpeg, DXGI_FORMAT_NV12, &nv12)) || !nv12)
            throw Failure{ROCJPEG_STATUS_HW_JPEG_DECODER_NOT_SUPPORTED};
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

    }

    RocJpegStatus Decode(const JpegStream& jpeg, const RocJpegDecodeParams& params, RocJpegImage& destination) {
        if (!pending_.empty()) return ROCJPEG_STATUS_EXECUTION_FAILED;
        DeviceScope scope(device_);
        auto job = Submit(jpeg, params, true);
        return Finish(*job, destination);
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
    void DiscardPending(RocJpegImage* destination) {
        const auto it = pending_.find(destination);
        if (it != pending_.end()) { (void)WaitForDecode(*it->second); pending_.erase(it); }
    }

    std::mutex mutex;

private:
    std::shared_ptr<DecodeSession> CreateSession(uint32_t width, uint32_t height, DXGI_FORMAT format) {
        // VCN decode surfaces use 64-pixel padding. The picture buffer below
        // carries the unpadded image size; padding must never reach the output.
        D3D11_VIDEO_DECODER_DESC desc{kAmdMjpeg, (width + 63u) & ~63u, (height + 63u) & ~63u, format};
        BOOL supported = FALSE;
        if (FAILED(video_device_->CheckVideoDecoderFormat(&kAmdMjpeg, format, &supported)) || !supported)
            throw Failure{ROCJPEG_STATUS_JPEG_NOT_SUPPORTED};
        UINT count = 0;
        if (FAILED(video_device_->GetVideoDecoderConfigCount(&desc, &count)) || !count)
            throw Failure{ROCJPEG_STATUS_JPEG_NOT_SUPPORTED};
        D3D11_VIDEO_DECODER_CONFIG config{};
        bool found = false;
        for (UINT i = 0; i < count; ++i) {
            Check(video_device_->GetVideoDecoderConfig(&desc, i, &config));
            if (config.ConfigBitstreamRaw == 1 && config.guidConfigBitstreamEncryption == DXVA_NoEncrypt) {
                found = true; break;
            }
        }
        if (!found) throw Failure{ROCJPEG_STATUS_IMPLEMENTATION_NOT_SUPPORTED};
        auto session = std::make_shared<DecodeSession>();
        session->width = width; session->height = height; session->format = format;
        Check(video_device_->CreateVideoDecoder(&desc, &config, &session->decoder));
        D3D11_TEXTURE2D_DESC texture{};
        texture.Width = desc.SampleWidth; texture.Height = desc.SampleHeight;
        texture.MipLevels = texture.ArraySize = texture.SampleDesc.Count = 1;
        texture.Format = format;
        texture.BindFlags = D3D11_BIND_DECODER | D3D11_BIND_SHADER_RESOURCE;
        texture.MiscFlags = D3D11_RESOURCE_MISC_SHARED;
        Check(device11_->CreateTexture2D(&texture, nullptr, &session->texture));
        D3D11_VIDEO_DECODER_OUTPUT_VIEW_DESC view{};
        view.DecodeProfile = kAmdMjpeg;
        view.ViewDimension = D3D11_VDOV_DIMENSION_TEXTURE2D;
        Check(video_device_->CreateVideoDecoderOutputView(session->texture.Get(), &view, &session->output));
        return session;
    }

    D3D11_VIDEO_DECODER_BUFFER_DESC Upload(ID3D11VideoDecoder* decoder,
                                           D3D11_VIDEO_DECODER_BUFFER_TYPE type,
                                           const void* data, size_t size) {
        UINT capacity = 0;
        void* buffer = nullptr;
        Check(video_context_->GetDecoderBuffer(decoder, type, &capacity, &buffer));
        if (!buffer || size > capacity) {
            (void)video_context_->ReleaseDecoderBuffer(decoder, type);
            throw Failure{ROCJPEG_STATUS_JPEG_NOT_SUPPORTED};
        }
        std::memcpy(buffer, data, size);
        Check(video_context_->ReleaseDecoderBuffer(decoder, type));
        D3D11_VIDEO_DECODER_BUFFER_DESC desc{};
        desc.BufferType = type;
        desc.DataSize = static_cast<UINT>(size);
        return desc;
    }

    bool WaitForDecode(const Submission& job) noexcept {
        if (!job.completion) return true;
        context11_->Flush();
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (std::chrono::steady_clock::now() < deadline) {
            BOOL complete = FALSE;
            const auto status = context11_->GetData(job.completion.Get(), &complete, sizeof(complete), 0);
            if (status == S_OK && complete) return true;
            if (FAILED(status)) return false;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return false;
    }

    std::unique_ptr<Submission> Submit(const JpegStream& jpeg, const RocJpegDecodeParams& params, bool reuse) {
        if (jpeg.bytes.empty()) throw Failure{ROCJPEG_STATUS_BAD_JPEG};
        if (jpeg.width > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION || jpeg.height > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION)
            throw Failure{ROCJPEG_STATUS_JPEG_NOT_SUPPORTED};
        if (params.output_format < ROCJPEG_OUTPUT_NATIVE || params.output_format >= ROCJPEG_OUTPUT_FORMAT_MAX)
            throw Failure{ROCJPEG_STATUS_INVALID_PARAMETER};
        // This profile exposes NV12/YUY2. Reducing 4:4:4/4:4:0 chroma would lose
        // image detail; let the application select its fallback instead.
        // Its 4:0:0 output has an incorrect row layout on the validated driver
        // (also present through AMF). Do not report a corrupt grayscale success.
        if (jpeg.subsampling == ROCJPEG_CSS_444 || jpeg.subsampling == ROCJPEG_CSS_440 || jpeg.subsampling == ROCJPEG_CSS_400)
            throw Failure{ROCJPEG_STATUS_JPEG_NOT_SUPPORTED};
        // Never submit header-only streams or invalid/missing tables to the driver.
        if (!jpeg.complete_tables) throw Failure{ROCJPEG_STATUS_BAD_JPEG};
        const DXGI_FORMAT format = jpeg.subsampling == ROCJPEG_CSS_422 ? DXGI_FORMAT_YUY2 : DXGI_FORMAT_NV12;
        auto job = std::make_unique<Submission>();
        job->params = params;
        job->width = jpeg.width; job->height = jpeg.height;
        job->subsampling = jpeg.subsampling;
        if (reuse) {
            if (!reusable_ || reusable_->width != jpeg.width || reusable_->height != jpeg.height || reusable_->format != format)
                reusable_ = CreateSession(jpeg.width, jpeg.height, format);
            job->session = reusable_;
        } else {
            // Each in-flight output owns its surface; later submissions cannot
            // overwrite it, even when sync calls arrive in a different order.
            job->session = CreateSession(jpeg.width, jpeg.height, format);
        }
        D3D11_QUERY_DESC query{D3D11_QUERY_EVENT, 0};
        Check(device11_->CreateQuery(&query, &job->completion));
        auto* decoder = job->session->decoder.Get();
        HRESULT begin;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        do {
            begin = video_context_->DecoderBeginFrame(decoder, job->session->output.Get(), 0, nullptr);
            if (begin != E_PENDING) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        } while (std::chrono::steady_clock::now() < deadline);
        Check(begin);
        try {
            DXVA_PictureParameters picture{};
            // Despite their legacy names, AMD's JPEG profile uses pixels here.
            picture.wPicWidthInMBminus1 = static_cast<WORD>(jpeg.width - 1);
            picture.wPicHeightInMBminus1 = static_cast<WORD>(jpeg.height - 1);
            picture.bPicStructure = 3; // frame; zero selects a field and corrupts row layout
            picture.bPicIntra = 1;
            const std::array buffers{
                Upload(decoder, D3D11_VIDEO_DECODER_BUFFER_PICTURE_PARAMETERS, &picture, sizeof(picture)),
                Upload(decoder, D3D11_VIDEO_DECODER_BUFFER_BITSTREAM, jpeg.bytes.data(), jpeg.bytes.size())};
            Check(video_context_->SubmitDecoderBuffers(decoder, static_cast<UINT>(buffers.size()), buffers.data()));
        } catch (...) {
            (void)video_context_->DecoderEndFrame(decoder);
            context11_->End(job->completion.Get());
            (void)WaitForDecode(*job);
            reusable_.reset();
            throw;
        }
        Check(video_context_->DecoderEndFrame(decoder));
        context11_->End(job->completion.Get());
        context11_->Flush();
        return job;
    }

    RocJpegStatus Finish(const Submission& job, RocJpegImage& destination) {
        // A previous failed conversion still owns work on these queues. Never reset
        // the command allocator or overwrite its imported buffer before it finishes.
        CheckHip(hipStreamSynchronize(hip_stream_));
        if (!WaitForQueue()) return ROCJPEG_STATUS_EXECUTION_FAILED;
        auto* texture = job.session->texture.Get();
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
        view.format = job.session->format == DXGI_FORMAT_NV12 ? SurfaceFormat::NV12 : SurfaceFormat::YUY2;
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
        // Create on D3D11 first: the driver cannot open a D3D12-created NV12
        // allocation with the decoder's required bindings on this path.
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
    std::shared_ptr<DecodeSession> reusable_;
    ComPtr<ID3D11VideoDevice> video_device_;
    ComPtr<ID3D11VideoContext> video_context_;
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
