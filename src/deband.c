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
    struct pl_render_params *render_params;
    uint8_t frame_index;

    pthread_mutex_t lock;
} DebandData;

bool vspl_deband_do_image(DebandData *dbd_data, struct pl_frame *src_img, struct pl_frame *dst_img, const VSAPI *vsapi)
{
    struct priv *p = dbd_data->vf;
    bool ok = true;

    for (int i = 0; i < src_img->num_planes; i++) {
        pl_shader sh = pl_dispatch_begin(p->dp);
        pl_shader_reset(sh, pl_shader_params(
            .gpu = p->gpu,
            .index = dbd_data->frame_index++,
        ));

        struct pl_sample_src *src = pl_sample_src(
            .tex = p->tex_in[i]
        );

        int new_depth = p->tex_out[i]->params.format->component_depth[i];

        pl_shader_deband(sh, src, dbd_data->render_params->deband_params);

        if (dbd_data->dither)
            pl_shader_dither(sh, new_depth, &p->dither_state, dbd_data->render_params->dither_params);

        ok &= pl_dispatch_finish(p->dp, pl_dispatch_params(
            .target = p->tex_out[i],
            .shader = &sh,
        ));
    }

    // ok &= pl_render_image(p->rr, src_img, dst_img, dbd_data->render_params);

    if (!ok) {
        vsapi->logMessage(mtCritical, "placebo.Deband: Failed processing planes!");
    }

    return ok;
}

bool vspl_deband_reconfig(DebandData *dbd_data, VSFrameRef *dst, const VSAPI *vsapi, int plane_idx, const struct pl_plane_data *data)
{
    struct priv *p = dbd_data->vf;

    bool ok = true;
    const struct pl_fmt *fmt = pl_plane_find_fmt(p->gpu, NULL, data);

    if (!fmt) {
        vsapi->logMessage(mtCritical, "placebo.Deband: Failed configuring filter: no good texture format!");
        return false;
    }

    ok &= pl_tex_recreate(p->gpu, &p->tex_in[plane_idx], pl_tex_params(
        .w = data->width,
        .h = data->height,
        .format = fmt,
        .sampleable = true,
        .host_writable = true,
    ));

    int vs_plane = data->component_map[0];
    ok &= pl_tex_recreate(p->gpu, &p->tex_out[plane_idx], pl_tex_params(
        .w = vsapi->getFrameWidth(dst, vs_plane),
        .h = vsapi->getFrameHeight(dst, vs_plane),
        .format = fmt,
        .renderable = true,
        .host_readable = true,
    ));

    if (!ok) {
        vsapi->logMessage(mtCritical, "placebo.Deband: Failed creating GPU textures!");
    }

    return ok;
}

bool vspl_deband_upload_plane(DebandData *dbd_data, const VSAPI *vsapi, int plane_idx, const struct pl_plane_data *data, struct pl_plane *plane)
{
    struct priv *p = dbd_data->vf;

    // Upload planes

    bool ok = pl_upload_plane(p->gpu, plane, &p->tex_in[plane_idx], data);

    if (!ok) {
        vsapi->logMessage(mtCritical, "placebo.Deband: Failed downloading data from the GPU!");
    }

    return ok;
}

bool vspl_deband_download_planes(DebandData *dbd_data, const VSAPI *vsapi, VSFrameRef *vs_dst, const struct pl_plane_data *data, struct pl_frame *dst_img)
{
    struct priv *p = dbd_data->vf;

    bool ok = true;

    // Download planes
    for (int i = 0; i < dst_img->num_planes; i++) {
        struct pl_plane *target_plane = &dst_img->planes[i];

        int vs_plane = target_plane->component_mapping[0];

        pl_fmt out_fmt = p->tex_out[i]->params.format;
        uint8_t *dst_ptr = vsapi->getWritePtr(vs_dst, vs_plane);
        int dst_row_pitch = (vsapi->getStride(vs_dst, vs_plane) / data[i].pixel_stride) * out_fmt->texel_size;

        ok &= pl_tex_download(p->gpu, pl_tex_transfer_params(
            .tex = p->tex_out[i],
            .row_pitch = dst_row_pitch,
            .ptr = (void *) dst_ptr,
        ));
    }

    if (!ok) {
        vsapi->logMessage(mtCritical, "placebo.Deband: Failed downloading data from the GPU!");
    }

    return ok;
}

static void VS_CC VSPlaceboDebandInit(VSMap *in, VSMap *out, void **instanceData, VSNode *node, VSCore *core, const VSAPI *vsapi) {
    DebandData *d = (DebandData *) * instanceData;
    VSVideoInfo new_vi = (VSVideoInfo) * (d->vi);
    vsapi->setVideoInfo(&new_vi, 1, node);
}

