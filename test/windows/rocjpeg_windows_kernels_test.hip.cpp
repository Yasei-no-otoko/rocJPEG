// SPDX-License-Identifier: MIT
#include "rocjpeg_windows_kernels.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

using rocjpeg::windows::Convert;
using rocjpeg::windows::SurfaceFormat;
using rocjpeg::windows::SurfaceView;

namespace {
constexpr size_t kGuard = 19;
constexpr uint8_t kUntouched = 205;

void Hip(hipError_t result) {
    if (result != hipSuccess)
        throw std::runtime_error(hipGetErrorString(result));
}

void Require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}

struct Buffer {
    uint8_t* allocation = nullptr;
    std::vector<uint8_t> host;
    explicit Buffer(size_t bytes) : host(bytes + 2 * kGuard, kUntouched) {
        Hip(hipMalloc(&allocation, host.size()));
    }
    ~Buffer() { if (allocation) (void)hipFree(allocation); }
    Buffer(const Buffer&) = delete;
    Buffer& operator=(const Buffer&) = delete;
    uint8_t* device() const { return allocation + kGuard; }
    uint8_t* data() { return host.data() + kGuard; }
    void Upload() { Hip(hipMemcpy(allocation, host.data(), host.size(), hipMemcpyHostToDevice)); }
    std::vector<uint8_t> Download() const {
        std::vector<uint8_t> result(host.size());
        Hip(hipMemcpy(result.data(), allocation, result.size(), hipMemcpyDeviceToHost));
        return result;
    }
};

uint8_t Round(double value) {
    return static_cast<uint8_t>(std::clamp(std::lround(value), 0L, 255L));
}

struct Pixel { uint8_t y, u, v, r, g, b; };

Pixel Reference(SurfaceFormat format, RocJpegChromaSubsampling css, unsigned x, unsigned y) {
    Pixel p{};
    if (format == SurfaceFormat::RGBA || format == SurfaceFormat::BGRA) {
        p.r = static_cast<uint8_t>(43 * x + 11 * y + 1);
        p.g = static_cast<uint8_t>(13 * x + 71 * y + 127);
        p.b = static_cast<uint8_t>(97 * x + 7 * y + 255);
        p.y = Round(0.299 * p.r + 0.587 * p.g + 0.114 * p.b);
        return p;
    }
    p.y = static_cast<uint8_t>(17 * x + 43 * y);
    const unsigned cy = format == SurfaceFormat::NV12 ? y / 2 : y;
    p.u = css == ROCJPEG_CSS_400 ? 128 : static_cast<uint8_t>(29 * (x / 2) + 71 * cy);
    p.v = css == ROCJPEG_CSS_400 ? 128 : static_cast<uint8_t>(97 * (x / 2) + 11 * cy + 255);
    p.r = Round(p.y + 1.402 * (static_cast<int>(p.v) - 128));
    p.g = Round(p.y - 0.344136 * (static_cast<int>(p.u) - 128) - 0.714136 * (static_cast<int>(p.v) - 128));
    p.b = Round(p.y + 1.772 * (static_cast<int>(p.u) - 128));
    return p;
}

struct Input {
    SurfaceView view;
    Buffer first;
    Buffer second;
    Input(SurfaceFormat format, RocJpegChromaSubsampling css, unsigned width, unsigned height, unsigned padding)
        : view{format, {}, {}, width, height, css},
          first((format == SurfaceFormat::YUY2 ? 4 * ((width + 1) / 2) :
                 format == SurfaceFormat::RGBA || format == SurfaceFormat::BGRA ? 4 * width : width) * height + padding * height),
          second((2 * ((width + 1) / 2) + padding) * ((height + 1) / 2)) {
        view.pitches[0] = (first.host.size() - 2 * kGuard) / height;
        view.pitches[1] = 2 * ((width + 1) / 2) + padding;
        view.planes[0] = first.device();
        view.planes[1] = second.device();
        for (unsigned y = 0; y < height; ++y) {
            for (unsigned x = 0; x < width; ++x) {
                const Pixel p = Reference(format, css, x, y);
                uint8_t* row = first.data() + y * view.pitches[0];
                if (format == SurfaceFormat::BGRA || format == SurfaceFormat::RGBA) {
                    row[4 * x] = format == SurfaceFormat::RGBA ? p.r : p.b;
                    row[4 * x + 1] = p.g;
                    row[4 * x + 2] = format == SurfaceFormat::RGBA ? p.b : p.r;
                    row[4 * x + 3] = 37;
                } else if (format == SurfaceFormat::YUY2) {
                    row[2 * x] = p.y;
                    if ((x & 1) == 0) {
                        const auto chroma = Reference(format, ROCJPEG_CSS_422, x, y);
                        row[2 * x + 1] = chroma.u;
                        row[2 * x + 3] = chroma.v;
                        if (x + 1 == width) row[2 * x + 2] = p.y;
                    }
                } else {
                    row[x] = p.y;
                }
                if (format == SurfaceFormat::NV12 && (x & 1) == 0 && (y & 1) == 0) {
                    uint8_t* uv = second.data() + (y / 2) * view.pitches[1] + x;
                    const auto chroma = Reference(format, ROCJPEG_CSS_420, x, y);
                    uv[0] = chroma.u;
                    uv[1] = chroma.v;
                }
            }
        }
        first.Upload();
        second.Upload();
    }
};

