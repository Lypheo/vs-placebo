#include <math.h>
#include <pthread.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>

#include <VapourSynth4.h>
#include <VSHelper4.h>

#include <libplacebo/filters.h>
#include <libplacebo/colorspace.h>

#include "vs-placebo.h"

typedef struct {
    VSNode *node;
    const VSVideoInfo *vi;
    void *vf;
    int width;
    int height;

    /** Width of the source region. */
    float src_width;

    /** Height of the source region. */
    float src_height;
    float src_x;
    float src_y;
    struct pl_sample_filter_params *sampleParams;
    pl_shader_obj lut;
    struct pl_sigmoid_params *sigmoid_params;
    enum pl_color_transfer trc;
    bool linear;

    /** Minimum luminance. */
    float min_luma;
} ResampleData;

bool vspl_resample_do_plane(
    struct priv *p,
    void *data,
    int w,
    int h,
    float src_width,
    float src_height,
    VSCore *core,
    const VSAPI *vsapi,
    float sx,
    float sy
)
{
    ResampleData *d = (ResampleData*) data;
    pl_shader sh = pl_dispatch_begin(p->dp);
    pl_tex sample_fbo = NULL;
    pl_tex sep_fbo = NULL;

    struct pl_sample_filter_params sampleFilterParams = *d->sampleParams;
    sampleFilterParams.lut = &d->lut;

    struct pl_color_space *color = pl_color_space(
        .transfer = d->trc,
        .hdr.min_luma = d->min_luma,
    );

    struct pl_sample_src *src = pl_sample_src(
        .tex = p->tex_in[0]
    );

    //
    // linearization and sigmoidization
    //

    pl_shader ish = pl_dispatch_begin(p->dp);
    struct pl_tex_params *tex_params = pl_tex_params(
        .w = src->tex->params.w,
        .h = src->tex->params.h,
        .renderable = true,
        .sampleable = true,
        .format = src->tex->params.format
    );

    if (!pl_tex_recreate(p->gpu, &sample_fbo, tex_params))
        vsapi->logMessage(mtCritical, "failed creating intermediate color texture!\n", core);

    pl_shader_sample_direct(ish, src);
    if (d->linear)
        pl_shader_linearize(ish, color);

    if (d->sigmoid_params)
        pl_shader_sigmoidize(ish, d->sigmoid_params);

    if (!pl_dispatch_finish(p->dp, pl_dispatch_params(
        .target = sample_fbo,
        .shader = &ish
    ))) {
        vsapi->logMessage(mtCritical, "Failed linearizing/sigmoidizing! \n", core);
        return false;
    }

    //
    // sampling
    //

    struct pl_rect2df rect = {
        sx,
        sy,
        src_width + sx,
        src_height + sy,
    };
    src->tex = sample_fbo;
    src->rect = rect;
    src->new_h = h;
    src->new_w = w;

    if (d->sampleParams->filter.polar) {
        if (!pl_shader_sample_polar(sh, src, &sampleFilterParams))
            vsapi->logMessage(mtCritical, "Failed dispatching scaler...\n", core);
    } else {
        struct pl_sample_src src1 = *src, src2 = *src;
        src1.new_w = src->tex->params.w;
        src1.rect.x0 = 0;
        src1.rect.x1 = src1.new_w;
        src2.rect.y0 = 0;
        src2.rect.y1 = src1.new_h;

        pl_shader tsh = pl_dispatch_begin(p->dp);

        if (!pl_shader_sample_ortho2(tsh, &src1, &sampleFilterParams)) {
            vsapi->logMessage(mtCritical, "Failed dispatching vertical pass!\n", core);
            pl_dispatch_abort(p->dp, &tsh);
        }

        struct pl_tex_params *tex_params = pl_tex_params(
            .w = src1.new_w,
            .h = src1.new_h,
            .renderable = true,
            .sampleable = true,
            .format = src->tex->params.format,
        );

        if (!pl_tex_recreate(p->gpu, &sep_fbo, tex_params))
            vsapi->logMessage(mtCritical, "failed creating intermediate texture!\n", core);

        if (!pl_dispatch_finish(p->dp, pl_dispatch_params (
            .target = sep_fbo,
            .shader = &tsh
        ))) {
            vsapi->logMessage(mtCritical, "Failed rendering vertical pass! \n", core);
            return false;
        }

        src2.tex = sep_fbo;
        src2.scale = 1.0;
        if (!pl_shader_sample_ortho2(sh, &src2, &sampleFilterParams))
            vsapi->logMessage(mtCritical, "Failed dispatching horizontal pass! \n", core);
    }

    if (d->sigmoid_params)
        pl_shader_unsigmoidize(sh, d->sigmoid_params);

    if (d->linear)
        pl_shader_delinearize(sh, color);


    bool ok = pl_dispatch_finish(p->dp, pl_dispatch_params(
        .target = p->tex_out[0],
        .shader = &sh
    ));

    pl_tex_destroy(p->gpu, &sep_fbo);
    pl_tex_destroy(p->gpu, &sample_fbo);
    return ok;

//    struct pl_plane plane = (struct pl_plane) {.texture = p->tex_in[0], .components = 1, .component_mapping[0] = 0};
//
//    struct pl_color_repr crpr = {.bits = {.sample_depth = d->vi->format->bytesPerSample * 8, .color_depth =
//    d->vi->format->bytesPerSample * 8, .bit_shift = 0},
//            .levels = PL_COLOR_LEVELS_UNKNOWN, .alpha = PL_ALPHA_UNKNOWN, .sys = PL_COLOR_SYSTEM_UNKNOWN};
//
//    struct pl_image img = {.num_planes = 1, .width = d->vi->width, .height = d->vi->height,
//            .planes[0] = plane,
//            .repr = crpr, .color = (struct pl_color_space) {0}};
//    struct pl_render_target out = {.color = (struct pl_color_space) {0}, .repr = crpr, .fbo = p->tex_out[0]};
//    struct pl_render_params par = {
//            .downscaler = &d->sampleParams->filter,
//            .upscaler = &d->sampleParams->filter,
//            .sigmoid_params = d->sigmoid_params,
//            .disable_linear_scaling = !d->linear,
//    };
//    return pl_render_image(p->rr, &img, &out, &par);

}

