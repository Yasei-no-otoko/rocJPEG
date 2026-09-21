// SPDX-License-Identifier: MIT
#include "rocjpeg_windows_kernels.hpp"

#include <cmath>
#include <limits>

namespace rocjpeg::windows {
namespace {

struct Region {
    uint32_t left, top, width, height;
};

struct Span {
    uintptr_t begin = 0;
    uintptr_t end = 0;
};

bool PlaneSpan(const void* pointer, size_t pitch, size_t row_bytes,
               uint32_t height, Span& span) {
    if (!pointer || !row_bytes || !height || pitch < row_bytes ||
        height - 1 > (std::numeric_limits<size_t>::max() - row_bytes) / pitch)
        return false;
    const size_t bytes = (height - 1) * pitch + row_bytes;
    const uintptr_t begin = reinterpret_cast<uintptr_t>(pointer);
    if (begin > std::numeric_limits<uintptr_t>::max() - bytes)
        return false;
    span = {begin, begin + bytes};
    return true;
}

bool Overlaps(const Span& a, const Span& b) {
    return a.begin < b.end && b.begin < a.end;
}

RocJpegStatus Validate(const SurfaceView& source, const RocJpegDecodeParams& params,
                       const RocJpegImage& destination, Region& roi) {
    if (!source.width || !source.height || source.width > 65535 || source.height > 65535 ||
        source.chromaSubsampling < ROCJPEG_CSS_444 || source.chromaSubsampling > ROCJPEG_CSS_400 ||
        params.output_format < ROCJPEG_OUTPUT_NATIVE || params.output_format >= ROCJPEG_OUTPUT_FORMAT_MAX)
        return ROCJPEG_STATUS_INVALID_PARAMETER;

    Span input[2];
    unsigned input_count = 1;
    size_t input_row = source.width;
    switch (source.format) {
        case SurfaceFormat::NV12:
            input_count = 2;
            if (!PlaneSpan(source.planes[1], source.pitches[1],
                           2 * ((source.width + 1) / 2), (source.height + 1) / 2, input[1]))
                return ROCJPEG_STATUS_INVALID_PARAMETER;
            break;
        case SurfaceFormat::YUY2:
            input_row = 4 * ((source.width + 1) / 2);
            break;
        case SurfaceFormat::BGRA:
        case SurfaceFormat::RGBA:
            input_row = 4 * source.width;
            break;
        case SurfaceFormat::Y8:
            if (source.chromaSubsampling != ROCJPEG_CSS_400)
                return ROCJPEG_STATUS_INVALID_PARAMETER;
            break;
        default:
            return ROCJPEG_STATUS_INVALID_PARAMETER;
    }
    if (!PlaneSpan(source.planes[0], source.pitches[0], input_row, source.height, input[0]))
        return ROCJPEG_STATUS_INVALID_PARAMETER;

    const auto& crop = params.crop_rectangle;
    roi = {0, 0, source.width, source.height};
    if (crop.left || crop.top || crop.right || crop.bottom) {
        if (crop.left < 0 || crop.top < 0 || crop.right <= crop.left || crop.bottom <= crop.top ||
            static_cast<uint32_t>(crop.right) > source.width ||
            static_cast<uint32_t>(crop.bottom) > source.height)
            return ROCJPEG_STATUS_INVALID_PARAMETER;
        roi = {static_cast<uint32_t>(crop.left), static_cast<uint32_t>(crop.top),
               static_cast<uint32_t>(crop.right - crop.left), static_cast<uint32_t>(crop.bottom - crop.top)};
    }
    const auto& target = params.target_dimension;
    if ((target.width == 0) != (target.height == 0))
        return ROCJPEG_STATUS_INVALID_PARAMETER;
    if (target.width && (target.width != roi.width || target.height != roi.height))
        return ROCJPEG_STATUS_IMPLEMENTATION_NOT_SUPPORTED;

    unsigned output_count = 1;
    size_t row_bytes[3] = {roi.width, 0, 0};
    uint32_t rows[3] = {roi.height, roi.height, roi.height};
    switch (params.output_format) {
        case ROCJPEG_OUTPUT_RGB:
            row_bytes[0] = 3 * roi.width;
            break;
        case ROCJPEG_OUTPUT_RGB_PLANAR:
            output_count = 3;
            row_bytes[1] = row_bytes[2] = roi.width;
            break;
        case ROCJPEG_OUTPUT_NATIVE:
        case ROCJPEG_OUTPUT_YUV_PLANAR:
            if (source.chromaSubsampling == ROCJPEG_CSS_400)
                break;
            if (source.format == SurfaceFormat::NV12 && source.chromaSubsampling == ROCJPEG_CSS_420) {
                if ((roi.left | roi.top) & 1)
                    return ROCJPEG_STATUS_INVALID_PARAMETER;
                output_count = params.output_format == ROCJPEG_OUTPUT_NATIVE ? 2 : 3;
                row_bytes[1] = params.output_format == ROCJPEG_OUTPUT_NATIVE ? 2 * ((roi.width + 1) / 2)
                                                                          : (roi.width + 1) / 2;
                row_bytes[2] = (roi.width + 1) / 2;
                rows[1] = rows[2] = (roi.height + 1) / 2;
            } else if (source.format == SurfaceFormat::YUY2 && source.chromaSubsampling == ROCJPEG_CSS_422) {
                if (roi.left & 1)
                    return ROCJPEG_STATUS_INVALID_PARAMETER;
                if (params.output_format == ROCJPEG_OUTPUT_NATIVE) {
                    row_bytes[0] = 4 * ((roi.width + 1) / 2);
                } else {
                    output_count = 3;
                    row_bytes[1] = row_bytes[2] = (roi.width + 1) / 2;
                }
            } else {
                return ROCJPEG_STATUS_IMPLEMENTATION_NOT_SUPPORTED;
            }
            break;
        case ROCJPEG_OUTPUT_Y:
            break;
        default:
            return ROCJPEG_STATUS_INVALID_PARAMETER;
    }

    Span output[3];
    for (unsigned i = 0; i < output_count; ++i) {
        if (!PlaneSpan(destination.channel[i], destination.pitch[i], row_bytes[i], rows[i], output[i]))
            return ROCJPEG_STATUS_INVALID_PARAMETER;
        for (unsigned j = 0; j < input_count; ++j)
            if (Overlaps(input[j], output[i]))
                return ROCJPEG_STATUS_INVALID_PARAMETER;
        for (unsigned j = 0; j < i; ++j)
            if (Overlaps(output[j], output[i]))
                return ROCJPEG_STATUS_INVALID_PARAMETER;
    }
    return ROCJPEG_STATUS_SUCCESS;
}

__device__ uint8_t Byte(float value) {
    return static_cast<uint8_t>(fminf(255.0f, fmaxf(0.0f, floorf(value + 0.5f))));
}

__device__ void Read(const SurfaceView& source, uint32_t x, uint32_t y,
                    uint8_t& luma, uint8_t& u, uint8_t& v,
                    uint8_t& r, uint8_t& g, uint8_t& b) {
    const uint8_t* row = source.planes[0] + static_cast<size_t>(y) * source.pitches[0];
    if (source.format == SurfaceFormat::BGRA || source.format == SurfaceFormat::RGBA) {
        const uint8_t* pixel = row + 4 * x;
        r = pixel[source.format == SurfaceFormat::RGBA ? 0 : 2];
        g = pixel[1];
        b = pixel[source.format == SurfaceFormat::RGBA ? 2 : 0];
        luma = Byte(0.299f * r + 0.587f * g + 0.114f * b);
        u = v = 128;
        return;
    }
    u = v = 128;
    if (source.format == SurfaceFormat::YUY2) {
        const uint8_t* pair = row + 4 * (x / 2);
        luma = pair[2 * (x & 1)];
        if (source.chromaSubsampling != ROCJPEG_CSS_400) {
            u = pair[1];
            v = pair[3];
        }
    } else {
        luma = row[x];
        if (source.format == SurfaceFormat::NV12 && source.chromaSubsampling != ROCJPEG_CSS_400) {
            const uint8_t* pair = source.planes[1] + static_cast<size_t>(y / 2) * source.pitches[1] + 2 * (x / 2);
            u = pair[0];
            v = pair[1];
        }
    }
    // JPEG uses full-range YCbCr and the BT.601 coefficients, including at the
    // extrema. Video's limited-range Y offset and 1.164 scale do not apply.
    const float cb = static_cast<float>(u) - 128.0f;
    const float cr = static_cast<float>(v) - 128.0f;
    r = Byte(static_cast<float>(luma) + 1.402f * cr);
    g = Byte(static_cast<float>(luma) - 0.344136f * cb - 0.714136f * cr);
    b = Byte(static_cast<float>(luma) + 1.772f * cb);
}

__global__ void ConvertKernel(SurfaceView source, RocJpegImage destination,
                              Region roi, RocJpegOutputFormat format) {
    const uint32_t x = blockIdx.x * blockDim.x + threadIdx.x;
    const uint32_t y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= roi.width || y >= roi.height)
        return;
    uint8_t luma, u, v, r, g, b;
    Read(source, roi.left + x, roi.top + y, luma, u, v, r, g, b);
    uint8_t* first = destination.channel[0] + static_cast<size_t>(y) * destination.pitch[0];
    if (format == ROCJPEG_OUTPUT_RGB) {
        first[3 * x] = r;
        first[3 * x + 1] = g;
        first[3 * x + 2] = b;
    } else if (format == ROCJPEG_OUTPUT_RGB_PLANAR) {
        first[x] = r;
        destination.channel[1][static_cast<size_t>(y) * destination.pitch[1] + x] = g;
        destination.channel[2][static_cast<size_t>(y) * destination.pitch[2] + x] = b;
    } else if (format == ROCJPEG_OUTPUT_Y || source.chromaSubsampling == ROCJPEG_CSS_400) {
        first[x] = luma;
    } else if (format == ROCJPEG_OUTPUT_NATIVE && source.format == SurfaceFormat::YUY2) {
        if ((x & 1) == 0) {
            const uint8_t* pair = source.planes[0] + static_cast<size_t>(roi.top + y) * source.pitches[0]
                                  + 2 * (roi.left + x);
            first[2 * x] = luma;
            first[2 * x + 1] = u;
            first[2 * x + 2] = x + 1 < roi.width ? pair[2] : luma;
            first[2 * x + 3] = v;
        }
    } else {
        first[x] = luma;
        const bool nv12 = source.format == SurfaceFormat::NV12;
        if ((x & 1) == 0 && (!nv12 || (y & 1) == 0)) {
            const uint32_t cy = nv12 ? y / 2 : y;
            uint8_t* second = destination.channel[1] + static_cast<size_t>(cy) * destination.pitch[1];
            if (format == ROCJPEG_OUTPUT_NATIVE) {
                second[x] = u;
                second[x + 1] = v;
            } else {
                second[x / 2] = u;
                destination.channel[2][static_cast<size_t>(cy) * destination.pitch[2] + x / 2] = v;
            }
        }
    }
}

}  // namespace

RocJpegStatus Convert(const SurfaceView& source, const RocJpegDecodeParams& params,
                      RocJpegImage& destination, hipStream_t stream) {
    Region roi;
    const RocJpegStatus status = Validate(source, params, destination, roi);
    if (status != ROCJPEG_STATUS_SUCCESS)
        return status;
    const dim3 block(16, 16);
    const dim3 grid((roi.width + block.x - 1) / block.x, (roi.height + block.y - 1) / block.y);
    hipLaunchKernelGGL(ConvertKernel, grid, block, 0, stream, source, destination, roi, params.output_format);
    return hipGetLastError() == hipSuccess ? ROCJPEG_STATUS_SUCCESS : ROCJPEG_STATUS_EXECUTION_FAILED;
}

}  // namespace rocjpeg::windows
