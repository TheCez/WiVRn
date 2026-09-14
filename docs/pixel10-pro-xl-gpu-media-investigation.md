# Pixel 10 Pro XL GPU/Media driver investigation

Permanent technical record of a multi-session investigation into video
corruption and zero-copy encoding on the reference Android-phone-server
device. Written so that (1) another developer can understand what
happened without reading chat history, (2) the PowerVR findings can be
turned into reproducible bug reports for Google/Imagination, and (3)
nobody accidentally "cleans up" the workarounds this produced.

**Read `docs/ANDROID_PORT.md` first** for the overall port status — this
document is the detailed backing record for that file's Milestone 4.5
and 4.6 sections specifically, referenced from there.

## Bug / workaround summary table

| Issue | Layer | Reproducible | Workaround | Status |
|---|---|---|---|---|
| Green chroma / array-layer compute corruption | PowerVR Vulkan driver | YES, deterministic | One dedicated `arrayLayers=1` multi-planar image per stream instead of one shared 3-layer image | **WORKAROUND ACTIVE** |
| `VK_ANDROID_external_format_resolve` silently discards writes | PowerVR Vulkan driver | YES, deterministic | None known — path abandoned | **OPEN DRIVER BUG** |
| `GL_EXT_YUV_target` FBO never becomes valid (`glCheckFramebufferStatus`→0) | PowerVR GLES driver | YES, deterministic | None known — path abandoned | **OPEN / likely related to the bug above** |
| MediaCodec `Surface`-input encoder stall (`instanceQueueCount(1)`) | Pixel media/vendor codec stack (`GC2_EncComp`) | YES, deterministic, both Surface APIs | Use Block Model (`QueueRequest.setHardwareBuffer()`) or plain ByteBuffer input instead of a `Surface` | **WORKAROUND AVAILABLE, not yet adopted in production** |
| `std::bad_alloc` from `inplace_vector<vk::ImageMemoryBarrier2, 3>` overflow | Application (`compositor.cpp`) | YES | Bumped fixed capacity 3→5 | **FIXED** |
| Per-frame descriptor-set rewrite latency regression | Application (`foveation.cpp`/`.h`) | YES (qualitative — see §J) | Only rewrite descriptor bindings that actually changed since last call | **MITIGATED** (user-confirmed improved; not numerically measured) |
| IDR-recovery false-positive livelock causing stereo desync (a *different* cause of green frames, pre-dating the bug above) | Application (`idr_handler.cpp`/`.h`) | YES | Track which frame indices each encoder actually sent; only treat "not sent to decoder" as loss if in that set | **FIXED** |

---

## A. Test environment

**CONFIRMED** (re-verified live on-device while writing this document,
2026-09-14, and cross-checked against `docs/ANDROID_PORT.md`'s existing
records where noted):

| Property | Value | How confirmed |
|---|---|---|
| Device model | Pixel 10 Pro XL | `getprop ro.product.model` |
| Build fingerprint | `google/mustang/mustang:17/CP2A.260805.005/15828068:user/release-keys` | `getprop ro.build.fingerprint` |
| Android version | 17 (API level 37) | `getprop ro.build.version.release` / `.sdk`, cross-checked via `Build.VERSION.SDK_INT` in every Java probe this session |
| SoC | Tensor G5 | `getprop ro.soc.model` |
| `ro.hardware` / `ro.board.platform` | `mustang` / `laguna` | `getprop` |
| Root status | Stock, unrooted, `user` build (`release-keys`) | fingerprint itself; this constrained the entire investigation to public NDK/SDK APIs only |
| GPU | PowerVR D-Series DXT-48-1536 MC1 | `VkPhysicalDeviceProperties::deviceName`, also `GL_RENDERER` |
| GPU vendor ID / device ID | `0x1010` (Imagination Technologies) / `0x71061212` | `VkPhysicalDeviceProperties::vendorID`/`deviceID` |
| GPU device type | `VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU` (1) | `VkPhysicalDeviceProperties::deviceType` |
| Vulkan API version reported by the device | **1.4.317** | `VkPhysicalDeviceProperties::apiVersion`, decoded via `VK_API_VERSION_MAJOR/MINOR/PATCH` |
| Vulkan `driverVersion` (raw) | `6908880` (`0x696bd0`) | `VkPhysicalDeviceProperties::driverVersion` |
| GLES driver build string | `OpenGL ES 3.2 build 25.3@6908880` | `glGetString(GL_VERSION)` — note the driver build number `6908880` matches the Vulkan `driverVersion` above exactly: same underlying driver package for both APIs |
| `pipelineCacheUUID` | `d06b69004702009000bc0056fec5ab77` | `VkPhysicalDeviceProperties::pipelineCacheUUID`, recorded in case it's useful for a bug report |
| Application-requested Vulkan `apiVersion` | 1.3 (`VK_API_VERSION_1_3`) in all probes and in the real server | the probes' own `VkApplicationInfo`; the device accepts this and reports 1.4.317 capability |
| Relevant Vulkan **instance-independent device extensions** (full enumeration, filtered to the relevant subset) | `VK_KHR_dedicated_allocation` (spec 3), `VK_KHR_external_memory` (1), `VK_KHR_external_memory_fd` (1), `VK_KHR_external_fence`/`_fd` (1), `VK_KHR_external_semaphore`/`_fd` (1), `VK_KHR_sampler_ycbcr_conversion` (spec **14**), `VK_KHR_depth_stencil_resolve` (1), `VK_EXT_queue_family_foreign` (1), `VK_EXT_external_memory_dma_buf` (1), `VK_EXT_external_memory_acquire_unmodified` (1), `VK_ANDROID_external_memory_android_hardware_buffer` (spec **5**), `VK_ANDROID_external_format_resolve` (spec **1** — i.e. this is the extension's first-ever revision) | `vkEnumerateDeviceExtensionProperties`, full dump, re-run while writing this doc |
| GLES/EGL extensions confirmed present | `GL_EXT_YUV_target`, `GL_OES_EGL_image_external`, `GL_OES_EGL_image_external_essl3`, `GL_OES_EGL_image`, `EGL_ANDROID_get_native_client_buffer`, `EGL_ANDROID_image_native_buffer`, `EGL_KHR_image_base`, `EGL_KHR_image`, `EGL_ANDROID_native_fence_sync` | `egl_gles_ext_probe.c`, string-searched `eglQueryString(EGL_EXTENSIONS)` / `glGetString(GL_EXTENSIONS)` |
| EGL version | 1.5, vendor "Android" | `eglInitialize` + `eglQueryString(EGL_VENDOR)` |
| Codec components enumerated | `c2.google.avc.encoder` (software/Codec2-generic AVC path used by default), `c2.android.avc.encoder` (the only other AVC encoder component on this device — no distinct vendor-hardware-named component, no legacy OMX path at all) | `MediaCodecList` enumeration during Milestone 4.6 (recorded in `ANDROID_PORT.md`); both showed the identical Surface-input failure |
| Vendor hardware codec component actually doing the work | `GC2_EncComp` (logcat tag) — the real hardware VPU path underneath `c2.google.avc.encoder` | logcat, all Milestone 4.6 probes |
| Resolution used throughout this investigation | 896×960 (matches the real per-eye render target size used by the compositor) | hardcoded in every probe; matches `compositor.cpp`'s real `extent` |
| Monado/server architecture | WiVRn's own compositor (`server/compositor/`), not upstream Monado's own compositor backends — composites directly into the same image that gets encoded, never touches a physical on-screen `Surface` | established in Milestone 4.5, see "Key architecture facts" in `ANDROID_PORT.md` |

**UNKNOWN / NOT RECORDED**:
- The PowerVR driver's own internal version string/codename (only the numeric `driverVersion`/build-number `6908880` was captured; Imagination's public driver release notes, if any exist for this SoC, were not cross-referenced).
- Whether `VK_ANDROID_external_format_resolve`'s bug (§C) or `GL_EXT_YUV_target`'s bug (§D) have been reported by anyone else publicly — no search for existing upstream issues was performed as part of this investigation.
- Exact GPU clock/thermal state during any test run (not considered relevant — every failure was deterministic and reproduced identically across many separate runs, not intermittent).