bool vspl_resample_reconfig(void *priv, struct pl_plane_data *data, int w, int h, VSCore *core, const VSAPI *vsapi)
{
    struct priv *p = priv;

    pl_fmt fmt = pl_plane_find_fmt(p->gpu, NULL, data);
    if (!fmt) {
        vsapi->logMessage(mtCritical, "Failed configuring filter: no good texture format!\n", core);
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
        .w = w,
        .h = h,
        .format = fmt,
        .renderable = true,
        .host_readable = true,
        .storable = true,
    ));

    if (!ok) {
        vsapi->logMessage(mtCritical, "Failed creating GPU textures!\n", core);
        return false;
    }

    return true;
}

bool vspl_resample_filter(
    void *priv,
    VSFrame *dst,
    struct pl_plane_data *src,
    void *d,
    int w,
    int h,
    float src_width,
    float src_height,
    float sx,
    float sy,
    VSCore *core,
    const VSAPI *vsapi,
    int planeIdx
)
{
    struct priv *p = priv;

    pl_fmt in_fmt = p->tex_in[0]->params.format;
    pl_fmt out_fmt = p->tex_out[0]->params.format;

    // Upload planes
    bool ok = true;
    ok &= pl_tex_upload(p->gpu, pl_tex_transfer_params(
        .tex = p->tex_in[0],
        .row_pitch = (src->row_stride / src->pixel_stride) * in_fmt->texel_size,
        .ptr = (void *) src->pixels,
    ));

    if (!ok) {
        vsapi->logMessage(mtCritical, "Failed uploading data to the GPU!\n", core);
        return false;
    }
    // Process plane
    if (!vspl_resample_do_plane(p, d, w, h, src_width, src_height, core, vsapi, sx, sy)) {
        vsapi->logMessage(mtCritical, "Failed processing planes!\n", core);
        return false;
    }

    uint8_t *dst_ptr = vsapi->getWritePtr(dst, planeIdx);
    int dst_row_pitch = (vsapi->getStride(dst, planeIdx) / src->pixel_stride) * out_fmt->texel_size;

    // Download planes
    ok = pl_tex_download(p->gpu, pl_tex_transfer_params(
        .tex = p->tex_out[0],
        .row_pitch = dst_row_pitch,
        .ptr = (void *) dst_ptr,
    ));

    if (!ok) {
        vsapi->logMessage(mtCritical, "Failed downloading data from the GPU!\n", core);
        return false;
    }

    return true;
}

