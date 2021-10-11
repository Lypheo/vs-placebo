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
    int renderer; // for debugging purposes
    pthread_mutex_t lock;
} MData;

bool do_plane(struct priv *p, void* data, int chroma)
{
    MData* d = (MData*) data;

    if (!d->renderer) {

        struct pl_shader *sh = pl_dispatch_begin(p->dp);
        int new_depth = p->tex_out[0]->params.format->component_depth[0];
        pl_shader_deband(sh, &(struct pl_sample_src) {.tex = p->tex_in[0]},
                         d->debandParams);
        if (d->dither)
            pl_shader_dither(sh, new_depth, &p->dither_state, d->ditherParams);
        return pl_dispatch_finish(p->dp, &(struct pl_dispatch_params) {
            .shader = &sh,
            .target = p->tex_out[0],
        });

    } else {
        struct pl_color_repr crpr = {
            .bits = {
                .sample_depth = d->vi->format->bytesPerSample * 8,
                .color_depth = d->vi->format->bytesPerSample * 8,
                .bit_shift = 0
            },
            .levels = PL_COLOR_LEVELS_UNKNOWN,
            .alpha = PL_ALPHA_UNKNOWN,
            .sys = PL_COLOR_SYSTEM_UNKNOWN
        };

        int src_w = d->vi->width >> (chroma ? d->vi->format->subSamplingW : 0);
        int src_h = d->vi->height >> (chroma ? d->vi->format->subSamplingH : 0);

        struct pl_color_space color = (struct pl_color_space) {0};

        struct pl_frame img = {
            .num_planes = 1,
            .planes = {{
                .texture = p->tex_in[0],
                .components = p->tex_in[0]->params.format->num_components,
                .component_mapping = {0, 1, 2, 3},
            }},
            .repr = crpr,
            .color = color,
            .crop = {0, 0, src_w, src_h},
        };

        struct pl_frame out = {
            .num_planes = 1,
            .planes = {{
                .texture = p->tex_out[0],
                .components = p->tex_out[0]->params.format->num_components,
                .component_mapping = {0, 1, 2, 3},
            }},
            .repr = crpr,
            .color = color,
            .crop = {0, 0, src_w, src_h},
        };

        pl_rect2df_aspect_copy(&out.crop, &img.crop, 0.0);

        struct pl_render_params par = pl_render_default_params;
        par.deband_params = d->debandParams;
        par.dither_params = d->dither ? d->ditherParams : NULL;

        return pl_render_image(p->rr, &img, &out, &par);

    }
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

bool filter(void *priv, void *dst, struct pl_plane_data *src, void* d, const VSAPI *vsapi, int chroma)
{
    struct priv *p = priv;

    // Upload planes
    bool ok = true;
    ok &= pl_tex_upload(p->gpu, &(struct pl_tex_transfer_params) {
        .tex = p->tex_in[0],
        .stride_w = src->row_stride / src->pixel_stride,
        .ptr = (void *) src->pixels,
    });

    if (!ok) {
        vsapi->logMessage(mtCritical, "Failed uploading data to the GPU!\n");
        return false;
    }

    // Process plane
    if (!do_plane(p, d, chroma)) {
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

        int copy[3];
        for (unsigned int j = 0; j < 3; ++j)
            copy[j] = ((1u << j) & d->planes) == 0;

        VSFrameRef *dst = vsapi->newVideoFrame(d->vi->format, iw, ih, frame, core);

        for (int i=0; i<d->vi->format->numPlanes; i++) {
            if (copy[i]) {
                vs_bitblt(vsapi->getWritePtr(dst, i), vsapi->getStride(dst, i), vsapi->getWritePtr((VSFrameRef *) frame, i),
                          vsapi->getStride(frame, i), vsapi->getFrameWidth(dst, i) * d->vi->format->bytesPerSample,
                          vsapi->getFrameHeight(dst, i));
            } else {
                struct pl_plane_data plane = {
                        .type = d->vi->format->sampleType == stInteger ? PL_FMT_UNORM : PL_FMT_FLOAT,
                        .width = vsapi->getFrameWidth(frame, i),
                        .height = vsapi->getFrameHeight(frame, i),
                        .pixel_stride = 1 /* components */ * d->vi->format->bytesPerSample /* bytes per sample*/,
                        .row_stride =  vsapi->getStride(frame, i),
                        .pixels =  vsapi->getWritePtr((VSFrameRef *) frame, i),
                        .component_size[0] = d->vi->format->bitsPerSample,
                        .component_pad[0] = 0,
                        .component_map[0] = 0,
                };
                pthread_mutex_lock(&d->lock); // libplacebo isn’t thread-safe
                if (reconfig(d->vf, &plane, vsapi))
                    filter(d->vf, vsapi->getWritePtr(dst, i), &plane, d, vsapi, i != 0);
                pthread_mutex_unlock(&d->lock);
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

    d.renderer = vsapi->propGetInt(in, "renderer_api", 0, &err);
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
    data = malloc(sizeof(d));
    *data = d;

    vsapi->createFilter(in, out, "Deband", DebandInit, DebandGetFrame, DebandFree, fmParallel, 0, data, core);
}