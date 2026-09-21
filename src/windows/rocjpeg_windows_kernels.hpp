// SPDX-License-Identifier: MIT
#pragma once

#include "rocjpeg/rocjpeg.h"

#include <cstddef>
#include <cstdint>

namespace rocjpeg::windows {

enum class SurfaceFormat { NV12, YUY2, Y8, BGRA, RGBA };

// Plane pointers address GPU-accessible linear buffers, not texture objects.
// Dimensions exclude decoder padding; pitches are bytes. NV12 and YUY2 retain
// a complete chroma pair at an odd right edge (2*ceil(width/2) and
// 4*ceil(width/2) bytes per chroma/packed row, respectively).
struct SurfaceView {
    SurfaceFormat format = SurfaceFormat::NV12;
    const uint8_t* planes[2] = {};
    size_t pitches[2] = {};
    uint32_t width = 0;
    uint32_t height = 0;
    RocJpegChromaSubsampling chromaSubsampling = ROCJPEG_CSS_UNKNOWN;
};

// All-zero crop selects the full image. RGB and Y accept arbitrary valid ROIs;
// subsampled YUV output requires a chroma-aligned origin, with odd extents OK.
// No resizing is performed. Nonzero target dimensions must equal the ROI.
// Conversion is asynchronous; the caller must observe stream completion before
// releasing either surface. The caller owns the buffers and their allocation
// sizes. Source and destination spans must not overlap.
RocJpegStatus Convert(const SurfaceView& source,
                      const RocJpegDecodeParams& params,
                      RocJpegImage& destination,
                      hipStream_t stream);

}  // namespace rocjpeg::windows