/** Recalculate aspect ratio frame properties. */
void vspl_propagate_sar(
    const VSMap *src_props,
    VSMap *dst_props,
    int width,
    int height,
    float src_width,
    float src_height,
    float dst_width,
    float dst_height,
    const VSAPI *vsapi
) {
    int64_t sar_num = 0;
    int64_t sar_den = 0;

    if (vsapi->mapNumElements(src_props, "_SARNum") > 0)
        sar_num = vsapi->mapGetInt(src_props, "_SARNum", 0, NULL);
    if (vsapi->mapNumElements(src_props, "_SARDen") > 0)
        sar_den = vsapi->mapGetInt(src_props, "_SARDen", 0, NULL);

    if (sar_num <= 0 || sar_den <= 0) {
        vsapi->mapDeleteKey(dst_props, "_SARNum");
        vsapi->mapDeleteKey(dst_props, "_SARDen");
    } else {
        if (!isnan(src_width) && src_width != width)
            vsh_muldivRational(&sar_num, &sar_den, llround(src_width * 16), dst_width * 16);
        else
            vsh_muldivRational(&sar_num, &sar_den, width, dst_width);

        if (!isnan(src_height) && src_height != height)
            vsh_muldivRational(&sar_num, &sar_den, dst_height * 16, llround(src_height * 16));
        else
            vsh_muldivRational(&sar_num, &sar_den, dst_height, height);

        vsapi->mapSetInt(dst_props, "_SARNum", sar_num, maReplace);
        vsapi->mapSetInt(dst_props, "_SARDen", sar_den, maReplace);
    }
}

static const VSFrame *VS_CC VSPlaceboResampleGetFrame(int n, int activationReason, void *instanceData, void **frameData, VSFrameContext *frameCtx, VSCore *core, const VSAPI *vsapi) {
    ResampleData *d = (ResampleData *) instanceData;

    if (activationReason == arInitial) {
        vsapi->requestFrameFilter(n, d->node, frameCtx);
    } else if (activationReason == arAllFramesReady) {
        const VSFrame *frame = vsapi->getFrameFilter(n, d->node, frameCtx);

        const VSVideoFormat *srcFmt = vsapi->getVideoFrameFormat(frame);
        const float subsampling_w = 1 << srcFmt->subSamplingW;
        const float subsampling_h = 1 << srcFmt->subSamplingH;

        VSFrame *dst = vsapi->newVideoFrame(srcFmt, d->width, d->height, frame, core);

        for (unsigned int i = 0; i < srcFmt->numPlanes; i++) {
            struct pl_plane_data plane = {
                .type = srcFmt->sampleType == stInteger ? PL_FMT_UNORM : PL_FMT_FLOAT,
                .width = vsapi->getFrameWidth(frame, i),
                .height = vsapi->getFrameHeight(frame, i),
                .pixel_stride = srcFmt->bytesPerSample,
                .row_stride = vsapi->getStride(frame, i),
                .pixels = vsapi->getReadPtr((VSFrame *) frame, i),
                .component_size[0] = srcFmt->bitsPerSample,
                .component_pad[0] = 0,
                .component_map[0] = 0,
            };

            int w = vsapi->getFrameWidth(dst, i), h = vsapi->getFrameHeight(dst, i);

            // FIXME: support other chroma locations as well.
            const float subsampling_shift_w = (0.5f * (1.0f - (float) w / (float) d->vi->width)) / subsampling_w;
            const float subsampling_shift_h = 0.0;

            const bool shift = srcFmt->colorFamily == cfYUV && (i == 1 || i == 2);
            const float sx = shift ? subsampling_shift_w + d->src_x / subsampling_w : d->src_x;
            const float sy = shift ? subsampling_shift_h + d->src_y / subsampling_h : d->src_y;

            const float src_w = shift ? d->src_width / subsampling_w : d->src_width;
            const float src_h = shift ? d->src_height / subsampling_h : d->src_height;

            pthread_mutex_lock(&vspl_vulkan_mutex);

            if (vspl_resample_reconfig(d->vf, &plane, w, h, core, vsapi)) {
                vspl_resample_filter(d->vf, dst, &plane, d, w, h, src_w, src_h, sx, sy, core, vsapi, i);
            }

            pthread_mutex_unlock(&vspl_vulkan_mutex);
        }

        const VSMap *src_props = vsapi->getFramePropertiesRO(frame);
        VSMap *dst_props = vsapi->getFramePropertiesRW(dst);
        vspl_propagate_sar(
            src_props,
            dst_props,
            vsapi->getFrameWidth(frame, 0),
            vsapi->getFrameHeight(frame, 0),
            d->src_width,
            d->src_height,
            d->width,
            d->height,
            vsapi
        );

        vsapi->freeFrame(frame);
        return dst;
    }

    return 0;
}

