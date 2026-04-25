#include <pthread.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>

#include <VapourSynth4.h>

#include "p2p_api.h"
#include <libplacebo/shaders/custom.h>
#include <libplacebo/colorspace.h>

#include "vs-placebo.h"
#include "shader.h"

typedef  struct {
    VSNode *node;
    int width;
    int height;
    const VSVideoInfo *vi;
    VSVideoInfo vi_out;
    struct priv *vf;
    const struct pl_hook *shader;
    enum pl_color_system matrix;
    enum pl_color_levels range;
    enum pl_chroma_location chromaLocation;
    struct pl_sample_filter_params *sampleParams;
    struct pl_sigmoid_params *sigmoid_params;
    enum pl_color_transfer trc;
    bool linear;
} ShaderData;


bool vspl_shader_do_plane(struct priv *p, void *data, int n, struct pl_plane *planes)
{
    ShaderData *d = (ShaderData*) data;

    const struct pl_color_repr crpr = {
        .bits = {
            .sample_depth = 16,
            .color_depth = 16,
            .bit_shift = 0
        },
        .sys = d->matrix,
        .levels = d->range
    };
    const struct pl_color_space csp = {
        .transfer = d->trc
    };

    struct pl_frame img = {
        .num_planes = 3,
        .repr = crpr,
        .planes = {planes[0], planes[1], planes[2]},
        .color = csp,
    };

    if (d->vi->format.subSamplingW || d->vi->format.subSamplingH) {
        pl_frame_set_chroma_location(&img, d->chromaLocation);
    }

    struct pl_frame out = {
        .num_planes = 1,
        .planes = {{
            .texture = p->tex_out[0],
            .components = p->tex_out[0]->params.format->num_components,
            .component_mapping = {0, 1, 2, 3},
        }},
        .repr = crpr,
        .color = csp,
    };

    struct pl_render_params renderParams = {
        .hooks = &d->shader,
        .num_hooks = 1,
        .sigmoid_params = d->sigmoid_params,
        .disable_linear_scaling = !d->linear,
        .upscaler = &d->sampleParams->filter,
        .downscaler = &d->sampleParams->filter,
        .antiringing_strength = d->sampleParams->antiring,
    };

    return pl_render_image(p->rr, &img, &out, &renderParams);
}

bool vspl_shader_reconfig(void *priv, struct pl_plane_data *data, VSCore *core, const VSAPI *vsapi, ShaderData *d)
{
    struct priv *p = priv;

    pl_fmt fmt[3];
    for (int j = 0; j < 3; ++j) {
        fmt[j] = pl_plane_find_fmt(p->gpu, NULL, &data[j]);
        if (!fmt[j]) {
            vsapi->logMessage(mtCritical, "Failed configuring filter: no good texture format!\n", core);
            return false;
        }
    }

    bool ok = true;
    for (int i = 0; i < 3; ++i) {
        ok &= pl_tex_recreate(p->gpu, &p->tex_in[i], pl_tex_params(
            .w = data[i].width,
            .h = data[i].height,
            .format = fmt[i],
            .sampleable = true,
            .host_writable = true,
        ));
    }

    const struct pl_plane_data plane_data = {
        .type = PL_FMT_UNORM,
        .component_map = {0, 1, 2, 0},
        .component_pad = {0, 0, 0, 0},
        .component_size = {16, 16, 16, 0},
        .pixel_stride = 6
    };

    pl_fmt out = pl_plane_find_fmt(p->gpu, NULL, &plane_data);

    ok &= pl_tex_recreate(p->gpu, &p->tex_out[0], pl_tex_params(
        .w = d->width,
        .h = d->height,
        .format = out,
        .renderable = true,
        .host_readable = true,
    ));

    if (!ok) {
        vsapi->logMessage(mtCritical, "Failed creating GPU textures!\n", core);
        return false;
    }

    return true;
}

bool vspl_shader_filter(void *priv, void *dst, struct pl_plane_data *src,  ShaderData *d, int n, VSCore *core, const VSAPI *vsapi)
{
    struct priv *p = priv;
    // Upload planes
    struct pl_plane planes[4] = {0};
    bool ok = true;

    for (int i = 0; i < 3; ++i) {
        ok &= pl_upload_plane(p->gpu, &planes[i], &p->tex_in[i], &src[i]);
    }

    if (!ok) {
        vsapi->logMessage(mtCritical, "Failed uploading data to the GPU!\n", core);
        return false;
    }

    // Process plane
    if (!vspl_shader_do_plane(p, d, n, planes)) {
        vsapi->logMessage(mtCritical, "Failed processing planes!\n", core);
        return false;
    }

    // Download planes
    ok = pl_tex_download(p->gpu, pl_tex_transfer_params(
        .tex = p->tex_out[0],
        .ptr = dst,
    ));

    if (!ok) {
        vsapi->logMessage(mtCritical, "Failed downloading data from the GPU!\n", core);
        return false;
    }

    return true;
}