void CheckCase(Input& input, RocJpegOutputFormat format, unsigned crop, unsigned padding, hipStream_t stream) {
    const unsigned left = crop == 2 ? 1 : crop ? 2 : 0;
    const unsigned top = crop == 2 ? 1 : crop ? 2 : 0;
    const unsigned width = input.view.width - left;
    const unsigned height = input.view.height - top;
    const bool gray = input.view.chromaSubsampling == ROCJPEG_CSS_400;
    const bool planar = format == ROCJPEG_OUTPUT_RGB_PLANAR || (format == ROCJPEG_OUTPUT_YUV_PLANAR && !gray);
    const bool nv12 = input.view.format == SurfaceFormat::NV12;
    std::array<unsigned, 3> columns{width, 0, 0}, rows{height, 0, 0};
    if (format == ROCJPEG_OUTPUT_RGB) columns[0] *= 3;
    if (format == ROCJPEG_OUTPUT_RGB_PLANAR) {
        columns.fill(width);
        rows.fill(height);
    } else if (format == ROCJPEG_OUTPUT_YUV_PLANAR && !gray) {
        columns[1] = columns[2] = (width + 1) / 2;
        rows[1] = rows[2] = nv12 ? (height + 1) / 2 : height;
    } else if (format == ROCJPEG_OUTPUT_NATIVE && !gray) {
        if (nv12) {
            columns[1] = 2 * ((width + 1) / 2);
            rows[1] = (height + 1) / 2;
        } else {
            columns[0] = 4 * ((width + 1) / 2);
        }
    }
    std::array<unsigned, 3> pitch{columns[0] + padding,
                                  columns[1] + padding + (padding ? 3U : 0U),
                                  columns[2] + padding + (padding ? 7U : 0U)};
    Buffer first(pitch[0] * rows[0]), second(pitch[1] * rows[1]), third(pitch[2] * rows[2]);
    std::array<Buffer*, 3> outputs{&first, &second, &third};
    RocJpegImage destination{};
    std::array<std::vector<uint8_t>, 3> expected{first.host, second.host, third.host};
    std::array<std::vector<bool>, 3> written;
    for (unsigned c = 0; c < 3; ++c) {
        outputs[c]->Upload();
        destination.channel[c] = outputs[c]->device();
        destination.pitch[c] = pitch[c];
        written[c].resize(expected[c].size());
    }
    auto store = [&](unsigned c, unsigned x, unsigned y, uint8_t value) {
        const size_t offset = kGuard + y * pitch[c] + x;
        expected[c].at(offset) = value;
        written[c].at(offset) = true;
    };
    for (unsigned y = 0; y < height; ++y) {
        for (unsigned x = 0; x < width; ++x) {
            const Pixel p = Reference(input.view.format, input.view.chromaSubsampling, left + x, top + y);
            if (format == ROCJPEG_OUTPUT_RGB) {
                store(0, 3 * x, y, p.r); store(0, 3 * x + 1, y, p.g); store(0, 3 * x + 2, y, p.b);
            } else if (format == ROCJPEG_OUTPUT_RGB_PLANAR) {
                store(0, x, y, p.r); store(1, x, y, p.g); store(2, x, y, p.b);
            } else if (format == ROCJPEG_OUTPUT_Y || gray) {
                store(0, x, y, p.y);
            } else if (format == ROCJPEG_OUTPUT_NATIVE && !nv12) {
                if (!(x & 1)) {
                    store(0, 2 * x, y, p.y); store(0, 2 * x + 1, y, p.u);
                    store(0, 2 * x + 2, y, x + 1 < width ? Reference(input.view.format, input.view.chromaSubsampling, left + x + 1, top + y).y : p.y);
                    store(0, 2 * x + 3, y, p.v);
                }
            } else {
                store(0, x, y, p.y);
                if (!(x & 1) && (!nv12 || !(y & 1))) {
                    const unsigned cy = nv12 ? y / 2 : y;
                    if (planar) {
                        store(1, x / 2, cy, p.u); store(2, x / 2, cy, p.v);
                    } else {
                        store(1, x, cy, p.u); store(1, x + 1, cy, p.v);
                    }
                }
            }
        }
    }
    RocJpegDecodeParams params{};
    params.output_format = format;
    if (crop) params.crop_rectangle = {static_cast<int16_t>(left), static_cast<int16_t>(top),
                                      static_cast<int16_t>(input.view.width), static_cast<int16_t>(input.view.height)};
    if (padding) params.target_dimension = {width, height}; // Explicit no-op resize is valid.
    Require(Convert(input.view, params, destination, stream) == ROCJPEG_STATUS_SUCCESS, "valid conversion rejected");
    Hip(hipStreamSynchronize(stream));
    const bool rounded = format == ROCJPEG_OUTPUT_RGB || format == ROCJPEG_OUTPUT_RGB_PLANAR ||
                         input.view.format == SurfaceFormat::BGRA || input.view.format == SurfaceFormat::RGBA;
    for (unsigned c = 0; c < 3; ++c) {
        const auto actual = outputs[c]->Download();
        for (size_t i = 0; i < actual.size(); ++i) {
            const int tolerance = rounded && written[c][i] ? 1 : 0;
            if (std::abs(static_cast<int>(actual[i]) - expected[c][i]) > tolerance)
                throw std::runtime_error("pixel or padding mismatch: input=" + std::to_string(static_cast<int>(input.view.format)) +
                                         " output=" + std::to_string(format) + " channel=" + std::to_string(c) +
                                         " offset=" + std::to_string(i));
        }
    }
    Require(input.first.Download() == input.first.host && input.second.Download() == input.second.host,
            "conversion modified its source");
}

