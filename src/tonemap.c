#include <inttypes.h>
#include <pthread.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>

#include "VapourSynth.h"

#include "libp2p/p2p_api.h"

#include "vs-placebo.h"

#ifdef HAVE_DOVI
#include "libdovi/rpu_parser.h"
#include "dovi_meta.h"
#endif

enum supported_colorspace {
    CSP_SDR = 0,
    CSP_HDR10,
    CSP_HLG,
    CSP_DOVI,
};

typedef struct {
    VSNodeRef *node;
    const VSVideoInfo *vi;
    struct priv * vf;

    struct pl_render_params *renderParams;

    enum supported_colorspace src_csp;
    struct pl_color_space *src_pl_csp;
    struct pl_color_space *dst_pl_csp;

    pthread_mutex_t lock;

    int64_t original_src_max;
    int64_t original_src_min;
    
    bool is_subsampled;
    enum pl_chroma_location chromaLocation;

    bool use_dovi;
} TMData;

bool vspl_tonemap_do_planes(TMData *tm_data, struct pl_plane* planes,
                 const struct pl_color_repr src_repr, const struct pl_color_repr dst_repr)
{
    struct priv *p = tm_data->vf;

    struct pl_frame img = {
        .num_planes = 3,
        .planes     = {planes[0], planes[1], planes[2]},
        .repr       = src_repr,
        .color      = *tm_data->src_pl_csp,
    };

    if (tm_data->is_subsampled) {
        pl_frame_set_chroma_location(&img, tm_data->chromaLocation);
    }

    struct pl_frame out = {
        .num_planes = 1,
        .planes = {{
            .texture = p->tex_out[0],
            .components = p->tex_out[0]->params.format->num_components,
            .component_mapping = {0, 1, 2, 3},
        }},
        .repr = dst_repr,
        .color = *tm_data->dst_pl_csp,
    };

    return pl_render_image(p->rr, &img, &out, tm_data->renderParams);
}

bool vspl_tonemap_reconfig(void *priv, struct pl_plane_data *data, const VSAPI *vsapi)
{
    struct priv *p = priv;

    const struct pl_fmt *fmt = pl_plane_find_fmt(p->gpu, NULL, &data[0]);
    if (!fmt) {
        vsapi->logMessage(mtCritical, "Failed configuring filter: no good texture format!\n");
        return false;
    }

    bool ok = true;
    for (int i = 0; i < 3; ++i) {
        ok &= pl_tex_recreate(p->gpu, &p->tex_in[i], pl_tex_params(
            .w = data->width,
            .h = data->height,
            .format = fmt,
            .sampleable = true,
            .host_writable = true,
        ));
    }

    const struct pl_plane_data plane_data = {
        .type = PL_FMT_UNORM,
        .component_map = {0, 1, 2, 0},
        .component_pad = {0, 0, 0, 0},
        .component_size = {16, 16, 16, 0},
        .width = 10,
        .height = 10,
        .row_stride = 60,
        .pixel_stride = 6
    };

    const struct pl_fmt *out = pl_plane_find_fmt(p->gpu, NULL, &plane_data);

    ok &= pl_tex_recreate(p->gpu, &p->tex_out[0], pl_tex_params(
        .w = data->width,
        .h = data->height,
        .format = out,
        .renderable = true,
        .host_readable = true,
        .storable = true,
        .blit_dst = true,
    ));

    if (!ok) {
        vsapi->logMessage(mtCritical, "Failed creating GPU textures!\n");
        return false;
    }

    return true;
}

bool vspl_tonemap_filter(TMData *tm_data, void *dst, struct pl_plane_data *src, const VSAPI *vsapi,
               const struct pl_color_repr src_repr, const struct pl_color_repr dst_repr)
{
    struct priv *p = tm_data->vf;

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
    if (!vspl_tonemap_do_planes(tm_data, planes, src_repr, dst_repr)) {
        vsapi->logMessage(mtCritical, "Failed processing planes!\n");
        return false;
    }

    pl_fmt out_fmt = p->tex_out[0]->params.format;

    // Download planes
    ok = pl_tex_download(p->gpu, pl_tex_transfer_params(
        .tex = p->tex_out[0],
        .row_pitch = (src->row_stride / src->pixel_stride) * out_fmt->texel_size,
        .ptr = dst,
    ));

    if (!ok) {
        vsapi->logMessage(mtCritical, "Failed downloading data from the GPU!\n");
        return false;
    }

    return true;
}