static void VS_CC VSPlaceboResampleFree(void *instanceData, VSCore *core, const VSAPI *vsapi) {
    ResampleData *d = (ResampleData *) instanceData;
    vsapi->freeNode(d->node);
    pl_shader_obj_destroy(&d->lut);
    free((void *) d->sampleParams->filter.kernel);
    free(d->sampleParams);
    free(d->sigmoid_params);
    VSPlaceboUninit(d->vf);
    free(d);
}

void VS_CC VSPlaceboResampleCreate(const VSMap *in, VSMap *out, void *useResampleData, VSCore *core, const VSAPI *vsapi) {
    ResampleData d;
    ResampleData *data;
    int err;
    enum pl_log_level log_level;

    log_level = vsapi->mapGetInt(in, "log_level", 0, &err);
    if (err)
        log_level = PL_LOG_ERR;

    d.node = vsapi->mapGetNode(in, "clip", 0, 0);
    d.vi = vsapi->getVideoInfo(d.node);
    VSVideoInfo vi_out = *d.vi;

    if ((d.vi->format.bitsPerSample != 8 && d.vi->format.bitsPerSample != 16 && d.vi->format.bitsPerSample != 32)) {
        vsapi->mapSetError(out, "placebo.Resample: Input bitdepth should be 8, 16 (Integer) or 32 (Float)!.");
        vsapi->freeNode(d.node);
    }

    d.vf = VSPlaceboInit(log_level);

    d.width = vsapi->mapGetInt(in, "width", 0, &err);
    if (err)
        d.width = d.vi->width;

    d.height = vsapi->mapGetInt(in, "height", 0, &err);
    if (err)
        d.height = d.vi->height;

    vi_out.width = d.width;
    vi_out.height = d.height;

    d.src_width = vsapi->mapGetFloat(in, "src_width", 0, &err);
    if (err)
        d.src_width = d.vi->width;

    d.src_height = vsapi->mapGetFloat(in, "src_height", 0, &err);
    if (err)
        d.src_height = d.vi->height;

    d.src_x = vsapi->mapGetFloat(in, "sx", 0, &err);
    d.src_y = vsapi->mapGetFloat(in, "sy", 0, &err);
    d.linear = vsapi->mapGetInt(in, "linearize", 0, &err);
    // only enable by default for RGB because linearizing YCbCr directly is incorrect and Gray may be a YCbCr plane
    if (err) d.linear = d.vi->format.colorFamily == cfRGB;
    // allow linearizing Gray manually, though, if the user knows what he’s doing
    d.linear = d.linear && (d.vi->format.colorFamily == cfRGB || d.vi->format.colorFamily == cfGray);

    d.min_luma = vsapi->mapGetFloat(in, "min_luma", 0, &err);
    if (err) {
        // Default to infinite contrast to match zimg.
        d.min_luma = PL_COLOR_HDR_BLACK;
    }

    d.trc = vsapi->mapGetInt(in, "trc", 0, &err);
    if (err) d.trc = 1;

    // same reasoning as with linear
    bool sigm = vsapi->mapGetInt(in, "sigmoidize", 0, &err);
    if (err)
        sigm = d.vi->format.colorFamily == cfRGB;

    sigm = sigm && (d.vi->format.colorFamily == cfRGB || d.vi->format.colorFamily == cfGray);
    d.sigmoid_params = NULL;

    if (sigm) {
        struct pl_sigmoid_params *sigmoidParams = malloc(sizeof(struct pl_sigmoid_params));
        *sigmoidParams = pl_sigmoid_default_params;

        sigmoidParams->center = vsapi->mapGetFloat(in, "sigmoid_center", 0, &err);
        if (err)
            sigmoidParams->center = pl_sigmoid_default_params.center;

        sigmoidParams->slope = vsapi->mapGetFloat(in, "sigmoid_slope", 0, &err);
        if (err)
            sigmoidParams->slope = pl_sigmoid_default_params.slope;

        d.sigmoid_params = sigmoidParams;
    }

    struct pl_sample_filter_params *sampleFilterParams = calloc(1, sizeof(struct pl_sample_filter_params));;

    d.lut = NULL;
    sampleFilterParams->no_widening = false;
    sampleFilterParams->no_compute = false;
    sampleFilterParams->antiring = vsapi->mapGetFloat(in, "antiring", 0, &err);

    const char *filter = vsapi->mapGetData(in, "filter", 0, &err);
    if (err) {
        vsapi->logMessage(mtWarning, "Unspecified filter... selecting ewa_lanczos.\n", core);
        filter = "ewa_lanczos";
    }

    const struct pl_filter_config *filter_config = pl_find_filter_config(filter, PL_FILTER_SCALING);
    if (filter_config) {
        sampleFilterParams->filter = *filter_config;
    } else {
        vsapi->logMessage(mtWarning, "Unknown filter... selecting ewa_lanczos.\n", core);
        sampleFilterParams->filter = pl_filter_ewa_lanczos;
    }

    sampleFilterParams->filter.clamp = vsapi->mapGetFloat(in, "clamp", 0, &err);
    sampleFilterParams->filter.blur = vsapi->mapGetFloat(in, "blur", 0, &err);
    sampleFilterParams->filter.taper = vsapi->mapGetFloat(in, "taper", 0, &err);

    struct pl_filter_function *f = calloc(1, sizeof(struct pl_filter_function));

    *f = *sampleFilterParams->filter.kernel;
    if (f->resizable) {
        vsapi->mapGetFloat(in, "radius", 0, &err);
        if (!err)
            f->radius = vsapi->mapGetFloat(in, "radius", 0, &err);
    }

    vsapi->mapGetFloat(in, "param1", 0, &err);
    if (!err && f->tunable[0])
        sampleFilterParams->filter.params[0] = vsapi->mapGetFloat(in, "param1", 0, &err);

    vsapi->mapGetFloat(in, "param2", 0, &err);
    if (!err && f->tunable[1])
        sampleFilterParams->filter.params[1] = vsapi->mapGetFloat(in, "param2", 0, &err);

    sampleFilterParams->filter.kernel = f;
    d.sampleParams = sampleFilterParams;

    data = malloc(sizeof(d));
    *data = d;

    VSFilterDependency deps[] = {{d.node, rpStrictSpatial}};

    vsapi->createVideoFilter(
        out,
        "Resample",
        &vi_out,
        VSPlaceboResampleGetFrame,
        VSPlaceboResampleFree,
        fmParallelRequests,
        deps,
        1,
        data,
        core
    );
}