void CheckInvalid(hipStream_t stream) {
    Input input(SurfaceFormat::NV12, ROCJPEG_CSS_420, 17, 9, 7);
    Buffer output(64 * 9);
    output.Upload();
    RocJpegImage destination{};
    destination.channel[0] = output.device();
    destination.pitch[0] = 64;
    RocJpegDecodeParams params{};
    params.output_format = ROCJPEG_OUTPUT_RGB;
    auto reject = [&](SurfaceView view, RocJpegDecodeParams parameters, RocJpegImage image, RocJpegStatus expected) {
        Require(Convert(view, parameters, image, stream) == expected, "wrong validation status");
    };
    auto view = input.view;
    view.width = 0;
    reject(view, params, destination, ROCJPEG_STATUS_INVALID_PARAMETER);
    view = input.view; view.planes[1] = nullptr;
    reject(view, params, destination, ROCJPEG_STATUS_INVALID_PARAMETER);
    view = input.view; view.pitches[1] = 17;
    reject(view, params, destination, ROCJPEG_STATUS_INVALID_PARAMETER);
    view = input.view; view.pitches[0] = std::numeric_limits<size_t>::max();
    reject(view, params, destination, ROCJPEG_STATUS_INVALID_PARAMETER);
    auto image = destination; image.channel[0] = nullptr;
    reject(input.view, params, image, ROCJPEG_STATUS_INVALID_PARAMETER);
    image = destination; image.pitch[0] = 50;
    reject(input.view, params, image, ROCJPEG_STATUS_INVALID_PARAMETER);
    image = destination; image.channel[0] = const_cast<uint8_t*>(input.view.planes[0]);
    reject(input.view, params, image, ROCJPEG_STATUS_INVALID_PARAMETER);
    auto p = params; p.crop_rectangle = {-1, 0, 4, 4};
    reject(input.view, p, destination, ROCJPEG_STATUS_INVALID_PARAMETER);
    p = params; p.crop_rectangle = {1, 1, 18, 4};
    reject(input.view, p, destination, ROCJPEG_STATUS_INVALID_PARAMETER);
    p = params; p.crop_rectangle = {2, 2, 2, 4};
    reject(input.view, p, destination, ROCJPEG_STATUS_INVALID_PARAMETER);
    p = params; p.target_dimension = {8, 4};
    reject(input.view, p, destination, ROCJPEG_STATUS_IMPLEMENTATION_NOT_SUPPORTED);
    p = params; p.target_dimension = {17, 0};
    reject(input.view, p, destination, ROCJPEG_STATUS_INVALID_PARAMETER);
    p = params; p.output_format = ROCJPEG_OUTPUT_YUV_PLANAR; p.crop_rectangle = {1, 0, 17, 9};
    reject(input.view, p, destination, ROCJPEG_STATUS_INVALID_PARAMETER);
    p = params; p.output_format = ROCJPEG_OUTPUT_RGB_PLANAR;
    reject(input.view, p, destination, ROCJPEG_STATUS_INVALID_PARAMETER);
    image = destination; image.channel[1] = image.channel[2] = image.channel[0];
    image.pitch[1] = image.pitch[2] = image.pitch[0];
    reject(input.view, p, image, ROCJPEG_STATUS_INVALID_PARAMETER);
    p = params; p.output_format = ROCJPEG_OUTPUT_NATIVE;
    view = input.view; view.chromaSubsampling = ROCJPEG_CSS_444;
    reject(view, p, destination, ROCJPEG_STATUS_IMPLEMENTATION_NOT_SUPPORTED);
    Hip(hipStreamSynchronize(stream));
    Require(output.Download() == output.host, "invalid request wrote output");
}
}  // namespace