---

## B. PowerVR Bug #1 — multi-planar image array-layer compute corruption

**Status: WORKAROUND ACTIVE, confirmed fixed live in-headset.**

### Original architecture (before the fix)

```
Monado layer_accum (RGBA)
    -> foveation.comp (compute shader): RGB -> NV12 + foveation resample
    -> ONE VkImage:
         format      = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM
         arrayLayers = 3            (left eye, right eye, alpha, packed as array layers)
         flags       = MUTABLE_FORMAT | EXTENDED_USAGE
         per-plane ImageViews (ePlane0/ePlane1) reinterpreted as R8_UNORM / R8G8_UNORM
           for STORAGE usage, one pair of views per array layer
    -> vkCmdCopyImageToBuffer (per stream, baseArrayLayer = stream_idx)
    -> host-visible staging buffer
    -> memcpy into AMediaCodec's ByteBuffer input
```

### Symptom

- Severe, structured green/chroma corruption in the streamed video, roughly
  the bottom portion of frames uniformly green, top portion correct, a
  consistent cutoff row.
- Fully deterministic — identical failure signature across runs.
- Present **before** MediaCodec: `WIVRN_DUMP_VIDEO` (a pre-existing WiVRn
  debug feature, wired into `wivrn_server_jni.cpp`'s `nativeStart()` for
  this investigation) captures exactly what the server sends to the
  network; decoding that capture independently on a PC (a static `ffmpeg`
  build, since the distro package needed shared libs not on the dev host)
  showed the identical corruption already present in the transmitted
  bitstream — so the encoder, network, and client decoder were **not**
  responsible.

### Ruled out (with how each was ruled out)

| Hypothesis | How ruled out |
|---|---|
| MediaCodec color format / stride / slice-height negotiation | Live `AMediaCodec_getInputFormat()` check right after `start()`: codec negotiates exactly the requested 896×960/896/960/color-format 21, no silent padding |
| `AMEDIAFORMAT_KEY_MAX_INPUT_SIZE` missing | Real, separately-confirmed bug (Codec2 was silently undersizing its input buffer) — fixed, but re-tested directly and the green corruption was byte-identical before and after |
| `foveation.comp`'s `subgroupShuffleDown` chroma-averaging math | Replaced with workgroup shared memory, reverted, retried multiple times; `WIVRN_DUMP_VIDEO` showed byte-identical corruption regardless of which version was active — not the shader's math |
| `foveation::compute_params()`'s math | Independently reviewed and traced against real runtime numbers, found correct |
| Monado's own swapchain output | Captured directly, clean for both eyes |
| The installed Quest client | Byte-identical to the official `WiVRn-release.apk` |
| Synchronization | An explicit `vkQueueWaitIdle()` right after the compute dispatch made zero difference |
| RGBA/BGRA channel confusion, 10-bit format confusion, CPU readback layout | All checked directly against the real captured buffer layout; none matched the observed pattern |

### Decisive finding

A from-scratch, standalone, cross-platform Vulkan reproduction harness
(`ForeverXR/tools/foveation-pc-test/foveation_test.c` — **not part of the
git-tracked port**, see the reproducibility warning in §M) loads the
exact unmodified `foveation.spv` shader binary and the exact real
captured UBO parameters + source image, and replays them against three
configurable image shapes:

- `sep` — two separate `R8_UNORM`/`R8G8_UNORM` images (no multi-planar format at all)
- `mp1` — one multi-planar `VK_FORMAT_G8_B8R8_2PLANE_420_UNORM` image, `arrayLayers=1`
- `mp3` — one multi-planar `VK_FORMAT_G8_B8R8_2PLANE_420_UNORM` image, `arrayLayers=3` (the real production configuration)

| Mode | NVIDIA (desktop) | Mesa llvmpipe (software) | Real Pixel 10 Pro XL |
|---|---|---|---|
| `sep` | clean | clean | clean |
| `mp1` | clean | clean | clean |
| `mp3` | clean | clean | **reproduces the production corruption exactly** |

**Root cause, stated precisely**: on this GPU/driver
(`vendorID=0x1010`, `deviceID=0x71061212`, `driverVersion=6908880`), a
compute shader write (`imageStore`) to array layer ≥1 of a multi-planar
(`VK_FORMAT_G8_B8R8_2PLANE_420_UNORM`) image is corrupted. Neither a
multi-planar image alone (single layer) nor a non-multi-planar array
image triggers it — **only the combination of multi-planar format AND
`arrayLayers≥2` together**, and only on this specific driver (both
tested desktop implementations, one proprietary/NVIDIA and one
open-source software rasterizer/Mesa llvmpipe, are clean in every
configuration including the real 3-layer one).

Minimal reproduction, in pseudocode:

```
BAD (corrupts on this driver):
  VkImage img:
    format      = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM
    arrayLayers = 3
    flags       = MUTABLE_FORMAT | EXTENDED_USAGE
  per-layer plane views (R8_UNORM / R8G8_UNORM), STORAGE usage
  compute shader:
    imageStore(plane0_view_layer0, ...)  -> correct
    imageStore(plane0_view_layer1, ...)  -> CORRUPTED
    imageStore(plane0_view_layer2, ...)  -> CORRUPTED

GOOD (this project's fix):
  three separate VkImage, each:
    format      = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM
    arrayLayers = 1
  compute shader writes each via its own layer-0 view -> all correct
```

### The fix, exactly as applied

**Before**: one `image_allocation` with `arrayLayers=3`, per-eye/alpha
data addressed by array layer index.

**After**: each stream (left eye, right eye, alpha) gets its own
dedicated single-array-layer multi-planar image.

