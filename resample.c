#include "vs-placebo.h"
#include <stdlib.h>
#include <stdio.h>
#include "VapourSynth.h"
#include "VSHelper.h"
#include <stdbool.h>
#include <libplacebo/dispatch.h>
#include <libplacebo/utils/upload.h>
#include <libplacebo/filters.h>

typedef struct {
    VSNodeRef *node;
    const VSVideoInfo *vi;
    void * vf;
    int width;
    int height;
    struct pl_sample_filter_params *sampleParams;
    struct pl_shader_obj *lut;
} RData;

bool do_plane_R(struct priv *p, void* data, int w, int h, const VSAPI *vsapi)
{
    struct pl_shader *sh = pl_dispatch_begin(p->dp);
    const struct pl_tex *sep_fbo = NULL;
    struct pl_sample_src src = (struct pl_sample_src){ .tex = p->tex_in[0], .new_h = h, .new_w = w};
    RData* d = (RData*) data;
    struct pl_sample_filter_params sampleFilterParams = *d->sampleParams;
    sampleFilterParams.lut = &d->lut;
    if (d->sampleParams->filter.polar) {
        if (!pl_shader_sample_polar(sh, &src, &sampleFilterParams))
            vsapi->logMessage(mtCritical, "Failed dispatching scaler...\n");
    } else {
        struct pl_shader *tsh = pl_dispatch_begin_ex(p->dp, true);
        if (!pl_shader_sample_ortho(tsh, PL_SEP_VERT, &src, &sampleFilterParams)) {
            vsapi->logMessage(mtCritical, "Failed vertical pass!\n");
            pl_dispatch_abort(p->dp, &tsh);
        }
        struct pl_sample_src src2 = src;
        struct pl_tex_params tex_params = {.w = src.tex->params.w, .h = src.new_h, .renderable = true, .sampleable = true, .format = src.tex->params.format, .sample_mode = PL_TEX_SAMPLE_LINEAR};
        if (!pl_tex_recreate(p->gpu, &sep_fbo, &tex_params)) {
            vsapi->logMessage(mtCritical, "failed creating intermediate texture!\n");
        }
        src2.tex = sep_fbo;
        if (!pl_dispatch_finish(p->dp, &tsh, sep_fbo, NULL, NULL)) {
            vsapi->logMessage(mtCritical, "Failed dispatching intermediate pass! \n");
            return false;
        }
        if (!pl_shader_sample_ortho(sh, PL_SEP_HORIZ, &src2, &sampleFilterParams)){
            vsapi->logMessage(mtCritical, "Failed horizontal pass! \n");
        }
    }
    bool ok = pl_dispatch_finish(p->dp, &sh, p->tex_out[0], NULL, NULL);
    pl_tex_destroy(p->gpu, &sep_fbo);
    return ok;
}

bool reconfig_R(void *priv, struct pl_plane_data *data, int w, int h, const VSAPI *vsapi)
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
            .w = w,
            .h = h,
            .format = fmt,
            .renderable = true,
            .host_readable = true,
            .storable = true,
    });

    if (!ok) {
        vsapi->logMessage(mtCritical, "Failed creating GPU textures!\n");
        return false;
    }

    return true;
}

bool filter_R(void *priv, void *dst, struct pl_plane_data *src, void* d, int w, int h, int dst_stride, const VSAPI *vsapi)
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
    if (!do_plane_R(p, d, w, h, vsapi)) {
        vsapi->logMessage(mtCritical, "Failed processing planes!\n");
        return false;
    }

    // Download planes
    ok = pl_tex_download(p->gpu, &(struct pl_tex_transfer_params) {
            .tex = p->tex_out[0],
            .stride_w = dst_stride,
            .ptr = dst,
    });

    if (!ok) {
        vsapi->logMessage(mtCritical, "Failed downloading data from the GPU!\n");
        return false;
    }

    return true;
}

static void VS_CC ResampleInit(VSMap *in, VSMap *out, void **instanceData, VSNode *node, VSCore *core, const VSAPI *vsapi) {
    RData *d = (RData *) * instanceData;
    VSVideoInfo new_vi = (VSVideoInfo) * (d->vi);
    new_vi.width = d->width;
    new_vi.height = d->height;
    vsapi->setVideoInfo(&new_vi, 1, node);
}

