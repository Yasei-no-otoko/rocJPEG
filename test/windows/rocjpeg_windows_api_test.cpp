// SPDX-License-Identifier: MIT
#define NOMINMAX
#include <windows.h>
#include <tlhelp32.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <rocjpeg/rocjpeg.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
unsigned checks = 0;
void Require(bool condition, const char* message) {
    ++checks;
    if (!condition) throw std::runtime_error(message);
}
void Status(RocJpegStatus actual, RocJpegStatus expected, const char* operation) {
    ++checks;
    if (actual != expected)
        throw std::runtime_error(std::string(operation) + ": " + rocJpegGetErrorName(actual) +
                                 ", expected " + rocJpegGetErrorName(expected));
}
void Hip(hipError_t status) {
    if (status != hipSuccess) throw std::runtime_error(hipGetErrorString(status));
}
void CheckNoAmf() {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
    Require(snapshot != INVALID_HANDLE_VALUE, "enumerate loaded modules");
    MODULEENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    bool found = false;
    if (Module32FirstW(snapshot, &entry)) {
        do {
            if (_wcsnicmp(entry.szModule, L"amf", 3) == 0) {
                std::wcerr << L"Unexpected AMF module: " << entry.szModule << L'\n';
                found = true;
            }
        } while (Module32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    Require(!found, "native rocJPEG must not load an AMF runtime or component");
}

void CompareCpu(const std::filesystem::path& path, const std::vector<uint8_t>& actual,
                uint32_t width, uint32_t height) {
    using Microsoft::WRL::ComPtr;
    const auto com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    Require(SUCCEEDED(com), "initialize CPU reference COM");
    struct ComScope { ~ComScope() { CoUninitialize(); } } scope;
    ComPtr<IWICImagingFactory> factory;
    Require(SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                      IID_PPV_ARGS(&factory))), "create CPU JPEG reference");
    ComPtr<IWICBitmapDecoder> decoder;
    Require(SUCCEEDED(factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
                                                       WICDecodeMetadataCacheOnLoad, &decoder)), "open CPU JPEG reference");
    ComPtr<IWICBitmapFrameDecode> frame;
    Require(SUCCEEDED(decoder->GetFrame(0, &frame)), "read CPU JPEG frame");
    UINT cpu_width = 0, cpu_height = 0;
    Require(SUCCEEDED(frame->GetSize(&cpu_width, &cpu_height)) && cpu_width == width && cpu_height == height,
            "CPU/GPU dimensions agree");
    ComPtr<IWICFormatConverter> converter;
    Require(SUCCEEDED(factory->CreateFormatConverter(&converter)), "create CPU RGB converter");
    Require(SUCCEEDED(converter->Initialize(frame.Get(), GUID_WICPixelFormat24bppRGB, WICBitmapDitherTypeNone,
                                           nullptr, 0, WICBitmapPaletteTypeCustom)), "convert CPU JPEG to RGB");
    std::vector<uint8_t> expected(actual.size());
    Require(SUCCEEDED(converter->CopyPixels(nullptr, width * 3, static_cast<UINT>(expected.size()), expected.data())),
            "read CPU RGB pixels");
    uint64_t total = 0;
    int maximum = 0;
    for (size_t i = 0; i < actual.size(); ++i) {
        const int error = std::abs(int(actual[i]) - int(expected[i]));
        total += error;
        maximum = std::max(maximum, error);
    }
    const double mean = double(total) / actual.size();
    std::cout << "CPU reference RGB: mean error=" << mean << ", max=" << maximum << '\n';
    // IDCT rounding and chroma interpolation may differ. These bounds detect
    // wrong pitch, field layout, channel order, or full/limited-range conversion.
    Require(mean <= 1.5 && maximum <= 32, "hardware RGB differs from independent CPU decoder");
}
void SamePixels(const std::vector<uint8_t>& actual, const std::vector<uint8_t>& expected,
                const char* operation) {
    Require(actual.size() == expected.size(), "pixel buffer size changed");
    size_t different = 0, first = 0;
    int maximum_error = 0;
    for (size_t i = 0; i < actual.size(); ++i) {
        if (actual[i] == expected[i]) continue;
        if (!different) first = i;
        ++different;
        maximum_error = std::max(maximum_error, std::abs(int(actual[i]) - int(expected[i])));
    }
    if (different)
        std::cerr << operation << ": " << different << '/' << actual.size()
                  << " bytes differ, max error " << maximum_error << ", first " << first
                  << " actual=" << int(actual[first]) << " expected=" << int(expected[first]) << '\n';
    Require(different == 0, operation);
}
struct Stream {
    RocJpegStreamHandle value = nullptr;
    Stream() { Status(rocJpegStreamCreate(&value), ROCJPEG_STATUS_SUCCESS, "create stream"); }
    ~Stream() { if (value) rocJpegStreamDestroy(value); }
    Stream(const Stream&) = delete;
    Stream& operator=(const Stream&) = delete;
};
struct Decoder {
    RocJpegHandle value = nullptr;
    ~Decoder() { if (value) rocJpegDestroy(value); }
};

