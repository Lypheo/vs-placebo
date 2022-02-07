#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

#include "VapourSynth.h"
#include "VSHelper.h"

#include "vs-placebo.h"

typedef struct {
    VSNodeRef *node;
    const VSVideoInfo *vi;
    void * vf;
    unsigned int planes;
    int dither;
    struct pl_dither_params *ditherParams;
    struct pl_deband_params *debandParams;
    uint8_t frame_index;

    pthread_mutex_t lock;
} DebandData;

bool vspl_deband_do_plane(struct priv *p, void* data)
{
    DebandData* d = (DebandData*) data;

    pl_shader sh = pl_dispatch_begin(p->dp);
    pl_shader_reset(sh, pl_shader_params(
        .gpu = p->gpu,
        .index = d->frame_index++,
    ));

    struct pl_sample_src *src = pl_sample_src(
        .tex = p->tex_in[0]
    );

    int new_depth = p->tex_out[0]->params.format->component_depth[0];

    pl_shader_deband(sh, src, d->debandParams);

    if (d->dither)
        pl_shader_dither(sh, new_depth, &p->dither_state, d->ditherParams);

    return pl_dispatch_finish(p->dp, pl_dispatch_params(
        .target = p->tex_out[0],
        .shader = &sh,
    ));
}

bool vspl_deband_reconfig(void *priv, const struct pl_plane_data *data, const VSAPI *vsapi, VSFrameRef *dst, int planeIdx)
{
    struct priv *p = priv;

    const struct pl_fmt *fmt = pl_plane_find_fmt(p->gpu, NULL, data);
    if (!fmt) {
        vsapi->logMessage(mtCritical, "Failed configuring filter: no good texture format!\n");
        return false;
    }

    bool ok = true;
    ok &= pl_tex_recreate(p->gpu, &p->tex_in[0], pl_tex_params(
        .w = data->width,
        .h = data->height,
        .format = fmt,
        .sampleable = true,
        .host_writable = true,
    ));

    ok &= pl_tex_recreate(p->gpu, &p->tex_out[0], pl_tex_params(
        .w = vsapi->getFrameWidth(dst, planeIdx),
        .h = vsapi->getFrameHeight(dst, planeIdx),
        .format = fmt,
        .renderable = true,
        .host_readable = true,
    ));

    if (!ok) {
        vsapi->logMessage(mtCritical, "Failed creating GPU textures!\n");
        return false;
    }

    return true;
}

bool vspl_deband_filter(void *priv, VSFrameRef *dst, const struct pl_plane_data *data, void* d, const VSAPI *vsapi, int planeIdx)
{
    struct priv *p = priv;
    pl_fmt in_fmt = p->tex_in[0]->params.format;
    pl_fmt out_fmt = p->tex_out[0]->params.format;

    // Upload planes
    bool ok = true;
    ok &= pl_tex_upload(p->gpu, pl_tex_transfer_params(
        .tex = p->tex_in[0],
        .row_pitch = (data->row_stride / data->pixel_stride) * in_fmt->texel_size,
        .ptr = (void *) data->pixels,
    ));

    if (!ok) {
        vsapi->logMessage(mtCritical, "Failed uploading data to the GPU!\n");
        return false;
    }

    // Process plane
    if (!vspl_deband_do_plane(p, d)) {
        vsapi->logMessage(mtCritical, "Failed processing planes!\n");
        return false;
    }

    uint8_t *dst_ptr = vsapi->getWritePtr(dst, planeIdx);
    int dst_row_pitch = (vsapi->getStride(dst, planeIdx) / data->pixel_stride) * out_fmt->texel_size;

    // Download planes
    ok = pl_tex_download(p->gpu, pl_tex_transfer_params(
        .tex = p->tex_out[0],
        .row_pitch = dst_row_pitch,
        .ptr = (void *) dst_ptr,
    ));

    if (!ok) {
        vsapi->logMessage(mtCritical, "Failed downloading data from the GPU!\n");
        return false;
    }

    return true;
}

static void VS_CC VSPlaceboDebandInit(VSMap *in, VSMap *out, void **instanceData, VSNode *node, VSCore *core, const VSAPI *vsapi) {
    DebandData *d = (DebandData *) * instanceData;
    VSVideoInfo new_vi = (VSVideoInfo) * (d->vi);
    vsapi->setVideoInfo(&new_vi, 1, node);
}