static void VS_CC VSPlaceboTMInit(VSMap *in, VSMap *out, void **instanceData, VSNode *node, VSCore *core, const VSAPI *vsapi) {
    TMData *d = (TMData *) * instanceData;
    VSVideoInfo new_vi = (VSVideoInfo) * (d->vi);
    const VSFormat f = *new_vi.format;

    new_vi.format = vsapi->registerFormat(f.colorFamily, f.sampleType, f.bitsPerSample, 0, 0, core);

    vsapi->setVideoInfo(&new_vi, 1, node);
}

static const VSFrameRef *VS_CC VSPlaceboTMGetFrame(int n, int activationReason, void **instanceData, void **frameData,
                                          VSFrameContext *frameCtx, VSCore *core, const VSAPI *vsapi)
{
    TMData *tm_data = (TMData *) * instanceData;

    if (activationReason == arInitial) {
        vsapi->requestFrameFilter(n, tm_data->node, frameCtx);
    } else if (activationReason == arAllFramesReady) {
        const VSFrameRef *frame = vsapi->getFrameFilter(n, tm_data->node, frameCtx);

        int w = vsapi->getFrameWidth(frame, 0);    
        int h = vsapi->getFrameHeight(frame, 0);

        const VSFormat *src_fmt = tm_data->vi->format;
        const VSFormat *dstFmt = vsapi->registerFormat(src_fmt->colorFamily, src_fmt->sampleType, src_fmt->bitsPerSample, 0, 0, core);

        VSFrameRef *dst = vsapi->newVideoFrame(dstFmt, w, h, frame, core);

        const bool srcIsRGB = src_fmt->colorFamily == cmRGB;

        enum pl_color_system src_sys = srcIsRGB
                                        ? PL_COLOR_SYSTEM_RGB
                                        : PL_COLOR_SYSTEM_BT_2020_NC;
        enum pl_color_system dst_sys = PL_COLOR_SYSTEM_RGB;

        struct pl_color_repr src_repr = {
            .bits = {
                .sample_depth = 16,
                .color_depth = 16,
                .bit_shift = 0
            },
            .sys = src_sys,
        };

        struct pl_color_repr dst_repr = {
            .bits = {
                .sample_depth = 16,
                .color_depth = 16,
                .bit_shift = 0
            },
            .sys = dst_sys,
            .levels = PL_COLOR_LEVELS_FULL,
            .alpha = PL_ALPHA_PREMULTIPLIED,
        };

        int err;
        const VSMap *props = vsapi->getFramePropsRO(frame);

        int64_t props_levels = vsapi->propGetInt(props, "_ColorRange", 0, &err);

        if (!err) {
            // Existing range prop
            src_repr.levels = props_levels ? PL_COLOR_LEVELS_LIMITED : PL_COLOR_LEVELS_FULL;
        }

        if (!srcIsRGB) {
            dst_repr.levels = PL_COLOR_LEVELS_LIMITED;

            if (!err && !props_levels) {
                // Existing range & not limited
                dst_repr.levels = PL_COLOR_LEVELS_FULL;
            }

            if (tm_data->dst_pl_csp->transfer == PL_COLOR_TRC_BT_1886) {
                dst_repr.sys = PL_COLOR_SYSTEM_BT_709;
            } else if (tm_data->dst_pl_csp->transfer == PL_COLOR_TRC_PQ || tm_data->dst_pl_csp->transfer == PL_COLOR_TRC_HLG) {
                dst_repr.sys = PL_COLOR_SYSTEM_BT_2020_NC;
            }
        }

        struct pl_color_space *src_pl_csp = tm_data->src_pl_csp;

        // ST2086 metadata
        // Update metadata from props
        const double maxCll = vsapi->propGetFloat(props, "ContentLightLevelMax", 0, &err);
        const double maxFall = vsapi->propGetFloat(props, "ContentLightLevelAverage", 0, &err);

        src_pl_csp->hdr.max_cll = maxCll;
        src_pl_csp->hdr.max_fall = maxFall;

        if (tm_data->original_src_max < 1) {
            src_pl_csp->hdr.max_luma = vsapi->propGetFloat(props, "MasteringDisplayMaxLuminance", 0, &err);
        }

        if (tm_data->original_src_min <= 0) {
            src_pl_csp->hdr.min_luma = vsapi->propGetFloat(props, "MasteringDisplayMinLuminance", 0, &err);
        }

        pl_color_space_infer(src_pl_csp);

        const double *primariesX = vsapi->propGetFloatArray(props, "MasteringDisplayPrimariesX", &err);
        const double *primariesY = vsapi->propGetFloatArray(props, "MasteringDisplayPrimariesY", &err);

        const int numPrimariesX = vsapi->propNumElements(props, "MasteringDisplayPrimariesX");
        const int numPrimariesY = vsapi->propNumElements(props, "MasteringDisplayPrimariesY");

        if (primariesX && primariesY && numPrimariesX == 3 && numPrimariesY == 3) {
            src_pl_csp->hdr.prim.red.x = primariesX[0];
            src_pl_csp->hdr.prim.red.y = primariesY[0];
            src_pl_csp->hdr.prim.green.x = primariesX[1];
            src_pl_csp->hdr.prim.green.y = primariesY[1];
            src_pl_csp->hdr.prim.blue.x = primariesX[2];
            src_pl_csp->hdr.prim.blue.y = primariesY[2];

            // White point comes with primaries
            const double whitePointX = vsapi->propGetFloat(props, "MasteringDisplayWhitePointX", 0, &err);
            const double whitePointY = vsapi->propGetFloat(props, "MasteringDisplayWhitePointY", 0, &err);

            if (whitePointX && whitePointY) {
                src_pl_csp->hdr.prim.white.x = whitePointX;
                src_pl_csp->hdr.prim.white.y = whitePointY;
            }
        } else {
            // Assume DCI-P3 D65 default?
            pl_raw_primaries_merge(&src_pl_csp->hdr.prim, pl_raw_primaries_get(PL_COLOR_PRIM_DISPLAY_P3));
        }

        tm_data->chromaLocation = vsapi->propGetInt(props, "_ChromaLocation", 0, &err);

        // FFMS2 prop is -1 to match zimg
        // However, libplacebo matches AVChromaLocation
        if (!err) {
            tm_data->chromaLocation += 1;
        }

        // DOVI
        #if PL_API_VER >= 185
            struct pl_dovi_metadata *dovi_meta = NULL;
            uint8_t dovi_profile = 0;

            #ifdef HAVE_DOVI
                if (tm_data->use_dovi && vsapi->propNumElements(props, "DolbyVisionRPU")) {
                    uint8_t *doviRpu = (uint8_t *) vsapi->propGetData(props, "DolbyVisionRPU", 0, &err);
                    size_t doviRpuSize = (size_t) vsapi->propGetDataSize(props, "DolbyVisionRPU", 0, &err);

                    if (doviRpu && doviRpuSize) {
                        // fprintf(stderr, "Got Dolby Vision RPU, size %"PRIi64" at %"PRIxPTR"\n", doviRpuSize, (uintptr_t) doviRpu);

                        DoviRpuOpaque *rpu = dovi_parse_unspec62_nalu(doviRpu, doviRpuSize);
                        const DoviRpuDataHeader *header = dovi_rpu_get_header(rpu);

                        if (!header) {
                            fprintf(stderr, "Failed parsing RPU: %s\n", dovi_rpu_get_error(rpu));
                        } else {
                            dovi_profile = header->guessed_profile;

                            dovi_meta = create_dovi_meta(rpu, header);
                            dovi_rpu_free_header(header);
                        }

                        // Profile 5
                        if (tm_data->src_csp == CSP_DOVI) {
                            src_repr.sys = PL_COLOR_SYSTEM_DOLBYVISION;
                            src_repr.dovi = dovi_meta;

                            if (dovi_profile == 5) {
                                dst_repr.levels = PL_COLOR_LEVELS_FULL;
                            }
                        }

                        // Update mastering display from RPU
                        if (header->vdr_dm_metadata_present_flag) {
                            const DoviVdrDmData *vdr_dm_data = dovi_rpu_get_vdr_dm_data(rpu);

                            src_pl_csp->hdr.min_luma =
                                pl_hdr_rescale(PL_HDR_PQ, PL_HDR_NITS, vdr_dm_data->source_min_pq / 4095.0f);
                            src_pl_csp->hdr.max_luma =
                                pl_hdr_rescale(PL_HDR_PQ, PL_HDR_NITS, vdr_dm_data->source_max_pq / 4095.0f);

                            if (vdr_dm_data->dm_data.level6) {
                                const DoviExtMetadataBlockLevel6 *meta = vdr_dm_data->dm_data.level6;
                                
                                if (!maxCll || !maxFall) {
                                    src_pl_csp->hdr.max_cll = meta->max_content_light_level;
                                    src_pl_csp->hdr.max_fall = meta->max_frame_average_light_level;
                                }
                            }

                            dovi_rpu_free_vdr_dm_data(vdr_dm_data);
                        }
                        
                        dovi_rpu_free(rpu);
                    }
                }
            #endif
        #endif

        struct pl_plane_data planes[3] = {};
        for (int i = 0; i < 3; ++i) {
            planes[i] = (struct pl_plane_data) {
                .type = PL_FMT_UNORM,
                .width = vsapi->getFrameWidth(frame, i),
                .height = vsapi->getFrameHeight(frame, i),
                .pixel_stride = dstFmt->bytesPerSample,
                .row_stride = vsapi->getStride(frame, i),
                .pixels = vsapi->getReadPtr((VSFrameRef *) frame, i),
            };

            planes[i].component_size[0] = 16;
            planes[i].component_pad[0] = 0;
            planes[i].component_map[0] = i;
        }

        void *packed_dst = malloc(w * h * 2 * 3);
        pthread_mutex_lock(&tm_data->lock); // libplacebo isn’t thread-safe

        if (vspl_tonemap_reconfig(tm_data->vf, planes, vsapi)) {
            vspl_tonemap_filter(tm_data, packed_dst, planes, vsapi, src_repr, dst_repr);
        }

        pthread_mutex_unlock(&tm_data->lock);

        struct p2p_buffer_param pack_params = {
            .width = w,
            .height = h,
            .packing = p2p_bgr48_le,
            .src[0] = packed_dst,
            .src_stride[0] = w * 2 * 3,
        };

        for (int i = 0; i < 3; ++i) {
            pack_params.dst[i] = vsapi->getWritePtr(dst, i);
            pack_params.dst_stride[i] = vsapi->getStride(dst, i);
        }

        p2p_unpack_frame(&pack_params, 0);
        free(packed_dst);

        #if PL_API_VER >= 185
            if (dovi_meta)
                free((void *) dovi_meta);
        #endif

        vsapi->freeFrame(frame);
        return dst;
    }

    return 0;
}