static const VSFrame *VS_CC VSPlaceboShaderGetFrame(int n, int activationReason, void *instanceData, void **frameData, VSFrameContext *frameCtx, VSCore *core, const VSAPI *vsapi) {
    ShaderData *d = (ShaderData *) instanceData;

    if (activationReason == arInitial) {
        vsapi->requestFrameFilter(n, d->node, frameCtx);
    } else if (activationReason == arAllFramesReady) {
        const VSFrame *frame = vsapi->getFrameFilter(n, d->node, frameCtx);

        if (d->range == PL_COLOR_LEVELS_UNKNOWN) {
            const VSMap *props = vsapi->getFramePropertiesRO(frame);

            int err = 0;
            int r = vsapi->mapGetInt(props, "_ColorRange", 0, &err);
            if (err)
                d->range = PL_COLOR_LEVELS_UNKNOWN;
            else
                d->range = r ? PL_COLOR_LEVELS_TV : PL_COLOR_LEVELS_PC;
        }

        VSVideoFormat dstfmt = d->vi_out.format;
        vsapi->queryVideoFormat(&dstfmt, dstfmt.colorFamily, dstfmt.sampleType, dstfmt.bitsPerSample, 0, 0, core);

        VSFrame *dst = vsapi->newVideoFrame(&dstfmt, d->width, d->height, frame, core);

        struct pl_plane_data planes[4] = {0};
        for (int j = 0; j < 3; ++j) {
            planes[j] = (struct pl_plane_data) {
                .type = PL_FMT_UNORM,
                .width = vsapi->getFrameWidth(frame, j),
                .height = vsapi->getFrameHeight(frame, j),
                .pixel_stride = 2,
                .row_stride =  vsapi->getStride(frame, j),
                .pixels = vsapi->getReadPtr((VSFrame *) frame, j),
            };

            planes[j].component_size[0] = 16;
            planes[j].component_pad[0] = 0;
            planes[j].component_map[0] = j;
        }

        void *packed_dst = malloc(d->width * d->height * 2 * 3);

        pthread_mutex_lock(&vspl_vulkan_mutex);

        if (vspl_shader_reconfig(d->vf, planes, core, vsapi, d)) {
            vspl_shader_filter(d->vf, packed_dst, planes, d, n, core, vsapi);
        }

        pthread_mutex_unlock(&vspl_vulkan_mutex);

        struct p2p_buffer_param pack_params = {
            .width = d->width,
            .height = d->height,
            .packing = p2p_bgr48_le,
            .src[0] = packed_dst,
            .src_stride[0] = d->width * 2 * 3
        };

        for (int k = 0; k < 3; ++k) {
            pack_params.dst[k] = vsapi->getWritePtr(dst, k);
            pack_params.dst_stride[k] = vsapi->getStride(dst, k);
        }

        p2p_unpack_frame(&pack_params, 0);
        free(packed_dst);

        vsapi->freeFrame(frame);
        return dst;
    }

    return 0;
}

static void VS_CC VSPlaceboShaderFree(void *instanceData, VSCore *core, const VSAPI *vsapi) {
    ShaderData *d = (ShaderData *)instanceData;
    vsapi->freeNode(d->node);
    pl_mpv_user_shader_destroy(&d->shader);
    free((void *) d->sampleParams->filter.kernel);
    free(d->sampleParams);
    free(d->sigmoid_params);
    VSPlaceboUninit(d->vf);
    free(d);
}

