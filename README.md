[![MIT licensed](https://img.shields.io/badge/license-MIT-blue.svg)](https://opensource.org/licenses/MIT)

<p align="center"><img width="70%" src="docs/data/AMD_rocJPEG_Logo.png" /></p>

rocJPEG is a high performance JPEG decode SDK for AMD GPUs. Using the rocJPEG API, you can access the JPEG decoding features available on your GPU.

> [!NOTE]
> The published documentation is available at [rocJPEG](https://rocm.docs.amd.com/projects/rocJPEG/en/latest/) in an organized, easy-to-read format, with search and a table of contents. The documentation source files reside in `docs` in this fork. As with all ROCm projects, the documentation is open source. For more information on contributing to the documentation, see [Contribute to ROCm documentation](https://rocm.docs.amd.com/en/latest/contribute/contributing.html).

## Supported JPEG chroma subsampling

* YUV 4:4:4
* YUV 4:4:0
* YUV 4:2:2
* YUV 4:2:0
* YUV 4:0:0

This Windows AMF fork tracks `projects/rocjpeg` from [ROCm/rocm-systems](https://github.com/ROCm/rocm-systems/tree/73e42c4112d08e05170340f3fd2b5e291c2d4957/projects/rocjpeg), commit `73e42c4112d08e05170340f3fd2b5e291c2d4957` (rocJPEG 1.10.0).

## Windows AMF backend (this fork)

The Windows backend uses the AMD display driver's AMF JPEG decoder and HIP
conversion kernels. It requires a Windows ROCm SDK with `clang++.exe`, the Visual
Studio C++ build tools, Windows SDK, CMake 3.24 or newer, and Ninja. The AMF runtime
comes from the installed AMD driver and is not bundled by this project.

From a Visual Studio developer PowerShell, select your SDK and GPU architecture:

```powershell
$rocm = 'C:/path/to/_rocm_sdk_core'
cmake -S . -B build/windows -G 'Ninja Multi-Config' `
  "-DROCM_PATH=$rocm" -DGPU_TARGETS=gfx1151 -DROCJPEG_BUILD_WINDOWS_TESTS=ON
cmake --build build/windows --config Release --parallel 32
ctest --test-dir build/windows -C Release --output-on-failure
cmake --install build/windows --config Release --prefix build/install
```

The build fetches AMF headers from pinned official commit
`d0b3e6dd544a5f207bb6a12a1ecb98532491176a`. Set `-DAMF_ROOT=C:/path/to/AMF`
to use an existing checkout. Split ROCm wheel installations automatically use
the sibling `_rocm_sdk_devel` headers; `ROCJPEG_ROCM_DEVEL_PATH` overrides that
location. `GPU_TARGETS` accepts a semicolon-separated architecture list and
defaults to `native`.

Consumers can use `add_subdirectory`/FetchContent or the installed package:

```cmake
find_package(rocjpeg CONFIG REQUIRED)
target_link_libraries(my_application PRIVATE rocjpeg::rocjpeg)
```

For the installed package, provide `ROCM_PATH` so its public HIP headers can be
found, and stage the selected SDK's HIP runtime beside your executable. The
build stages these DLLs for its own binaries; the install tree contains the
rocJPEG DLL, import library, headers, and licenses. A different HIP DLL in
Windows System32 can otherwise take precedence over `PATH`.

### Windows support and validation

The Windows backend implements all 13 public APIs from rocJPEG 1.10.0,
including `rocJpegDecodeAsync`/`rocJpegDecodeSync` and their batched variants.
Async submission returns before output pixels are copied; the matching sync
call completes the decode. Input streams own their compressed bytes, and
submitted async work retains its input even if the stream is subsequently
destroyed. Output buffers and destination structures must remain alive until
sync completes. Finish any caller-side asynchronous writes to these buffers
before submitting them to rocJPEG.

Validated on Windows 11, Radeon 8060S (`gfx1151`), ROCm SDK 10.2.0 / HIP
7.16.26373, and AMD clang 24:

* Baseline 8-bit JPEG 4:2:0, 4:2:2, and grayscale 4:0:0 hardware decoding.
* Real images at 3840×2160 and 979×546, repeated and batched decoding, and
  changes in image dimensions and subsampling on one decoder.
* All 13 API entry points, malformed and truncated input against an inaccessible
  guard page, async input lifetime, destination pitches, and overwrite guards.
* 406 GPU conversion cases covering NV12, YUY2, Y8, BGRA, and RGBA sources;
  native, planar YUV, luma, RGB, and planar RGB outputs; crops and odd sizes.
* Installed-package `find_package(rocjpeg CONFIG REQUIRED)` compilation,
  linking, and execution.

Progressive JPEG, CMYK, arithmetic coding, and unsupported sampling return
`ROCJPEG_STATUS_JPEG_NOT_SUPPORTED`; this backend does not silently decode on
the CPU. Applications can use that status to choose their CPU fallback.
Windows 4:4:4 and 4:4:0 image metadata can be parsed, but decoding returns
`ROCJPEG_STATUS_JPEG_NOT_SUPPORTED` before entering AMF. The tested AMD driver
crashed when initializing its BGRA decode path for a real 4:4:4 JPEG; callers
must use a fallback for these formats. The regression test includes generated
97×65 baseline 4:4:4 and 65×97 4:4:0 JPEGs and checks synchronous, asynchronous, and batched
rejection without modifying output. Resizing is not implemented by this backend;
nonzero target dimensions must match the selected crop. Hybrid backend
creation returns `ROCJPEG_STATUS_NOT_IMPLEMENTED`.

## Prerequisites

### Hardware
* **GPU**: [AMD Radeon&trade; Graphics](https://rocm.docs.amd.com/projects/install-on-linux/en/latest/reference/system-requirements.html) / [AMD Instinct&trade; Accelerators](https://rocm.docs.amd.com/projects/install-on-linux/en/latest/reference/system-requirements.html)

> [!IMPORTANT]
> `gfx908` or higher GPU required

### ROCm via TheRock

rocJPEG is built and installed as part of [TheRock](https://github.com/ROCm/TheRock). All core dependencies are provided by the TheRock build, including:

* HIP runtime and development libraries
* AMD Clang++ compiler (C++17 required)
* Libva and VA-API drivers
* Libdrm (amdgpu)
* CMake and pkg-config

## Build and install

rocJPEG is built as part of [TheRock](https://github.com/ROCm/TheRock). To build standalone from source:

```shell
git clone https://github.com/ROCm/rocm-systems.git
cd rocm-systems/projects/rocjpeg
mkdir build && cd build
cmake ../
make -j8
sudo make install
```

### Run tests

  ```shell
  make test
  ```

  > [!NOTE]
  > To run tests with verbose option, use `make test ARGS="-VV"`.

## Verify installation

After installation, the following files are available:

* Libraries in `/opt/rocm/lib`
* Header files in `/opt/rocm/include/rocjpeg`
* Samples in `/opt/rocm/share/rocjpeg`
* Documents in `/opt/rocm/share/doc/rocjpeg`

### Using sample application

To verify your installation using a sample application, run:

```shell
mkdir rocjpeg-sample && cd rocjpeg-sample
cmake /opt/rocm/share/rocjpeg/samples/jpegDecode/
make -j8
./jpegdecode -i /opt/rocm/share/rocjpeg/images/mug_420.jpg
```

### Using CTest

To verify your installation using CTest, run:

```shell
mkdir rocjpeg-test && cd rocjpeg-test
cmake /opt/rocm/share/rocjpeg/test/
ctest -VV
```

## Samples

You can access samples to decode your images in the
[samples](https://github.com/ROCm/rocm-systems/tree/develop/projects/rocjpeg/samples) directory. Refer to the
individual folders to build and run the samples.

## Tested configurations

* Linux
  * Ubuntu - `22.04` / `24.04`
* [TheRock](https://github.com/ROCm/TheRock) - `7.12` or later
