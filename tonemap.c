//
// Created by saifu on 4/6/2020.
//

#include "tonemap.h"
#include "vs-placebo.h"

#include <stdlib.h>
#include <stdio.h>
#include <math.h>
#include "../VapourSynth.h"
#include "../VSHelper.h"

#include <stdbool.h>

#include "libplacebo/dispatch.h"
#include "libplacebo/shaders/sampling.h"
#include "libplacebo/utils/upload.h"
#include "libplacebo/vulkan.h"

#include "../libp2p/p2p_api.h"

typedef  struct {
    VSNodeRef *node;
    const VSVideoInfo *vi;
    struct priv * vf;
    struct pl_render_params *renderParams;
    struct pl_color_space *src_csp;
    struct pl_color_space *dst_csp;
} TMData;

bool do_plane_TM(struct priv *p, const struct pl_tex *dst, const struct pl_tex *src, void* data, int n)
{
    TMData* d = (TMData*) data;
    struct pl_plane plane = {.texture = src, .components = 3};
    for (int i = 0; i < 4 ; ++i)
        plane.component_mapping[i] = i != 3 ? i : -1;
    static const struct pl_color_repr crpr = {.bits = {.sample_depth = 16, .color_depth = 16, .bit_shift = 0}, .levels = PL_COLOR_LEVELS_PC, .alpha = PL_ALPHA_INDEPENDENT, .sys = PL_COLOR_SYSTEM_RGB};

    struct pl_image img = {.signature = n, .num_planes = 1, .width = d->vi->width, .height = d->vi->height, .planes[0] = plane, .repr = crpr, .color = *d->src_csp};
    struct pl_render_target out = {.color = *d->dst_csp, .repr = crpr, .fbo = dst};
    return pl_render_image(p->rr, &img, &out, d->renderParams);
}

bool config_TM(void *priv, struct pl_plane_data *data)
{
    struct priv *p = priv;

    const struct pl_fmt *fmt = pl_plane_find_fmt(p->gpu, NULL, data);
    if (!fmt) {
        fprintf(stderr, "Failed configuring filter: no good texture format!\n");
        return false;
    }

    bool ok = true;
    ok &= pl_tex_recreate(p->gpu, &p->tex_in[0], &(struct pl_tex_params) {
            .w = data->width,
            .h = data->height,
            .format = fmt,
            .sampleable = true,
            .host_writable = true,
            .sample_mode = PL_TEX_SAMPLE_LINEAR,
    });

    ok &= pl_tex_recreate(p->gpu, &p->tex_out[0], &(struct pl_tex_params) {
            .w = data->width,
            .h = data->height,
            .format = fmt,
            .renderable = true,
            .host_readable = true,
    });

    if (!ok) {
        fprintf(stderr, "Failed creating GPU textures!\n");
        return false;
    }

    return true;
}

bool filter_TM(void *priv, struct image *dst, struct pl_plane_data *src, void* d, int n)
{
    struct priv *p = priv;

    // Upload planes
    bool ok = pl_tex_upload(p->gpu, &(struct pl_tex_transfer_params) {
            .tex = p->tex_in[0],
            .stride_w = src->row_stride / src->pixel_stride,
            .ptr = src->pixels,
    });

    if (!ok) {
        fprintf(stderr, "Failed uploading data to the GPU!\n");
        return false;
    }

    // Process plane
    if (!do_plane_TM(p, p->tex_out[0], p->tex_in[0], d, n)) {
        fprintf(stderr, "Failed processing planes!\n");
        return false;
    }

    // Download planes
    ok = pl_tex_download(p->gpu, &(struct pl_tex_transfer_params) {
            .tex = p->tex_out[0],
            .stride_w = src->row_stride / src->pixel_stride,
            .ptr = dst,
    });

    if (!ok) {
        fprintf(stderr, "Failed downloading data from the GPU!\n");
        return false;
    }

    return true;
}

static void VS_CC TMInit(VSMap *in, VSMap *out, void **instanceData, VSNode *node, VSCore *core, const VSAPI *vsapi) {
    TMData *d = (TMData *) * instanceData;
    VSVideoInfo new_vi = (VSVideoInfo) * (d->vi);
    vsapi->setVideoInfo(&new_vi, 1, node);
}

static const VSFrameRef *VS_CC TMGetFrame(int n, int activationReason, void **instanceData, void **frameData, VSFrameContext *frameCtx, VSCore *core, const VSAPI *vsapi) {
    TMData *d = (TMData *) * instanceData;

    if (activationReason == arInitial) {
        vsapi->requestFrameFilter(n, d->node, frameCtx);
    } else if (activationReason == arAllFramesReady) {
        const VSFrameRef *frame = vsapi->getFrameFilter(n, d->node, frameCtx);

        int ih = vsapi->getFrameHeight(frame, 0);
        int iw = vsapi->getFrameWidth(frame, 0);

        VSFrameRef *dst = vsapi->newVideoFrame(d->vi->format, iw, ih, frame, core);

        void* packed_src = malloc(ih * iw * 3 * 2);
        void* packed_dst = malloc(ih * iw * 3 * 2);

        struct p2p_buffer_param pack_params = {};
        pack_params.width = iw; pack_params.height = ih;
        pack_params.packing = p2p_bgr48_le;
        for (int j = 0; j < 3; ++j) {
            pack_params.src_stride[j] = vsapi->getStride(frame, j);;
            pack_params.src[j] = vsapi->getWritePtr(frame, j);
        }
        pack_params.src[3] = NULL;
        pack_params.dst_stride[0] = iw * 3 * 2;
        pack_params.dst[0] = packed_src;

        p2p_pack_frame(&pack_params, 0);

        struct pl_plane_data plane = (struct pl_plane_data) {
                .type = PL_FMT_UNORM,
                .width = iw,
                .height = ih,
                .pixel_stride = 3 /* components */ * 2 /* bytes per sample*/,
                .row_stride =  pack_params.dst_stride[0],
                .pixels =  pack_params.dst[0],
        };

        for (int c = 0; c < 4; c++) {
            plane.component_size[c] = c != 3 ? 16 : 0;
            plane.component_pad[c] = 0;
            plane.component_map[c] = c;
        }

        if (config_TM(d->vf, &plane))
            filter_TM(d->vf, packed_dst, &plane, d, n);

        pack_params.src[0] = packed_dst;
        pack_params.src_stride[0] = pack_params.dst_stride[0];
        for (int k = 0; k < 3; ++k) {
            pack_params.dst[k] = vsapi->getWritePtr(dst, k);
            pack_params.dst_stride[k] = vsapi->getStride(dst, k);
        }
        p2p_unpack_frame(&pack_params, 0);

        free(packed_dst);
        free(packed_src);
        vsapi->freeFrame(frame);
        return dst;
    }

    return 0;
}