void VS_CC VSPlaceboShaderCreate(const VSMap *in, VSMap *out, void *userData, VSCore *core, const VSAPI *vsapi) {
    ShaderData d;
    ShaderData *data;
    int err;
    enum pl_log_level log_level;

    log_level = vsapi->mapGetInt(in, "log_level", 0, &err);
    if (err)
        log_level = PL_LOG_ERR;

    const char *sh = vsapi->mapGetData(in, "shader", 0, &err);
    char *shader;
    size_t fsize;

    if (!err) {
        FILE *fl = fopen(sh, "rb");
        if (fl == NULL) {
            perror("Failed: ");
            vsapi->mapSetError(out, "placebo.Shader: Failed reading shader file!");
            return;
        }

        fseek(fl, 0, SEEK_END);
        fsize = (size_t) ftell(fl);
        rewind(fl);

        shader = malloc(fsize + 1);
        fread(shader, 1, fsize, fl);
        fclose(fl);

        shader[fsize] = '\0';
    } else {
        const char *shader_s = vsapi->mapGetData(in, "shader_s", 0, &err);

        if (err) {
            vsapi->mapSetError(out, "placebo.Shader: Either shader or shader_s must be specified!");
            return;
        }
        fsize =  strlen(shader_s);
        shader = malloc(fsize + 1);
        strcpy(shader, shader_s);
    }

    d.node = vsapi->mapGetNode(in, "clip", 0, 0);
    d.vi = vsapi->getVideoInfo(d.node);

    d.vi_out = *d.vi;
    vsapi->getVideoFormatByID(&d.vi_out.format, pfYUV444P16, core);

    d.vf = VSPlaceboInit(log_level);
    d.shader = pl_mpv_user_shader_parse(d.vf->gpu, shader, strlen(shader));
    free(shader);

    if (!d.shader) {
        VSPlaceboUninit(d.vf);
        pl_mpv_user_shader_destroy(&d.shader);
        vsapi->mapSetError(out, "placebo.Shader: Failed parsing shader!");
        vsapi->freeNode(d.node);
        return;
    }

    if (d.vi->format.colorFamily != cfYUV || d.vi->format.bitsPerSample != 16) {
        vsapi->mapSetError(out, "placebo.Shader: Input should be YUVxxxP16!");
        vsapi->freeNode(d.node);
        return;
    }

    d.range = PL_COLOR_LEVELS_UNKNOWN;
    d.matrix = vsapi->mapGetInt(in, "matrix", 0, &err);
    if (err)
        d.matrix = PL_COLOR_SYSTEM_BT_709;

    d.width = vsapi->mapGetInt(in, "width", 0, &err);
    if (err)
        d.width = d.vi->width;

    d.height = vsapi->mapGetInt(in, "height", 0, &err);
    if (err)
        d.height = d.vi->height;

    d.vi_out.width = d.width;
    d.vi_out.height = d.height;

    d.chromaLocation = vsapi->mapGetInt(in, "chroma_loc", 0, &err);
    if (err)
        d.chromaLocation = PL_CHROMA_LEFT;

    d.linear = vsapi->mapGetInt(in, "linearize", 0, &err);
    if (err) d.linear = 1;
    d.trc = vsapi->mapGetInt(in, "trc", 0, &err);
    if (err) d.trc = 1;

    struct pl_sigmoid_params *sigmoidParams = malloc(sizeof(struct pl_sigmoid_params));

    sigmoidParams->center = vsapi->mapGetFloat(in, "sigmoid_center", 0, &err);
    if (err)
        sigmoidParams->center = pl_sigmoid_default_params.center;

    sigmoidParams->slope = vsapi->mapGetFloat(in, "sigmoid_slope", 0, &err);
    if (err)
        sigmoidParams->slope = pl_sigmoid_default_params.slope;

    bool sigm = vsapi->mapGetInt(in, "sigmoidize", 0, &err);
    if (err)
        sigm = true;
    d.sigmoid_params = sigm ? sigmoidParams : NULL;


    struct pl_sample_filter_params *sampleFilterParams = calloc(1, sizeof(struct pl_sample_filter_params));

    sampleFilterParams->antiring = vsapi->mapGetFloat(in, "antiring", 0, &err);

    const char *filter = vsapi->mapGetData(in, "filter", 0, &err);

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
    FILTER_ELIF(bicubic)
    FILTER_ELIF(catmull_rom)
    FILTER_ELIF(mitchell)
    FILTER_ELIF(robidoux)
    FILTER_ELIF(robidouxsharp)
    FILTER_ELIF(ewa_robidoux)
    FILTER_ELIF(ewa_lanczos)
    FILTER_ELIF(ewa_robidouxsharp)
    else {
        vsapi->logMessage(mtWarning, "Unkown filter... selecting ewa_lanczos.\n", core);
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
        f->params[0] = vsapi->mapGetFloat(in, "param1", 0, &err);

    vsapi->mapGetFloat(in, "param2", 0, &err);
    if (!err && f->tunable[1])
        f->params[1] = vsapi->mapGetFloat(in, "param2", 0, &err);

    sampleFilterParams->filter.kernel = f;

    d.sampleParams = sampleFilterParams;

    data = malloc(sizeof(d));
    *data = d;

    VSFilterDependency deps[] = {{d.node, rpStrictSpatial}};

    vsapi->createVideoFilter(
        out,
        "Shader",
        &d.vi_out,
        VSPlaceboShaderGetFrame,
        VSPlaceboShaderFree,
        fmParallelRequests,
        deps,
        1,
        data,
        core
    );
}
