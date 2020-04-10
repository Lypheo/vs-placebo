//
// Created by saifu on 4/6/2020.
//

#include "deband.h"
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

typedef struct {
    VSNodeRef *node;
    const VSVideoInfo *vi;
    void * vf;
    unsigned int planes;
    float threshold;
    int iterations;
    float radius;
    float grain;
    int dither_algo;
    int dither;
} MData;

void setup_plane_data(const struct image *img,
                      struct pl_plane_data out[MAX_PLANES])
{
    for (int i = 0; i < img->num_planes; i++) {
        const struct plane *plane = &img->planes[i];

        out[i] = (struct pl_plane_data) {
                .type = PL_FMT_UNORM,
                .width = img->width >> plane->subx,
                .height = img->height >> plane->suby,
                .pixel_stride = plane->fmt.num_comps * plane->fmt.bitdepth / 8,
                .row_stride = plane->stride,
                .pixels = plane->data,
        };

        for (int c = 0; c < plane->fmt.num_comps; c++) {
            out[i].component_size[c] = plane->fmt.bitdepth;
            out[i].component_pad[c] = 0;
            out[i].component_map[c] = c;
        }
    }
}

bool do_plane(struct priv *p, const struct pl_tex *dst, const struct pl_tex *src, void* data)
{
    struct pl_shader *sh = pl_dispatch_begin(p->dp);
    MData* d = (MData*) data;
    int new_depth = dst->params.format->component_depth[0];
    pl_shader_deband(sh, &(struct pl_sample_src){ .tex = src },
                     &(struct pl_deband_params) {.iterations = d->iterations, .threshold = d->threshold, .radius = d->radius, .grain = d->grain});
    if (d->dither)
        pl_shader_dither(sh, new_depth, &p->dither_state, &(struct pl_dither_params) {.method = d->dither_algo,});

    return pl_dispatch_finish(p->dp, &sh, dst, NULL, NULL);
}

bool reconfig(void *priv, const struct image *proxy)
{
    struct priv *p = priv;
    struct pl_plane_data data[MAX_PLANES];
    setup_plane_data(proxy, data);

    for (int i = 0; i < proxy->num_planes; i++) {
        const struct pl_fmt *fmt = pl_plane_find_fmt(p->gpu, NULL, &data[i]);
        if (!fmt) {
            fprintf(stderr, "Failed configuring filter: no good texture format!\n");
            return false;
        }

        bool ok = true;
        ok &= pl_tex_recreate(p->gpu, &p->tex_in[i], &(struct pl_tex_params) {
                .w = data[i].width,
                .h = data[i].height,
                .format = fmt,
                .sampleable = true,
                .host_writable = true,
                .sample_mode = PL_TEX_SAMPLE_LINEAR,
        });

        ok &= pl_tex_recreate(p->gpu, &p->tex_out[i], &(struct pl_tex_params) {
                .w = data[i].width,
                .h = data[i].height,
                .format = fmt,
                .renderable = true,
                .host_readable = true,
        });

        if (!ok) {
            fprintf(stderr, "Failed creating GPU textures!\n");
            return false;
        }
    }

    return true;
}

bool filter(void *priv, struct image *dst, struct image *src, void* d)
{
    struct priv *p = priv;
    struct pl_plane_data data[MAX_PLANES];
    setup_plane_data(src, data);

    // Upload planes
    for (int i = 0; i < src->num_planes; i++) {
        bool ok = pl_tex_upload(p->gpu, &(struct pl_tex_transfer_params) {
                .tex = p->tex_in[i],
                .stride_w = data[i].row_stride / data[i].pixel_stride,
                .ptr = src->planes[i].data,
        });

        if (!ok) {
            fprintf(stderr, "Failed uploading data to the GPU!\n");
            return false;
        }
    }

    // Process planes
    for (int i = 0; i < src->num_planes; i++) {
        if (!do_plane(p, p->tex_out[i], p->tex_in[i], d)) {
            fprintf(stderr, "Failed processing planes!\n");
            return false;
        }
    }

    // Download planes
    for (int i = 0; i < src->num_planes; i++) {
        bool ok = pl_tex_download(p->gpu, &(struct pl_tex_transfer_params) {
                .tex = p->tex_out[i],
                .stride_w = dst->planes[i].stride / data[i].pixel_stride,
                .ptr = dst->planes[i].data,
        });

        if (!ok) {
            fprintf(stderr, "Failed downloading data from the GPU!\n");
            return false;
        }
    }

    return true;
}

