# vs-placebo
### A VapourSynth plugin interface to [libplacebo](https://code.videolan.org/videolan/libplacebo).

&nbsp;

#### `placebo.Deband(clip clip[, int planes = 1, int iterations = 1, float threshold = 4.0, float radius = 16.0, float grain = 6.0, int dither = True, int dither_algo = 0])`

Input needs to be 8 or 16 bit Integer or 32 bit Float.

- `planes`: the planes to filter. The n-th plane is processed if the n-th lowest bit of `planes` is 1, so for example to filter all planes, pass `planes = 1 | 2 | 4` .
(Yes, this is needlessly complex, but it was the simplest to implement.)

- `dither`: whether the debanded frame should be dithered or rounded from float to the output bitdepth. Only works for 8 bit.

For details on the [debanding params](https://github.com/haasn/libplacebo/blob/master/src/include/libplacebo/shaders/sampling.h#L39)
and the [dither methods](https://github.com/haasn/libplacebo/blob/master/src/include/libplacebo/shaders/colorspace.h#L275),
see the libplacebo header files.

&nbsp;

#### `placebo.Tonemap(clip clip[, int src_csp, int dst_csp, int dst_prim, float src_max, float src_min, float dst_max, float dst_min, int dynamic_peak_detection, float smoothing_period, float scene_threshold_low, scene_threshold_high, int intent, int tone_mapping_function, int tone_mapping_mode, float tone_mapping_param, float tone_mapping_crosstalk, bool use_dovi, bool visualize_lut])`

Performs color mapping (which includes tonemapping from HDR to SDR, but can do a lot more).  
Expects RGB48 or YUVxxxP16 input.  
Outputs RGB48 or YUV444P16, depending on input color family.

- `src_csp, dst_csp`:  
See the `supported_colorspace` in `tonemap.c` for the valid src/dst colorspaces.  
For example, to map from [BT.2020, PQ] (HDR) to traditional [BT.709, BT.1886] (SDR), pass `src_csp=1, dst_csp=0`.
- `dst_prim`: Target color primaries. See [pl_color_primaries](https://github.com/haasn/libplacebo/blob/master/src/include/libplacebo/colorspace.h#L193) for valid values.
- `src_max, src_min, dst_max, dst_min`: Source/target display levels, in nits (cd/m^2). Source can be derived from props if available.

- `dynamic_peak_detection`: enables computation of signal stats to optimize HDR tonemapping quality. Enabled by default.
- `smoothing_period, scene_threshold_low, scene_threshold_high, percentile`: peak detection params. See [here](https://github.com/haasn/libplacebo/blob/master/src/include/libplacebo/shaders/colorspace.h#L103).
    - `percentile` only in v5.264.0+.
- `tone_mapping_function, tone_mapping_mode, tone_mapping_param, tone_mapping_crosstalk, metadata`:
 [Color mapping params](https://github.com/haasn/libplacebo/blob/master/src/include/libplacebo/shaders/colorspace.h#L261).
- `tone_mapping_function_s`: Tone mapping function name, overwrites `tone_mapping_function` number.
- `use_dovi`: Whether to use the Dolby Vision RPU for ST2086 metadata. Defaults to true when tonemapping from Dolby Vision.
- `visualize_lut`: Display a (PQ-PQ) graph of the active tone-mapping LUT. See [mpv docs](https://mpv.io/manual/master/#options-tone-mapping-visualize).
- `show_clipping`: Highlight hard-clipped pixels during tone-mapping

For Dolby Vision support, FFmpeg 5.0 minimum and git ffms2 are required.

&nbsp;

**Supported frame props**
- `PLSceneMax`, `PLSceneAvg`: Per-scene dynamic brightness metadata, in nits (cd/m^2).
    - `float[] PLSceneMax`: the scene's peak brightness. Can be specified by component (RGB) or a single value.
    - `float PLSceneAvg`: the scene's average brightness.

    Requires `libplacebo` v5.246.0 or newer, otherwise ignored.  
    If `PLSceneMax` is per component, the metadata is set to `scene_max` and `scene_avg`.
    If it's a single luminance value and v5.257.0+, sets `max_pq_y` and `avg_pq_y`.

    If `use_dovi` is enabled, `max_pq_y` and `avg_pq_y` are derived from the Dolby Vision RPU L1 metadata instead of the props.

&nbsp;

#### `placebo.Resample(clip clip[, int width, int height, string filter = "ewa_lanczos", float radius, float clamp, float taper, float blur, float param1, float param2, float sx = 0.0, float sy = 0.0, float antiring = 0.0, int lut_entries = 64, float cutoff = 0.001, bool sigmoidize = 1, bool linearize = 1, float sigmoid_center = 0.75, float sigmoid_slope = 6.5, int trc = 1])`

Input needs to be 8 or 16 bit Integer or 32 bit Float   

- `filter`: See [the header](https://github.com/haasn/libplacebo/blob/210131146739e4e84d689f32c17a97b27a6550bd/src/include/libplacebo/filters.h#L187) for possible values (remove the “pl_filter” before the filter name, e.g. `filter="lanczos"`).  
- `sx`, `sy`: Top left corner of the source region. Can be used for subpixel shifts
- `clamp, taper, blur`: [Filter config](https://github.com/haasn/libplacebo/blob/885e89bccfb932d9e8c8659039ab6975e885e996/src/include/libplacebo/filters.h#L148).

- `radius, param1, param2`: [Kernel config](https://github.com/haasn/libplacebo/blob/885e89bccfb932d9e8c8659039ab6975e885e996/src/include/libplacebo/filters.h#L30-L131).
- `sigmoidize, linearize`: Whether to linearize/sigmoidize before scaling.
Enabled by default for RGB, disabled for YCbCr because NCL YCbCr can’t be correctly linearized without conversion to RGB.
Defaults to disabled for GRAY since it may be a YCbCr plane, but can be manually enabled. 
When sigmodizing, `linearize` should be True as well. (Currently mangles HDR video, so disable for that.) 
- `sigmoid_center, sigmoid_slope`: Sigmoid curve parameters.
- `trc`: The [transfer curve](https://github.com/haasn/libplacebo/blob/master/src/include/libplacebo/colorspace.h#L183) to use for linearizing.

&nbsp;

#### `placebo.Shader(clip clip, [string shader, int width, int height, int chroma_loc = 1, int matrix = 2, int trc = 1, string filter = "ewa_lanczos", float radius, float clamp, float taper, float blur, float param1, float param2, float antiring = 0.0, int lut_entries = 64, float cutoff = 0.001, bool sigmoidize = 1, bool linearize = 1, float sigmoid_center = 0.75, float sigmoid_slope = 6.5, string shader_s])`

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
- `shader_s`: Alternatively, String containing the shader. (`shader` takes precedence.)
- `width, height`: Output dimensions. Need to be specified for scaling shaders to be run. 
Any planes the shader doesn’t scale appropiately will be scaled to output res by libplacebo
using the supplied filter options, which are identical to `Resample`’s.
(To be exact, chroma will be scaled to what the luma prescaler outputs
(or the source luma res); then the image will be scaled to output res in RGB and converted back to YUV.)
- `chroma_loc`: Chroma location to derive chroma shift from. Uses [pl_chroma_location](https://github.com/haasn/libplacebo/blob/524e3965c6f8f976b3f8d7d82afe3083d61a7c4d/src/include/libplacebo/colorspace.h#L332) enum values.
- `matrix`: [YUV matrix](https://github.com/haasn/libplacebo/blob/524e3965c6f8f976b3f8d7d82afe3083d61a7c4d/src/include/libplacebo/colorspace.h#L26).
- `sigmoidize, linearize, sigmoid_center, sigmoid_slope, trc`: For shaders that hook into the LINEAR or SIGMOID texture.

&nbsp;

### Debugging `libplacebo` processing

All the filters can take a `log_level` argument corresponding to a `pl_log_level`.  
Defaults to 2, meaning only errors are logged.

&nbsp;

### Installing

If you’re on Arch, just do
```
$ yay -S vapoursynth-plugin-placebo-git
```

&nbsp;

Building on Linux using meson:
```
meson build
ninja -C build
```
It is not recommended to install the library on the system without using a package manager.  
Otherwise it's as simple as `DESTDIR= ninja -C build install`.

&nbsp;

Building on Linux for Windows:  
Some experimental build system based on `mpv-winbuild-cmake`: https://github.com/quietvoid/mpv-winbuild-cmake/commits/vs-placebo-libdovi  
Suggested to use on Arch Linux. YMMV.
