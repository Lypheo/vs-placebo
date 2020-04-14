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

bool do_plane_R(struct priv *p, void* data, int w, int h)
{
    struct pl_shader *sh = pl_dispatch_begin(p->dp);
    struct pl_shader_obj *lut = NULL;
    RData* d = (RData*) data;
//    pl_shader_sample_bicubic(sh, &(struct pl_sample_src){ .tex = p->tex_in[0], .new_h = h, .new_w = w});
//    if (d->sampleParams->filter.polar)

    if (!pl_shader_sample_polar(sh, &(struct pl_sample_src){ .tex = p->tex_in[0], .new_h = h, .new_w = w},
            &(struct pl_sample_filter_params) {.filter = pl_filter_ewa_lanczos, .lut = &lut, .no_compute = false, .no_widening = false}))
        printf("Failed dispatching scaler...");
//    else {
//        pl_shader_sample_ortho(sh, 0, &(struct pl_sample_src) {.tex = p->tex_in[0], .new_h = h, .new_w = w},
//                               d->sampleParams);
//        pl_shader_sample_ortho(sh, 1, &(struct pl_sample_src) {.tex = p->tex_in[0], .new_h = h, .new_w = w},
//                               d->sampleParams);
//    }
    return pl_dispatch_finish(p->dp, &sh, p->tex_out[0], NULL, NULL);
}

bool reconfig_R(void *priv, struct pl_plane_data *data, int w, int h)
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
            .w = w,
            .h = h,
            .format = fmt,
            .renderable = true,
            .host_readable = true,
            .storable = true,
    });

    if (!ok) {
        fprintf(stderr, "Failed creating GPU textures!\n");
        return false;
    }

    return true;
}

bool filter_R(void *priv, void *dst, struct pl_plane_data *src, void* d, int w, int h, int dst_stride)
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
        fprintf(stderr, "Failed uploading data to the GPU!\n");
        return false;
    }
    // Process plane
    if (!do_plane_R(p, d, w, h)) {
        fprintf(stderr, "Failed processing planes!\n");
        return false;
    }

    // Download planes
    ok = pl_tex_download(p->gpu, &(struct pl_tex_transfer_params) {
            .tex = p->tex_out[0],
            .stride_w = dst_stride,
            .ptr = dst,
    });

    if (!ok) {
        fprintf(stderr, "Failed downloading data from the GPU!\n");
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
            if (reconfig_R(d->vf, &plane, w, h))
                filter_R(d->vf, vsapi->getWritePtr(dst, i), &plane, d, w, h, vsapi->getStride(dst, i) / d->vi->format->bytesPerSample);

        }

        vsapi->freeFrame(frame);
        return dst;
    }

    return 0;
}

static void VS_CC ResampleFree(void *instanceData, VSCore *core, const VSAPI *vsapi) {
    RData *d = (RData *)instanceData;
    vsapi->freeNode(d->node);
    uninit(d->vf);
    free(d->sampleParams);
    free(d);
}

void VS_CC ResampleCreate(const VSMap *in, VSMap *out, void *userData, VSCore *core, const VSAPI *vsapi) {
    RData d;
    RData *data;
    int err;

    d.node = vsapi->propGetNode(in, "clip", 0, 0);
    d.vi = vsapi->getVideoInfo(d.node);

    if ((d.vi->format->bitsPerSample != 8 && d.vi->format->bitsPerSample != 16) || d.vi->format->sampleType != stInteger) {
        vsapi->setError(out, "placebo.Deband: Input bitdepth should be 8 or 16!.");
    }

    d.vf = init();

    d.width = vsapi->propGetInt(in, "width", 0, &err);
    if (err)
        d.width = d.vi->width;

    d.height = vsapi->propGetInt(in, "height", 0, &err);
    if (err)
        d.height = d.vi->height;

    struct pl_sample_filter_params *sampleFilterParams = malloc(sizeof(struct pl_sample_filter_params));;
    sampleFilterParams->lut = NULL;

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
        printf("Unkown filter... selecting ewa_lanczos.");
        sampleFilterParams->filter = pl_filter_ewa_lanczos;
    }

    d.sampleParams = sampleFilterParams;
    data = malloc(sizeof(d));
    *data = d;
    vsapi->createFilter(in, out, "Resample", ResampleInit, ResampleGetFrame, ResampleFree, fmUnordered, 0, data, core);
}