static void VS_CC VSPlaceboTMFree(void *instanceData, VSCore *core, const VSAPI *vsapi) {
    TMData *tm_data = (TMData *) instanceData;
    vsapi->freeNode(tm_data->node);
    VSPlaceboUninit(tm_data->vf);

    free((void *) tm_data->src_pl_csp);
    free((void *) tm_data->dst_pl_csp);
    free((void *) tm_data->renderParams->peak_detect_params);
    free((void *) tm_data->renderParams->color_map_params);
    free(tm_data->renderParams);

    pthread_mutex_destroy(&tm_data->lock);
    free(tm_data);
}

void VS_CC VSPlaceboTMCreate(const VSMap *in, VSMap *out, void *userData, VSCore *core, const VSAPI *vsapi) {
    TMData d;
    TMData *tm_data;
    int err;
    enum pl_log_level log_level;

    if (pthread_mutex_init(&d.lock, NULL) != 0)
    {
        vsapi->setError(out, "placebo.Tonemap: mutex init failed\n");
        return;
    }

    log_level = vsapi->propGetInt(in, "log_level", 0, &err);
    if (err)
        log_level = PL_LOG_ERR;

    d.node = vsapi->propGetNode(in, "clip", 0, 0);
    d.vi = vsapi->getVideoInfo(d.node);

    d.vf = VSPlaceboInit(log_level);

    if (d.vi->format->bitsPerSample != 16) {
        vsapi->setError(out, "placebo.Tonemap: Input must be 16 bits per sample!");
        vsapi->freeNode(d.node);
        return;
    }

    struct pl_color_map_params *colorMapParams = malloc(sizeof(struct pl_color_map_params));
    *colorMapParams = pl_color_map_default_params;

    // Tone mapping function
    int64_t function_index = vsapi->propGetInt(in, "tone_mapping_function", 0, &err);

    if (function_index >= pl_num_tone_map_functions) {
        function_index = 0;
    }

    colorMapParams->tone_mapping_function = pl_tone_map_functions[function_index];

    const double tone_mapping_param = vsapi->propGetFloat(in, "tone_mapping_param", 0, &err);
    colorMapParams->tone_mapping_param = tone_mapping_param;

    if (err) {
        // Set default param from selected function
        colorMapParams->tone_mapping_param = colorMapParams->tone_mapping_function->param_def;
    }

#define COLORM_PARAM(par, type) colorMapParams->par = vsapi->propGet##type(in, #par, 0, &err); \
        if (err) colorMapParams->par = pl_color_map_default_params.par;

    COLORM_PARAM(intent, Int)
    COLORM_PARAM(gamut_mode, Int)
    COLORM_PARAM(tone_mapping_mode, Int)
    COLORM_PARAM(tone_mapping_crosstalk, Float)

    struct pl_peak_detect_params *peakDetectParams = malloc(sizeof(struct pl_peak_detect_params));
    *peakDetectParams = pl_peak_detect_default_params;

#define PEAK_PARAM(par, type) peakDetectParams->par = vsapi->propGet##type(in, #par, 0, &err); \
        if (err) peakDetectParams->par = pl_peak_detect_default_params.par;

    PEAK_PARAM(smoothing_period, Float)
    PEAK_PARAM(scene_threshold_low, Float)
    PEAK_PARAM(scene_threshold_high, Float)

    struct pl_color_space *src_pl_csp = malloc((sizeof(struct pl_color_space)));
    struct pl_color_space *dst_pl_csp = malloc((sizeof(struct pl_color_space)));

    int src_csp = vsapi->propGetInt(in, "src_csp", 0, &err);
    int dst_csp = vsapi->propGetInt(in, "dst_csp", 0, &err);

    if (src_csp == CSP_DOVI && d.vi->format->colorFamily == cmRGB) {
        vsapi->setError(out, "placebo.Tonemap: Dolby Vision source colorspace must be a YUV clip!");
        vsapi->freeNode(d.node);

        if (colorMapParams)
            free((void *) colorMapParams);
        if (peakDetectParams)
            free((void *) peakDetectParams);
        if (src_pl_csp)
            free((void *) src_pl_csp);
        if (dst_pl_csp)
            free((void *) dst_pl_csp);

        return;
    }

    switch (src_csp) {
        case CSP_HDR10:
        case CSP_DOVI:
            *src_pl_csp = pl_color_space_hdr10;
            break;
        case CSP_HLG:
            *src_pl_csp = pl_color_space_bt2020_hlg;
            break;
        default:
            vsapi->setError(out, "Invalid source colorspace for tonemapping.\n");
            return;
    };
    
    switch (dst_csp) {
        case CSP_SDR:
            *dst_pl_csp = pl_color_space_bt709;
            break;
        case CSP_HDR10:
            *dst_pl_csp = pl_color_space_hdr10;
            break;
        case CSP_HLG:
            *dst_pl_csp = pl_color_space_bt2020_hlg;
            break;
        default:
            vsapi->setError(out, "Invalid target colorspace for tonemapping.\n");
            return;
    };

    int64_t src_max = vsapi->propGetFloat(in, "src_max", 0, &err);
    int64_t src_min = vsapi->propGetFloat(in, "src_min", 0, &err);

    src_pl_csp->hdr.max_luma = src_max;
    src_pl_csp->hdr.min_luma = src_min;

    pl_color_space_infer(src_pl_csp);

    dst_pl_csp->hdr.max_luma = vsapi->propGetFloat(in, "dst_max", 0, &err);
    dst_pl_csp->hdr.min_luma = vsapi->propGetFloat(in, "dst_min", 0, &err);

    pl_color_space_infer(dst_pl_csp);

    int peak_detection = vsapi->propGetInt(in, "dynamic_peak_detection", 0, &err);
    if (err)
        peak_detection = 1;

    bool use_dovi = vsapi->propGetInt(in, "use_dovi", 0, &err);
    if (err)
        use_dovi = src_csp == CSP_DOVI;

    struct pl_render_params *renderParams = malloc(sizeof(struct pl_render_params));
    *renderParams = pl_render_default_params;

    renderParams->color_map_params = colorMapParams;
    renderParams->peak_detect_params = peak_detection ? peakDetectParams : NULL;
    renderParams->sigmoid_params = &pl_sigmoid_default_params;
    renderParams->dither_params = &pl_dither_default_params;
    renderParams->cone_params = NULL;
    renderParams->color_adjustment = NULL;
    renderParams->deband_params = NULL;

    d.renderParams = renderParams;
    d.src_pl_csp = src_pl_csp;
    d.dst_pl_csp = dst_pl_csp;
    d.src_csp = src_csp;
    d.original_src_max = src_max;
    d.original_src_min = src_min;
    d.is_subsampled = d.vi->format->subSamplingW || d.vi->format->subSamplingH;
    d.use_dovi = use_dovi;

    tm_data = malloc(sizeof(d));
    *tm_data = d;

    vsapi->createFilter(in, out, "Tonemap", VSPlaceboTMInit, VSPlaceboTMGetFrame, VSPlaceboTMFree, fmParallel, 0, tm_data, core);
}