static const VSFrameRef *VS_CC VSPlaceboDebandGetFrame(int n, int activationReason, void **instanceData, void **frameData, VSFrameContext *frameCtx, VSCore *core, const VSAPI *vsapi) {
    DebandData *d = (DebandData *) * instanceData;

    if (activationReason == arInitial) {
        vsapi->requestFrameFilter(n, d->node, frameCtx);
    } else if (activationReason == arAllFramesReady) {
        const VSFrameRef *frame = vsapi->getFrameFilter(n, d->node, frameCtx);

        int ih = vsapi->getFrameHeight(frame, 0);
        int iw = vsapi->getFrameWidth(frame, 0);

        int copy[3] = {0};
        for (unsigned int j = 0; j < 3; ++j)
            copy[j] = ((1u << j) & d->planes) == 0;

        const VSFormat *srcFmt = d->vi->format;
        VSFrameRef *dst = vsapi->newVideoFrame(srcFmt, iw, ih, frame, core);

        for (unsigned int i = 0; i < srcFmt->numPlanes; i++) {
            if (copy[i]) {
                vs_bitblt(vsapi->getWritePtr(dst, i), vsapi->getStride(dst, i), vsapi->getWritePtr((VSFrameRef *) frame, i),
                          vsapi->getStride(frame, i), vsapi->getFrameWidth(dst, i) * d->vi->format->bytesPerSample,
                          vsapi->getFrameHeight(dst, i));
            } else {
                const struct pl_plane_data plane = {
                    .type = srcFmt->sampleType == stInteger ? PL_FMT_UNORM : PL_FMT_FLOAT,
                    .width = vsapi->getFrameWidth(frame, i),
                    .height = vsapi->getFrameHeight(frame, i),
                    .pixel_stride = srcFmt->bytesPerSample,
                    .row_stride = vsapi->getStride(frame, i),
                    .pixels = vsapi->getReadPtr((VSFrameRef *) frame, i),
                    .component_size[0] = srcFmt->bitsPerSample,
                    .component_pad[0] = 0,
                    .component_map[0] = 0,
                };

                pthread_mutex_lock(&d->lock); // libplacebo isn’t thread-safe

                if (vspl_deband_reconfig(d->vf, &plane, vsapi, dst, i)) {
                    vspl_deband_filter(d->vf, dst, &plane, d, vsapi, i);
                }

                pthread_mutex_unlock(&d->lock);
            }
        }

        vsapi->freeFrame(frame);
        return dst;
    }

    return 0;
}

static void VS_CC VSPlaceboDebandFree(void *instanceData, VSCore *core, const VSAPI *vsapi) {
    DebandData *d = (DebandData *) instanceData;
    vsapi->freeNode(d->node);
    VSPlaceboUninit(d->vf);
    free(d->ditherParams);
    free(d->debandParams);
    pthread_mutex_destroy(&d->lock);
    free(d);
}

void VS_CC VSPlaceboDebandCreate(const VSMap *in, VSMap *out, void *userData, VSCore *core, const VSAPI *vsapi) {
    DebandData d;
    DebandData *data;
    int err;
    enum pl_log_level log_level;

    if (pthread_mutex_init(&d.lock, NULL) != 0)
    {
        vsapi->setError(out, "placebo.Deband: mutex init failed\n");
        return;
    }

    log_level = vsapi->propGetInt(in, "log_level", 0, &err);
    if (err)
        log_level = PL_LOG_ERR;

    d.node = vsapi->propGetNode(in, "clip", 0, 0);
    d.vi = vsapi->getVideoInfo(d.node);

    if ((d.vi->format->bitsPerSample != 8 && d.vi->format->bitsPerSample != 16 && d.vi->format->bitsPerSample != 32)) {
        vsapi->setError(out, "placebo.Deband: Input bitdepth should be 8, 16 (Integer) or 32 (Float)!.");
        vsapi->freeNode(d.node);
    }

    d.vf = VSPlaceboInit(log_level);

    d.dither = vsapi->propGetInt(in, "dither", 0, &err) && d.vi->format->bitsPerSample == 8;
    if (err)
        d.dither = d.vi->format->bitsPerSample == 8;

    d.planes = (unsigned int) vsapi->propGetInt(in, "planes", 0, &err);
    if (err)
        d.planes = 1u;

    struct pl_deband_params *debandParams = malloc(sizeof(struct pl_deband_params));
    *debandParams = pl_deband_default_params;

#define DB_PARAM(par, type) debandParams->par = vsapi->propGet##type(in, #par, 0, &err); \
        if (err) debandParams->par = pl_deband_default_params.par;

    DB_PARAM(iterations, Int)
    DB_PARAM(threshold, Float)
    DB_PARAM(radius, Float)
    DB_PARAM(grain, Float)

    struct pl_dither_params *plDitherParams = malloc(sizeof(struct pl_dither_params));
    *plDitherParams = pl_dither_default_params;

    plDitherParams->method = vsapi->propGetInt(in, "dither_algo", 0, &err);
    if (err)
        plDitherParams->method = pl_dither_default_params.method;

    d.ditherParams = plDitherParams;
    d.debandParams = debandParams;
    d.frame_index = 0;
    data = malloc(sizeof(d));
    *data = d;

    vsapi->createFilter(in, out, "Deband", VSPlaceboDebandInit, VSPlaceboDebandGetFrame, VSPlaceboDebandFree, fmParallel, 0, data, core);
}