static const VSFrameRef *VS_CC VSPlaceboDebandGetFrame(int n, int activationReason, void **instanceData, void **frameData, VSFrameContext *frameCtx, VSCore *core, const VSAPI *vsapi) {
    DebandData *dbd_data = (DebandData *) * instanceData;

    if (activationReason == arInitial) {
        vsapi->requestFrameFilter(n, dbd_data->node, frameCtx);
    } else if (activationReason == arAllFramesReady) {
        const VSFrameRef *frame = vsapi->getFrameFilter(n, dbd_data->node, frameCtx);

        int ih = vsapi->getFrameHeight(frame, 0);
        int iw = vsapi->getFrameWidth(frame, 0);

        const VSFormat *srcFmt = dbd_data->vi->format;
        VSFrameRef *dst = vsapi->newVideoFrame(srcFmt, iw, ih, frame, core);

        struct pl_color_repr repr = {
            .bits = {
                .sample_depth = dbd_data->vi->format->bitsPerSample,
                .color_depth = dbd_data->vi->format->bitsPerSample,
                .bit_shift = 0
            },
            .sys = PL_COLOR_SYSTEM_UNKNOWN,
        };

        struct pl_frame src_img = {
            .repr       = repr,
            .color      = pl_color_space_unknown,
        };
        struct pl_frame dst_img = src_img;

        pthread_mutex_lock(&dbd_data->lock); // libplacebo isn’t thread-safe

        struct priv *p = dbd_data->vf;

        int numPlanes = srcFmt->numPlanes;
        int plane_idx = 0;

        struct pl_plane_data data[3] = {};
        for (unsigned int i = 0; i < numPlanes; ++i) {
            bool copied = !((1u << i) & dbd_data->planes);

            if (copied) {
                vs_bitblt(vsapi->getWritePtr(dst, i), vsapi->getStride(dst, i),
                          vsapi->getWritePtr((VSFrameRef *) frame, i),
                          vsapi->getStride(frame, i),
                          vsapi->getFrameWidth(dst, i) * dbd_data->vi->format->bytesPerSample,
                          vsapi->getFrameHeight(dst, i));
            } else {
                src_img.num_planes += 1;

                data[plane_idx] = (struct pl_plane_data) {
                    .type = srcFmt->sampleType == stInteger ? PL_FMT_UNORM : PL_FMT_FLOAT,
                    .width = vsapi->getFrameWidth(frame, i),
                    .height = vsapi->getFrameHeight(frame, i),
                    .pixel_stride = srcFmt->bytesPerSample,
                    .row_stride = vsapi->getStride(frame, i),
                    .pixels = vsapi->getReadPtr((VSFrameRef *) frame, i),
                    .component_size[0] = srcFmt->bitsPerSample,
                    .component_pad[0] = 0,
                    .component_map[0] = i,
                };

                if (vspl_deband_reconfig(dbd_data, dst, vsapi, plane_idx, &data[plane_idx])) {
                    vspl_deband_upload_plane(dbd_data, vsapi, plane_idx, &data[plane_idx], &src_img.planes[plane_idx]);
                }

                // Create a plane for target
                dst_img.planes[plane_idx] = (struct pl_plane) {
                    .texture = p->tex_out[plane_idx],
                    .components = p->tex_out[plane_idx]->params.format->num_components,
                    .component_mapping[0] = i,
                };

                plane_idx += 1;
            }
        }

        dst_img.num_planes = src_img.num_planes;

        if (vspl_deband_do_image(dbd_data, &src_img, &dst_img, vsapi)) {
            vspl_deband_download_planes(dbd_data, vsapi, dst, data, &dst_img);
        }

        pthread_mutex_unlock(&dbd_data->lock);

        vsapi->freeFrame(frame);
        return dst;
    }

    return 0;
}

static void VS_CC VSPlaceboDebandFree(void *instanceData, VSCore *core, const VSAPI *vsapi) {
    DebandData *d = (DebandData *) instanceData;
    vsapi->freeNode(d->node);
    VSPlaceboUninit(d->vf);
    free((void *) d->render_params->dither_params);
    free((void *) d->render_params->deband_params);
    free(d->render_params);
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
        vsapi->setError(out, "placebo.Deband: mutex init failed!");
        return;
    }

    log_level = vsapi->propGetInt(in, "log_level", 0, &err);
    if (err)
        log_level = PL_LOG_ERR;

    d.node = vsapi->propGetNode(in, "clip", 0, 0);
    d.vi = vsapi->getVideoInfo(d.node);

    if ((d.vi->format->bitsPerSample != 8 && d.vi->format->bitsPerSample != 16 && d.vi->format->bitsPerSample != 32)) {
        vsapi->setError(out, "placebo.Deband: Input bitdepth should be 8, 16 (Integer) or 32 (Float)!");
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

    struct pl_render_params *render_params = malloc(sizeof(struct pl_render_params));
    *render_params = pl_render_fast_params;

    render_params->dither_params = plDitherParams;
    render_params->deband_params = debandParams;

    d.render_params = render_params;
    d.frame_index = 0;
    data = malloc(sizeof(d));
    *data = d;

    vsapi->createFilter(in, out, "Deband", VSPlaceboDebandInit, VSPlaceboDebandGetFrame, VSPlaceboDebandFree, fmParallel, 0, data, core);
}
