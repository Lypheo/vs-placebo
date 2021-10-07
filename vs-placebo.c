#include <vapoursynth/VapourSynth4.h>

#include <stdlib.h>
#include <stdio.h>

#include <libplacebo/dispatch.h>
#include <libplacebo/vulkan.h>

#include "render.h"
#include "vs-placebo.h"


int vs_to_pl_matrix(int matrix) {
    switch (matrix) {
        case 0:
            return PL_COLOR_SYSTEM_RGB;
        case 1:
            return PL_COLOR_SYSTEM_BT_709;
        case 5:
        case 6:
            return PL_COLOR_SYSTEM_BT_601;
        case 7:
            return PL_COLOR_SYSTEM_SMPTE_240M;
        case 8:
            return PL_COLOR_SYSTEM_YCGCO;
        case 9:
            return PL_COLOR_SYSTEM_BT_2020_NC;
        case 10:
            return PL_COLOR_SYSTEM_BT_2020_C;
        case 14:
            return PL_COLOR_SYSTEM_BT_2100_PQ; // TODO: differentiate between PQ and HLG

        default:
            return PL_COLOR_SYSTEM_UNKNOWN;

    }
}

int vs_to_pl_trc(int trc) {
    switch (trc) {
        case 1:
        case 6:
        case 14:
        case 15:
            return PL_COLOR_TRC_BT_1886;
        case 4:
            return PL_COLOR_TRC_GAMMA22;
        case 5:
            return PL_COLOR_TRC_GAMMA28;
        case 8:
            return PL_COLOR_TRC_LINEAR;
        case 16:
            return PL_COLOR_TRC_PQ;
        case 18:
            return PL_COLOR_TRC_HLG;
        case 13:
            return PL_COLOR_TRC_SRGB;
        default:
            return PL_COLOR_SYSTEM_UNKNOWN;
    }
}

int vs_to_pl_prm(int prim) {
    switch (prim) {
        case 1:
            return PL_COLOR_PRIM_BT_709;
        case 2:
            return PL_COLOR_PRIM_UNKNOWN;
        case 4:
            return PL_COLOR_PRIM_BT_470M;
        case 5:
            return PL_COLOR_PRIM_BT_601_625;
        case 6:
        case 7:
            return PL_COLOR_PRIM_BT_601_525;
        case 8:
            return PL_COLOR_PRIM_FILM_C;
        case 9:
            return PL_COLOR_PRIM_BT_2020;
        case 11:
            return PL_COLOR_PRIM_DCI_P3;
        case 12:
            return PL_COLOR_PRIM_DISPLAY_P3;
        default:
            return PL_COLOR_PRIM_UNKNOWN;
    }
}

void *init(void) {
    struct priv *p = calloc(1, sizeof(struct priv));
    if (!p)
        return NULL;

    p->log = pl_log_create(PL_API_VER, &(struct pl_log_params) {
            .log_cb = pl_log_simple,
            .log_level = PL_LOG_WARN,
            .log_priv = stdout,
    });

    p->vk = pl_vulkan_create(p->log, NULL);

    if (!p->vk) {
        fprintf(stderr, "Failed creating vulkan context\n");
        goto error;
    }

    p->gpu = p->vk->gpu;

    p->dp = pl_dispatch_create(p->log, p->gpu);
    if (!p->dp) {
        fprintf(stderr, "Failed creating shader dispatch object\n");
        goto error;
    }

    p->rr = pl_renderer_create(p->log, p->gpu);

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

    pl_shader_obj_destroy(&p->dither_state);
    pl_dispatch_destroy(&p->dp);
    pl_vulkan_destroy(&p->vk);
    pl_log_destroy(&p->log);
    pl_renderer_destroy(&p->rr);
    free(p);
}

VS_EXTERNAL_API(void) VapourSynthPluginInit2(VSPlugin *plugin, const VSPLUGINAPI *vspapi) {
    vspapi->configPlugin("com.vs.placebo", "placebo", "libplacebo plugin for VapourSynth", VS_MAKE_VERSION(1, 0), VAPOURSYNTH_API_VERSION, 0, plugin);
    vspapi->registerFunction("Render", "clip:vnode;", "clip:vnode;", RCreate, NULL, plugin);
}

//VS_EXTERNAL_API(void) VapourSynthPluginInit(VSConfigPlugin configFunc, VSRegisterFunction registerFunc, VSPlugin *plugin) {
//    configFunc("com.vs.placebo", "placebo", "libplacebo plugin for VapourSynth", VAPOURSYNTH_API_VERSION, 1, plugin);
//    registerFunc("Deband", "clip:clip;planes:int:opt;iterations:int:opt;threshold:float:opt;radius:float:opt;grain:float:opt;dither:int:opt;dither_algo:int:opt;renderer_api:int:opt", DebandCreate, 0, plugin);
//    registerFunc("Resample", "clip:clip;width:int;height:int;filter:data:opt;clamp:float:opt;blur:float:opt;taper:float:opt;radius:float:opt;param1:float:opt;param2:float:opt;"
//                             "sx:float:opt;sy:float:opt;antiring:float:opt;lut_entries:int:opt;cutoff:float:opt;"
//                             "sigmoidize:int:opt;sigmoid_center:float:opt;sigmoid_slope:float:opt;linearize:int:opt;trc:int:opt", ResampleCreate, 0, plugin);
//    registerFunc("Tonemap", "clip:clip;"
//                            "srcp:int:opt;srct:int:opt;srcl:int:opt;src_peak:float:opt;src_avg:float:opt;src_scale:float:opt;"
//                            "dstp:int:opt;dstt:int:opt;dstl:int:opt;dst_peak:float:opt;dst_avg:float:opt;dst_scale:float:opt;"
//                            "dynamic_peak_detection:int:opt;smoothing_period:float:opt;scene_threshold_low:float:opt;scene_threshold_high:float:opt;"
//                            "intent:int:opt;"
//                            "tone_mapping_algo:int:opt;tone_mapping_param:float:opt;desaturation_strength:float:opt;desaturation_exponent:float:opt;desaturation_base:float:opt;"
//                            "max_boost:float:opt;gamut_warning:int:opt;gamut_clipping:int:opt"
//                            , TMCreate, 0, plugin);
//    registerFunc("Shader", "clip:clip;shader:data:opt;width:int:opt;height:int:opt;chroma_loc:int:opt;matrix:int:opt;trc:int:opt;"
//                           "linearize:int:opt;sigmoidize:int:opt;sigmoid_center:float:opt;sigmoid_slope:float:opt;"
//                           "lut_entries:int:opt;antiring:float:opt;"
//                           "filter:data:opt;clamp:float:opt;blur:float:opt;taper:float:opt;radius:float:opt;param1:float:opt;param2:float:opt;shader_s:data:opt;", SCreate, 0, plugin);
//}
