# Native Windows JPEG decoding

The `codex/windows-d3d11` branch replaces this fork's AMF backend with direct
D3D11 video decoding. The previous AMF implementation remains available on
`codex/windows-amf`. Neither building nor running the native backend uses AMF.
The Linux VA-API build is unchanged.

## AMD driver contract

The decoder selects the AMD adapter by the HIP device LUID, enumerates video
profiles, and requires `D1C20509-AE7B-4E72-AE3B-49F88D58992F`
(`DXVA2_MJPEG_VLD_AMD`). It uses the driver's returned unencrypted configuration
with `ConfigBitstreamRaw == 1` and verifies each requested output format and
dimension through D3D11 before creating a decoder.

This proprietary profile uses the 44-byte `DXVA_PictureParameters` structure,
not Windows 11's 64-byte `DXVA_PicParams_MJPEG`. The implementation zeroes the
entire structure and sets:

| Field | Value |
| --- | --- |
| `wPicWidthInMBminus1` | JPEG width in **pixels**, minus one |
| `wPicHeightInMBminus1` | JPEG height in **pixels**, minus one |
| `bPicStructure` | 3 (frame) |
| `bPicIntra` | 1 |

Despite their names, the size fields are not macroblock counts for this profile.
Omitting the frame flag produced an incorrect field/row layout in validation.
The bitstream buffer contains the complete JPEG, including SOI, tables, SOS,
entropy data, and EOI. No separate quantizer or Huffman decoder buffers are
submitted. The host validates table lengths/selectors and Huffman code counts
before allowing hardware submission; metadata-only streams cannot be decoded.

Decoder surfaces are padded to multiples of 64 pixels and use
`D3D11_BIND_DECODER | D3D11_BIND_SHADER_RESOURCE` with a shared resource.
The unpadded dimensions are passed to the picture parameters and HIP output
conversion. 4:2:0 uses NV12; 4:2:2 uses YUY2. 4:4:4, 4:4:0, grayscale,
progressive, and CMYK inputs return a fallback status without GPU output writes.
Grayscale is deliberately rejected: pixel comparison found an incorrect driver
row layout in both native and AMF decoding, even when submission returned S_OK.

This ABI was established by observing the working decoder's D3D11 submissions
and validating an independent implementation against CPU JPEG output. It is an
observed AMD driver contract, not a published guarantee for every AMD driver.
The newer Microsoft JPEG/MJPEG profiles were not exposed by the tested adapter.

## GPU ownership and completion

Decoded pixels remain on the GPU. A D3D11 shared texture is opened on the same
adapter's D3D12 device, copied into a linear shared D3D12 buffer, imported through
HIP external memory, and converted by HIP kernels. D3D11/D3D12 shared fences and
HIP external semaphore waits order the copies. No decoded-pixel CPU readback is
used by the library; only validation reads output pixels back.

Synchronous calls reuse decoder and transfer resources. Each asynchronous
submission owns a separate decoder/output surface, so input streams may be
destroyed and different images may be completed in reverse order. Destination
storage must remain alive until sync. Partial batch failure drains and removes
the batch's submitted jobs. Destruction waits for pending video work.

## Validation

Validated on Windows 11 build 26200, Radeon 8060S (`gfx1151`), AMD display driver
32.0.31041.1004, ROCm SDK 10.2 / HIP 7.16.26373, AMD clang 24, and Windows SDK
10.0.26100.0. Build with at least 32 jobs as shown in the README.

The CTest suite checks all 13 public APIs, 406 HIP conversion cases, guarded
output pitches, malformed input, frame reuse, format/size changes, async source
lifetime, reverse-order completion, batch rollback, and pending destruction.
Hardware RGB is compared with WIC's CPU JPEG decoder. The 3840×2160 fixtures
measured mean/max byte errors of 0.080859/9 (4:2:0) and 0.019099/7 (4:2:2).
Loaded-module assertions reject any module whose name starts with `amf`.

The small baseline fixtures are generated gradients, encoded by Pillow at
quality 95 with optimized Huffman tables and a restart interval of seven MCUs.
`baseline_420_odd.jpg` is 97×65, `baseline_422_odd.jpg` is 65×97, and
`baseline_400.jpg` is 97×65 grayscale. Their RGB generator is
`(32 + 160*x/(w-1), 48 + 128*y/(h-1), 80 + 24*(x+y)/(w+h-2))`, with integer
division. Grayscale uses Pillow's RGB-to-L conversion. The older 4:4:4/4:4:0
fixtures are described in the API test.

References:

* [Microsoft DXVA_PictureParameters](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/dxva/ns-dxva-_dxva_pictureparameters)
* [Microsoft's newer DXVA_PicParams_MJPEG](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/dxva/ns-dxva-dxva_picparams_mjpeg)
* [AMD profile GUID in MPC-BE](https://github.com/Aleksoid1978/MPC-BE/blob/6a6316411f8b12d42f22028e43cc137db6bae64d/include/dxva2_guids.h#L44-L45)
