#include "vs-placebo.h"

#include <stdlib.h>
#include <stdio.h>
#include "VapourSynth.h"
#include <stdbool.h>
#include <libplacebo/dispatch.h>
#include <libplacebo/utils/upload.h>
#include <libplacebo/vulkan.h>
#include "libp2p/p2p_api.h"

typedef  struct {
    VSNodeRef *node;
    const VSVideoInfo *vi;
    struct priv * vf;
    struct pl_render_params *renderParams;
    struct pl_color_space *src_csp;
    struct pl_color_space *dst_csp;
} TMData;

bool do_plane_TM(struct priv *p, void* data, int n)
{
    TMData* d = (TMData*) data;
    struct pl_plane planes[3];
    for (int j = 0; j < 3; ++j) {
         planes[j] = (struct pl_plane) {.texture = p->tex_in[j], .components = 1, .component_mapping[0] = j};
    }
    static const struct pl_color_repr crpr = {.bits = {.sample_depth = 16, .color_depth = 16, .bit_shift = 0},
                                              .levels = PL_COLOR_LEVELS_PC, .alpha = PL_ALPHA_UNKNOWN, .sys = PL_COLOR_SYSTEM_RGB};

    struct pl_image img = {.signature = n, .num_planes = 3, .width = d->vi->width, .height = d->vi->height,
                           .planes[0] = planes[0], .planes[1] = planes[1], .planes[2] = planes[2],
                           .repr = crpr, .color = *d->src_csp};
    struct pl_render_target out = {.color = *d->dst_csp, .repr = crpr, .fbo = p->tex_out[0]};
    return pl_render_image(p->rr, &img, &out, d->renderParams);
}

bool config_TM(void *priv, struct pl_plane_data *data, const VSAPI *vsapi)
{
    struct priv *p = priv;

    const struct pl_fmt *fmt = pl_plane_find_fmt(p->gpu, NULL, &data[0]);
    if (!fmt) {
        vsapi->logMessage(mtCritical, "Failed configuring filter: no good texture format!\n");
        return false;
    }

    bool ok = true;
    for (int i = 0; i < 3; ++i) {
        ok &= pl_tex_recreate(p->gpu, &p->tex_in[i], &(struct pl_tex_params) {
                .w = data->width,
                .h = data->height,
                .format = fmt,
                .sampleable = true,
                .host_writable = true,
                .sample_mode = PL_TEX_SAMPLE_LINEAR,
        });
    }

    struct pl_fmt *out = pl_plane_find_fmt(p->gpu, NULL,
                                           &(struct pl_plane_data) {.type = PL_FMT_UNORM, .component_map[0] = 0, .component_map[1] = 1, .component_map[2] = 2,
                                                   .component_pad[0] = 0, .component_pad[1] = 0, .component_pad[2] = 0,
                                                   .component_size[0] = 16, .component_size[1] = 16, .component_size[2] = 16,
                                                   .width = 10, .height = 10, .row_stride = 60, .pixel_stride = 6});

    ok &= pl_tex_recreate(p->gpu, &p->tex_out[0], &(struct pl_tex_params) {
            .w = data->width,
            .h = data->height,
            .format = out,
            .renderable = true,
            .host_readable = true,
    });

    if (!ok) {
        vsapi->logMessage(mtCritical, "Failed creating GPU textures!\n");
        return false;
    }

    return true;
}

bool filter_TM(void *priv, void *dst, struct pl_plane_data *src, void* d, int n, const VSAPI *vsapi)
{
    struct priv *p = priv;

    // Upload planes
    bool ok = true;
    for (int i = 0; i < 3; ++i) {
        ok &= pl_tex_upload(p->gpu, &(struct pl_tex_transfer_params) {
                .tex = p->tex_in[i],
                .stride_w = src->row_stride / src->pixel_stride,
                .ptr = src[i].pixels,
        });
    }
    if (!ok) {
        vsapi->logMessage(mtCritical, "Failed uploading data to the GPU!\n");
        return false;
    }

    // Process plane
    if (!do_plane_TM(p, d, n)) {
        vsapi->logMessage(mtCritical, "Failed processing planes!\n");
        return false;
    }

    // Download planes
    ok = pl_tex_download(p->gpu, &(struct pl_tex_transfer_params) {
            .tex = p->tex_out[0],
            .stride_w = src->row_stride / src->pixel_stride,
            .ptr = dst,
    });

    if (!ok) {
        vsapi->logMessage(mtCritical, "Failed downloading data from the GPU!\n");
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

        int stride = vsapi->getStride(frame, 0);
        struct pl_plane_data planes[3];
        for (int j = 0; j < 3; ++j) {
            planes[j] = (struct pl_plane_data) {
                    .type = PL_FMT_UNORM,
                    .width = iw,
                    .height = ih,
                    .pixel_stride = 1 /* components */ * 2 /* bytes per sample*/,
                    .row_stride =  stride,
                    .pixels =  vsapi->getWritePtr(frame, j),
            };

            planes[j].component_size[0] = 16;
            planes[j].component_pad[0] = 0;
            planes[j].component_map[0] = j;
        }

        void * packed_dst = malloc(iw*ih*2*3);

        if (config_TM(d->vf, planes, vsapi))
            filter_TM(d->vf, packed_dst, planes, d, n, vsapi);

        struct p2p_buffer_param pack_params = {};
        pack_params.width = iw; pack_params.height = ih;
        pack_params.packing = p2p_bgr48_le;
        pack_params.src[0] = packed_dst;
        pack_params.src_stride[0] = iw*2*3;
        for (int k = 0; k < 3; ++k) {
            pack_params.dst[k] = vsapi->getWritePtr(dst, k);
            pack_params.dst_stride[k] = stride;
        }
        p2p_unpack_frame(&pack_params, 0);

        free(packed_dst);

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

    COLORM_PARAM(tone_mapping_algo, Int)
    COLORM_PARAM(tone_mapping_param, Float)
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

    vsapi->createFilter(in, out, "Tonemap", TMInit, TMGetFrame, TMFree, peak_detection ? fmSerial : fmUnordered, 0, data, core);
}
