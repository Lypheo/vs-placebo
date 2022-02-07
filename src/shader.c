#include <pthread.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>

#include "VapourSynth.h"

#include "libp2p/p2p_api.h"
#include <libplacebo/shaders/custom.h>
#include <libplacebo/colorspace.h>

#include "vs-placebo.h"
#include "shader.h"

typedef  struct {
    VSNodeRef *node;
    int width;
    int height;
    const VSVideoInfo *vi;
    struct priv * vf;
    const struct pl_hook* shader;
    enum pl_color_system matrix;
    enum pl_color_levels range;
    enum pl_chroma_location chromaLocation;
    struct pl_sample_filter_params *sampleParams;
    struct pl_sigmoid_params * sigmoid_params;
    enum pl_color_transfer trc;
    bool linear;
    pthread_mutex_t lock;
} ShaderData;


bool vspl_shader_do_plane(struct priv *p, void* data, int n, struct pl_plane* planes)
{
    ShaderData* d = (ShaderData*) data;

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

    if (d->vi->format->subSamplingW || d->vi->format->subSamplingH) {
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
        .lut_entries = d->sampleParams->lut_entries,
        .polar_cutoff = d->sampleParams->cutoff
    };

    return pl_render_image(p->rr, &img, &out, &renderParams);
}

