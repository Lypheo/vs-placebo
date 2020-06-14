#include "vs-placebo.h"
#include <stdlib.h>
#include <stdio.h>
#include "VapourSynth.h"
#include "VSHelper.h"
#include <stdbool.h>
#include <libplacebo/dispatch.h>
#include <libplacebo/utils/upload.h>
#include <libplacebo/filters.h>
#include <libplacebo/colorspace.h>
#include <pthread.h>

typedef struct {
    VSNodeRef *node;
    const VSVideoInfo *vi;
    void * vf;
    int width;
    int height;
    float src_x;
    float src_y;
    enum pl_chroma_location chromaLocation;
    struct pl_sample_filter_params *sampleParams;
    struct pl_shader_obj *lut;
    struct pl_sigmoid_params * sigmoid_params;
    enum pl_color_transfer trc;
    bool linear;
    pthread_mutex_t lock;
} RData;

bool do_plane_R(struct priv *p, void* data, int process[3], const VSAPI *vsapi, float sx[3], float sy[3])
{
    bool ok = true;
    for (int i=0; i < 3; i++) {
        if (!process[i]) {
            continue;
        }

        RData *d = (RData *) data;
        struct pl_shader *sh = pl_dispatch_begin(p->dp);
        const struct pl_tex *sample_fbo = NULL;
        const struct pl_tex *sep_fbo = NULL;
        struct pl_sample_src src = (struct pl_sample_src) {.tex = p->tex_in[i]};
        struct pl_sample_filter_params sampleFilterParams = *d->sampleParams;
        sampleFilterParams.lut = &d->lut;

        //
        // linearization and sigmoidization
        //

        struct pl_shader *ish = pl_dispatch_begin(p->dp);
        struct pl_tex_params tex_params = {.w = src.tex->params.w, .h = src.tex->params.h, .renderable = true, .sampleable = true, .format = src.tex->params.format, .sample_mode = PL_TEX_SAMPLE_LINEAR};

        if (!pl_tex_recreate(p->gpu, &sample_fbo, &tex_params))
            vsapi->logMessage(mtCritical, "failed creating intermediate color texture!\n");

        pl_shader_sample_direct(ish, &src);
        if (d->linear)
            pl_shader_linearize(ish, d->trc);

        if (d->sigmoid_params)
            pl_shader_sigmoidize(ish, d->sigmoid_params);

        if (!pl_dispatch_finish(p->dp, &(struct pl_dispatch_params) {.target = sample_fbo, .shader = &ish})) {
            vsapi->logMessage(mtCritical, "Failed linearizing/sigmoidizing! \n");
            return false;
        }

        //
        // sampling
        //

        struct pl_rect2df rect = {
                -sx[i],
                -sy[i],
                p->tex_in[i]->params.w - sx[i],
                p->tex_in[i]->params.h - sy[i],
        };
        src.tex = sample_fbo;
        src.rect = rect;
        src.new_h = p->tex_out[i]->params.h;
        src.new_w = p->tex_out[i]->params.w;

        if (d->sampleParams->filter.polar) {
            if (!pl_shader_sample_polar(sh, &src, &sampleFilterParams))
                vsapi->logMessage(mtCritical, "Failed dispatching scaler...\n");
        } else {
            struct pl_shader *tsh = pl_dispatch_begin(p->dp);

            if (!pl_shader_sample_ortho(tsh, PL_SEP_VERT, &src, &sampleFilterParams)) {
                vsapi->logMessage(mtCritical, "Failed dispatching vertical pass!\n");
                pl_dispatch_abort(p->dp, &tsh);
            }

            struct pl_sample_src src2 = src;
            struct pl_tex_params tex_params = {.w = src.tex->params.w, .h = src.new_h, .renderable = true, .sampleable = true, .format = src.tex->params.format, .sample_mode = PL_TEX_SAMPLE_LINEAR};

            if (!pl_tex_recreate(p->gpu, &sep_fbo, &tex_params))
                vsapi->logMessage(mtCritical, "failed creating intermediate texture!\n");

            src2.tex = sep_fbo;
            if (!pl_dispatch_finish(p->dp, &(struct pl_dispatch_params) {.target = sep_fbo, .shader = &tsh})) {
                vsapi->logMessage(mtCritical, "Failed rendering vertical pass! \n");
                return false;
            }

            if (!pl_shader_sample_ortho(sh, PL_SEP_HORIZ, &src2, &sampleFilterParams))
                vsapi->logMessage(mtCritical, "Failed dispatching horizontal pass! \n");
        }

        if (d->sigmoid_params)
            pl_shader_unsigmoidize(sh, d->sigmoid_params);

        if (d->linear)
            pl_shader_delinearize(sh, d->trc);


        ok &= pl_dispatch_finish(p->dp, &(struct pl_dispatch_params) {.target = p->tex_out[i], .shader = &sh});
        pl_tex_destroy(p->gpu, &sep_fbo);
        pl_tex_destroy(p->gpu, &sample_fbo);
    }
    return ok;
}