static const VSFrameRef *VS_CC ResampleGetFrame(int n, int activationReason, void **instanceData, void **frameData, VSFrameContext *frameCtx, VSCore *core, const VSAPI *vsapi) {
    RData *d = (RData *) * instanceData;

    if (activationReason == arInitial) {
        vsapi->requestFrameFilter(n, d->node, frameCtx);
    } else if (activationReason == arAllFramesReady) {
        const VSFrameRef *frame = vsapi->getFrameFilter(n, d->node, frameCtx);

        VSFrameRef *dst = vsapi->newVideoFrame(d->vi->format, d->width, d->height, frame, core);

        for (unsigned int i=0; i < d->vi->format->numPlanes; i++) {
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

            int w = vsapi->getFrameWidth(dst, i), h = vsapi->getFrameHeight(dst, i);
            if (reconfig_R(d->vf, &plane, w, h, vsapi))
                filter_R(d->vf, vsapi->getWritePtr(dst, i), &plane, d, w, h, vsapi->getStride(dst, i) / d->vi->format->bytesPerSample, vsapi);

        }

        vsapi->freeFrame(frame);
        return dst;
    }

    return 0;
}

static void VS_CC ResampleFree(void *instanceData, VSCore *core, const VSAPI *vsapi) {
    RData *d = (RData *)instanceData;
    vsapi->freeNode(d->node);
    pl_shader_obj_destroy(&d->lut);
    free(d->sampleParams);
    uninit(d->vf);
    free(d);
}

void VS_CC ResampleCreate(const VSMap *in, VSMap *out, void *userData, VSCore *core, const VSAPI *vsapi) {
    RData d;
    RData *data;
    int err;

    d.node = vsapi->propGetNode(in, "clip", 0, 0);
    d.vi = vsapi->getVideoInfo(d.node);

    if ((d.vi->format->bitsPerSample != 8 && d.vi->format->bitsPerSample != 16) || d.vi->format->sampleType != stInteger) {
        vsapi->setError(out, "placebo.Rsample: Input bitdepth should be 8 or 16!.");
        vsapi->freeNode(d.node);
    }

    d.vf = init();

    d.width = vsapi->propGetInt(in, "width", 0, &err);
    if (err)
        d.width = d.vi->width;

    d.height = vsapi->propGetInt(in, "height", 0, &err);
    if (err)
        d.height = d.vi->height;

    struct pl_sample_filter_params *sampleFilterParams = malloc(sizeof(struct pl_sample_filter_params));;

    d.lut = NULL;
    sampleFilterParams->no_widening = false;
    sampleFilterParams->no_compute = false;
    sampleFilterParams->lut_entries = vsapi->propGetInt(in, "lut_entries", 0, &err);
    sampleFilterParams->cutoff = vsapi->propGetFloat(in, "cutoff", 0, &err);
    sampleFilterParams->antiring = vsapi->propGetFloat(in, "antiring", 0, &err);
    char * filter = vsapi->propGetData(in, "filter", 0, &err);
    if (!filter) filter = "ewa_lanczos";
#define FILTER_ELIF(name) else if (strcmp(filter, #name) == 0) sampleFilterParams->filter = pl_filter_##name;
    if (strcmp(filter, "spline16") == 0)
        sampleFilterParams->filter = pl_filter_spline16;
    FILTER_ELIF(spline36)
    FILTER_ELIF(spline64)
    FILTER_ELIF(box)
    FILTER_ELIF(triangle)
    FILTER_ELIF(gaussian)
    FILTER_ELIF(sinc)
    FILTER_ELIF(lanczos)
    FILTER_ELIF(ginseng)
    FILTER_ELIF(ewa_jinc)
    FILTER_ELIF(ewa_ginseng)
    FILTER_ELIF(ewa_hann)
    FILTER_ELIF(haasnsoft)
    FILTER_ELIF(bicubic)
    FILTER_ELIF(mitchell)
    FILTER_ELIF(robidoux)
    FILTER_ELIF(robidouxsharp)
    FILTER_ELIF(ewa_robidoux)
    FILTER_ELIF(ewa_lanczos)
    FILTER_ELIF(ewa_robidouxsharp)
    else {
        vsapi->logMessage(mtWarning, "Unkown filter... selecting ewa_lanczos.\n");
        sampleFilterParams->filter = pl_filter_ewa_lanczos;
    }

    d.sampleParams = sampleFilterParams;
    data = malloc(sizeof(d));
    *data = d;
    vsapi->createFilter(in, out, "Resample", ResampleInit, ResampleGetFrame, ResampleFree, fmUnordered, 0, data, core);
}