// -----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
// Deband

static void VS_CC DebandInit(VSMap *in, VSMap *out, void **instanceData, VSNode *node, VSCore *core, const VSAPI *vsapi) {
    MData *d = (MData *) * instanceData;
    VSVideoInfo new_vi = (VSVideoInfo) * (d->vi);
    vsapi->setVideoInfo(&new_vi, 1, node);
}

static const VSFrameRef *VS_CC DebandGetFrame(int n, int activationReason, void **instanceData, void **frameData, VSFrameContext *frameCtx, VSCore *core, const VSAPI *vsapi) {
    MData *d = (MData *) * instanceData;

    if (activationReason == arInitial) {
        vsapi->requestFrameFilter(n, d->node, frameCtx);
    } else if (activationReason == arAllFramesReady) {
        const VSFrameRef *frame = vsapi->getFrameFilter(n, d->node, frameCtx);

        int ih = vsapi->getFrameHeight(frame, 0);
        int iw = vsapi->getFrameWidth(frame, 0);

        VSFrameRef *dst = vsapi->newVideoFrame(d->vi->format, iw, ih, frame, core);

        for (unsigned int i=0; i<3; i++) {
            if (!((1u << i) & d->planes))
                vs_bitblt(vsapi->getWritePtr(dst, i), vsapi->getStride(dst, i), vsapi->getWritePtr(frame, i),
                          vsapi->getStride(frame, i), ((unsigned int) iw >> (unsigned int) d->vi->format->subSamplingW) * d->vi->format->bytesPerSample,
                          vsapi->getFrameHeight(dst, i));
            else {
                struct image proxy = {
                        .width = vsapi->getFrameWidth(frame, i), .height = vsapi->getFrameHeight(frame, i), .num_planes = 1,
                        .planes = { { .subx = 0, .suby = 0, .stride = vsapi->getStride(frame, i), .fmt = { .num_comps = 1, .bitdepth = d->vi->format->bytesPerSample * 8, }, }, },
                };
                struct image src = proxy, out = proxy;
                src.planes[0].data = vsapi->getWritePtr(frame, i);
                out.planes[0].data = vsapi->getWritePtr(dst, i);

                if (reconfig(d->vf, &proxy))
                    filter(d->vf, &out, &src, d);
            }
        }

        vsapi->freeFrame(frame);
        return dst;
    }

    return 0;
}

static void VS_CC DebandFree(void *instanceData, VSCore *core, const VSAPI *vsapi) {
    MData *d = (MData *)instanceData;
    vsapi->freeNode(d->node);
    uninit(d->vf);
    free(d);
}

void VS_CC DebandCreate(const VSMap *in, VSMap *out, void *userData, VSCore *core, const VSAPI *vsapi) {
    MData d;
    MData *data;
    int err;

    d.node = vsapi->propGetNode(in, "clip", 0, 0);
    d.vi = vsapi->getVideoInfo(d.node);

    if ((d.vi->format->bitsPerSample != 8 && d.vi->format->bitsPerSample != 16) || d.vi->format->sampleType != stInteger) {
        vsapi->setError(out, "placebo.Deband: Input bitdepth should be 8 or 16!.");
    }

    d.vf = init();

    d.dither = vsapi->propGetInt(in, "dither", 0, &err);
    if (err)
        d.dither = 1;

    d.dither_algo = vsapi->propGetInt(in, "dither_algo", 0, &err);
    if (err)
        d.dither_algo = 1;

    d.planes = (unsigned int) vsapi->propGetInt(in, "planes", 0, &err);
    if (err)
        d.planes = 1u;

    d.iterations = vsapi->propGetInt(in, "iterations", 0, &err);
    if (err)
        d.iterations = 1;

    d.threshold = vsapi->propGetFloat(in, "threshold", 0, &err);
    if (err)
        d.threshold = 30.f;

    d.grain = vsapi->propGetFloat(in, "grain", 0, &err);
    if (err)
        d.grain = 6.f;

    d.radius = vsapi->propGetFloat(in, "radius", 0, &err);
    if (err)
        d.radius = 16.f;

    data = malloc(sizeof(d));
    *data = d;

    vsapi->createFilter(in, out, "Deband", DebandInit, DebandGetFrame, DebandFree, fmUnordered, 0, data, core);
}