bool vspl_shader_reconfig(void *priv, struct pl_plane_data *data, const VSAPI *vsapi, ShaderData* d)
{
    struct priv *p = priv;

    const struct pl_fmt* fmt[3];
    for (int j = 0; j < 3; ++j) {
        fmt[j] = pl_plane_find_fmt(p->gpu, NULL, &data[j]);
        if (!fmt[j]) {
            vsapi->logMessage(mtCritical, "Failed configuring filter: no good texture format!\n");
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

    const struct pl_fmt *out = pl_plane_find_fmt(p->gpu, NULL, &plane_data);

    ok &= pl_tex_recreate(p->gpu, &p->tex_out[0], pl_tex_params(
        .w = d->width,
        .h = d->height,
        .format = out,
        .renderable = true,
        .host_readable = true,
    ));

    if (!ok) {
        vsapi->logMessage(mtCritical, "Failed creating GPU textures!\n");
        return false;
    }

    return true;
}

bool vspl_shader_filter(void *priv, void *dst, struct pl_plane_data *src,  ShaderData* d, int n, const VSAPI *vsapi)
{
    struct priv *p = priv;
    // Upload planes
    struct pl_plane planes[4] = {0};
    bool ok = true;

    for (int i = 0; i < 3; ++i) {
        ok &= pl_upload_plane(p->gpu, &planes[i], &p->tex_in[i], &src[i]);
    }

    if (!ok) {
        vsapi->logMessage(mtCritical, "Failed uploading data to the GPU!\n");
        return false;
    }

    // Process plane
    if (!vspl_shader_do_plane(p, d, n, planes)) {
        vsapi->logMessage(mtCritical, "Failed processing planes!\n");
        return false;
    }

    // Download planes
    ok = pl_tex_download(p->gpu, pl_tex_transfer_params(
        .tex = p->tex_out[0],
        .ptr = dst,
    ));

    if (!ok) {
        vsapi->logMessage(mtCritical, "Failed downloading data from the GPU!\n");
        return false;
    }

    return true;
}

static void VS_CC VSPlaceboShaderInit(VSMap *in, VSMap *out, void **instanceData, VSNode *node, VSCore *core, const VSAPI *vsapi) {
    ShaderData *d = (ShaderData *) * instanceData;
    VSVideoInfo new_vi = (VSVideoInfo) * (d->vi);
    new_vi.width = d->width;
    new_vi.height = d->height;
    VSFormat f = *new_vi.format;
    new_vi.format = vsapi->registerFormat(f.colorFamily, f.sampleType, f.bitsPerSample, 0, 0, core);
    vsapi->setVideoInfo(&new_vi, 1, node);
}

static const VSFrameRef *VS_CC VSPlaceboShaderGetFrame(int n, int activationReason, void **instanceData, void **frameData, VSFrameContext *frameCtx, VSCore *core, const VSAPI *vsapi) {
    ShaderData *d = (ShaderData *) * instanceData;

    if (activationReason == arInitial) {
        vsapi->requestFrameFilter(n, d->node, frameCtx);
    } else if (activationReason == arAllFramesReady) {
        const VSFrameRef *frame = vsapi->getFrameFilter(n, d->node, frameCtx);

        if (d->range == -1) {
            const VSMap *props = vsapi->getFramePropsRO(frame);

            int err = 0;
            int r = vsapi->propGetInt(props, "_ColorRange", 0, &err);
            if (err)
                d->range = PL_COLOR_LEVELS_PC;
            else
                d->range = r ? PL_COLOR_LEVELS_TV : PL_COLOR_LEVELS_PC;
        }

        const VSFormat *dstfmt = d->vi->format;
        dstfmt = vsapi->registerFormat(dstfmt->colorFamily, dstfmt->sampleType, dstfmt->bitsPerSample, 0, 0, core);

        VSFrameRef *dst = vsapi->newVideoFrame(dstfmt, d->width, d->height, frame, core);

        struct pl_plane_data planes[4] = {0};
        for (int j = 0; j < 3; ++j) {
            planes[j] = (struct pl_plane_data) {
                .type = PL_FMT_UNORM,
                .width = vsapi->getFrameWidth(frame, j),
                .height = vsapi->getFrameHeight(frame, j),
                .pixel_stride = 2,
                .row_stride =  vsapi->getStride(frame, j),
                .pixels = vsapi->getReadPtr((VSFrameRef *) frame, j),
            };

            planes[j].component_size[0] = 16;
            planes[j].component_pad[0] = 0;
            planes[j].component_map[0] = j;
        }

        void * packed_dst = malloc(d->width * d->height * 2 * 3);

        pthread_mutex_lock(&d->lock);

        if (vspl_shader_reconfig(d->vf, planes, vsapi, d)) {
            vspl_shader_filter(d->vf, packed_dst, planes, d, n, vsapi);
        }

        pthread_mutex_unlock(&d->lock);

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
    pthread_mutex_destroy(&d->lock);
    free(d);
}

void VS_CC VSPlaceboShaderCreate(const VSMap *in, VSMap *out, void *userData, VSCore *core, const VSAPI *vsapi) {
    ShaderData d;
    ShaderData *data;
    int err;
    enum pl_log_level log_level;

    if (pthread_mutex_init(&d.lock, NULL) != 0) {
        vsapi->setError(out, "placebo.Shader: mutex init failed\n");
        return;
    }

    log_level = vsapi->propGetInt(in, "log_level", 0, &err);
    if (err)
        log_level = PL_LOG_ERR;

    const char* sh = vsapi->propGetData(in, "shader", 0, &err);
    char *shader;
    long fsize;

    if (!err) {
        FILE *fl = fopen(sh, "r");
        if (fl == NULL) {
            perror("Failed: ");
            vsapi->setError(out, "placebo.Shader: Failed reading shader file!");
            return;
        }

        fseek(fl, 0, SEEK_END);
        fsize = ftell(fl);
        fseek(fl, 0, SEEK_SET);
        shader = malloc(fsize + 1);
        fread(shader, 1, fsize, fl);
        fclose(fl);
        shader[fsize] = '\0';
    } else {
        const char* shader_s = vsapi->propGetData(in, "shader_s", 0, &err);

        if (err) {
            vsapi->setError(out, "placebo.Shader: Either shader or shader_s must be specified!");
            return;
        }
        fsize =  strlen(shader_s);
        shader = malloc(fsize + 1);
        strcpy(shader, shader_s);
    }

    d.node = vsapi->propGetNode(in, "clip", 0, 0);
    d.vi = vsapi->getVideoInfo(d.node);

    d.vf = VSPlaceboInit(log_level);
    d.shader = pl_mpv_user_shader_parse(d.vf->gpu, shader, strlen(shader));
    free(shader);

    if (!d.shader) {
        VSPlaceboUninit(d.vf);
        pl_mpv_user_shader_destroy(&d.shader);
        vsapi->setError(out, "placebo.Shader: Failed parsing shader!");
        vsapi->freeNode(d.node);
        return;
    }

    if (d.vi->format->colorFamily != cmYUV || d.vi->format->bitsPerSample != 16) {
        vsapi->setError(out, "placebo.Shader: Input should be YUVxxxP16!");
        vsapi->freeNode(d.node);
        return;
    }

    d.range = -1;
    d.matrix = vsapi->propGetInt(in, "matrix", 0, &err);
    if (err)
        d.matrix = PL_COLOR_SYSTEM_BT_709;

    d.width = vsapi->propGetInt(in, "width", 0, &err);
    if (err)
        d.width = d.vi->width;

    d.height = vsapi->propGetInt(in, "height", 0, &err);
    if (err)
        d.height = d.vi->height;

    d.chromaLocation = vsapi->propGetInt(in, "chroma_loc", 0, &err);
    if (err)
        d.chromaLocation = PL_CHROMA_LEFT;

    d.linear = vsapi->propGetInt(in, "linearize", 0, &err);
    if (err) d.linear = 1;
    d.trc = vsapi->propGetInt(in, "trc", 0, &err);
    if (err) d.trc = 1;

    struct pl_sigmoid_params *sigmoidParams = malloc(sizeof(struct pl_sigmoid_params));

    sigmoidParams->center = vsapi->propGetFloat(in, "sigmoid_center", 0, &err);
    if (err)
        sigmoidParams->center = pl_sigmoid_default_params.center;

    sigmoidParams->slope = vsapi->propGetFloat(in, "sigmoid_slope", 0, &err);
    if (err)
        sigmoidParams->slope = pl_sigmoid_default_params.slope;

    bool sigm = vsapi->propGetInt(in, "sigmoidize", 0, &err);
    if (err)
        sigm = true;
    d.sigmoid_params = sigm ? sigmoidParams : NULL;


    struct pl_sample_filter_params *sampleFilterParams = calloc(1, sizeof(struct pl_sample_filter_params));

    sampleFilterParams->lut_entries = vsapi->propGetInt(in, "lut_entries", 0, &err);
    sampleFilterParams->cutoff = vsapi->propGetFloat(in, "cutoff", 0, &err);
    sampleFilterParams->antiring = vsapi->propGetFloat(in, "antiring", 0, &err);

    const char * filter = vsapi->propGetData(in, "filter", 0, &err);

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

    d.sampleParams = sampleFilterParams;

    data = malloc(sizeof(d));
    *data = d;

    vsapi->createFilter(in, out, "Shader", VSPlaceboShaderInit, VSPlaceboShaderGetFrame, VSPlaceboShaderFree, fmParallel, 0, data, core);
}