bool filter_R(void *priv, int process[3], struct pl_plane_data dst[3], struct pl_plane_data src[3], void* data, const VSAPI *vsapi, float sx[3], float sy[3])
{
    struct priv *p = priv;
    RData * d = (RData *) data;
    // confgiure textures
    const struct pl_fmt *fmt = pl_plane_find_fmt(p->gpu, NULL, &src[0]);
    if (!fmt) {
        vsapi->logMessage(mtCritical, "Failed configuring filter: no good texture format!\n");
        return false;
    }

    bool ok = true;
    for (int i = 0; i < 3; ++i) {
        if (!process[i])
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
                .w = dst[i].width,
                .h = dst[i].height,
                .format = fmt,
                .renderable = true,
                .host_readable = true,
                .storable = true,
        });
    }

    if (!ok) {
        vsapi->logMessage(mtCritical, "Failed creating GPU textures!\n");
        return false;
    }

    // Upload planes
    for (int i = 0; i < 3; ++i) {
        if (!process[i])
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
    if (!do_plane_R(p, d, process, vsapi, sx, sy)) {
        vsapi->logMessage(mtCritical, "Failed processing planes!\n");
        return false;
    }

    // Download planes
    for (int i = 0; i < 3; ++i) {
        if (!process[i])
            continue;
        ok = pl_tex_download(p->gpu, &(struct pl_tex_transfer_params) {
                .tex = p->tex_out[i],
                .stride_w = dst[i].row_stride / dst[i].pixel_stride,
                .ptr = dst[i].pixels,
        });
    }

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
        struct pl_plane_data planes[3] = {0};
        struct pl_plane_data dst_planes[3] = {0};
        float sx[3], sy[3];
        int process[3] = {1,d->vi->format->numPlanes > 1, d->vi->format->numPlanes > 2};

        for (int i=0; i < d->vi->format->numPlanes; i++) {
            planes[i] = (struct pl_plane_data) {
                    .type = d->vi->format->sampleType == stInteger ? PL_FMT_UNORM : PL_FMT_FLOAT,
                    .width = vsapi->getFrameWidth(frame, i),
                    .height = vsapi->getFrameHeight(frame, i),
                    .pixel_stride = d->vi->format->bytesPerSample,
                    .row_stride =  vsapi->getStride(frame, i),
                    .pixels =  vsapi->getReadPtr(frame, i),
                    .component_size[0] = d->vi->format->bitsPerSample,
                    .component_pad[0] = 0,
                    .component_map[0] = 0,
            };

            dst_planes[i] = (struct pl_plane_data) {
                    .type = d->vi->format->sampleType == stInteger ? PL_FMT_UNORM : PL_FMT_FLOAT,
                    .width = vsapi->getFrameWidth(dst, i),
                    .height = vsapi->getFrameHeight(dst, i),
                    .pixel_stride = d->vi->format->bytesPerSample,
                    .row_stride =  vsapi->getStride(dst, i),
                    .pixels =  vsapi->getWritePtr(dst, i)
            };

            float x_sub_scale = (float) vsapi->getFrameWidth(frame, i) / (float) d->vi->width;
            float y_sub_scale = (float) vsapi->getFrameHeight(frame, i)/ (float) d->vi->height;
            float x_scale = (float) d->vi->width / (float) d->width;
            float y_scale = (float) d->vi->height / (float) d->height;
            if (i > 0 && d->vi->format->colorFamily == cmYUV) {
                pl_chroma_location_offset(d->chromaLocation, &sx[i], &sy[i]);
                sx[i] = (sx[i] * x_sub_scale) * (1 - x_scale);
                sy[i] = (sy[i] * y_sub_scale) * (1 - y_scale);
            } else {
                sx[i] = 0;
                sy[i] = 0;
            }
            sx[i] -= d->src_x * x_sub_scale; // scale shift by subsampling factor for chroma
            sy[i] -= d->src_y * y_sub_scale;
        }

        pthread_mutex_lock(&d->lock);
        filter_R(d->vf, process, dst_planes, planes, d, vsapi, sx, sy);
        pthread_mutex_unlock(&d->lock);

        vsapi->freeFrame(frame);
        return dst;
    }

    return 0;
}

