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

#include "vs-placebo.h"
#include "../libp2p/p2p_api.h"

typedef  struct {
    VSNodeRef *node;
    const VSVideoInfo *vi;
    struct priv * vf;
} TMData;

bool do_plane_TM(struct priv *p, const struct pl_tex *dst, const struct pl_tex *src, void* data, int n)
{
    TMData* d = (TMData*) data;
    struct pl_plane plane = {.texture = src, .components = 4};
    for (int i = 0; i < 4 ; ++i) {
        plane.component_mapping[i] = i;
    }
    static const struct pl_color_repr crpr = {.bits = {.sample_depth = 16, .color_depth = 16, .bit_shift = 0}, .levels = PL_COLOR_LEVELS_PC, .alpha = PL_ALPHA_INDEPENDENT, .sys = PL_COLOR_SYSTEM_RGB};
    static const struct pl_color_space csp = {.primaries = PL_COLOR_PRIM_BT_2020, .transfer = PL_COLOR_TRC_PQ, .light =  PL_COLOR_LIGHT_DISPLAY};
    static const struct pl_color_space csp_out = {.primaries = PL_COLOR_PRIM_BT_709, .transfer = PL_COLOR_TRC_UNKNOWN, .light = PL_COLOR_LIGHT_DISPLAY};
    struct pl_image img = {.signature = n, .num_planes = 1, .width = d->vi->width, .height = d->vi->height, .planes[0] = plane, .repr = crpr, .color = csp};
    struct pl_render_target out = {.color = csp_out, .repr = crpr, .fbo = dst};
    struct pl_render_params renderParams = pl_render_default_params;
    return pl_render_image(p->rr, &img, &out, &renderParams);
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

        void* packed_src = malloc(ih * iw * 4 * d->vi->format->bytesPerSample);
        void* packed_dst = malloc(ih * iw * 4 * d->vi->format->bytesPerSample);

        struct p2p_buffer_param pack_params = {};
        pack_params.width = iw; pack_params.height = ih;
        pack_params.packing = p2p_abgr64_le;
        for (int j = 0; j < 3; ++j) {
            pack_params.src_stride[j] = vsapi->getStride(frame, j);;
            pack_params.src[j] = vsapi->getWritePtr(frame, j);
        }
        pack_params.src[3] = NULL;
        pack_params.dst_stride[0] = iw * 4 * 2;
        pack_params.dst[0] = packed_src;

        p2p_pack_frame(&pack_params, P2P_ALPHA_SET_ONE);

        struct pl_plane_data plane = (struct pl_plane_data) {
                .type = PL_FMT_UNORM,
                .width = iw,
                .height = ih,
                .pixel_stride = 4 /* components */ * 2 /* bytes per sample*/,
                .row_stride =  pack_params.dst_stride[0],
                .pixels =  pack_params.dst[0],
        };

        for (int c = 0; c < 4 /* components */; c++) {
            plane.component_size[c] = 16; //bitdepth
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
    free(d);
}

void VS_CC TMCreate(const VSMap *in, VSMap *out, void *userData, VSCore *core, const VSAPI *vsapi) {
    TMData d;
    TMData *data;
    int err;

    d.node = vsapi->propGetNode(in, "clip", 0, 0);
    d.vi = vsapi->getVideoInfo(d.node);

    if ((d.vi->format->bitsPerSample != 8 && d.vi->format->bitsPerSample != 16) || d.vi->format->sampleType != stInteger || d.vi->format->colorFamily != cmRGB) {
        vsapi->setError(out, "placebo.Tonemap: Input should be 16 bit RGB!.");
    }

    d.vf = init();

    data = malloc(sizeof(d));
    *data = d;

    vsapi->createFilter(in, out, "Tonemap", TMInit, TMGetFrame, TMFree, fmSerial, 0, data, core);
}
