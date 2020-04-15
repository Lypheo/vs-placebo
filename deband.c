#include "vs-placebo.h"
#include <stdlib.h>
#include <stdio.h>
#include "VapourSynth.h"
#include "VSHelper.h"
#include <stdbool.h>
#include <libplacebo/dispatch.h>
#include <libplacebo/utils/upload.h>

typedef struct {
    VSNodeRef *node;
    const VSVideoInfo *vi;
    void * vf;
    unsigned int planes;
    int dither;
    struct pl_dither_params *ditherParams;
    struct pl_deband_params *debandParams;
} MData;

bool do_plane(struct priv *p, void* data)
{
    struct pl_shader *sh = pl_dispatch_begin(p->dp);
    MData* d = (MData*) data;
    int new_depth = p->tex_out[0]->params.format->component_depth[0];
    pl_shader_deband(sh, &(struct pl_sample_src){ .tex = p->tex_in[0]},
                     d->debandParams);
    if (d->dither)
        pl_shader_dither(sh, new_depth, &p->dither_state, d->ditherParams);
    return pl_dispatch_finish(p->dp, &sh, p->tex_out[0], NULL, NULL);
}

bool reconfig(void *priv, struct pl_plane_data *data, const VSAPI *vsapi)
{
    struct priv *p = priv;

    const struct pl_fmt *fmt = pl_plane_find_fmt(p->gpu, NULL, data);
    if (!fmt) {
        vsapi->logMessage(mtCritical, "Failed configuring filter: no good texture format!\n");
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
        vsapi->logMessage(mtCritical, "Failed creating GPU textures!\n");
        return false;
    }

    return true;
}

bool filter(void *priv, void *dst, struct pl_plane_data *src, void* d, const VSAPI *vsapi)
{
    struct priv *p = priv;

    // Upload planes
    bool ok = true;
    ok &= pl_tex_upload(p->gpu, &(struct pl_tex_transfer_params) {
            .tex = p->tex_in[0],
            .stride_w = src->row_stride / src->pixel_stride,
            .ptr = src->pixels,
    });

    if (!ok) {
        vsapi->logMessage(mtCritical, "Failed uploading data to the GPU!\n");
        return false;
    }

    // Process plane
    if (!do_plane(p, d)) {
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

        for (unsigned int i=0; i<d->vi->format->numPlanes; i++) {
            if (!((1u << i) & d->planes))
                vs_bitblt(vsapi->getWritePtr(dst, i), vsapi->getStride(dst, i), vsapi->getWritePtr(frame, i),
                          vsapi->getStride(frame, i), ((unsigned int) iw >> (unsigned int) d->vi->format->subSamplingW) * d->vi->format->bytesPerSample,
                          vsapi->getFrameHeight(dst, i));
            else {
                struct pl_plane_data plane = {
                        .type = PL_FMT_UNORM,
                        .width = vsapi->getFrameWidth(frame, i),
                        .height = vsapi->getFrameHeight(frame, i),
                        .pixel_stride = 1 /* components */ * d->vi->format->bytesPerSample /* bytes per sample*/,
                        .row_stride =  vsapi->getStride(frame, i),
                        .pixels =  vsapi->getWritePtr(frame, i),
                        .component_size[0] = d->vi->format->bitsPerSample,
                        .component_pad[0] = 0,
                        .component_map[0] = 0,
                };

                if (reconfig(d->vf, &plane, vsapi))
                    filter(d->vf, vsapi->getWritePtr(dst, i), &plane, d, vsapi);
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
    free(d->ditherParams);
    free(d->debandParams);
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
        vsapi->freeNode(d.node);
    }

    d.vf = init();

    d.dither = vsapi->propGetInt(in, "dither", 0, &err);
    if (err)
        d.dither = 1;

    d.planes = (unsigned int) vsapi->propGetInt(in, "planes", 0, &err);
    if (err)
        d.planes = 1u;

    struct pl_deband_params *debandParams = malloc(sizeof(struct pl_deband_params));
#define DB_PARAM(par, type) debandParams->par = vsapi->propGet##type(in, #par, 0, &err); \
        if (err) debandParams->par = pl_deband_default_params.par;

    DB_PARAM(iterations, Int)
    DB_PARAM(threshold, Float)
    DB_PARAM(grain, Float)
    DB_PARAM(radius, Float)

    struct pl_dither_params *plDitherParams = malloc(sizeof(struct pl_dither_params));
    *plDitherParams = pl_dither_default_params;
    plDitherParams->method = vsapi->propGetInt(in, "dither_algo", 0, &err);
    if (err)
        plDitherParams->method = pl_dither_default_params.method;

    d.ditherParams = plDitherParams;
    d.debandParams = debandParams;
    data = malloc(sizeof(d));
    *data = d;

    vsapi->createFilter(in, out, "Deband", DebandInit, DebandGetFrame, DebandFree, fmUnordered, 0, data, core);
}