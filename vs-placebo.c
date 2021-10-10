#include "VapourSynth.h"
#include "deband.h"
#include "tonemap.h"
#include "resample.h"
#include "shader.h"

#include <stdlib.h>
#include <stdio.h>
#include <math.h>

#include <libplacebo/dispatch.h>
#include <libplacebo/utils/upload.h>
#include <libplacebo/vulkan.h>

#include "vs-placebo.h"

void *init(void) {
    struct priv *p = calloc(1, sizeof(struct priv));
    if (!p)
        return NULL;

    p->log = pl_log_create(PL_API_VER, &(struct pl_log_params) {
        .log_cb = pl_log_color,
        .log_level = PL_LOG_ERR
    });

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
    uninit(p);
    return NULL;
}

void uninit(void *priv)
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
    registerFunc("Deband", "clip:clip;planes:int:opt;iterations:int:opt;threshold:float:opt;radius:float:opt;grain:float:opt;dither:int:opt;dither_algo:int:opt;renderer_api:int:opt", DebandCreate, 0, plugin);
    registerFunc("Resample", "clip:clip;width:int;height:int;filter:data:opt;clamp:float:opt;blur:float:opt;taper:float:opt;radius:float:opt;param1:float:opt;param2:float:opt;"
                             "sx:float:opt;sy:float:opt;antiring:float:opt;lut_entries:int:opt;cutoff:float:opt;"
                             "sigmoidize:int:opt;sigmoid_center:float:opt;sigmoid_slope:float:opt;linearize:int:opt;trc:int:opt", ResampleCreate, 0, plugin);
    registerFunc("Tonemap", "clip:clip;"
                            "srcp:int:opt;srct:int:opt;srcl:int:opt;src_peak:float:opt;src_avg:float:opt;src_scale:float:opt;"
                            "dstp:int:opt;dstt:int:opt;dstl:int:opt;dst_peak:float:opt;dst_avg:float:opt;dst_scale:float:opt;"
                            "dynamic_peak_detection:int:opt;smoothing_period:float:opt;scene_threshold_low:float:opt;scene_threshold_high:float:opt;"
                            "intent:int:opt;"
                            "tone_mapping_algo:int:opt;tone_mapping_param:float:opt;desaturation_strength:float:opt;desaturation_exponent:float:opt;desaturation_base:float:opt;"
                            "max_boost:float:opt;gamut_warning:int:opt;gamut_clipping:int:opt"
                            , TMCreate, 0, plugin);
    registerFunc("Shader", "clip:clip;shader:data:opt;width:int:opt;height:int:opt;chroma_loc:int:opt;matrix:int:opt;trc:int:opt;"
                           "linearize:int:opt;sigmoidize:int:opt;sigmoid_center:float:opt;sigmoid_slope:float:opt;"
                           "lut_entries:int:opt;antiring:float:opt;"
                           "filter:data:opt;clamp:float:opt;blur:float:opt;taper:float:opt;radius:float:opt;param1:float:opt;param2:float:opt;shader_s:data:opt;", SCreate, 0, plugin);
}
