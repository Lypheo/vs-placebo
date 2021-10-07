#include <vapoursynth/VapourSynth4.h>
#include <vapoursynth/VSHelper4.h>
#include <stdio.h>
#include <libp2p/p2p_api.h>
#include <pthread.h>
#include "vs-placebo.h"

typedef struct {
    VSNode *node;
    const VSVideoInfo *vi;
    struct priv *p;
    enum pl_chroma_location chromaLocation;
    enum pl_color_system matrix;
    enum pl_color_levels range;
    enum pl_color_primaries prims;
    enum pl_color_transfer trc;
    pthread_mutex_t lock;
} FilterData;

bool filter_S(void *priv, void *dst, struct pl_plane_data *src, FilterData * d, const VSAPI *vsapi, VSCore* core)
{
    struct priv *p = priv;
    // Upload planes
    struct pl_plane planes[PL_MAX_PLANES];
    struct pl_plane out_planes[PL_MAX_PLANES];

    bool ok = true;
    for (int i = 0; i < d->vi->format.numPlanes; ++i) {
        ok &= pl_upload_plane(p->gpu, &planes[i], &p->tex_in[i], &src[i]);
        out_planes[i] = planes[i];
        out_planes[i].texture = p->tex_out[i];
    }

    if (!ok) {
        vsapi->logMessage(mtCritical, "Failed uploading data to the GPU!\n", core);
        return false;
    }

    // Process planes

    const struct pl_color_repr crpr = {
            .bits = {
                    .sample_depth = d->vi->format.bytesPerSample * 8,
                    .color_depth = d->vi->format.bitsPerSample,
                    .bit_shift = 0
            },
            .sys = d->matrix,
            .levels = d->range
    };
    const struct pl_color_space csp = {
            .transfer = d->trc,
            .primaries = d->prims,
            .light = PL_COLOR_LIGHT_DISPLAY
    };

    struct pl_frame src_frame = {
            .planes = { planes[0], planes[1], planes[2], 0 },
            .num_planes = d->vi->format.numPlanes,
            .repr = crpr,
            .color = csp
    };
    pl_frame_set_chroma_location(&src_frame, d->chromaLocation);

    struct pl_frame out = {
            .num_planes = d->vi->format.numPlanes,
            .repr = crpr,
            .color = csp,
            .planes = { out_planes[0], out_planes[1], out_planes[2], 0 }
    };
    pl_frame_set_chroma_location(&out, d->chromaLocation);

    struct pl_render_params renderParams = pl_render_default_params;
    ok &= pl_render_image(p->rr, &src_frame, &out, &renderParams);

    if (!ok) {
        vsapi->logMessage(mtCritical, "Failed rendering!\n", core);
        return false;
    }
    uint8_t** dst_planes = (uint8_t**) dst;
    for (int i = 0; i < d->vi->format.numPlanes; ++i) {
        ok = pl_tex_download(p->gpu, &(struct pl_tex_transfer_params) {
                .tex = p->tex_out[i],
                .ptr = dst_planes[i],
        });
    }
    if (!ok) {
        vsapi->logMessage(mtCritical, "Failed downloading data from the GPU!\n", core);
        return false;
    }

    return true;
}

bool config_S(void *priv, struct pl_plane_data *data, const VSAPI *vsapi, FilterData* d, VSCore* core)
{
    struct priv *p = priv;
    bool ok = true;
    const struct pl_fmt* fmt[d->vi->format.numPlanes];
    for (int j = 0; j < d->vi->format.numPlanes; ++j) {
        fmt[j] = pl_plane_find_fmt(p->gpu, NULL, &data[j]);
        if (!fmt[j]) {
            vsapi->logMessage(mtCritical, "Failed configuring filter: no good texture format!\n", core);
            return false;
        }

        ok &= pl_tex_recreate(p->gpu, &p->tex_in[j], &(struct pl_tex_params) {
            .w = data[j].width,
            .h = data[j].height,
            .format = fmt[j],
            .sampleable = true,
            .host_writable = true,
        });

        ok &= pl_tex_recreate(p->gpu, &p->tex_out[j], &(struct pl_tex_params) {
                .w = data[j].width,
                .h = data[j].height,
                .format = fmt[j],
                .renderable = true,
                .host_readable = true,
        });
    }

    if (!ok) {
        vsapi->logMessage(mtCritical, "Failed creating GPU textures!\n", core);
        return false;
    }

    return true;
}

