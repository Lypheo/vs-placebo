#include "vs-placebo.h"
#include <stdlib.h>
#include <stdio.h>
#include "VapourSynth.h"
#include "VSHelper.h"
#include <stdbool.h>
#include <libplacebo/dispatch.h>
#include <libplacebo/utils/upload.h>
#include <pthread.h>

typedef struct {
    VSNodeRef *node;
    const VSVideoInfo *vi;
    void * vf;
    unsigned int planes;
    int dither;
    struct pl_dither_params *ditherParams;
    struct pl_deband_params *debandParams;
    pthread_mutex_t lock;
} MData;

bool do_plane(struct priv *p, void* data, void* process[3])
{
    MData* d = (MData*) data;
    bool ok = true;
    for (int i=0; i < 3; i++) {
        if (!process[i]) {
            continue;
        }
        struct pl_shader *sh = pl_dispatch_begin(p->dp);
        int new_depth = p->tex_out[i]->params.format->component_depth[0];
        pl_shader_deband(sh, &(struct pl_sample_src) {.tex = p->tex_in[i]},
                         d->debandParams);
        if (d->dither)
            pl_shader_dither(sh, new_depth, &p->dither_state, d->ditherParams);
        ok &= pl_dispatch_finish(p->dp, &(struct pl_dispatch_params) {
                .shader = &sh,
                .target = p->tex_out[i],
        });
    }
    return ok;
}
bool filter(void *priv, void *dst[3], struct pl_plane_data src[3], void* d, const VSAPI *vsapi)
{
    struct priv *p = priv;

    // confgiure textures
    const struct pl_fmt *fmt = pl_plane_find_fmt(p->gpu, NULL, &src[0]);
    if (!fmt) {
        vsapi->logMessage(mtCritical, "Failed configuring filter: no good texture format!\n");
        return false;
    }

    bool ok = true;
    for (int i = 0; i < 3; ++i) {
        if (!dst[i])
            continue;
        ok &= pl_tex_recreate(p->gpu, &p->tex_in[i], &(struct pl_tex_params) {
                .w = src[i].width,
                .h = src[i].height,
                .format = fmt,
                .sampleable = true,
                .host_writable = true,
                .sample_mode = PL_TEX_SAMPLE_LINEAR,
        });
        ok &= pl_tex_recreate(p->gpu, &p->tex_out[i], &(struct pl_tex_params) {
                .w = src[i].width,
                .h = src[i].height,
                .format = fmt,
                .renderable = true,
                .host_readable = true,
        });
    }

    if (!ok) {
        vsapi->logMessage(mtCritical, "Failed creating GPU textures!\n");
        return false;
    }

    // Upload planes
    for (int i = 0; i < 3; ++i) {
        if (!dst[i])
            continue;
        ok &= pl_tex_upload(p->gpu, &(struct pl_tex_transfer_params) {
                .tex = p->tex_in[i],
                .stride_w = src[i].row_stride / src[i].pixel_stride,
                .ptr = src[i].pixels,
        });
    }

    if (!ok) {
        vsapi->logMessage(mtCritical, "Failed uploading data to the GPU!\n");
        return false;
    }

    // Process planes
    if (!do_plane(p, d, dst)) {
        vsapi->logMessage(mtCritical, "Failed processing planes!\n");
        return false;
    }

    // Download planes
    for (int i = 0; i < 3; ++i) {
        if (!dst[i])
            continue;
        ok = pl_tex_download(p->gpu, &(struct pl_tex_transfer_params) {
                .tex = p->tex_out[i],
                .stride_w = src[i].row_stride / src[i].pixel_stride,
                .ptr = dst[i],
        });
    }

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

        int copy[3] = {1,1,1};
        for (unsigned int j = 0; j < d->vi->format->numPlanes; ++j)
            copy[j] = ((1u << j) & d->planes) == 0;

        VSFrameRef *dst = vsapi->newVideoFrame(d->vi->format, iw, ih, frame, core);
        struct pl_plane_data planes[3] = {0};
        void *dst_planes[3] = {NULL, NULL, NULL};

        for (int i=0; i<d->vi->format->numPlanes; i++) {
            if (copy[i]) {
                vs_bitblt(vsapi->getWritePtr(dst, i), vsapi->getStride(dst, i), vsapi->getWritePtr(frame, i),
                          vsapi->getStride(frame, i), vsapi->getFrameWidth(dst, i) * d->vi->format->bytesPerSample,
                          vsapi->getFrameHeight(dst, i));
            } else {
                planes[i] = (struct pl_plane_data) {
                        .type = d->vi->format->sampleType == stInteger ? PL_FMT_UNORM : PL_FMT_FLOAT,
                        .width = vsapi->getFrameWidth(frame, i),
                        .height = vsapi->getFrameHeight(frame, i),
                        .pixel_stride = 1 /* components */ * d->vi->format->bytesPerSample /* bytes per sample*/,
                        .row_stride =  vsapi->getStride(frame, i),
                        .pixels =  vsapi->getWritePtr(frame, i),
                        .component_size[0] = d->vi->format->bitsPerSample,
                        .component_pad[0] = 0,
                        .component_map[0] = 0,
                };
                dst_planes[i] = vsapi->getWritePtr(dst, i);
            }
        }
        pthread_mutex_lock(&d->lock);
        filter(d->vf, dst_planes, planes, d, vsapi);
        pthread_mutex_unlock(&d->lock);

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
    pthread_mutex_destroy(&d->lock);
    free(d);
}

void VS_CC DebandCreate(const VSMap *in, VSMap *out, void *userData, VSCore *core, const VSAPI *vsapi) {
    MData d;
    MData *data;
    int err;

    if (pthread_mutex_init(&d.lock, NULL) != 0)
    {
        vsapi->setError(out, "placebo.Deband: mutex init failed\n");
        return;
    }

    d.node = vsapi->propGetNode(in, "clip", 0, 0);
    d.vi = vsapi->getVideoInfo(d.node);

    if ((d.vi->format->bitsPerSample != 8 && d.vi->format->bitsPerSample != 16 && d.vi->format->bitsPerSample != 32)) {
        vsapi->setError(out, "placebo.Deband: Input bitdepth should be 8, 16 (Integer) or 32 (Float)!.");
        vsapi->freeNode(d.node);
    }

    d.vf = init();

    d.dither = vsapi->propGetInt(in, "dither", 0, &err) && d.vi->format->bitsPerSample == 8;
    if (err)
        d.dither = d.vi->format->bitsPerSample == 8;

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
    data = calloc(1, sizeof(d));
    *data = d;

    vsapi->createFilter(in, out, "Deband", DebandInit, DebandGetFrame, DebandFree, fmParallel, 0, data, core);
}