int main() {
    hipStream_t stream = nullptr;
    try {
        Hip(hipSetDevice(0));
        hipDeviceProp_t device{};
        Hip(hipGetDeviceProperties(&device, 0));
        std::printf("GPU: %s (%s)\n", device.name, device.gcnArchName);
        Hip(hipStreamCreateWithFlags(&stream, hipStreamNonBlocking));
        unsigned cases = 0;
        for (const auto format : {SurfaceFormat::NV12, SurfaceFormat::YUY2, SurfaceFormat::Y8, SurfaceFormat::BGRA, SurfaceFormat::RGBA}) {
            const auto css = format == SurfaceFormat::NV12 ? ROCJPEG_CSS_420 : format == SurfaceFormat::YUY2 ? ROCJPEG_CSS_422 :
                             format == SurfaceFormat::Y8 ? ROCJPEG_CSS_400 : ROCJPEG_CSS_444;
            for (const auto size : {std::array<unsigned, 2>{1, 1}, {3, 5}, {17, 9}, {32, 8}}) {
                for (const unsigned padding : {0U, 13U}) {
                    Input input(format, css, size[0], size[1], padding);
                    for (int output = ROCJPEG_OUTPUT_NATIVE; output < ROCJPEG_OUTPUT_FORMAT_MAX; ++output) {
                        if ((format == SurfaceFormat::BGRA || format == SurfaceFormat::RGBA) && output < ROCJPEG_OUTPUT_Y)
                            continue;
                        for (const unsigned crop : {0U, 1U, 2U}) {
                            if (crop && (size[0] < 3 || size[1] < 3)) continue;
                            if (crop == 2 && css != ROCJPEG_CSS_400 && output < ROCJPEG_OUTPUT_Y) continue;
                            CheckCase(input, static_cast<RocJpegOutputFormat>(output), crop, padding, stream);
                            ++cases;
                        }
                    }
                }
            }
        }
        for (const auto format : {SurfaceFormat::NV12, SurfaceFormat::YUY2}) {
            Input input(format, ROCJPEG_CSS_400, 17, 9, 13);
            for (int output = ROCJPEG_OUTPUT_NATIVE; output < ROCJPEG_OUTPUT_FORMAT_MAX; ++output) {
                CheckCase(input, static_cast<RocJpegOutputFormat>(output), 2, 13, stream);
                ++cases;
            }
        }
        CheckInvalid(stream);
        Hip(hipStreamDestroy(stream));
        stream = nullptr;
        std::printf("PASS: %u GPU conversions, guards/padding/source preservation, invalid inputs\n", cases);
        return 0;
    } catch (const std::exception& error) {
        if (stream) (void)hipStreamDestroy(stream);
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
}