static const VSFrame *VS_CC RGetFrame(int n, int activationReason, void *instanceData, void **frameData, VSFrameContext *frameCtx, VSCore *core, const VSAPI *vsapi) {
    FilterData *d = (FilterData *)instanceData;

    if (activationReason == arInitial) {
        vsapi->requestFrameFilter(n, d->node, frameCtx);
    } else if (activationReason == arAllFramesReady) {
        const VSFrame *frame = vsapi->getFrameFilter(n, d->node, frameCtx);
        int ih = vsapi->getFrameHeight(frame, 0);
        int iw = vsapi->getFrameWidth(frame, 0);
        VSFrame *dst = vsapi->newVideoFrame(&d->vi->format, iw, ih, frame, core);

        VSMap *props = vsapi->getFramePropertiesRO(frame);
        int err;
        d->chromaLocation = GetIntDefault(_ChromaLocation, (d->vi->format.subSamplingW == 1) ? (PL_CHROMA_LEFT-1) : (PL_CHROMA_CENTER-1)) + 1;
        d->range = GetIntDefault(_ColorRange, d->vi->format.colorFamily == cfYUV ? 0 /* limited*/ : 1) + 1;
        d->matrix = vs_to_pl_matrix(GetIntDefault(_Matrix, 1)); // default to 709
        d->trc = vs_to_pl_trc(GetIntDefault(_Transfer, 1));
        d->prims = vs_to_pl_prm(GetIntDefault(_Primaries, 1));

        struct pl_plane_data planes[PL_MAX_PLANES] = {0};
        uint8_t* dst_planes[d->vi->format.numPlanes];
        for (int j = 0; j < d->vi->format.numPlanes; ++j) {
            planes[j] = (struct pl_plane_data) {
                    .type = PL_FMT_UNORM, // TODO: support float
                    .width = vsapi->getFrameWidth(frame, j),
                    .height = vsapi->getFrameHeight(frame, j),
                    .pixel_stride = d->vi->format.bytesPerSample,
                    .row_stride =  vsapi->getStride(frame, j),
                    .pixels =  vsapi->getReadPtr(frame, j),
            };

            planes[j].component_size[0] = d->vi->format.bytesPerSample*8;
            planes[j].component_pad[0] = 0;
            planes[j].component_map[0] = j;

            dst_planes[j] = vsapi->getWritePtr(dst, j);
        }
        pthread_mutex_lock(&d->lock);
        if (config_S(d->p, planes, vsapi, d, core)) {
            filter_S(d->p, dst_planes, planes, d, vsapi, core);
        }
        pthread_mutex_unlock(&d->lock);

        vsapi->freeFrame(frame);
        return dst;
    }

    return NULL;
}

static void VS_CC RFree(void *instanceData, VSCore *core, const VSAPI *vsapi) {
    FilterData *d = (FilterData *)instanceData;
    vsapi->freeNode(d->node);
    free(d);
}

void VS_CC RCreate(const VSMap *in, VSMap *out, void *userData, VSCore *core, const VSAPI *vsapi) {
    FilterData d;
    FilterData *data;

    d.p = init();
    d.node = vsapi->mapGetNode(in, "clip", 0, 0);
    d.vi = vsapi->getVideoInfo(d.node);

    data = (FilterData *) malloc(sizeof(d));
    *data = d;

    VSFilterDependency deps[] = {{d.node, rpStrictSpatial}}; /* Depending the the request patterns you may want to change this */
    vsapi->createVideoFilter(out, "Render", data->vi, RGetFrame, RFree, fmParallel, deps, 1, data, core);
}