Files and functions changed (commit `c77b7ad5`, "Milestone 4.5: root
cause of the green-chroma corruption found and fixed"):

- **`server/compositor/compositor.h`** — `struct image` restructured:
  now holds `std::array<stream_image, 2> content` (left/right) plus a
  shared `stream_image alpha`, each a `struct stream_image { image_allocation image; vk::raii::ImageView view_y; vk::raii::ImageView view_cbcr; }`,
  instead of one 3-array-layer `image_allocation`. The struct's own
  comment (lines ~49-60) documents the driver bug in place.
- **`server/compositor/compositor.cpp`**:
  - `make_stream_image()` (new helper, line 116) and `make_images()`
    (line 183) build three separate 1-layer images per slot instead of
    one 3-layer image.
  - `compositor::layer_commit()` (line 300): the pre-dispatch barrier
    loop now iterates `{content[0], content[1], alpha}` as three
    separate `vk::ImageMemoryBarrier2` entries instead of one shared
    barrier — this is what overflowed the `inplace_vector` fixed
    capacity (see §J).
  - `stream_vk_image(stream_idx)` lambda (line 467), used by both the
    QFOT (queue-family-ownership-transfer) barrier loop and the
    `present_image()` call site, to resolve which per-stream image a
    given encoder should read from.
  - `compositor::encoder_work()` (line 663): unrelated concurrency
    change from the same investigation (see §J), not the corruption fix
    itself.
- **`server/compositor/shaders/foveation.comp`**: added a push constant
  (`layout(push_constant) uniform PushConstants { int eye; } pc;`)
  carrying which eye is being dispatched, replacing
  `gl_GlobalInvocationID.z`; `imageStore` calls now always target array
  layer 0 (`ivec3(ix, iy, 0)`) of the per-eye image instead of a
  variable layer index; new dedicated `alpha_luma`/`alpha_cbcr` bindings
  (4, 5) for the alpha stream's own separate image.
- **`server/compositor/foveation.h`/`.cpp`**: `foveate()`'s signature
  changed to take separate `y`/`cbcr` view arrays plus separate
  `alpha_y`/`alpha_cbcr` views (no longer one array-layer-indexed view);
  now dispatches once per eye with the push constant set accordingly.
  This is also where the latency mitigation lives (§J).
- **`server/encoder/video_encoder_mediacodec.cpp`**: `present_image()`
  (line 218) — its copy regions' `baseArrayLayer` changed from
  `stream_idx` to a hardcoded `0` (line 240 comment explains why: each
  stream image now has only one layer, so there is nothing else to
  index).

### Confirmation

User-tested live, in-headset, after this fix: the green corruption is
gone. A real crash was found and fixed during this same live test (see
§J), and a real latency regression was found and mitigated in the same
session (see §J); the user separately reported a *different*, still-open
pixelation artifact after this fix (not yet root-caused — see
`docs/ANDROID_PORT.md`'s open items).

### The workaround must not be reverted casually

**DO NOT consolidate these per-stream images back into array layers of
one shared multi-planar image on this driver without re-running
`ForeverXR/tools/foveation-pc-test/foveation_test.c`'s `mp3` mode first.**
This is a deliberate driver workaround for a confirmed hardware/driver
defect, not accidental code duplication. Reverting it would silently
reintroduce the corruption.

---

## C. PowerVR Bug #2 — `VK_ANDROID_external_format_resolve` silently discards writes

**Status: OPEN DRIVER BUG, no known workaround, path abandoned for
production use.**

Found during the Milestone 4.6 zero-copy investigation, "Checkpoint 2,
PATH A". Full narrative also in `docs/ANDROID_PORT.md`'s Milestone 4.6
section; this is the detailed backing record with exact commands/results.

### Exact setup

`AHardwareBuffer` descriptor used:

```
width  = 896
height = 960
layers = 1
format = AHARDWAREBUFFER_FORMAT_Y8Cb8Cr8_420   (= HardwareBuffer.YCBCR_420_888 = 35)
usage  = AHARDWAREBUFFER_USAGE_VIDEO_ENCODE | AHARDWAREBUFFER_USAGE_GPU_COLOR_OUTPUT
```

`vkGetAndroidHardwareBufferPropertiesANDROID()` (chained with
`VkAndroidHardwareBufferFormatPropertiesANDROID`) on this buffer
returned, **exactly**, confirmed live via `vk_ahb_probe.c`:

```
format         = 0  (VK_FORMAT_UNDEFINED)
externalFormat = 0x301
formatFeatures = 0xbad081   (STORAGE=0, TRANSFER_SRC=1, TRANSFER_DST=1, SAMPLED=1)
allocationSize = 1298432
memoryTypeBits = 0x17
```

The device advertises `VK_ANDROID_external_format_resolve`
(`spec=1` — this extension's first-ever revision). Querying its
capabilities (`vk_ext_format_resolve_probe.c`) returned, exactly:

```
VkPhysicalDeviceExternalFormatResolveFeaturesANDROID::externalFormatResolve = VK_TRUE
VkPhysicalDeviceExternalFormatResolvePropertiesANDROID::
    nullColorAttachmentWithExternalFormatResolve = VK_TRUE
    externalFormatResolveChromaOffsetX = 1
    externalFormatResolveChromaOffsetY = 1
VkAndroidHardwareBufferFormatResolvePropertiesANDROID::colorAttachmentFormat
    = 37  (VK_FORMAT_R8G8B8A8_UNORM)
```

I.e., **according to the driver's own capability reporting, this exact
`AHardwareBuffer` is explicitly eligible for external-format-resolve
rendering** — this is not a case of an unsupported/unadvertised feature.

### Both documented resolve configurations were tried

1. **Null color attachment** (using the
   `nullColorAttachmentWithExternalFormatResolve` feature): the main
   `VkRenderingAttachmentInfo.imageView = VK_NULL_HANDLE`, letting the
   driver manage an implicit transient RGBA target internally;
   `resolveImageView` points at the AHB-imported image,
   `resolveMode = VK_RESOLVE_MODE_EXTERNAL_FORMAT_DOWNSAMPLE_BIT_ANDROID`.
2. **Explicit real RGBA attachment**: a real, persistent, device-local
   `VK_FORMAT_R8G8B8A8_UNORM` image as the main attachment, with the same
   `resolveImageView`/`resolveMode` pointing at the AHB-imported image.

Both are implemented in `ForeverXR/tools/foveation-pc-test/vkyuvresolve.c`
(the null-attachment form was the first implementation; the explicit-
attachment form replaced it after the null-attachment form's result was
identical to the bug described below, ruling out the null-attachment
mechanism specifically as the cause).

### The remarkable result

Every single relevant Vulkan call returns `VK_SUCCESS`, for all 60
frames of every test run:

- `vkCreateImage` (the AHB-imported resolve-target image, `VK_FORMAT_UNDEFINED` + `VkExternalFormatANDROID` chained, usage `COLOR_ATTACHMENT_BIT`)
- `vkAllocateMemory` (dedicated import)
- `vkBindImageMemory`
- `vkCreateImageView`
- pipeline creation (dynamic rendering, `VkPipelineRenderingCreateInfo` with `colorAttachmentFormats[0] = VK_FORMAT_R8G8B8A8_UNORM`)
- `vkQueueSubmit`
- `vkWaitForFences` (up to a conservative 3-second timeout, never hit)

MediaCodec's Block Model (`QueueRequest.setHardwareBuffer()`, see §G)
also accepts the resulting `HardwareBuffer` without complaint and
produces 60 real encoded AVC access units, real SPS/PPS
(`onOutputFormatChanged`), and a clean `BUFFER_FLAG_END_OF_STREAM`.

**But the GPU write is silently discarded.** The AHardwareBuffer's real
memory content is never actually modified by the render+resolve
operation, despite every API in the chain reporting success.

### Three independent pieces of evidence

1. **Shader-swap / MD5 test**: the fragment shader was swapped three
   times across otherwise-identical full 60-frame test runs — solid
   quadrants (red/green/blue/white), a per-pixel RGB gradient
   (`vec4(x/896, y/960, 0.5, 1)`), and a flat solid magenta
   (`vec4(1,0,1,1)`). The resulting encoded `.h264` file was
   **byte-for-byte identical across all three runs**:
   ```
   $ md5sum yuv_resolve_test_quadrants.h264 yuv_resolve_test_gradient.h264 yuv_resolve_test_magenta.h264
   46ba31ef6719b2f2cfda2e22c6aed60b  (all three, identical)
   ```
   totalEncodedBytes was also identical (2468 bytes) across all three
   runs.
2. **Shader correctness control test**: the *exact same* fragment shader
   source, rendering into a real, normal, CPU-readable
   `VK_FORMAT_R8G8B8A8_UNORM` image (no AHB, no resolve at all — a plain
   render + `vkCmdCopyImageToBuffer` + `vkMapMemory` readback,
   `vk_render_check.c`), produced the exactly correct per-quadrant RGB
   values:
   ```
   TL(expect RED)    (100,100): R=255 G=0   B=0   A=255
   TR(expect GREEN)  (750,100): R=0   G=255 B=0   A=255
   BL(expect BLUE)   (100,850): R=0   G=0   B=255 A=255
   BR(expect WHITE)  (750,850): R=255 G=255 B=255 A=255
   ```
   This rules out a shader/pipeline mistake as the cause — the shader
   itself is correct.
3. **Direct CPU-lock verification**: a diagnostic `AHardwareBuffer` was
   allocated with an added `AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN` usage
   bit (so it could legally be CPU-locked — the production buffer cannot
   be, `AHardwareBuffer_lockPlanes()` returns `-38`/`ENOSYS` on it
   without that bit). The identical render+resolve call was made against
   it with a solid magenta shader, then the buffer was locked directly
   via `AHardwareBuffer_lockPlanes()` and its Y-plane bytes inspected:
   ```
   nativeDumpY: Y(100,100)=0 Y(750,100)=0 Y(100,850)=0 Y(750,850)=0
   ```
   All zero — magenta should produce a luma value around 105-120
   depending on the color matrix, never 0. The write never landed.

### How to phrase this finding

Call this **"a silent driver failure / advertised-capability-vs-runtime-behavior
mismatch."**

**Do NOT** phrase this as "external format resolve is unsupported" — the
driver explicitly reports `externalFormatResolve = VK_TRUE` and a valid,
non-`UNDEFINED` `colorAttachmentFormat`. The distinction matters for a
future bug report: this is not a missing-feature question, it's a
correctness bug in a feature the driver claims to implement.

### Files / commit / harness

- Probes (not git-tracked — see the reproducibility warning in §M):
  `ForeverXR/tools/foveation-pc-test/vk_ext_format_resolve_probe.c`
  (capability query only), `vkyuvresolve.c` (the render+resolve JNI
  native library) + `YuvResolveTest.java` (the MediaCodec-side driver,
  structurally identical to `BlockModelTest.java`), `vk_render_check.c`
  (the shader-correctness control test), `shaders/quad.vert`/`quad.frag`
  (GLSL source, compiled with the NDK's bundled `glslc`).
- Documented in `docs/ANDROID_PORT.md`, commit `28c448d1` ("docs:
  Milestone 4.6 final conclusion - PATH A/B... both fail at driver
  level").

### Minimal reproduction outline (for a future upstream bug report)

1. Allocate an `AHardwareBuffer`: `896x960`, `YCBCR_420_888`, `layers=1`,
   `usage = VIDEO_ENCODE | GPU_COLOR_OUTPUT`.
2. Confirm `vkGetAndroidHardwareBufferPropertiesANDROID()` reports
   `externalFormat != 0`, `format == VK_FORMAT_UNDEFINED`, and
   `VkAndroidHardwareBufferFormatResolvePropertiesANDROID::colorAttachmentFormat`
   is non-`VK_FORMAT_UNDEFINED` (confirming eligibility).
3. Import the AHB as a `VkImage` (`VK_FORMAT_UNDEFINED` +
   `VkExternalFormatANDROID`, usage `COLOR_ATTACHMENT_BIT`), bind
   dedicated imported memory.
4. Render a solid, unmistakable color (e.g. magenta) via dynamic
   rendering, `resolveMode = VK_RESOLVE_MODE_EXTERNAL_FORMAT_DOWNSAMPLE_BIT_ANDROID`,
   `resolveImageView` = the imported image's view. Wait on a fence.
5. Read back the buffer's actual content — either by re-allocating with
   `CPU_READ_OFTEN` added and using `AHardwareBuffer_lockPlanes()`
   directly, or by handing it to any YUV consumer and inspecting the
   output.
6. **Expected**: buffer contains the rendered magenta content, converted
   to YUV. **Actual**: buffer content is unchanged/zero; no error is
   reported anywhere in the API chain.

---

## D. Related PowerVR GLES/YUV anomaly — `GL_EXT_YUV_target`

**Status: OPEN, likely related to Bug #2's underlying YUV-write driver
path, but recorded separately since it was not proven to share the exact
same root cause.**

Found during Milestone 4.6, "Checkpoint 2, PATH B" — tried as an
alternative to PATH A specifically because it's a GLES-native mechanism,
distinct from Vulkan entirely.

### Setup

`GL_EXT_YUV_target` is advertised, along with every extension required to
use it: `GL_OES_EGL_image_external`, `GL_OES_EGL_image_external_essl3`,
`GL_OES_EGL_image`, `EGL_ANDROID_get_native_client_buffer`,
`EGL_ANDROID_image_native_buffer`, `EGL_KHR_image_base` (all confirmed
present via `egl_gles_ext_probe.c`, listed exactly in §A).

The same `AHardwareBuffer` shape as Bug #2 (`YCBCR_420_888`, `layers=1`,
`VIDEO_ENCODE | GPU_COLOR_OUTPUT`) bridges to EGL cleanly:

- `AHardwareBuffer` → `EGLClientBuffer`: **success**
  (`eglGetNativeClientBufferANDROID`)
- `EGLClientBuffer` → `EGLImage`: **success**
  (`eglCreateImageKHR(EGL_NATIVE_BUFFER_ANDROID, ...)`, `eglGetError()`
  returns `EGL_SUCCESS`/`0x3000`)

### What was tried

Two distinct, independently-valid mechanisms for using an `EGLImage` as
a *write* (render) target, both implemented in
`ForeverXR/tools/foveation-pc-test/glyuvtarget.c`:

1. `GL_TEXTURE_EXTERNAL_OES` via `glEGLImageTargetTexture2DOES` +
   `glFramebufferTexture` (note: not `glFramebufferTexture2D` — its
   `textarget` parameter does not accept `GL_TEXTURE_EXTERNAL_OES`; the
   target-less `glFramebufferTexture`, core since GLES 3.1, was used
   instead, requiring `<GLES3/gl32.h>` rather than `<GLES3/gl3.h>` for
   the declaration).
2. `GL_RENDERBUFFER` via `glEGLImageTargetRenderbufferStorageOES` +
   `glFramebufferRenderbuffer` — the mechanism `GL_OES_EGL_image` itself
   defines specifically for using an `EGLImage` as a render target
   (`GL_TEXTURE_EXTERNAL_OES` is normally sample-only in plain GLES).

A third variant (binding the `layout(yuv)`-qualified program before the
completeness check, in case of driver-specific ordering sensitivity) was
also tried.

### Result

**Every variant gives the identical result.** Zero GL errors are ever
reported at any step (`glBindTexture`/`glBindRenderbuffer`,
`glEGLImageTargetTexture2DOES`/`glEGLImageTargetRenderbufferStorageOES`,
`glBindFramebuffer`, `glFramebufferTexture`/`glFramebufferRenderbuffer`,
`glUseProgram`) — but:

```
glCheckFramebufferStatus(GL_FRAMEBUFFER) = 0
```

`0` is not `GL_FRAMEBUFFER_COMPLETE` (`0x8CD5`), and it is not any other
spec-defined `GL_FRAMEBUFFER_*` status enum either (all real ones are
non-zero, in the `0x8CDx`/`0x8DAx` range) — the driver returns a value
the GLES spec does not define, with zero accompanying error, for two
independently-correct binding mechanisms. `GL_EXT_YUV_target` is
therefore unusable for the encoder-compatible YUV AHB on this device
despite being advertised as present.

**Deliberately recorded as "additional PowerVR GLES/YUV-driver anomaly"
rather than automatically the same root cause as Bug #2** — both point
at the same underlying "can't actually write GPU-rendered content into
this specific opaque YUV buffer shape" wall, but the concrete failure
mode (silent no-op vs. an invalid framebuffer status) is different
enough, and on a different API surface (GLES vs Vulkan) entirely, that
conflating them would overstate what was actually proven.

### Files / harness

`ForeverXR/tools/foveation-pc-test/egl_gles_ext_probe.c` (extension
query), `glyuvtarget.c` + `GlYuvTargetTest.java` (the render attempt,
structurally identical to `YuvResolveTest.java`/`BlockModelTest.java`).

---

## E. Other Vulkan capability mismatches found during the investigation

These are additional data points gathered while narrowing down Bug #2,
kept here for completeness and clearly labeled by confirmation level.

| Finding | Status |
|---|---|
| `YCBCR_420_888` imports into Vulkan as `VK_FORMAT_UNDEFINED` + `externalFormat=0x301` for every GPU-usage combination tried (`GPU_DATA_BUFFER`, `GPU_SAMPLED_IMAGE`, `GPU_COLOR_OUTPUT`) | **CONFIRMED LIVE** — `vk_ahb_probe.c` |
| `STORAGE` usage is absent from `formatFeatures` for this `externalFormat`, both via direct AHB import inspection and via a plain (non-AHB) `vkGetPhysicalDeviceImageFormatProperties2` capability query for the concrete `G8_B8R8_2PLANE_420_UNORM` format + `STORAGE` + AHB-export chain (the latter returns `VK_SUCCESS` but a degenerate `maxExtent=0x0`, effectively "not really supported") | **CONFIRMED LIVE**, both directions |
| `HardwareBuffer.isSupported()`/`AHardwareBuffer_isSupported()` reports `true` for several `VIDEO_ENCODE + <GPU usage>` combinations — this is a permissive, low-bar check and does **not** imply the buffer is actually GPU-writable in practice (see Bug #2) | **CONFIRMED LIVE** — the checks pass; their optimism is what makes Bug #2 non-obvious without deeper testing |
| A Vulkan-first, full-control approach — creating a concrete-format (`G8_B8R8_2PLANE_420_UNORM`) image ourselves with `TRANSFER_DST` usage and exporting its memory as a fresh `AHardwareBuffer` via `vkGetMemoryAndroidHardwareBufferANDROID()` — passes its own capability query (`vkGetPhysicalDeviceImageFormatProperties2` for `TRANSFER_DST`-only usage + AHB-export chain reports real, non-degenerate limits: `maxExtent=32768x32768`) but the actual `vkAllocateMemory(export, dedicated)` call fails with **`VK_ERROR_OUT_OF_DEVICE_MEMORY` (`VkResult = -2`)** for a 1.29MB allocation with ample real device memory available | **CONFIRMED LIVE** — `vk_ahb_export_test.c`; exact `VkResult` value retrieved and recorded |
| Ordinary `vkCmdCopyImage` from the compositor's real concrete-format NV12 image directly into the opaque `externalFormat=0x301` image | **SPEC-REASONED, NOT EXECUTED.** Not run live — deliberately, since (a) the Vulkan external-format image copy-compatibility rule requires both sides of a copy to share the identical `externalFormat` when either side has one (the destination's per-texel layout has no Vulkan-visible size/geometry otherwise), which the concrete-format source cannot satisfy, and a same-format source is independently unobtainable (subject to the same `STORAGE=0` constraint), and (b) attempting a genuinely spec-illegal Vulkan command with no validation layers available on this stock device (none installed; installing one would need root or a debuggable-app path not available to `app_process`) risks an unrecoverable device-side hang for near-zero additional information, given two already-decisive real failures above. **State explicitly when citing this finding: it is reasoning from the Vulkan spec's external-format compatibility rules, not an executed test.** |

---

## F. MediaCodec `Surface`-input failure

**Status: not necessarily a PowerVR-specific bug** — this sits in the
Android media framework / vendor Codec2 integration layer
(`GraphicBufferSource`), one level below the app and (as far as this
investigation could determine without private APIs) one level below
Vulkan/GLES too. Kept as its own section deliberately.

### All Surface-input combinations tested

| Producer | Consumer | Result |
|---|---|---|
| Vulkan WSI (`vkCreateAndroidSurfaceKHR` + swapchain) | `AMediaCodec_createInputSurface()` | **FAIL** |
| EGL/GLES (`eglCreateWindowSurface`) | `AMediaCodec_createInputSurface()` | **FAIL** |
| EGL/GLES (`eglCreateWindowSurface`) | `AMediaCodec_createPersistentInputSurface()` + `AMediaCodec_setInputSurface()` (the *other* public NDK Surface-input mechanism) | **FAIL** |

(Vulkan WSI + the persistent-surface variant was not separately tried —
by this point the EGL-vs-Vulkan axis was already shown not to matter,
see below.)

### What worked mechanically (all four combinations)

- `COLOR_FormatSurface` (`0x7f000789`) was confirmed the correct format
  constant and accepted by `AMediaCodec_configure()`.
- The `BufferQueue`'s buffers genuinely cycled — up to 40 real frames
  acquired/rendered/presented cleanly with zero stalling, confirmed via
  `VK_GOOGLE_display_timing`/`eglPresentationTimeANDROID` explicit
  presentation timestamps.
- Vulkan swapchain creation succeeded (19 images, `supportedUsageFlags`
  included `STORAGE_BIT`, 3 usable RGBA surface formats — no manual
  NV12/YUV handling was needed for this approach at all, the device
  negotiates its own RGB→encoder-native conversion).
- EGL context/surface creation and `eglSwapBuffers` succeeded for many
  frames before eventually blocking (a real, informative hang, not a
  test bug — recovered via `adb shell kill -9`).
- An explicit `request-sync` (force sync frame) parameter call was made
  for frame 0, matching what the real, working ByteBuffer backend's
  `idr_handler` always does — no change.
- `debug.stagefright.c2inputsurface` was confirmed already `-1` (the
  framework-side `GraphicBufferSource` path Google's own device trees
  recommend) — nothing to toggle.
- Both AVC encoder components on this device (`c2.google.avc.encoder`,
  `c2.android.avc.encoder` — no distinct vendor-hardware-named component
  exists, no legacy OMX path at all) showed the **exact same failure**,
  ruling out a single-component bug.
- The VPU opened successfully every time (see log evidence below).

### What failed, and the strongest evidence

Despite all of the above, **zero encoded output ever arrives** — not
even the usually-immediate `INFO_OUTPUT_FORMAT_CHANGED` event —
regardless of graphics API, encoder component, presentation timestamps,
or explicit sync-frame requests.

`GraphicBufferSource` (Android's own framework-level Codec2 Surface
consumer — confirmed by the property check above to be the one in use,
not a vendor implementation) logs "got buffer with new dataSpace ..."
**exactly once**, ever, then goes silent — no further buffer
acquisition, no errors. The `BufferQueue` has 64 slots and happily
accepts dozens of queued frames from the producer side; it is
specifically the consumer that stops pulling after buffer #1.

The single strongest, most precise piece of evidence — captured during
the persistent-input-surface variant, one level deeper than
`GraphicBufferSource`, straight from the vendor's own hardware encoder
component's self-reported diagnostics, exact log lines:

```
GC2_EncComp: [0][Id=202] VPU_EncOpen codec AVC succeeded
GC2_EncComp: wait cmd queue for 0 times, instanceQueueCount(1) interrupted(0) flushing(0)
GC2_EncComp: wait cmd queue for 5 times, instanceQueueCount(1) interrupted(0) flushing(0)
GC2_EncComp: wait cmd queue for 10 times, instanceQueueCount(1) interrupted(0) flushing(0)
GC2_EncComp: wait cmd queue for 15 times, instanceQueueCount(1) interrupted(0) flushing(0)
```

The hardware VPU opens successfully, receives exactly one work item into
its internal command queue (`instanceQueueCount(1)`), and that one item
never gets processed — the component periodically self-reports this
exact stall for the entire run. This is the vendor's own component
naming its own stuck state, not an inference from absence of output.

### What this rules in/out

This failure is independent of:
- Vulkan vs. GLES (both fail identically)
- ordinary vs. persistent input surface (both fail identically)
- explicit presentation timestamps
- an explicit keyframe/sync-frame request

Therefore it appears to sit specifically in the
`Surface`/`GraphicBufferSource`/vendor-codec integration path on this
firmware, below anything the app can control via public APIs.

**Do NOT state "hardware cannot encode GPU buffers."** This was
disproven later — see §G. The failure is specific to the `Surface`
input mechanism, not to GPU-resident buffers in general.

### Files / commits

`ForeverXR/tools/foveation-pc-test/mediacodec_surface_test.c` (Vulkan
WSI), `mediacodec_egl_test.c` (EGL/GLES), `mediacodec_persistent_test.c`
(persistent-surface variant, the one with the `GC2_EncComp` log capture
above). Documented across commits `dfdd0505` and `5aee0115`.

---

## G. The Block Model breakthrough

**Status: MediaCodec-side proven working; this corrected an earlier,
too-broad conclusion.**

A third, architecturally distinct public API family exists that bypasses
`Surface`/`ANativeWindow`/`BufferQueue`/`GraphicBufferSource`/EGL/Vulkan-WSI
entirely: `MediaCodec.CONFIGURE_FLAG_USE_BLOCK_MODEL` (Java-only, not
exposed via NDK `AMediaCodec` — requires async `setCallback()`, confirmed
live via `IllegalStateException: Block model is only valid with callback set (async mode)`)
paired with per-buffer `MediaCodec.QueueRequest.setHardwareBuffer(HardwareBuffer)`.

### Checkpoint 1 result: works completely

Setup: `HardwareBuffer.YCBCR_420_888`, `896x960`, `layers=1`,
`usage = USAGE_VIDEO_ENCODE`, CPU-filled via a small JNI helper
(`hwbfill.c`, using the public `AHardwareBuffer_fromHardwareBuffer()` /
`AHardwareBuffer_lockPlanes()` / `AHardwareBuffer_unlock()` bridge).

Result, exactly, from `BlockModelTest.java`:

```
framesQueued=60 / 60
sawFormatChanged=true
totalEncodedFrames=60
totalEncodedBytes=3096
sawEos=true
```

Real `csd-0`(SPS)/`csd-1`(PPS) produced (22 and 8 bytes respectively),
periodic full-size IDR frames among smaller P-frames, clean
`BUFFER_FLAG_END_OF_STREAM`. Logcat confirms `GC2_EncComp` never shows
the `instanceQueueCount(1)` stall this time — a clean `VPU_EncOpen`
followed by continuous encoding through to a clean `VPU_EncClose`.

**This proves MediaCodec/encoder graphics-buffer input itself is
functional on this device.** The broken component identified in §F is
specifically the `Surface` route — not all non-ByteBuffer encoding paths.

### Two implementation gotchas discovered

1. Block-model output is **not** exposed via `getOutputBuffer()` — that
   throws `MediaCodec.IncompatibleWithBlockModelException`. Must use
   `getOutputFrame(index).getLinearBlock().map()` instead.
2. `app_process`'s main thread has no `Looper` prepared by default, and
   `MediaCodec.setCallback()` needs one on the *calling* thread
   internally regardless of the explicit `Handler` argument passed
   pointing at a different (`HandlerThread`-backed) Looper — confirmed
   live via a `NullPointerException` in
   `MediaCodec.getEventHandlerOn()` → `EventHandler.getLooper()`. Fixed
   with an explicit `Looper.prepare()` as the first statement in
   `main()`. **This is specific to the `app_process` test-harness
   environment** — a real Android `Service`/`Activity` thread already
   has a prepared `Looper`, so this is not a concern for production
   integration.

### Files / commit

`ForeverXR/tools/foveation-pc-test/BlockModelTest.java` + `hwbfill.c`.
Documented in `docs/ANDROID_PORT.md`, commit `83041c78`.

---

## H. Why full public zero-copy still failed overall

The architecture being evaluated:

```
Monado/compositor (Vulkan, GPU-resident RGBA)
    -> GPU-side conversion to YUV
    -> a HardwareBuffer QueueRequest.setHardwareBuffer() accepts (proven, §G)
    -> MediaCodec Block Model
    -> hardware encoder
```

§G proved the *last two arrows* work. The entire remaining question was
whether the *first two arrows* — populating that encoder-compatible YUV
`HardwareBuffer` from the GPU — are achievable via any public API. Every
known public route was tried:

| # | Route | Result |
|---|---|---|
| 1 | Vulkan `STORAGE` direct compute write | **Unavailable** — `formatFeatures` reports `STORAGE=0` (§E, CONFIRMED LIVE) |
| 2 | Ordinary Vulkan `vkCmdCopyImage`/transfer from a concrete-format source | **Not established as usable** — external-format copy-compatibility restrictions per spec; **not executed live** (§E, SPEC-REASONED) |
| 3 | Vulkan-created/exported concrete NV12 `AHardwareBuffer` | **Fails** — `vkAllocateMemory` returns `VK_ERROR_OUT_OF_DEVICE_MEMORY` despite passing its own capability query (§E, CONFIRMED LIVE) |
| 4 | `VK_ANDROID_external_format_resolve` | **Advertised, all calls succeed, silently writes nothing** (§C, CONFIRMED LIVE, 3 independent verifications) |
| 5 | `GL_EXT_YUV_target` | **Advertised, but the framebuffer target is never usable** (§D, CONFIRMED LIVE) |
| 6 | `MediaCodec` `Surface` input | **Independent vendor codec/Surface integration bug**, unrelated to any of the above (§F, CONFIRMED LIVE) |

### The carefully worded conclusion

> On the tested stock Pixel 10 Pro XL firmware, no functioning public
> GPU→MediaCodec zero-copy route was found. MediaCodec Block Model
> successfully accepts encoder-compatible YUV HardwareBuffers, but the
> available public GPU mechanisms tested do not provide a functioning
> way to populate those buffers on this driver/firmware.

**Do NOT write "zero-copy is impossible on this hardware."** That has
not been proven — every failure above has a specific, distinct,
independently-diagnosed cause (mostly real driver bugs, one spec-reasoned
restriction, one apparently-unrelated media-framework integration bug),
not a single confirmed hardware ceiling. A driver update, a different
device, or access to non-public APIs (explicitly out of scope for this
project, see the project's own stock/unrooted-device constraint) could
each independently change this conclusion.

---

## I. Current working (production) architecture

This is the path the project returns to after this investigation — the
known-good baseline for the next optimization phase.

```
Monado/compositor (server/compositor/compositor.cpp)
    -> RGB->NV12 compute (foveation.comp)
    -> dedicated single-array-layer multi-planar VkImage PER STREAM
       (the Bug #1 workaround, §B -- do not consolidate)
    -> vkCmdCopyImageToBuffer                                    [video_encoder_mediacodec.cpp:288, present_image()]
    -> host-visible staging buffer (in[slot].buffer)
    -> CPU memcpy(in_buf, src, payload_size)                     [video_encoder_mediacodec.cpp:372, encode()]
    -> AMediaCodec ByteBuffer input (AMediaCodec_queueInputBuffer)
    -> encoded H.264 access units
    -> network (WiVRn's existing protocol)
    -> Quest 1 client decoder
```

**The unavoidable-for-now CPU transfer is exactly one `memcpy()` call**,
`server/encoder/video_encoder_mediacodec.cpp:372`, inside
`video_encoder_mediacodec::encode()`. Everything upstream of it
(`present_image()`, the `vkCmdCopyImageToBuffer` call) is GPU work; the
buffer it copies from (`in[slot].buffer`) is a host-visible-but-still
GPU-written VMA allocation, not a second CPU-side copy.

This is the class's own documented tradeoff — see
`server/encoder/video_encoder_mediacodec.h`'s class-level comment
(updated as part of this documentation pass, §L, to reflect that the
originally-planned upgrade path was tried and found non-viable on this
device, rather than pointing at a stale "just do X" note).

---

## J. Other bug fixes from this investigation (application-level, not driver bugs)

### `std::bad_alloc` crash

- **Symptom**: SIGABRT on the very first live test of the Bug #1 fix,
  full tombstone backtrace pinned it exactly to
  `beman::inplace_vector::emplace_back` inside `compositor::layer_commit()`.
- **Root cause**: `inplace_vector<vk::ImageMemoryBarrier2, N>` is a
  *fixed-capacity* vector — it does not grow, it throws `std::bad_alloc`
  when full. Splitting one shared 3-layer image into three separate
  per-stream images (§B's fix) turned one shared pre-dispatch barrier
  into three separate barriers, overflowing the vector's fixed capacity
  of 3 (1 squasher-path barrier + up to 3 per-stream barriers = up to 4,
  exceeding the old capacity of 3).
- **Fix**: bumped the fixed capacity from 3 to 5 (a small margin over the
  exact worst case of 4).
- **File/function/commit**: `server/compositor/compositor.cpp:356`,
  inside `compositor::layer_commit()`; commit `c77b7ad5` (same commit as
  the Bug #1 fix — found and fixed in the same live test session). The
  comment explaining this is already in place at that line (see the
  source excerpt in §B).

### Descriptor-set update latency regression

- **When it appeared**: immediately after the Bug #1 fix went live —
  splitting one shared descriptor set/dispatch into two (one per eye,
  since each eye now has its own image) doubled the per-frame
  `vkUpdateDescriptorSets` calls and compute dispatches.
- **Contributing cost**: the compositor double-buffers 2 image slots and
  calls `foveate()` with whichever slot is active that frame, so the
  `(y, cbcr, alpha_y, alpha_cbcr)` view handles only actually change
  every *other* frame (when the slot flips) — but every one of
  `foveation`'s 2 descriptor sets was being rewritten unconditionally on
  *every* frame regardless, real and measurable added CPU cost.
- **Optimization**: `server/compositor/foveation.h`'s `bound_views`
  struct + `last_bound` array (one per eye) cache the last-written view
  handles; `foveation.cpp`'s `foveate()` now splits descriptor writes
  into an always-update part (binding 0, the source image, which
  legitimately changes every frame) and a cached-conditional part
  (bindings 1-5), only reissuing `vkUpdateDescriptorSets` for the latter
  when `bound_views` actually differs from `last_bound[eye]`.
- **Result**: user-confirmed qualitatively improved ("I checked already
  the pixelations are still happening maybe latency is better" — the
  user's own words after testing live). **No numerical measurement
  exists for this improvement** — do not cite a specific number if this
  document is referenced later; only a qualitative live confirmation was
  obtained.
- **File/commit**: `server/compositor/foveation.h`/`.cpp`, commit
  `c77b7ad5` (same commit as the Bug #1 fix).

### IDR-recovery false-positive livelock (a separate, earlier cause of green frames)

Not part of the Bug #1 investigation's own root cause, but relevant
context: this is a *different* bug, found and fixed earlier in the same
overall investigation, that also produced green-appearing frames via an
entirely different mechanism (stale-frame display from a desynced
decoder, not corrupted pixel content) — worth distinguishing clearly so
the two are never conflated.

- **Symptom**: `server/encoder/idr_handler.cpp`'s
  `default_idr_handler` (one instance per stream) could not distinguish
  "the compositor dropped this frame before any encoder ever saw it"
  (`compositor.cpp`'s `layer_commit()` drops frames under load, before
  `present_image()`) from "this encoder sent the frame and it was
  genuinely lost downstream." It treated both cases identically,
  demanding a spurious IDR and skipping every subsequent frame until
  that IDR was acknowledged — independently per stream, desyncing the
  streams' frame timelines until the client's two decoders could no
  longer find any common frame index between them, leaving one eye
  showing a stale (often solid green, from an earlier keyframe) frame.
- **Fix**: track which frame indices each encoder actually sent
  (`sent_frames`, a 512-entry ring), and only treat "not sent to
  decoder" feedback as genuine loss — triggering a real IDR request —
  when that frame index is in the set this specific encoder actually
  sent.
- **Result**: verified live across multiple fresh sessions — "IDR frame
  needed" / "Failed to find a common frame for all decoders" dropped
  from dozens-per-second to zero.
- **Also fixed in the same commit**: `non_ref_frames{512, uint64_t(-1)}`
  brace-init was picking the `initializer_list` constructor over the
  intended fill constructor (a 2-element vector, not 512) — switched to
  parens; and `video_encoder_mediacodec.cpp` was telling
  `AMediaCodec_queueInputBuffer` the full `payload_size` when only
  `copy_size = min(in_size, payload_size)` bytes were actually copied —
  a latent truncation risk, fixed to pass the correct size.
- **File/commit**: `server/encoder/idr_handler.cpp`/`.h`, commit
  `33895ed6`.

Also from the same general investigation period (commit `89655cd4`):
concurrent per-stream encoding (`compositor::encoder_work()` now encodes
streams concurrently instead of sequentially on one thread, since
Android's `AMediaCodec` has real per-call JNI/Binder overhead unlike
desktop NVENC/VAAPI) and `AMEDIAFORMAT_KEY_MAX_INPUT_SIZE` being
explicitly set (Codec2 was silently undersizing its input buffer below
one real NV12 frame — a real, independently-confirmed bug, verified via
zero "too small" truncation warnings across full sessions after the fix,
but explicitly **not** the cause of the green-chroma corruption — see
§B's "ruled out" table).

---

## K. Source comments added/updated as part of this documentation pass

See §L below for the exact list; kept separate here only to satisfy the
outline this document was requested against. All source comments point
back to this file rather than duplicating its content.

---

## L. Where the source comments live

- `server/compositor/compositor.h` (`struct image`'s existing comment,
  lines ~49-60) — already documents Bug #1 in detail; extended with an
  explicit link to this document.
- `server/compositor/compositor.cpp` (`inplace_vector` capacity comment,
  line ~348-355) — already documents the `std::bad_alloc` fix in detail;
  left as-is (already sufficient, already references the right context).
- `server/compositor/foveation.h` (`bound_views`/`last_bound` comment,
  lines ~62-70) — already documents the latency fix in detail; left
  as-is.
- `server/encoder/video_encoder_mediacodec.cpp` (`present_image()`'s
  comment about `baseArrayLayer`, line ~235-240) — already documents why
  it's always 0 now; left as-is.
- `server/encoder/video_encoder_mediacodec.h` — **updated** by this pass:
  the class-level `ponytail:` comment previously pointed at
  `AMediaCodec_createInputSurface()` + Vulkan-imported `AHardwareBuffer`
  as "the" zero-copy upgrade path. That path (and every other public
  route) was tried and found non-viable on this device (§F, §H) — the
  comment now reflects that and points here instead of describing a
  stale plan as still-open.

---

## M. Potential upstream bug reports

Enough information is recorded above to extract three tiny standalone
repro projects without re-doing the investigation. Summarized per-bug:

### PowerVR Bug #1 — multi-planar array-layer compute corruption

- **Minimum Vulkan object configuration**: one `VkImage`,
  `format = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM`, `arrayLayers = 3`,
  `flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT | VK_IMAGE_CREATE_EXTENDED_USAGE_BIT`,
  per-array-layer plane `VkImageView`s reinterpreted as `VK_FORMAT_R8_UNORM`
  (plane 0) / `VK_FORMAT_R8G8_UNORM` (plane 1) with `VK_IMAGE_USAGE_STORAGE_BIT`,
  written via `imageStore()` from a compute shader.
- **Expected**: `imageStore` writes to any array layer produce correct,
  consistent results (matches two independent desktop Vulkan
  implementations, NVIDIA and Mesa llvmpipe, in this exact configuration).
- **Actual**: writes to array layer 0 are correct; writes to array layers
  ≥1 are corrupted, on this device only.
- **Workaround**: use `arrayLayers = 1` (one image per logical layer)
  instead.
- **Device/driver**: Pixel 10 Pro XL, PowerVR D-Series DXT-48-1536 MC1,
  `vendorID=0x1010`, `deviceID=0x71061212`, `driverVersion=6908880`
  (raw)/`25.3@6908880` (GLES build string), Vulkan `apiVersion=1.4.317`.
- **Minimal repro idea**: allocate a 3-array-layer
  `G8_B8R8_2PLANE_420_UNORM` image, clear/write a distinct solid color to
  each layer via a trivial compute shader, read back all three layers,
  compare against the same operation on an `arrayLayers=1` image done
  three times. `ForeverXR/tools/foveation-pc-test/foveation_test.c`
  already implements a superset of this (with real captured production
  data) and is the fastest path to a clean minimal repro.

### PowerVR Bug #2 — `VK_ANDROID_external_format_resolve` silent no-op

- **AHB descriptor**: `896x960`, `YCBCR_420_888`, `layers=1`,
  `usage = VIDEO_ENCODE | GPU_COLOR_OUTPUT`.
- **`externalFormat`**: `0x301`.
- **Extension capability**: `VK_ANDROID_external_format_resolve` present
  (`spec=1`), `externalFormatResolve` feature `VK_TRUE`,
  `nullColorAttachmentWithExternalFormatResolve` `VK_TRUE`.
- **`colorAttachmentFormat`**: `37` (`VK_FORMAT_R8G8B8A8_UNORM`) —
  reported renderable.
- **Expected**: rendering via `resolveMode = VK_RESOLVE_MODE_EXTERNAL_FORMAT_DOWNSAMPLE_BIT_ANDROID`
  into this AHB-backed image modifies its real memory content.
- **Actual**: every Vulkan call succeeds; the buffer's content is
  unchanged (verified via direct CPU lock: all-zero luma after a solid
  magenta render) and downstream consumers (MediaCodec) see identical
  output regardless of what was rendered.
- **CPU verification**: `AHardwareBuffer_lockPlanes()` with
  `CPU_READ_OFTEN` added to the usage bits, `Y(x,y)=0` at all sample
  points after a magenta (`vec4(1,0,1,1)`) render.
- **Shader-change/MD5 verification**: three different fragment shaders
  (quadrants, gradient, solid magenta) produce byte-identical encoded
  output, MD5 `46ba31ef6719b2f2cfda2e22c6aed60b` in all three cases.

### `GL_EXT_YUV_target` FBO-never-valid issue

- **Exposed extensions**: `GL_EXT_YUV_target`,
  `GL_OES_EGL_image_external[_essl3]`, `GL_OES_EGL_image`,
  `EGL_ANDROID_get_native_client_buffer`,
  `EGL_ANDROID_image_native_buffer`, `EGL_KHR_image_base` — all present.
- **AHB configuration**: identical to Bug #2's (`896x960`,
  `YCBCR_420_888`, `layers=1`, `VIDEO_ENCODE | GPU_COLOR_OUTPUT`).
- **FBO setup**: either `GL_TEXTURE_EXTERNAL_OES` (via
  `glEGLImageTargetTexture2DOES` + `glFramebufferTexture`) or
  `GL_RENDERBUFFER` (via `glEGLImageTargetRenderbufferStorageOES` +
  `glFramebufferRenderbuffer`) attached to `GL_COLOR_ATTACHMENT0`.
- **Returned status**: `glCheckFramebufferStatus(GL_FRAMEBUFFER) = 0`
  (not any valid enum).
- **GL errors**: zero, at every step, via `glGetError()` checked after
  each call.

---

## N. Verification against git history

Performed while writing this document (not from memory):

- `git log --oneline -20` on `wivrn-android` (branch
  `android-phone-server`) — confirmed the exact commit hashes cited
  throughout: `c77b7ad5`, `dfdd0505`, `5aee0115`, `83041c78`, `1af83e77`,
  `28c448d1`, `33895ed6`, `89655cd4`.
- `git show --stat` on each of the above — confirmed the exact file
  lists changed per commit, cited in §B/§F/§G/§J.
- Source line numbers/function names (`make_stream_image()`,
  `make_images()`, `compositor::layer_commit()`,
  `compositor::encoder_work()`, `stream_vk_image` lambda,
  `video_encoder_mediacodec::present_image()`/`::encode()`, the exact
  `memcpy()` line) were confirmed by directly reading the current
  checked-out source, not recalled from conversation history.
- Device/driver facts in §A (Vulkan `apiVersion`/`driverVersion`,
  extension list with `specVersion`s, build fingerprint) were re-queried
  live from the device while writing this document, not carried forward
  from earlier in the investigation unverified.

**Nothing in this document is a fabricated commit ID or invented source
location.** Anything that could not be independently re-confirmed is
explicitly marked UNKNOWN / NOT RECORDED in §A, or SPEC-REASONED /
NOT EXECUTED in §E/§H where relevant.

### Reproducibility warning

**All test harnesses referenced throughout this document
(`ForeverXR/tools/foveation-pc-test/*.c`, `*.java`) live outside any git
repository** — `ForeverXR/tools/` is plain, untracked filesystem on the
development machine, confirmed via `git rev-parse --is-inside-work-tree`
failing at every level from `ForeverXR/tools/foveation-pc-test/` up to
`ForeverXR/` itself. They are **not backed up, not committed, and not
part of this repository's history**. If they are ever needed for a real
upstream bug report or to re-verify a finding, and they are no longer
present on this machine, they will need to be reconstructed from the
descriptions and code excerpts in this document. Preserving them
properly (e.g. committing a snapshot into this repository under a
clearly-marked non-shipping path) is recommended as a follow-up, but was
out of scope for this documentation pass.