// A header-only baseline stream is sufficient to test parser boundaries. It is
// deliberately never passed to the hardware decoder, which also needs tables.
std::vector<uint8_t> Header(uint8_t marker = 0xc0, uint8_t components = 3) {
    std::vector<uint8_t> bytes{0xff, 0xd8, 0xff, marker, 0,
        static_cast<uint8_t>(8 + 3 * components), 8, 0, 17, 0, 19, components};
    for (uint8_t c = 0; c < components; ++c) {
        bytes.push_back(c + 1);
        bytes.push_back(c == 0 ? 0x22 : 0x11);
        bytes.push_back(0);
    }
    bytes.insert(bytes.end(), {0xff, 0xda, 0, static_cast<uint8_t>(6 + 2 * components), components});
    for (uint8_t c = 0; c < components; ++c) {
        bytes.push_back(c + 1);
        bytes.push_back(0);
    }
    bytes.insert(bytes.end(), {0, 63, 0, 1, 0xff, 0xd9});
    return bytes;
}

class GuardPage {
public:
    GuardPage() {
        SYSTEM_INFO info{};
        GetSystemInfo(&info);
        page_ = info.dwPageSize;
        memory_ = static_cast<uint8_t*>(VirtualAlloc(nullptr, 2 * page_, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        Require(memory_ != nullptr, "VirtualAlloc parser guard");
        DWORD previous = 0;
        if (!VirtualProtect(memory_ + page_, page_, PAGE_NOACCESS, &previous)) {
            VirtualFree(memory_, 0, MEM_RELEASE);
            throw std::runtime_error("VirtualProtect parser guard");
        }
    }
    ~GuardPage() { VirtualFree(memory_, 0, MEM_RELEASE); }
    const uint8_t* Place(const std::vector<uint8_t>& bytes, size_t length) {
        Require(length < page_, "parser test fits guarded page");
        auto* start = memory_ + page_ - length;
        if (length) std::memcpy(start, bytes.data(), length);
        return start;
    }
private:
    uint8_t* memory_ = nullptr;
    size_t page_ = 0;
};

void ParserAndArguments() {
    const auto baseline = Header();
    Stream stream;
    Status(rocJpegStreamCreate(nullptr), ROCJPEG_STATUS_INVALID_PARAMETER, "null stream output");
    Status(rocJpegStreamDestroy(nullptr), ROCJPEG_STATUS_INVALID_PARAMETER, "null stream destroy");
    Status(rocJpegStreamParse(nullptr, 1, stream.value), ROCJPEG_STATUS_INVALID_PARAMETER, "null JPEG data");
    Status(rocJpegStreamParse(baseline.data(), baseline.size(), nullptr), ROCJPEG_STATUS_INVALID_PARAMETER, "null stream parse");
    Status(rocJpegCreate(ROCJPEG_BACKEND_HARDWARE, 0, nullptr), ROCJPEG_STATUS_INVALID_PARAMETER, "null decoder output");
    Decoder decoder;
    Status(rocJpegCreate(static_cast<RocJpegBackend>(99), 0, &decoder.value), ROCJPEG_STATUS_INVALID_PARAMETER, "invalid backend");
    Require(decoder.value == nullptr, "failed create clears handle");
    Status(rocJpegCreate(ROCJPEG_BACKEND_HARDWARE, -1, &decoder.value), ROCJPEG_STATUS_INVALID_PARAMETER, "negative device");
    Status(rocJpegCreate(ROCJPEG_BACKEND_HYBRID, 0, &decoder.value), ROCJPEG_STATUS_NOT_IMPLEMENTED, "hybrid is explicit unsupported");
    Status(rocJpegDestroy(nullptr), ROCJPEG_STATUS_INVALID_PARAMETER, "null decoder destroy");
    Status(rocJpegGetImageInfo(nullptr, nullptr, nullptr, nullptr, nullptr, nullptr), ROCJPEG_STATUS_INVALID_PARAMETER, "null image info");
    Status(rocJpegDecode(nullptr, nullptr, nullptr, nullptr), ROCJPEG_STATUS_INVALID_PARAMETER, "null decode");
    Status(rocJpegDecodeBatched(nullptr, nullptr, 0, nullptr, nullptr), ROCJPEG_STATUS_INVALID_PARAMETER, "null batch");
    Status(rocJpegDecodeAsync(nullptr, nullptr, nullptr, nullptr), ROCJPEG_STATUS_INVALID_PARAMETER, "null async decode");
    Status(rocJpegDecodeSync(nullptr, nullptr), ROCJPEG_STATUS_INVALID_PARAMETER, "null async sync");
    Status(rocJpegDecodeBatchedAsync(nullptr, nullptr, 0, nullptr, nullptr), ROCJPEG_STATUS_INVALID_PARAMETER, "null async batch");
    Status(rocJpegDecodeBatchedSync(nullptr, nullptr, 0), ROCJPEG_STATUS_INVALID_PARAMETER, "null async batch sync");
    Require(std::string(rocJpegGetErrorName(ROCJPEG_STATUS_BAD_JPEG)) == "ROCJPEG_STATUS_BAD_JPEG", "error name ABI");
    Require(rocJpegGetErrorName(static_cast<RocJpegStatus>(1234)) != nullptr, "unknown status has name");

    GuardPage guard;
    for (size_t length = 0; length < baseline.size(); ++length)
        Status(rocJpegStreamParse(guard.Place(baseline, length), length, stream.value),
               ROCJPEG_STATUS_BAD_JPEG, "truncated JPEG at guard page");
    Status(rocJpegStreamParse(baseline.data(), baseline.size(), stream.value), ROCJPEG_STATUS_SUCCESS, "baseline header");
    for (const auto& unsupported : {Header(0xc2), Header(0xc0, 4)})
        Status(rocJpegStreamParse(unsupported.data(), unsupported.size(), stream.value),
               ROCJPEG_STATUS_JPEG_NOT_SUPPORTED, "progressive/CMYK fallback status");
    Status(rocJpegStreamParse(baseline.data(), baseline.size(), stream.value), ROCJPEG_STATUS_SUCCESS, "parser reusable after failure");

    const auto invalid_segment = [&](std::initializer_list<uint8_t> segment) {
        auto jpeg = baseline;
        jpeg.insert(jpeg.begin() + 2, segment);
        Status(rocJpegStreamParse(jpeg.data(), jpeg.size(), stream.value), ROCJPEG_STATUS_BAD_JPEG, "malformed JPEG tables");
    };
    invalid_segment({0xff, 0xdb, 0, 4, 0, 1}); // incomplete quantizer
    invalid_segment({0xff, 0xc4, 0, 4, 0, 1}); // incomplete Huffman counts
    invalid_segment({0xff, 0xdd, 0, 3, 0}); // truncated restart interval
    auto bad_selector = baseline;
    bad_selector[14] = 4;
    Status(rocJpegStreamParse(bad_selector.data(), bad_selector.size(), stream.value), ROCJPEG_STATUS_BAD_JPEG, "invalid quantizer selector");
}

class GuardedRgb {
public:
    GuardedRgb(uint32_t width, uint32_t height)
        : row_(size_t(width) * 3), pitch_(row_ + 37), height_(height), size_(64 + pitch_ * height + 64) {
        Hip(hipMalloc(reinterpret_cast<void**>(&memory_), size_));
        Reset();
        image.channel[0] = memory_ + 64;
        image.pitch[0] = static_cast<uint32_t>(pitch_);
    }
    ~GuardedRgb() { (void)hipFree(memory_); }
    GuardedRgb(const GuardedRgb&) = delete;
    GuardedRgb& operator=(const GuardedRgb&) = delete;
    void Reset() {
        Hip(hipMemset(memory_, 0xa5, size_));
        // The decoder uses a nonblocking HIP stream, so caller initialization
        // must finish before ownership passes to rocJPEG.
        Hip(hipStreamSynchronize(nullptr));
    }
    std::vector<uint8_t> Pixels() const {
        std::vector<uint8_t> all(size_), pixels(row_ * height_);
        Hip(hipMemcpy(all.data(), memory_, size_, hipMemcpyDeviceToHost));
        const auto sentinel = [](uint8_t value) { return value == 0xa5; };
        Require(std::all_of(all.begin(), all.begin() + 64, sentinel), "RGB prefix guard overwritten");
        Require(std::all_of(all.end() - 64, all.end(), sentinel), "RGB suffix guard overwritten");
        for (size_t y = 0; y < height_; ++y) {
            const auto first = all.begin() + 64 + y * pitch_;
            Require(std::all_of(first + row_, first + pitch_, sentinel), "RGB row padding overwritten");
            std::copy_n(first, row_, pixels.begin() + y * row_);
        }
        return pixels;
    }
    RocJpegImage image{};
private:
    uint8_t* memory_ = nullptr;
    size_t row_, pitch_, height_, size_;
};

void UnsupportedSampling(const std::filesystem::path& path_444, const std::filesystem::path& path_440,
                         const std::filesystem::path& path_400,
                         Decoder& decoder) {
    const auto read = [](const std::filesystem::path& path) {
        std::ifstream input(path, std::ios::binary);
        Require(input.good(), "open unsupported JPEG fixture");
        std::vector<uint8_t> bytes(std::istreambuf_iterator<char>{input}, {});
        Require(!bytes.empty() && bytes.size() < 1024 * 1024, "unsupported JPEG fixture size bound");
        return bytes;
    };
    if (!decoder.value)
        Status(rocJpegCreate(ROCJPEG_BACKEND_HARDWARE, 0, &decoder.value), ROCJPEG_STATUS_SUCCESS, "create fallback test decoder");
    // Self-generated 97x65 RGB gradient: ((3*x+y)%256, (x+5*y)%256,
    // (2*x+7*y)%256), Pillow quality=95/subsampling=0 for 4:4:4. For 4:4:0,
    // subsampling=1 followed by a lossless TurboJPEG transpose gives 65x97.
    // The real 4:4:4 fixture previously reached a crashing driver path.
    const std::array<std::vector<uint8_t>, 3> samples{read(path_444), read(path_440), read(path_400)};
    const std::array<RocJpegChromaSubsampling, 3> formats{ROCJPEG_CSS_444, ROCJPEG_CSS_440, ROCJPEG_CSS_400};
    for (size_t i = 0; i < samples.size(); ++i) {
        Stream stream;
        Status(rocJpegStreamParse(samples[i].data(), samples[i].size(), stream.value), ROCJPEG_STATUS_SUCCESS, "parse unsupported sampling");
        uint8_t components = 0;
        RocJpegChromaSubsampling chroma = ROCJPEG_CSS_UNKNOWN;
        uint32_t widths[ROCJPEG_MAX_COMPONENT]{}, heights[ROCJPEG_MAX_COMPONENT]{};
        Status(rocJpegGetImageInfo(decoder.value, stream.value, &components, &chroma, widths, heights), ROCJPEG_STATUS_SUCCESS, "unsupported sampling info");
        Require(components == (formats[i] == ROCJPEG_CSS_400 ? 1 : 3) && chroma == formats[i], "unsupported sampling metadata");
        Require(widths[0] && heights[0] && uint64_t(widths[0]) * heights[0] < 1024 * 1024, "unsupported fixture dimensions");
        GuardedRgb output(widths[0], heights[0]);
        RocJpegDecodeParams params{};
        params.output_format = ROCJPEG_OUTPUT_RGB;
        Status(rocJpegDecode(decoder.value, stream.value, &params, &output.image), ROCJPEG_STATUS_JPEG_NOT_SUPPORTED, "unsupported synchronous decode");
        Status(rocJpegDecodeAsync(decoder.value, stream.value, &params, &output.image), ROCJPEG_STATUS_JPEG_NOT_SUPPORTED, "unsupported async decode");
        Status(rocJpegDecodeSync(decoder.value, &output.image), ROCJPEG_STATUS_INVALID_PARAMETER, "failed async submission is not pending");
        auto handle = stream.value;
        Status(rocJpegDecodeBatched(decoder.value, &handle, 1, &params, &output.image), ROCJPEG_STATUS_JPEG_NOT_SUPPORTED, "unsupported batch decode");
        Status(rocJpegDecodeBatchedAsync(decoder.value, &handle, 1, &params, &output.image), ROCJPEG_STATUS_JPEG_NOT_SUPPORTED, "unsupported async batch decode");
        Status(rocJpegDecodeBatchedSync(decoder.value, &output.image, 1), ROCJPEG_STATUS_INVALID_PARAMETER, "failed async batch is not pending");
        const auto pixels = output.Pixels();
        Require(std::all_of(pixels.begin(), pixels.end(), [](uint8_t x) { return x == 0xa5; }), "unsupported decode wrote output");
    }
    std::cout << "4:4:4/4:4:0/4:0:0 fallback: sync/async/batch guards passed\n";
}

void DecodeSample(const std::filesystem::path& path, Decoder& decoder) {
    std::ifstream input(path, std::ios::binary);
    Require(input.good(), "open sample JPEG");
    std::vector<uint8_t> bytes(std::istreambuf_iterator<char>{input}, {});
    Require(!bytes.empty() && bytes.size() < 64 * 1024 * 1024, "sample JPEG size bound");
    Stream first, second, asynchronous;
    Status(rocJpegStreamParse(bytes.data(), bytes.size(), first.value), ROCJPEG_STATUS_SUCCESS, "parse sample");
    Status(rocJpegStreamParse(bytes.data(), bytes.size(), second.value), ROCJPEG_STATUS_SUCCESS, "parse second sample");
    Status(rocJpegStreamParse(bytes.data(), bytes.size(), asynchronous.value), ROCJPEG_STATUS_SUCCESS, "parse async sample");
    // StreamParse must own the compressed input through every later decode.
    std::fill(bytes.begin(), bytes.end(), 0);
    bytes.clear();
    bytes.shrink_to_fit();
    if (!decoder.value)
        Status(rocJpegCreate(ROCJPEG_BACKEND_HARDWARE, 0, &decoder.value), ROCJPEG_STATUS_SUCCESS, "create D3D11 hardware decoder");
    uint8_t components = 0;
    RocJpegChromaSubsampling chroma = ROCJPEG_CSS_UNKNOWN;
    uint32_t widths[ROCJPEG_MAX_COMPONENT]{}, heights[ROCJPEG_MAX_COMPONENT]{};
    Status(rocJpegGetImageInfo(decoder.value, first.value, &components, &chroma, widths, heights), ROCJPEG_STATUS_SUCCESS, "sample image info");
    Require((components == 1 || components == 3) && widths[0] && heights[0], "sample dimensions/components");
    Require(uint64_t(widths[0]) * heights[0] <= 32 * 1024 * 1024, "sample decoded size bound");
    Status(rocJpegGetImageInfo(decoder.value, first.value, nullptr, &chroma, widths, heights), ROCJPEG_STATUS_INVALID_PARAMETER, "null components");
    Status(rocJpegGetImageInfo(decoder.value, first.value, &components, nullptr, widths, heights), ROCJPEG_STATUS_INVALID_PARAMETER, "null chroma");
    Status(rocJpegGetImageInfo(decoder.value, first.value, &components, &chroma, nullptr, heights), ROCJPEG_STATUS_INVALID_PARAMETER, "null widths");
    Status(rocJpegGetImageInfo(decoder.value, first.value, &components, &chroma, widths, nullptr), ROCJPEG_STATUS_INVALID_PARAMETER, "null heights");

    GuardedRgb output(widths[0], heights[0]), other(widths[0], heights[0]);
    RocJpegDecodeParams params{};
    params.output_format = ROCJPEG_OUTPUT_RGB;
    Status(rocJpegDecode(decoder.value, first.value, nullptr, &output.image), ROCJPEG_STATUS_INVALID_PARAMETER, "null decode params");
    Status(rocJpegDecode(decoder.value, first.value, &params, nullptr), ROCJPEG_STATUS_INVALID_PARAMETER, "null decode output");
    auto bad_pitch = output.image;
    bad_pitch.pitch[0] = widths[0] * 3 - 1;
    Status(rocJpegDecode(decoder.value, first.value, &params, &bad_pitch), ROCJPEG_STATUS_INVALID_PARAMETER, "short RGB pitch");
    auto untouched = output.Pixels();
    Require(std::all_of(untouched.begin(), untouched.end(), [](uint8_t x) { return x == 0xa5; }), "invalid decode wrote output");
    Status(rocJpegDecode(decoder.value, first.value, &params, &output.image), ROCJPEG_STATUS_SUCCESS, "hardware RGB decode");
    const auto expected = output.Pixels();
    CompareCpu(path, expected, widths[0], heights[0]);
    const auto range = std::minmax_element(expected.begin(), expected.end());
    Require(*range.first != *range.second, "decoded image is constant/unwritten");
    for (int i = 0; i < 3; ++i) {
        output.Reset();
        Status(rocJpegDecode(decoder.value, first.value, &params, &output.image), ROCJPEG_STATUS_SUCCESS, "repeated RGB decode");
        SamePixels(output.Pixels(), expected, "repeated decode changed pixels");
    }
    std::array<RocJpegStreamHandle, 2> streams{first.value, second.value};
    std::array<RocJpegDecodeParams, 2> parameters{params, params};
    std::array<RocJpegImage, 2> images{output.image, other.image};
    Status(rocJpegDecodeBatched(decoder.value, streams.data(), 0, parameters.data(), images.data()), ROCJPEG_STATUS_INVALID_PARAMETER, "zero batch size");
    output.Reset();
    Status(rocJpegDecodeBatched(decoder.value, streams.data(), 2, parameters.data(), images.data()), ROCJPEG_STATUS_SUCCESS, "batch RGB decode");
    SamePixels(output.Pixels(), expected, "first batch decode differs from single");
    SamePixels(other.Pixels(), expected, "second batch decode differs from single");

    output.Reset();
    Status(rocJpegDecodeAsync(decoder.value, asynchronous.value, &params, &output.image), ROCJPEG_STATUS_SUCCESS, "async submit");
    const auto before_sync = output.Pixels();
    Require(std::all_of(before_sync.begin(), before_sync.end(), [](uint8_t x) { return x == 0xa5; }), "async submit copied output before sync");
    Status(rocJpegDecodeAsync(decoder.value, first.value, &params, &output.image), ROCJPEG_STATUS_INVALID_PARAMETER, "duplicate pending destination");
    Status(rocJpegDecodeSync(decoder.value, &other.image), ROCJPEG_STATUS_INVALID_PARAMETER, "unknown sync destination");
    Status(rocJpegStreamDestroy(asynchronous.value), ROCJPEG_STATUS_SUCCESS, "release async source stream");
    asynchronous.value = nullptr;
    Status(rocJpegDecodeSync(decoder.value, &output.image), ROCJPEG_STATUS_SUCCESS, "async sync after source release");
    SamePixels(output.Pixels(), expected, "async result differs from single");
    Status(rocJpegDecodeSync(decoder.value, &output.image), ROCJPEG_STATUS_INVALID_PARAMETER, "completed destination is no longer pending");
    output.Reset();
    other.Reset();
    Status(rocJpegDecodeBatchedAsync(decoder.value, streams.data(), 2, parameters.data(), images.data()), ROCJPEG_STATUS_SUCCESS, "async batch submit");
    Status(rocJpegDecodeBatchedSync(decoder.value, images.data(), 2), ROCJPEG_STATUS_SUCCESS, "async batch sync");
    SamePixels(output.Pixels(), expected, "first async batch differs from single");
    SamePixels(other.Pixels(), expected, "second async batch differs from single");
    CheckNoAmf();
    std::cout << "Native D3D11 hardware RGB: " << widths[0] << 'x' << heights[0]
              << ", repeat/batch/async/pitch guards passed\n";
}

void AsyncDifferentImages(const std::filesystem::path& first_path, const std::filesystem::path& second_path) {
    const std::array paths{first_path, second_path};
    Decoder decoder;
    Status(rocJpegCreate(ROCJPEG_BACKEND_HARDWARE, 0, &decoder.value), ROCJPEG_STATUS_SUCCESS, "create concurrent decoder");
    std::array<Stream, 2> streams;
    std::array<std::unique_ptr<GuardedRgb>, 2> outputs;
    uint32_t widths[2]{}, heights[2]{};
    RocJpegDecodeParams params{};
    params.output_format = ROCJPEG_OUTPUT_RGB;
    for (size_t i = 0; i < paths.size(); ++i) {
        std::ifstream file(paths[i], std::ios::binary);
        Require(file.good(), "read async fixture");
        const std::vector<uint8_t> data(std::istreambuf_iterator<char>{file}, {});
        Status(rocJpegStreamParse(data.data(), data.size(), streams[i].value), ROCJPEG_STATUS_SUCCESS, "parse distinct async JPEG");
        uint8_t components;
        RocJpegChromaSubsampling css;
        uint32_t w[ROCJPEG_MAX_COMPONENT]{}, h[ROCJPEG_MAX_COMPONENT]{};
        Status(rocJpegGetImageInfo(decoder.value, streams[i].value, &components, &css, w, h), ROCJPEG_STATUS_SUCCESS, "async shape");
        widths[i] = w[0]; heights[i] = h[0];
        outputs[i] = std::make_unique<GuardedRgb>(w[0], h[0]);
        Status(rocJpegDecodeAsync(decoder.value, streams[i].value, &params, &outputs[i]->image), ROCJPEG_STATUS_SUCCESS, "submit distinct async JPEG");
        Status(rocJpegStreamDestroy(streams[i].value), ROCJPEG_STATUS_SUCCESS, "destroy submitted input");
        streams[i].value = nullptr;
    }
    for (int i = 1; i >= 0; --i) {
        Status(rocJpegDecodeSync(decoder.value, &outputs[i]->image), ROCJPEG_STATUS_SUCCESS, "sync in reverse order");
        CompareCpu(paths[i], outputs[i]->Pixels(), widths[i], heights[i]);
    }
    // A partial asynchronous batch must release its submissions without touching
    // caller outputs, and leave the decoder usable after the failed item.
    Stream good, bad;
    std::ifstream file(first_path, std::ios::binary);
    const std::vector<uint8_t> data(std::istreambuf_iterator<char>{file}, {});
    Status(rocJpegStreamParse(data.data(), data.size(), good.value), ROCJPEG_STATUS_SUCCESS, "parse rollback input");
    const auto header = Header();
    Status(rocJpegStreamParse(header.data(), header.size(), bad.value), ROCJPEG_STATUS_SUCCESS, "parse header-only metadata");
    std::array handles{good.value, bad.value};
    std::array parameters{params, params};
    std::array images{outputs[0]->image, outputs[1]->image};
    for (auto& output : outputs) output->Reset();
    Status(rocJpegDecodeBatchedAsync(decoder.value, handles.data(), 2, parameters.data(), images.data()),
           ROCJPEG_STATUS_BAD_JPEG, "reject missing JPEG tables and roll back batch");
    for (auto& output : outputs) {
        const auto pixels = output->Pixels();
        Require(std::all_of(pixels.begin(), pixels.end(), [](uint8_t x) { return x == 0xa5; }), "failed async batch wrote output");
    }
    Status(rocJpegDecodeSync(decoder.value, &images[0]), ROCJPEG_STATUS_INVALID_PARAMETER, "rolled-back output is not pending");
    Status(rocJpegDecode(decoder.value, good.value, &params, &outputs[0]->image), ROCJPEG_STATUS_SUCCESS, "decode after rollback");
    CompareCpu(first_path, outputs[0]->Pixels(), widths[0], heights[0]);
    Status(rocJpegDecodeAsync(decoder.value, good.value, &params, &outputs[0]->image), ROCJPEG_STATUS_SUCCESS, "pending work before destruction");
    Status(rocJpegDestroy(decoder.value), ROCJPEG_STATUS_SUCCESS, "destroy decoder with pending work");
    decoder.value = nullptr;
    CheckNoAmf();
}
} // namespace

int wmain(int argc, wchar_t** argv) {
    try {
        ParserAndArguments();
        Decoder decoder;
        for (int i = 1; i < argc; ++i) {
            if (std::wstring(argv[i]) == L"--async-mixed") {
                Require(i + 2 < argc, "--async-mixed needs two JPEG fixtures");
                AsyncDifferentImages(std::filesystem::path(argv[i + 1]), std::filesystem::path(argv[i + 2]));
                i += 2;
            } else if (std::wstring(argv[i]) == L"--unsupported") {
                Require(i + 3 < argc, "--unsupported needs 4:4:4, 4:4:0, and 4:0:0 JPEG fixtures");
                UnsupportedSampling(std::filesystem::path(argv[i + 1]), std::filesystem::path(argv[i + 2]),
                                    std::filesystem::path(argv[i + 3]), decoder);
                i += 3;
            } else {
                DecodeSample(std::filesystem::path(argv[i]), decoder);
            }
        }
        std::cout << "rocJPEG public API: " << checks << " checks passed\n";
        CheckNoAmf();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "rocJPEG API test failed: " << error.what() << '\n';
        return 1;
    }
}