static void VS_CC ResampleFree(void *instanceData, VSCore *core, const VSAPI *vsapi) {
    RData *d = (RData *)instanceData;
    vsapi->freeNode(d->node);
    pl_shader_obj_destroy(&d->lut);
    free(d->sampleParams->filter.kernel);
    free(d->sampleParams);
    free(d->sigmoid_params);
    uninit(d->vf);
    pthread_mutex_destroy(&d->lock);
    free(d);
}

void VS_CC ResampleCreate(const VSMap *in, VSMap *out, void *userData, VSCore *core, const VSAPI *vsapi) {
    RData d;
    RData *data;
    int err;
    if (pthread_mutex_init(&d.lock, NULL) != 0)
    {
        vsapi->setError(out, "placebo.Resample: mutex init failed\n");
        return;
    }

    d.node = vsapi->propGetNode(in, "clip", 0, 0);
    d.vi = vsapi->getVideoInfo(d.node);

    if ((d.vi->format->bitsPerSample != 8 && d.vi->format->bitsPerSample != 16 && d.vi->format->bitsPerSample != 32)) {
        vsapi->setError(out, "placebo.Resample: Input bitdepth should be 8, 16 (Integer) or 32 (Float)!.");
        vsapi->freeNode(d.node);
    }

    d.vf = init();

    d.width = vsapi->propGetInt(in, "width", 0, &err);
    if (err)
        d.width = d.vi->width;

    d.height = vsapi->propGetInt(in, "height", 0, &err);
    if (err)
        d.height = d.vi->height;

    d.src_x = vsapi->propGetFloat(in, "sx", 0, &err);
    d.src_y = vsapi->propGetFloat(in, "sy", 0, &err);
    d.linear = vsapi->propGetInt(in, "linearize", 0, &err);
    // only enable by default for RGB because linearizing YCbCr directly is incorrect and Gray may be a YCbCr plane
    if (err) d.linear = d.vi->format->colorFamily == cmRGB;
    // allow linearizing Gray manually, though, if the user knows what he’s doing
    d.linear = d.linear && (d.vi->format->colorFamily == cmRGB || d.vi->format->colorFamily == cmGray);
    d.trc = vsapi->propGetInt(in, "trc", 0, &err);
    if (err) d.trc = 1;

    struct pl_sigmoid_params *sigmoidParams = malloc(sizeof(struct pl_sigmoid_params));
    sigmoidParams->center = vsapi->propGetFloat(in, "sigmoid_center", 0, &err);
    if (err) sigmoidParams->center = pl_sigmoid_default_params.center;
    sigmoidParams->slope = vsapi->propGetFloat(in, "sigmoid_slope", 0, &err);
    if (err) sigmoidParams->slope = pl_sigmoid_default_params.slope;
    // same reasoning as with linear
    bool sigm = vsapi->propGetInt(in, "sigmoidize", 0, &err);
    if (err) sigm = d.vi->format->colorFamily == cmRGB;
    sigm = sigm && (d.vi->format->colorFamily == cmRGB || d.vi->format->colorFamily == cmGray);
    d.sigmoid_params = sigm ? sigmoidParams : NULL;


    struct pl_sample_filter_params *sampleFilterParams = calloc(1, sizeof(struct pl_sample_filter_params));;

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
    FILTER_ELIF(catmull_rom)
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
    sampleFilterParams->filter.clamp = vsapi->propGetFloat(in, "clamp", 0, &err);
    sampleFilterParams->filter.blur = vsapi->propGetFloat(in, "blur", 0, &err);
    sampleFilterParams->filter.taper = vsapi->propGetFloat(in, "taper", 0, &err);
    struct pl_filter_function *f = calloc(1, sizeof(struct pl_filter_function));
    *f = *sampleFilterParams->filter.kernel;
    if (f->resizable) {
        vsapi->propGetFloat(in, "radius", 0, &err);
        if (!err)
            f->radius = vsapi->propGetFloat(in, "radius", 0, &err);
    }
    vsapi->propGetFloat(in, "param1", 0, &err);
    if (!err && f->tunable[0])
        f->params[0] = vsapi->propGetFloat(in, "param1", 0, &err);
    vsapi->propGetFloat(in, "param2", 0, &err);
    if (!err && f->tunable[1])
        f->params[1] = vsapi->propGetFloat(in, "param2", 0, &err);
    sampleFilterParams->filter.kernel = f;

    d.chromaLocation = vsapi->propGetInt(in, "chroma_loc", 0, &err);
    if (err)
        d.chromaLocation = PL_CHROMA_LEFT;

    d.sampleParams = sampleFilterParams;
    data = malloc(sizeof(d));
    *data = d;
    vsapi->createFilter(in, out, "Resample", ResampleInit, ResampleGetFrame, ResampleFree, fmParallel, 0, data, core);
}
