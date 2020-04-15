# vs-placebo


#### ``placebo.Deband(clip clip[, int planes = 1, int iterations = 1, float threshold = 4.0, float radius = 16.0, float grain = 6.0, int dither = True, int dither_algo = 0])``

Input needs to be 8 or 16 bit.

- ``planes``: the planes to filter. The n-th plane is processed if the n-th lowest bit of ``planes`` is 1, so for example to filter all planes, pass ``planes = 1 | 2 | 4`` .
(Yes, this is needlessly complex, but it was the simplest to implement.)

- ``dither``: whether the debanded frame should be dithered or rounded from float to the output bitdepth.

For details on the [debanding params](https://github.com/haasn/libplacebo/blob/master/src/include/libplacebo/shaders/sampling.h#L39)
and the [dither methods](https://github.com/haasn/libplacebo/blob/master/src/include/libplacebo/shaders/colorspace.h#L275),
see the libplacebo header files.

#### ``placebo.Tonemap(clip clip[, int srcp, int srct, int srcl, float src_peak, float src_avg, float src_scale, int dstp, int dstt, int dstl, float dst_peak, float dst_avg, float dst_scale, int dynamic_peak_detection, float smoothing_period, float scene_threshold_low, scene_threshold_high, int intent, int tone_mapping_algo, float tone_mapping_param, float desaturation_strength, float desaturation_exponent, float desaturation_base, float max_boost, int gamut_warning])``
Performs color mapping (which includes tonemapping from HDR to SDR, but can do a lot more). Expects **16 bit RGB**.

- ``srcp, srct, srcl, dstp, dstt, dstl, src_peak, src_avg, src_scale, dst_peak, dst_avg, dst_scale``:
See the [documentation](https://github.com/haasn/libplacebo/blob/master/src/include/libplacebo/colorspace.h#L244) in the header file.
For example, to map from [BT.2020, PQ\] (HDR) to traditional [BT.709, BT.1886\] (SDR), pass ``srcp=5, dstp=2, srct=8, dstt=1``. 
(If you want equivalent output to mpv, pass ``dstt=0``.)  
- ``dynamic_peak_detection``: enables computation of signal stats to optimize HDR tonemapping quality. Enabled by default.
- ``smoothing_period, scene_threshold_low, scene_threshold_high``: peak detection params. See [here](https://github.com/haasn/libplacebo/blob/master/src/include/libplacebo/shaders/colorspace.h#L85).
- ``tone_mapping_algo, tone_mapping_param, desaturation_strength, desaturation_exponent, desaturation_base, max_boost, gamut_warning``:
 [Color mapping params](https://github.com/haasn/libplacebo/blob/master/src/include/libplacebo/shaders/colorspace.h#L199).

#### ``placebo.Resample(clip clip[, int width, int height, string filter = "ewa_lanczos", float sx = 0.0, float sy = 0.0, float antiring = 0.0, int lut_entries = 64, float cutoff = 0.001])``

- ``filter``: See [the header](https://github.com/haasn/libplacebo/blob/210131146739e4e84d689f32c17a97b27a6550bd/src/include/libplacebo/filters.h#L187) for possible values (remove the “pl_filter” before the filter name, e.g. ``filter="lanczos"``).
Advanced configuration can be added on request.
- ``sx``, ``sy``: subpixel shifts