static void VS_CC TMFree(void *instanceData, VSCore *core, const VSAPI *vsapi) {
    TMData *d = (TMData *)instanceData;
    vsapi->freeNode(d->node);
    uninit(d->vf);
    free(d->dst_csp);
    free(d->src_csp);
    free(d->renderParams->peak_detect_params);
    free(d->renderParams->color_map_params);
    free(d->renderParams);
    free(d);
}

void VS_CC TMCreate(const VSMap *in, VSMap *out, void *userData, VSCore *core, const VSAPI *vsapi) {
    TMData d;
    TMData *data;
    int err;

    d.node = vsapi->propGetNode(in, "clip", 0, 0);
    d.vi = vsapi->getVideoInfo(d.node);

    if (d.vi->format->colorFamily != cmRGB || d.vi->format->bitsPerSample != 16) {
        vsapi->setError(out, "placebo.Tonemap: Input should be RGB48!.");
        vsapi->freeNode(d.node);
    }

    d.vf = init();
    struct pl_color_map_params *colorMapParams = malloc(sizeof(struct pl_color_map_params));

#define COLORM_PARAM(par, type) colorMapParams->par = vsapi->propGet##type(in, #par, 0, &err); \
        if (err) colorMapParams->par = pl_color_map_default_params.par;

    COLORM_PARAM(tone_mapping_algo, Float)
    COLORM_PARAM(desaturation_base, Float)
    COLORM_PARAM(desaturation_strength, Float)
    COLORM_PARAM(desaturation_exponent, Float)
    COLORM_PARAM(max_boost, Float)
    COLORM_PARAM(gamut_warning, Int)
    COLORM_PARAM(intent, Int)

    struct pl_peak_detect_params *peakDetectParams = malloc(sizeof(struct pl_peak_detect_params));
#define PEAK_PARAM(par, type) peakDetectParams->par = vsapi->propGet##type(in, #par, 0, &err); \
        if (err) peakDetectParams->par = pl_peak_detect_default_params.par;

    PEAK_PARAM(smoothing_period, Float)
    PEAK_PARAM(scene_threshold_low, Float)
    PEAK_PARAM(scene_threshold_high, Float)

    struct pl_color_space *src_csp = malloc((sizeof(struct pl_color_space)));
    *src_csp = (struct pl_color_space) {
            .primaries = vsapi->propGetInt(in, "srcp", 0, &err),
            .transfer = vsapi->propGetInt(in, "srct", 0, &err),
            .light = vsapi->propGetInt(in, "srcl", 0, &err),
            .sig_avg = vsapi->propGetFloat(in, "src_avg", 0, &err),
            .sig_peak = vsapi->propGetFloat(in, "src_peak", 0, &err),
            .sig_scale = vsapi->propGetFloat(in, "src_scale", 0, &err)
    };
    struct pl_color_space *dst_csp = malloc((sizeof(struct pl_color_space)));
    *dst_csp = (struct pl_color_space) {
            .primaries = vsapi->propGetInt(in, "dstp", 0, &err),
            .transfer = vsapi->propGetInt(in, "dstt", 0, &err),
            .light = vsapi->propGetInt(in, "dstl", 0, &err),
            .sig_avg = vsapi->propGetFloat(in, "dst_avg", 0, &err),
            .sig_peak = vsapi->propGetFloat(in, "dst_peak", 0, &err),
            .sig_scale = vsapi->propGetFloat(in, "dst_scale", 0, &err)
    };
    int peak_detection = vsapi->propGetInt(in, "dynamic_peak_detection", 0, &err);
    if (err) peak_detection = 1;

    struct pl_render_params *renderParams = malloc(sizeof(struct pl_render_params));
    *renderParams = pl_render_default_params;
    renderParams->color_map_params = colorMapParams;
    renderParams->peak_detect_params = peak_detection ? peakDetectParams : NULL;
    renderParams->deband_params = NULL;
    renderParams->sigmoid_params = NULL;
    renderParams->cone_params = NULL;
    renderParams->dither_params = NULL;
    d.renderParams = renderParams;
    d.src_csp = src_csp;
    d.dst_csp = dst_csp;

    data = malloc(sizeof(d));
    *data = d;

    vsapi->createFilter(in, out, "Tonemap", TMInit, TMGetFrame, TMFree, fmSerial, 0, data, core);
}
