#include <stdlib.h>
#include <stdio.h>
#include <math.h>

#include "VapourSynth.h"

#include "vs-placebo.h"
#include "deband.h"
#include "tonemap.h"
#include "resample.h"
#include "shader.h"

void *VSPlaceboInit(enum pl_log_level log_level) {
    struct priv *p = calloc(1, sizeof(struct priv));
    if (!p)
        return NULL;

    p->log = pl_log_create(PL_API_VER, pl_log_params(
        .log_cb = pl_log_color,
        .log_level = log_level
    ));

    if (!p->log) {
        fprintf(stderr, "Failed initializing libplacebo\n");
        goto error;
    }

    struct pl_vulkan_params vp = pl_vulkan_default_params;
    struct pl_vk_inst_params ip = pl_vk_inst_default_params;
//    ip.debug = true;
    vp.instance_params = &ip;
    p->vk = pl_vulkan_create(p->log, &vp);

    if (!p->vk) {
        fprintf(stderr, "Failed creating vulkan context\n");
        goto error;
    }

    // Give this a shorter name for convenience
    p->gpu = p->vk->gpu;

    p->dp = pl_dispatch_create(p->log, p->gpu);
    if (!p->dp) {
        fprintf(stderr, "Failed creating shader dispatch object\n");
        goto error;
    }

    p->rr = pl_renderer_create(p->log, p->gpu);
    if (!p->rr) {
        fprintf(stderr, "Failed creating renderer\n");
        goto error;
    }

    return p;

error:
    VSPlaceboUninit(p);
    return NULL;
}

void VSPlaceboUninit(void *priv)
{
    struct priv *p = priv;
    for (int i = 0; i < MAX_PLANES; i++) {
        pl_tex_destroy(p->gpu, &p->tex_in[i]);
        pl_tex_destroy(p->gpu, &p->tex_out[i]);
    }

    pl_renderer_destroy(&p->rr);
    pl_shader_obj_destroy(&p->dither_state);
    pl_dispatch_destroy(&p->dp);
    pl_vulkan_destroy(&p->vk);
    pl_log_destroy(&p->log);

    free(p);
}

VS_EXTERNAL_API(void) VapourSynthPluginInit(VSConfigPlugin configFunc, VSRegisterFunction registerFunc, VSPlugin *plugin) {
    configFunc("com.vs.placebo", "placebo", "libplacebo plugin for VapourSynth", VAPOURSYNTH_API_VERSION, 1, plugin);
    registerFunc("Deband", "clip:clip;planes:int:opt;iterations:int:opt;threshold:float:opt;"
                           "radius:float:opt;grain:float:opt;dither:int:opt;dither_algo:int:opt;"
                           "log_level:int:opt;", VSPlaceboDebandCreate, 0, plugin);

    registerFunc("Resample", "clip:clip;width:int;height:int;filter:data:opt;clamp:float:opt;blur:float:opt;"
                             "taper:float:opt;radius:float:opt;param1:float:opt;param2:float:opt;"
                             "sx:float:opt;sy:float:opt;antiring:float:opt;lut_entries:int:opt;cutoff:float:opt;"
                             "sigmoidize:int:opt;sigmoid_center:float:opt;sigmoid_slope:float:opt;linearize:int:opt;trc:int:opt;"
                             "log_level:int:opt;", VSPlaceboResampleCreate, 0, plugin);

    registerFunc("Tonemap", "clip:clip;"
                            "src_csp:int:opt;dst_csp:int:opt;"
                            "dst_prim:int:opt;"
                            "src_max:float:opt;src_min:float:opt;"
                            "dst_max:float:opt;dst_min:float:opt;"
                            "dynamic_peak_detection:int:opt;smoothing_period:float:opt;"
                            "scene_threshold_low:float:opt;scene_threshold_high:float:opt;"
                            "percentile:float:opt;"
                            "intent:int:opt;"
                            "gamut_mode:int:opt;"
                            "tone_mapping_function:int:opt;tone_mapping_function_s:data:opt;"
                            "tone_mapping_mode:int:opt;"
                            "tone_mapping_param:float:opt;tone_mapping_crosstalk:float:opt;"
                            "metadata:int:opt;"
                            "use_dovi:int:opt;"
                            "visualize_lut:int:opt;show_clipping:int:opt;"
                            "log_level:int:opt;", VSPlaceboTMCreate, 0, plugin);

    registerFunc("Shader", "clip:clip;shader:data:opt;width:int:opt;height:int:opt;chroma_loc:int:opt;matrix:int:opt;trc:int:opt;"
                           "linearize:int:opt;sigmoidize:int:opt;sigmoid_center:float:opt;sigmoid_slope:float:opt;"
                           "lut_entries:int:opt;antiring:float:opt;"
                           "filter:data:opt;clamp:float:opt;blur:float:opt;taper:float:opt;radius:float:opt;"
                           "param1:float:opt;param2:float:opt;shader_s:data:opt;"
                           "log_level:int:opt;", VSPlaceboShaderCreate, 0, plugin);
}
