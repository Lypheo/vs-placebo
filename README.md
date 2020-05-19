# vs-placebo


#### ``placebo.Deband(clip clip[, int planes = 1, int iterations = 1, float threshold = 4.0, float radius = 16.0, float grain = 6.0, int dither = True, int dither_algo = 0])``

Input needs to be 8 or 16 bit Integer or 32 bit Float.

- ``planes``: the planes to filter. The n-th plane is processed if the n-th lowest bit of ``planes`` is 1, so for example to filter all planes, pass ``planes = 1 | 2 | 4`` .
(Yes, this is needlessly complex, but it was the simplest to implement.)

- ``dither``: whether the debanded frame should be dithered or rounded from float to the output bitdepth. Only works for 8 bit.

For details on the [debanding params](https://github.com/haasn/libplacebo/blob/master/src/include/libplacebo/shaders/sampling.h#L39)
and the [dither methods](https://github.com/haasn/libplacebo/blob/master/src/include/libplacebo/shaders/colorspace.h#L275),
see the libplacebo header files.

#### ``placebo.Tonemap(clip clip[, int srcp, int srct, int srcl, float src_peak, float src_avg, float src_scale, int dstp, int dstt, int dstl, float dst_peak, float dst_avg, float dst_scale, int dynamic_peak_detection, float smoothing_period, float scene_threshold_low, scene_threshold_high, int intent, int tone_mapping_algo, float tone_mapping_param, float desaturation_strength, float desaturation_exponent, float desaturation_base, float max_boost, int gamut_warning])``
Performs color mapping (which includes tonemapping from HDR to SDR, but can do a lot more). Expects **16 bit RGB**.

- ``srcp, srct, srcl, dstp, dstt, dstl, src_peak, src_avg, src_scale, dst_peak, dst_avg, dst_scale``:
See the [documentation](https://github.com/haasn/libplacebo/blob/master/src/include/libplacebo/colorspace.h#L244) in the header file.
For example, to map from [BT.2020, PQ\] (HDR) to traditional [BT.709, BT.1886\] (SDR), pass ``srcp=5, dstp=3, srct=8, dstt=1``. 
(If you want equivalent output to mpv, pass ``dstt=0``.)  
- ``dynamic_peak_detection``: enables computation of signal stats to optimize HDR tonemapping quality. Enabled by default.
- ``smoothing_period, scene_threshold_low, scene_threshold_high``: peak detection params. See [here](https://github.com/haasn/libplacebo/blob/master/src/include/libplacebo/shaders/colorspace.h#L85).
- ``tone_mapping_algo, tone_mapping_param, desaturation_strength, desaturation_exponent, desaturation_base, max_boost, gamut_warning``:
 [Color mapping params](https://github.com/haasn/libplacebo/blob/master/src/include/libplacebo/shaders/colorspace.h#L199).

#### ``placebo.Resample(clip clip[, int width, int height, string filter = "ewa_lanczos", float radius, float clamp, float taper, float blur, float param1, float param2, float sx = 0.0, float sy = 0.0, float antiring = 0.0, int lut_entries = 64, float cutoff = 0.001, bool sigmoidize = 1, bool linearize = 1, float sigmoid_center = 0.75, float sigmoid_slope = 6.5, int trc = 1])``

Input needs to be 8 or 16 bit Integer or 32 bit Float   

- ``filter``: See [the header](https://github.com/haasn/libplacebo/blob/210131146739e4e84d689f32c17a97b27a6550bd/src/include/libplacebo/filters.h#L187) for possible values (remove the “pl_filter” before the filter name, e.g. ``filter="lanczos"``).  
- ``sx``, ``sy``: Top left corner of the source region. Can be used for subpixel shifts
- ``clamp, taper, blur``: [Filter config](https://github.com/haasn/libplacebo/blob/885e89bccfb932d9e8c8659039ab6975e885e996/src/include/libplacebo/filters.h#L148).

- ``radius, param1, param2``: [Kernel config](https://github.com/haasn/libplacebo/blob/885e89bccfb932d9e8c8659039ab6975e885e996/src/include/libplacebo/filters.h#L30-L131).
- ``sigmoidize, linearize``: Whether to linearize/sigmoidize before scaling.
When sigmodizing, ``linearize`` should be True as well. (Currently mangles HDR video, so disable for that.) 
- ``sigmoid_center, sigmoid_slope``: Sigmoid curve parameters.
- ``trc``: The [transfer curve](https://github.com/haasn/libplacebo/blob/master/src/include/libplacebo/colorspace.h#L183) to use for linearizing.

In theory, ewa_* filters should be significantly slower than seperable ones,
and disabling linearization/sigmoidization should provide a speed-up,
however in practice they all perform equally since they’re bottlenecked by GPU transfers. 

#### ``placebo.Shader(clip clip, string shader[, int width, int height, int chroma_loc = 1, int matrix = 2, int trc = 1, string filter = "ewa_lanczos", float radius, float clamp, float taper, float blur, float param1, float param2, float antiring = 0.0, int lut_entries = 64, float cutoff = 0.001, bool sigmoidize = 1, bool linearize = 1, float sigmoid_center = 0.75, float sigmoid_slope = 6.5])``

Runs a GLSL shader in [mpv syntax](https://mpv.io/manual/master/#options-glsl-shader).

Takes a YUVxxxP16 clips as input and outputs YUV444P16.
This is necessitated by the fundamental design of libplacebo/mpv’s custom shader feature:
the shaders aren’t meant (nor written) to be run by themselves,
but to be injected at arbitrary points into a [rendering pipeline](https://github.com/mpv-player/mpv/wiki/Video-output---shader-stage-diagram) with RGB output.
As such, the user needs to specify the output frame properties,
and libplacebo will produce a conforming image,
only running the supplied shader if the texture it hooks into is actually rendered.
For example, if a shader hooks into the LINEAR texture,
it will only be executed when ``linearize = True``. 

- ``shader``: Path to shader file.
- ``width, height``: Output dimensions. Need to be specified for scaling shaders to be run. 
Any planes the shader doesn’t scale appropiately will be scaled to output res by libplacebo
using the supplied filter options, which are identical to ``Resample``’s.
(To be exact, chroma will be scaled to what the luma prescaler outputs
(or the source luma res); then the image will be scaled to output res in RGB and converted back to YUV.)
- ``chroma_loc``: Chroma location to derive chroma shift from. Uses [pl_chroma_location](https://github.com/haasn/libplacebo/blob/524e3965c6f8f976b3f8d7d82afe3083d61a7c4d/src/include/libplacebo/colorspace.h#L332) enum values.
- ``matrix``: [YUV matrix](https://github.com/haasn/libplacebo/blob/524e3965c6f8f976b3f8d7d82afe3083d61a7c4d/src/include/libplacebo/colorspace.h#L26).
- ``sigmoidize, linearize, sigmoid_center, sigmoid_slope,trc``: For shaders that hook into the LINEARIZE or SIGMOID texture.