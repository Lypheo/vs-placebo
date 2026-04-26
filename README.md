# vs-placebo

[![build](https://github.com/Lypheo/vs-placebo/actions/workflows/build.yml/badge.svg)](https://github.com/Lypheo/vs-placebo/actions/workflows/build.yml)
![PyPI - Version](https://img.shields.io/pypi/v/vs-placebo)

A VapourSynth plugin interface to [libplacebo](https://code.videolan.org/videolan/libplacebo).

## API

### Deband

```python
placebo.Deband(
    clip: vs.VideoNode,
    planes: int = 1,
    iterations: int = 1,
    threshold: float = 4.0,
    radius: float = 16.0,
    grain: float = 6.0,
    dither: bool = True,
    dither_algo: int = 0,
    log_level: int = 2,
)
```

Input needs to be 8 or 16 bit Integer or 32 bit Float.

- `planes`: the planes to filter. The n-th plane is processed if the n-th lowest
  bit of `planes` is 1, so for example to filter all planes, pass
  `planes = 1 | 2 | 4`. (Yes, this is needlessly complex, but it was the
  simplest to implement.)
- `iterations`: The number of debanding steps to perform per sample.
- `threshold`: The debanding filter's cut-off threshold. Higher numbers increase
  the debanding strength dramatically, but progressively diminish image details.
- `radius`: The debanding filter's initial radius. The radius increases linearly
  for each iteration. A higher radius will find more gradients, but a lower
  radius will smooth more aggressively.
- `grain`: Add some extra noise to the image. This significantly helps cover up
  remaining quantization artifacts. Higher numbers add more noise.
- `dither`: Whether the debanded frame should be dithered or rounded from float
  to the output bitdepth. Only works for 8 bit.
- `dither_algo`: The dithering method to use. Defaults to `blue`.

### Tonemap

```python
placebo.Tonemap(
    clip: vs.VideoNode,
    src_csp: int,
    dst_csp: int,
    dst_prim: int | None = None,
    src_max: float | None = None,
    src_min: float | None = None,
    dst_max: float | None = None,
    dst_min: float | None = None,
    dynamic_peak_detection: bool = True,
    smoothing_period: float = 20.0,
    scene_threshold_low: float = 1.0,
    scene_threshold_high: float = 3.0,
    percentile: float = 100.0,
    gamut_mapping: int = 1,
    tone_mapping_function: int = 1,
    tone_mapping_function_s: str = "spline",
    tone_mapping_param: float | None = None,
    metadata: int = 0,
    use_dovi: bool | None = None,
    visualize_lut: bool = False,
    show_clipping: bool = False,
    contrast_recovery: float = 0.0,
    log_level: int = 2,
)
```

Performs color mapping (which includes tonemapping from HDR to SDR, but can do a
lot more).  
Expects RGB48 or YUVxxxP16 input.  
Outputs RGB48 or YUV444P16, depending on input color family.

- `src_csp, dst_csp`: Source and destination colorspaces respectively. For
  example, to map from [BT.2020, PQ] (HDR) to traditional [BT.709, BT.1886] (SDR),
  pass `src_csp=1, dst_csp=0`.
  | Value | Description |
  | ----- | ----------- |
  | 0 | SDR |
  | 1 | HDR10 |
  | 2 | HLG |
  | 3 | Dolby Vision |
- `dst_prim`: Destination color primaries.
  | Value | Description |
  | ----- | ----------- |
  | 0 | Unknown |
  | 1 | ITU-R Rec. BT.601 (525-line = NTSC, SMPTE-C) |
  | 2 | ITU-R Rec. BT.601 (625-line = PAL, SECAM) |
  | 3 | ITU-R Rec. BT.709 (HD), also sRGB |
  | 4 | ITU-R Rec. BT.470 M |
  | 5 | EBU Tech. 3213-E / JEDEC P22 phosphors |
  | 6 | ITU-R Rec. BT.2020 (UltraHD) |
  | 7 | Apple RGB |
  | 8 | Adobe RGB (1998) |
  | 9 | ProPhoto RGB (ROMM) |
  | 10 | CIE 1931 RGB primaries |
  | 11 | DCI-P3 (Digital Cinema) |
  | 12 | DCI-P3 (Digital Cinema) with D65 white point |
  | 13 | Panasonic V-Gamut (VARICAM) |
  | 14 | Sony S-Gamut |
  | 15 | Traditional film primaries with Illuminant C |
  | 16 | ACES Primaries #0 (ultra wide) |
  | 17 | ACES Primaries #1 |
- `src_max, src_min, dst_max, dst_min`: Source/destination display levels, in
  nits (cd/m^2). Source can be derived from props if available.
- HDR peak detection.
  - `dynamic_peak_detection`: Enables HDR peak detection. Enabled by default.
  - `smoothing_period`: Smoothing coefficient for the detected values. This
    controls the time parameter (tau) of an IIR low pass filter. In other words,
    it represent the cutoff period (= 1 / cutoff frequency) in frames.
    Frequencies below this length will be suppressed. This helps block out
    annoying "sparkling" or "flickering" due to small variations in
    frame-to-frame brightness. If left as `0.0`, this smoothing is completely
    disabled. Defaults to `20.0`.
  - `scene_threshold_low`, `scene_threshold_high`: In order to avoid reacting
    sluggishly on scene changes as a result of the low-pass filter, we disable
    it when the difference between the current frame brightness and the average
    frame brightness exceeds a given threshold difference. But rather than a
    single hard cutoff, which would lead to weird discontinuities on fades, we
    gradually disable it over a small window of brightness ranges. These
    parameters control the lower and upper bounds of this window, in units of 1%
    PQ. Setting either one of these to 0.0 disables this logic. Defaults to
    `1.0` and `3.0`, respectively.
  - `percentile`: Which percentile of the input image brightness histogram to
    consider as the true peak of the scene. If this is set to `100` (or `0`),
    the brightest pixel is measured. Otherwise, the top of the frequency
    distribution is progressively cut off. Setting this too low will cause
    clipping of very bright details, but can improve the dynamic brightness
    range of scenes with very bright isolated highlights. Defaults to `100.0`.
- `gamut_mapping`: Gamut mapping function to use to handle out-of-gamut colors,
  including colors which are out-of-gamut as a consequence of tone mapping.
  Defaults to 1 (`perceptual`). The following options are available:
  | `gamut_mapping` | Function | Description |
  | ----- | -------- | ----------- |
  | 0 | clip | Performs no gamut-mapping, just hard clips out-of-range colors per-channel. |
  | 1 | perceptual | Performs a perceptually balanced (saturation) gamut mapping, using a soft knee function to preserve in-gamut colors, followed by a final softclip operation. This works bidirectionally, meaning it can both compress and expand the gamut. Behaves similar to a blend of `saturation` and `softclip`. |
  | 2 | softclip | Performs a perceptually balanced gamut mapping using a soft knee function to roll-off clipped regions, and a hue shifting function to preserve saturation. |
  | 3 | relative | Performs relative colorimetric clipping, while maintaining an exponential relationship between brightness and chromaticity. |
  | 4 | saturation | Performs simple RGB->RGB saturation mapping. The input R/G/B channels are mapped directly onto the output R/G/B channels. Will never clip, but will distort all hues and/or result in a faded look. |
  | 5 | absolute | Performs absolute colorimetric clipping. Like `relative`, but does not adapt the white point. |
  | 6 | desaturate | Performs constant-luminance colorimetric clipping, desaturing colors towards white until they're in-range. |
  | 7 | darken | Uniformly darkens the input slightly to prevent clipping on blown-out highlights, then clamps colorimetrically to the input gamut boundary, biased slightly to preserve chromaticity over luminance. |
  | 8 | highlight | Performs no gamut mapping, but simply highlights out-of-gamut pixels. |
  | 9 | linear | Linearly/uniformly desaturates the image in order to bring the entire image into the target gamut. |
- `tone_mapping_function`, `tone_mapping_function_s`: Tone mapping function to
  use for adapting between difference luminance ranges, including black point
  adaptation. May be specified as either the integer value or the function name;
  if both are passed, the function name is used. Defaults to 1 (`spline`).
  | `tone_mapping_function` | `tone_mapping_function_s` | Description |
  | --- | --- | --- |
  | 0 | clip | Performs no tone-mapping, just clips out-of-range colors. Retains perfect color accuracy for in-range colors but completely destroys out-of-range information. Does not perform any black point adaptation. |
  | 1 | spline | Simple spline consisting of two polynomials, joined by a single pivot point, which is tuned based on the source scene average brightness (taking into account dynamic metadata if available). This function can be used for both forward and inverse tone mapping. |
  | 2 | st2094-40 | EETF from SMPTE ST 2094-40 Annex B, which uses the provided OOTF based on Bezier curves to perform tone-mapping. The OOTF used is adjusted based on the ratio between the targeted and actual display peak luminances. In the absence of HDR10+ metadata, falls back to a simple constant bezier curve. |
  | 3 | st2094-10 | EETF from SMPTE ST 2094-10 Annex B.2, which takes into account the input signal average luminance in addition to the maximum/minimum. |
  | 4 | bt2390 | EETF from the ITU-R Report BT.2390, a hermite spline roll-off with linear segment. |
  | 5 | bt2446a | EETF from ITU-R Report BT.2446, method A. Can be used for both forward and inverse tone mapping. |
  | 6 | reinhard | Very simple non-linear curve. Named after Erik Reinhard. |
  | 7 | mobius | Generalization of the `reinhard` tone mapping algorithm to support an additional linear slope near black. The name is derived from its function shape `(ax+b)/(cx+d)`, which is known as a Möbius transformation. This function is considered legacy/low-quality, and should not be used. |
  | 8 | hable | Piece-wise, filmic tone-mapping algorithm developed by John Hable for use in Uncharted 2, inspired by a similar tone-mapping algorithm used by Kodak. Popularized by its use in video games with HDR rendering. Preserves both dark and bright details very well, but comes with the drawback of changing the average brightness quite significantly. This is sort of similar to `reinhard` with `reinhard_contrast=0.24`. This function is considered legacy/low-quality, and should not be used. |
  | 9 | gamma | Fits a gamma (power) function to transfer between the source and target color spaces, effectively resulting in a perceptual hard-knee joining two roughly linear sections. This preserves details at all scales, but can result in an image with a muted or dull appearance. This function is considered legacy/low-quality and should not be used. |
  | 10 | linear | Linearly stretches the input range to the output range, in PQ space. This will preserve all details accurately, but results in a significantly different average brightness. Can be used for inverse tone-mapping in addition to regular tone-mapping. |
  | 11 | linearlight | Like `linear`, but in linear light (instead of PQ). Works well for small range adjustments but may cause severe darkening when downconverting from e.g. 10k nits to SDR. |
- `metadata`: Data source to use when tone-mapping. Setting this to a specific
  value allows overriding the default metadata preference logic. Defaults to 0
  (automatic selection).
  | `metadata` | Description |
  | --- | --- |
  | 0 | Automatic selection |
  | 1 | None (disabled) |
  | 2 | HDR10 (static) |
  | 3 | HDR10+ (MaxRGB) |
  | 4 | Luminance (CIE Y) |
- `use_dovi`: Whether to use the Dolby Vision RPU for ST2086 metadata. Defaults
  to true when tonemapping from Dolby Vision.
- `visualize_lut`: Display a (PQ-PQ) graph of the active tone-mapping LUT. See
  [mpv docs](https://mpv.io/manual/master/#options-tone-mapping-visualize).
- `show_clipping`: Highlight hard-clipped pixels during tone-mapping.
- `contrast_recovery`: HDR contrast recovery strength. If set to a value above
  `0.0`, the source image will be divided into high-frequency and low-frequency
  components, and a portion of the high-frequency image is added back onto the
  tone-mapped output. May cause excessive ringing artifacts for some HDR
  sources, but can improve the subjective sharpness and detail left over in the
  image after tone-mapping. Defaults to `0.0`.

For Dolby Vision support, FFmpeg 5.0 minimum and git ffms2 are required.

#### Supported frame props

- `PLSceneMax`, `PLSceneAvg`: Per-scene dynamic brightness metadata, in nits (cd/m^2).

  - `float[] PLSceneMax`: the scene's peak brightness. Can be specified by component (RGB) or a single value.
  - `float PLSceneAvg`: the scene's average brightness.

  Requires `libplacebo` v5.246.0 or newer, otherwise ignored.  
   If `PLSceneMax` is per component, the metadata is set to `scene_max` and `scene_avg`.
  If it's a single luminance value and v5.257.0+, sets `max_pq_y` and `avg_pq_y`.

  If `use_dovi` is enabled, `max_pq_y` and `avg_pq_y` are derived from the Dolby Vision RPU L1 metadata instead of the props.

#### `high_quality` preset

To replicate libplacebo's `high_quality` preset, only `contrast_recovery` and
`percentile` need to be changed from their defaults:

```python
core.placebo.Tonemap(
    clip,
    contrast_recovery=0.3,
    percentile=99.995,
    # ...
)
```

### Resample

```python
placebo.Resample(
    clip: vs.VideoNode,
    width: int,
    height: int,
    filter: str = "ewa_lanczos",
    radius: float = 0.0,
    clamp: float = 0.0,
    taper: float = 0.0,
    blur: float = 0.0,
    param1: float = 0.0,
    param2: float = 0.0,
    src_width: float = None,
    src_height: float = None,
    sx: float = 0.0,
    sy: float = 0.0,
    antiring: float = 0.0,
    sigmoidize: bool = True,
    linearize: bool = True,
    sigmoid_center: float = 0.75,
    sigmoid_slope: float = 6.5,
    trc: int = 1,
    min_luma: float = 1e-6,
    log_level: int = 2,
)
```

Input needs to be 8 or 16 bit Integer or 32 bit Float.

- `filter`: See [the header](https://github.com/haasn/libplacebo/blob/v7.349.0/src/include/libplacebo/filters.h#L268-L299) for possible values (remove the "pl_filter_" before the filter name, e.g. `filter="lanczos"`).
- `radius`: Override the filter kernel radius. Has no effect if the filter
  kernel is not resizeable.
- `clamp`: Represents an extra weighting/clamping coefficient for negative
  weights. A value of `0.0` represents no clamping. A value of `1.0` represents
  full clamping, i.e. all negative lobes will be removed.
- `taper`: Additional taper coefficient. This essentially flattens the
  function's center.
- `blur`: Additional blur coefficient. This effectively stretches the kernel,
  without changing the effective radius of the filter radius.
- `param1`, `param2`: Parameters for the filter function.
- `src_width`, `src_height`: Dimensions of the source region. Defaults to the
  dimensions of `clip`.
- `sx`, `sy`: Top left corner of the source region. Can be used for subpixel shifts.
- `antiring`: Antiringing strength.
- `sigmoidize, linearize`: Whether to linearize/sigmoidize before scaling.
  Enabled by default for RGB, disabled for YCbCr because NCL YCbCr can’t be correctly linearized without conversion to RGB.
  Defaults to disabled for GRAY since it may be a YCbCr plane, but can be manually enabled.
  When sigmodizing, `linearize` should be True as well. (Currently mangles HDR video, so disable for that.)
- `sigmoid_center`: The center (bias) of the sigmoid curve.
- `sigmoid_slope`: The slope (steepness) of the sigmoid curve.
- `trc`: The transfer curve to use for linearizing.
  | Value | Description |
  | ----- | ----------- |
  | 0 | Unknown |
  | 1 | ITU-R Rec. BT.1886 (CRT emulation + OOTF) |
  | 2 | IEC 61966-2-4 sRGB (CRT emulation) |
  | 3 | Linear light content |
  | 4 | Pure power gamma 1.8 |
  | 5 | Pure power gamma 2.0 |
  | 6 | Pure power gamma 2.2 |
  | 7 | Pure power gamma 2.4 |
  | 8 | Pure power gamma 2.6 |
  | 9 | Pure power gamma 2.8 |
  | 10 | ProPhoto RGB (ROMM) |
  | 11 | Digital Cinema Distribution Master (XYZ) |
  | 12 | ITU-R BT.2100 PQ (perceptual quantizer), aka SMPTE ST2048 |
  | 13 | ITU-R BT.2100 HLG (hybrid log-gamma), aka ARIB STD-B67 |
  | 14 | Panasonic V-Log (VARICAM) |
  | 15 | Sony S-Log1 |
  | 16 | Sony S-Log2 |
- `min_luma`: Minimum luminance. Defaults to 1e-6 which is infinite contrast.
  Set to 0 for 1000:1 contrast.

### Shader

```python
placebo.Shader(
    clip: vs.VideoNode,
    shader: str,
    width: int,
    height: int,
    chroma_loc: int = 1,
    matrix: int = 2,
    trc: int = 1,
    filter: str = "ewa_lanczos",
    radius: float,
    clamp: float,
    taper: float,
    blur: float,
    param1: float,
    param2: float,
    antiring: float = 0.0,
    sigmoidize: bool = True,
    linearize: bool = True,
    sigmoid_center: float = 0.75,
    sigmoid_slope: float = 6.5,
    shader_s: str,
    log_level: int = 2,
)
```

Runs a GLSL shader in [mpv syntax](https://mpv.io/manual/master/#options-glsl-shader).

Takes a YUVxxxP16 clips as input and outputs YUV444P16.
This is necessitated by the fundamental design of libplacebo/mpv’s custom shader feature:
the shaders aren’t meant (nor written) to be run by themselves,
but to be injected at arbitrary points into a [rendering pipeline](https://github.com/mpv-player/mpv/wiki/Video-output---shader-stage-diagram) with RGB output.
As such, the user needs to specify the output frame properties,
and libplacebo will produce a conforming image,
only running the supplied shader if the texture it hooks into is actually rendered.
For example, if a shader hooks into the LINEAR texture,
it will only be executed when `linearize = True`.

- `shader`: Path to shader file.
- `shader_s`: Alternatively, string containing the shader. (`shader` takes precedence.)
- `width, height`: Output dimensions. Need to be specified for scaling shaders to be run.
  Any planes the shader doesn’t scale appropriately will be scaled to output res by libplacebo
  using the supplied filter options, which are identical to `Resample`’s.
  (To be exact, chroma will be scaled to what the luma prescaler outputs
  (or the source luma res); then the image will be scaled to output res in RGB and converted back to YUV.)
- `chroma_loc`: Chroma location to derive chroma shift from. Uses [pl_chroma_location](https://github.com/haasn/libplacebo/blob/524e3965c6f8f976b3f8d7d82afe3083d61a7c4d/src/include/libplacebo/colorspace.h#L332) enum values.
- `matrix`: [YUV matrix](https://github.com/haasn/libplacebo/blob/524e3965c6f8f976b3f8d7d82afe3083d61a7c4d/src/include/libplacebo/colorspace.h#L26).
- `sigmoidize, linearize, sigmoid_center, sigmoid_slope, trc`: For shaders that hook into the LINEAR or SIGMOID texture.

## Debugging `libplacebo` processing

All the filters can take a `log_level` argument. Defaults to 2, meaning only
errors are logged.

| Value | Description |
| ----- | ----------- |
| 0     | None        |
| 1     | Fatal       |
| 2     | Error       |
| 3     | Warn        |
| 4     | Info        |
| 5     | Debug       |
| 6     | Trace       |
| 7     | All         |

## Installing

If you’re on Arch, just do

```bash
$ yay -S vapoursynth-plugin-placebo-git
```

Building on Linux using meson:

```bash
$ meson setup build
$ ninja -C build
```

It is not recommended to install the library on the system without using a package manager.  
Otherwise it's as simple as `DESTDIR= ninja -C build install`.

Building on Linux for Windows:  
Some experimental build system based on `mpv-winbuild-cmake`: https://github.com/quietvoid/mpv-winbuild-cmake/commits/vs-placebo-libdovi  
Suggested to use on Arch Linux. YMMV.
