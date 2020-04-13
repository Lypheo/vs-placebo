#include "../VapourSynth.h"
#include "deband.h"
#include "tonemap.h"

#include <stdlib.h>
#include <stdio.h>
#include <math.h>

#include "libplacebo/dispatch.h"
#include "libplacebo/shaders/sampling.h"
#include "libplacebo/utils/upload.h"
#include "libplacebo/vulkan.h"

#include "vs-placebo.h"

void *init(void) {
    struct priv *p = calloc(1, sizeof(struct priv));
    if (!p)
        return NULL;

    p->ctx = pl_context_create(PL_API_VER, NULL);
    if (!p->ctx) {
        fprintf(stderr, "Failed initializing libplacebo\n");
        goto error;
    }

    p->vk = pl_vulkan_create(p->ctx, NULL);

    if (!p->vk) {
        fprintf(stderr, "Failed creating vulkan context\n");
        goto error;
    }

    // Give this a shorter name for convenience
    p->gpu = p->vk->gpu;

    p->dp = pl_dispatch_create(p->ctx, p->gpu);
    if (!p->dp) {
        fprintf(stderr, "Failed creating shader dispatch object\n");
        goto error;
    }

    p->rr = pl_renderer_create(p->ctx, p->gpu);
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
    pl_context_destroy(&p->ctx);

    free(p);
}

VS_EXTERNAL_API(void) VapourSynthPluginInit(VSConfigPlugin configFunc, VSRegisterFunction registerFunc, VSPlugin *plugin) {
    configFunc("com.vs.placebo", "placebo", "libplacebo plugin for VapourSynth", VAPOURSYNTH_API_VERSION, 1, plugin);
    registerFunc("Deband", "clip:clip;planes:int:opt;iterations:int:opt;threshold:float:opt;radius:float:opt;grain:float:opt;dither:int:opt;dither_algo:int:opt", DebandCreate, 0, plugin);
    registerFunc("Tonemap", "clip:clip;"
                            "srcp:int:opt;srct:int:opt;srcl:int:opt;src_peak:float:opt;src_avg:float:opt;src_scale:float:opt;"
                            "dstp:int:opt;dstt:int:opt;dstl:int:opt;dst_peak:float:opt;dst_avg:float:opt;dst_scale:float:opt;"
                            "dynamic_peak_detection:int:opt;smoothing_period:float:opt;scene_threshold_low:float:opt;scene_threshold_high:float:opt;"
                            "intent:int:opt;"
                            "tone_mapping_algo:int:opt;tone_mapping_param:float:opt;desaturation_strength:float:opt;desaturation_exponent:float:opt;desaturation_base:float:opt;max_boost:float:opt;gamut_warning:int:opt;"
                            , TMCreate, 0, plugin);
}
