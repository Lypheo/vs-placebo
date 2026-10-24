#include <inttypes.h>
#include <pthread.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>

#include <VapourSynth4.h>

#include "p2p_api.h"
#include "vs-placebo.h"

#ifdef HAVE_DOVI
#include <libdovi/rpu_parser.h>
#include "dovi_meta.h"
#endif

enum supported_colorspace {
    CSP_SDR = 0,
    CSP_HDR10,
    CSP_HLG,
    CSP_DOVI,
};

typedef struct {
    VSNode *node;
    const VSVideoInfo *vi;
    VSVideoInfo vi_out;
    struct priv *vf;

    struct pl_render_params *renderParams;

    enum supported_colorspace src_csp;
    enum supported_colorspace dst_csp;

    struct pl_color_space *src_pl_csp;
    struct pl_color_space *dst_pl_csp;

    float original_src_max;
    float original_src_min;

    bool is_subsampled;
    enum pl_chroma_location chromaLocation;

    bool use_dovi;
    VSNode *el_node;
    const VSVideoInfo *el_vi;
} TMData;

bool vspl_tonemap_reconfig(TMData *tm_data, VSCore *core, const VSAPI *vsapi,
                           struct pl_plane_data *data)
{
    struct priv *p = tm_data->vf;

    pl_fmt fmt = pl_plane_find_fmt(p->gpu, NULL, data);
    if (!fmt) {
        vsapi->logMessage(mtCritical, "Failed configuring filter: no good texture format!\n", core);
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

    pl_fmt out = pl_plane_find_fmt(p->gpu, NULL, &plane_data);

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
        vsapi->logMessage(mtCritical, "Failed creating GPU textures!\n", core);
        return false;
    }

    return ok;
}

bool vspl_tonemap_el_reconfig(TMData *tm_data, VSCore *core, const VSAPI *vsapi,
                              struct pl_plane_data *data)
{
    struct priv *p = tm_data->vf;

    pl_fmt fmt = pl_plane_find_fmt(p->gpu, NULL, data);
    if (!fmt) {
        vsapi->logMessage(mtCritical, "Failed finding format for EL clip!\n", core);
        return false;
    }

    bool ok = true;
    for (int i = 0; i < 3; ++i) {
        ok &= pl_tex_recreate(p->gpu, &p->el_tex[i], pl_tex_params(
            .w = data->width,
            .h = data->height,
            .format = fmt,
            .sampleable = true,
            .host_writable = true,
        ));
    }

    return ok;
}

bool vspl_tonemap_filter(TMData *tm_data, VSCore *core, const VSAPI *vsapi, void *dst,
                         const struct pl_frame *src_frame, const struct pl_frame *dst_frame)
{
    struct priv *p = tm_data->vf;
    bool ok = true;

    // Process plane
    if (!pl_render_image(p->rr, src_frame, dst_frame, tm_data->renderParams)) {
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

bool vspl_tonemap_prepare_src_frame(TMData *tm_data, VSCore *core, const VSAPI *vsapi,
                                    const struct pl_plane_data *plane_data,
                                    struct pl_frame *src_frame, const VSFrame *el_frame,
                                    struct pl_frame *el_pl_frame)
{
    struct priv *p = tm_data->vf;

    bool ok = true;
    for (int i = 0; i < 3; ++i) {
        ok &= pl_upload_plane(p->gpu, &src_frame->planes[i], &p->tex_in[i], &plane_data[i]);
    }

    if (!ok) {
        vsapi->logMessage(mtCritical, "Failed uploading data to the GPU!\n", core);
        return false;
    }

    if (tm_data->is_subsampled)
        pl_frame_set_chroma_location(src_frame, tm_data->chromaLocation);

#if PL_API_VER >= 369
    if (el_frame && el_pl_frame) {
        struct pl_plane_data el_plane_data[3] = {};
        const VSVideoFormat *el_fmt = &tm_data->el_vi->format;

        for (int i = 0; i < 3; ++i) {
            el_plane_data[i] = (struct pl_plane_data) {
                .type = PL_FMT_UNORM,
                .width = vsapi->getFrameWidth(el_frame, i),
                .height = vsapi->getFrameHeight(el_frame, i),
                .pixel_stride = el_fmt->bytesPerSample,
                .row_stride = vsapi->getStride(el_frame, i),
                .pixels = vsapi->getReadPtr((VSFrame *) el_frame, i),
            };

            el_plane_data[i].component_size[0] = el_fmt->bytesPerSample * 8;
            el_plane_data[i].component_pad[0] = 0;
            el_plane_data[i].component_map[0] = i;
        }

        if (!vspl_tonemap_el_reconfig(tm_data, core, vsapi, &el_plane_data[0])) {
            vsapi->logMessage(mtCritical, "Failed creating EL GPU textures!\n", core);
            return false;
        }

        for (int i = 0; i < 3; ++i) {
            ok &=
                pl_upload_plane(p->gpu, &el_pl_frame->planes[i], &p->el_tex[i], &el_plane_data[i]);
        }

        pl_frame_set_chroma_location(el_pl_frame, PL_CHROMA_TOP_LEFT);

        if (!ok) {
            vsapi->logMessage(mtCritical, "Failed uploading EL data to the GPU!\n", core);
            return false;
        }

        src_frame->enhancement_layer = el_pl_frame;
    }
#endif

    return ok;
}

struct pl_frame vspl_tonemap_create_dst_frame(TMData *tm_data, const struct pl_color_repr dst_repr)
{
    struct priv *p = tm_data->vf;

    const struct pl_frame dst_frame = {
        .num_planes = 1,
        .planes = {{
            .texture = p->tex_out[0],
            .components = p->tex_out[0]->params.format->num_components,
            .component_mapping = {0, 1, 2, 3},
        }},
        .repr = dst_repr,
        .color = *tm_data->dst_pl_csp,
    };

    return dst_frame;
}

void vspl_tonemap_map_frame_csp(TMData *tm_data, const VSAPI *vsapi, const VSMap *props,
                                struct pl_color_space *frame_pl_csp)
{
    int err;

    // ST2086 metadata
    // Update metadata from props
    const double maxCll = vsapi->mapGetFloat(props, "ContentLightLevelMax", 0, &err);
    const double maxFall = vsapi->mapGetFloat(props, "ContentLightLevelAverage", 0, &err);

    frame_pl_csp->hdr.max_cll = maxCll;
    frame_pl_csp->hdr.max_fall = maxFall;

    if (tm_data->original_src_max < 1) {
        frame_pl_csp->hdr.max_luma =
            vsapi->mapGetFloat(props, "MasteringDisplayMaxLuminance", 0, &err);
    }

    if (tm_data->original_src_min <= 0) {
        frame_pl_csp->hdr.min_luma =
            vsapi->mapGetFloat(props, "MasteringDisplayMinLuminance", 0, &err);
    }

    const double scene_avg = vsapi->mapGetFloat(props, "PLSceneAvg", 0, &err);

    const int scene_max_len = vsapi->mapNumElements(props, "PLSceneMax");

    if (scene_max_len) {
        const double *prop_scene_max = vsapi->mapGetFloatArray(props, "PLSceneMax", &err);
        if (prop_scene_max) {
            if (scene_max_len == 1) {
                frame_pl_csp->hdr.avg_pq_y = pl_hdr_rescale(PL_HDR_NITS, PL_HDR_PQ, scene_avg);
                frame_pl_csp->hdr.max_pq_y =
                    pl_hdr_rescale(PL_HDR_NITS, PL_HDR_PQ, prop_scene_max[0]);
            } else if (scene_max_len == 3) {
                frame_pl_csp->hdr.scene_avg = scene_avg;
                frame_pl_csp->hdr.scene_max[0] = prop_scene_max[0];
                frame_pl_csp->hdr.scene_max[1] = prop_scene_max[1];
                frame_pl_csp->hdr.scene_max[2] = prop_scene_max[2];
            }
        }
    }

    const double *primariesX = vsapi->mapGetFloatArray(props, "MasteringDisplayPrimariesX", &err);
    const double *primariesY = vsapi->mapGetFloatArray(props, "MasteringDisplayPrimariesY", &err);

    const int numPrimariesX = vsapi->mapNumElements(props, "MasteringDisplayPrimariesX");
    const int numPrimariesY = vsapi->mapNumElements(props, "MasteringDisplayPrimariesY");

    if (primariesX && primariesY && numPrimariesX == 3 && numPrimariesY == 3) {
        frame_pl_csp->hdr.prim.red.x = primariesX[0];
        frame_pl_csp->hdr.prim.red.y = primariesY[0];
        frame_pl_csp->hdr.prim.green.x = primariesX[1];
        frame_pl_csp->hdr.prim.green.y = primariesY[1];
        frame_pl_csp->hdr.prim.blue.x = primariesX[2];
        frame_pl_csp->hdr.prim.blue.y = primariesY[2];

        // White point comes with primaries
        const double whitePointX =
            vsapi->mapGetFloat(props, "MasteringDisplayWhitePointX", 0, &err);
        const double whitePointY =
            vsapi->mapGetFloat(props, "MasteringDisplayWhitePointY", 0, &err);

        if (whitePointX && whitePointY) {
            frame_pl_csp->hdr.prim.white.x = whitePointX;
            frame_pl_csp->hdr.prim.white.y = whitePointY;
        }
    } else {
        // Assume DCI-P3 D65 default?
        pl_raw_primaries_merge(&frame_pl_csp->hdr.prim,
                               pl_raw_primaries_get(PL_COLOR_PRIM_DISPLAY_P3));
    }

    tm_data->chromaLocation = vsapi->mapGetInt(props, "_ChromaLocation", 0, &err);

    // FFMS2 prop is -1 to match zimg
    // However, libplacebo matches AVChromaLocation
    if (!err) {
        tm_data->chromaLocation += 1;
    }
}

void vspl_tonemap_map_repr(TMData *tm_data, const VSAPI *vsapi, const VSFrame *frame,
                           const VSMap *props, struct pl_color_repr *src_repr,
                           struct pl_color_repr *dst_repr)
{
    int err;
    const VSVideoFormat *src_fmt = vsapi->getVideoFrameFormat(frame);
    const bool srcIsRGB = src_fmt->colorFamily == cfRGB;

    src_repr->sys = srcIsRGB ? PL_COLOR_SYSTEM_RGB : PL_COLOR_SYSTEM_BT_2020_NC;
    dst_repr->sys = PL_COLOR_SYSTEM_RGB;

    int64_t props_levels = vsapi->mapGetInt(props, "_ColorRange", 0, &err);

    if (!err) {
        // Existing range prop
        src_repr->levels = props_levels ? PL_COLOR_LEVELS_LIMITED : PL_COLOR_LEVELS_FULL;
    }

    if (!srcIsRGB) {
        dst_repr->levels = PL_COLOR_LEVELS_LIMITED;

        if (!err && !props_levels) {
            // Existing range & not limited
            dst_repr->levels = PL_COLOR_LEVELS_FULL;
        }

        if (tm_data->dst_pl_csp->transfer == PL_COLOR_TRC_BT_1886) {
            dst_repr->sys = PL_COLOR_SYSTEM_BT_709;
        } else if (tm_data->dst_pl_csp->transfer == PL_COLOR_TRC_PQ ||
                   tm_data->dst_pl_csp->transfer == PL_COLOR_TRC_HLG) {
            dst_repr->sys = PL_COLOR_SYSTEM_BT_2020_NC;
        }
    }
}

bool vspl_tonemap_map_bl_frame(TMData *tm_data, VSFrameContext *frameCtx, VSCore *core,
                               const VSAPI *vsapi, const VSFrame *frame, const VSMap *el_props,
                               struct pl_color_repr *src_repr, struct pl_color_repr *dst_repr,
                               struct pl_color_repr *el_repr, struct pl_color_space *src_pl_csp)
{
    int err;

    // DOVI
    uint8_t *doviRpu = NULL;
    size_t doviRpuSize = 0;
    struct pl_dovi_metadata *dovi_meta = NULL;
    uint8_t dovi_profile = 0;

    const VSMap *props = vsapi->getFramePropertiesRO(frame);
    const VSMap *dovi_rpu_src_props = el_props ? el_props : props;

    if (dovi_rpu_src_props && vsapi->mapNumElements(dovi_rpu_src_props, "DolbyVisionRPU")) {
        doviRpu = (uint8_t *) vsapi->mapGetData(dovi_rpu_src_props, "DolbyVisionRPU", 0, &err);
        doviRpuSize = (size_t) vsapi->mapGetDataSize(dovi_rpu_src_props, "DolbyVisionRPU", 0, &err);
    }

    // Validate props for Dolby Vision mapping
    if (tm_data->src_csp == CSP_DOVI && !(doviRpu && doviRpuSize)) {
        vsapi->setFilterError(
            "placebo.Tonemap: Clips are missing `DolbyVisionRPU` prop for Dolby Vision mapping!",
            frameCtx);
        return false;
    }

    vspl_tonemap_map_repr(tm_data, vsapi, frame, props, src_repr, dst_repr);
    vspl_tonemap_map_frame_csp(tm_data, vsapi, props, src_pl_csp);

#ifdef HAVE_DOVI
    if (doviRpu && doviRpuSize) {
        DoviRpuOpaque *rpu = dovi_parse_unspec62_nalu(doviRpu, doviRpuSize);
        const DoviRpuDataHeader *header = dovi_rpu_get_header(rpu);

        if (!header) {
            fprintf(stderr, "Failed parsing RPU: %s\n", dovi_rpu_get_error(rpu));
        } else {
            dovi_profile = header->guessed_profile;
            dovi_meta = create_dovi_meta(rpu, header, el_props != NULL);
        }

        // Profile 5, 7 or 8 mapping
        if (tm_data->src_csp == CSP_DOVI) {
            src_repr->sys = PL_COLOR_SYSTEM_DOLBYVISION;
            src_repr->dovi = dovi_meta;

            if (dovi_profile == 5) {
                dst_repr->levels = PL_COLOR_LEVELS_FULL;
            }
        }

        if (header) {
           if (tm_data->src_csp == CSP_DOVI) {
               const int bl_bit_depth = header->bl_bit_depth_minus8 + 8;
               src_repr->bits.color_depth = bl_bit_depth;
               src_repr->bits.bit_shift = tm_data->vi->format.bitsPerSample - bl_bit_depth;

               if (el_props && tm_data->el_vi) {
                   const int el_bit_depth = header->el_bit_depth_minus8 + 8;
                   el_repr->bits.color_depth = el_bit_depth;
                   el_repr->bits.bit_shift = tm_data->el_vi->format.bitsPerSample - el_bit_depth;
               }
           }

            if (header->vdr_dm_metadata_present_flag) {
                const DoviVdrDmData *vdr_dm_data = dovi_rpu_get_vdr_dm_data(rpu);

                // Should avoid changing the source black point when mapping to PQ
                // As the source image already has a specific black point,
                // and the RPU isn't necessarily ground truth on the actual coded values
                //
                // Set target black point to the same as source
                if (tm_data->src_csp == CSP_DOVI && tm_data->dst_csp == CSP_HDR10) {
                    tm_data->dst_pl_csp->hdr.min_luma = src_pl_csp->hdr.min_luma;
                } else {
                    src_pl_csp->hdr.min_luma = pl_hdr_rescale(PL_HDR_PQ, PL_HDR_NITS,
                                                              vdr_dm_data->source_min_pq / 4095.0f);
                }

                src_pl_csp->hdr.max_luma =
                    pl_hdr_rescale(PL_HDR_PQ, PL_HDR_NITS, vdr_dm_data->source_max_pq / 4095.0f);

                if (vdr_dm_data->dm_data.level1) {
                    const DoviExtMetadataBlockLevel1 *l1 = vdr_dm_data->dm_data.level1;
                    src_pl_csp->hdr.avg_pq_y = l1->avg_pq / 4095.0f;
                    src_pl_csp->hdr.max_pq_y = l1->max_pq / 4095.0f;
                }

                if (vdr_dm_data->dm_data.level6) {
                    const DoviExtMetadataBlockLevel6 *meta = vdr_dm_data->dm_data.level6;

                    if (!src_pl_csp->hdr.max_cll || !src_pl_csp->hdr.max_fall) {
                        src_pl_csp->hdr.max_cll = meta->max_content_light_level;
                        src_pl_csp->hdr.max_fall = meta->max_frame_average_light_level;
                    }
                }

                dovi_rpu_free_vdr_dm_data(vdr_dm_data);
            }

            dovi_rpu_free_header(header);
        }

        dovi_rpu_free(rpu);
    }

    pl_color_space_infer_map(src_pl_csp, tm_data->dst_pl_csp);

    return true;
#endif
}

void vspl_tonemap_free_nodes(const VSAPI *vsapi, const TMData *tm_data)
{
    vsapi->freeNode(tm_data->node);

    if (tm_data->el_node)
        vsapi->freeNode(tm_data->el_node);
}

static const VSFrame *VS_CC VSPlaceboTMGetFrame(int n, int activationReason, void *instanceData,
                                                void **frameData, VSFrameContext *frameCtx,
                                                VSCore *core, const VSAPI *vsapi)
{
    TMData *tm_data = (TMData *) instanceData;

    const bool use_dovi_el = tm_data->use_dovi && tm_data->el_node;

    if (activationReason == arInitial) {
        vsapi->requestFrameFilter(n, tm_data->node, frameCtx);

        if (use_dovi_el) {
            vsapi->requestFrameFilter(n, tm_data->el_node, frameCtx);
        }

        return 0;
    } else if (activationReason != arAllFramesReady) {
        return 0;
    }

    VSFrame *dst = NULL;
    void *packed_dst = NULL;
    bool ok = true;

    const VSFrame *frame = vsapi->getFrameFilter(n, tm_data->node, frameCtx);
    const VSFrame *el_frame = NULL;
    const VSMap *el_props = NULL;

    if (use_dovi_el) {
        el_frame = vsapi->getFrameFilter(n, tm_data->el_node, frameCtx);
        el_props = vsapi->getFramePropertiesRO(el_frame);
    }

    const VSVideoFormat *src_fmt = &tm_data->vi->format;
    const VSVideoFormat *dst_fmt = &tm_data->vi_out.format;

    struct pl_color_repr src_repr = {
        .bits = {
            .sample_depth = src_fmt->bytesPerSample * 8,
            .color_depth = src_fmt->bitsPerSample,
        }
    };
    struct pl_color_repr dst_repr = {
        .bits = {
            .sample_depth = dst_fmt->bytesPerSample * 8,
            .color_depth = dst_fmt->bitsPerSample,
        },
        .levels = PL_COLOR_LEVELS_FULL,
        .alpha = PL_ALPHA_PREMULTIPLIED,
    };
    struct pl_color_repr el_repr = {
        .sys = PL_COLOR_SYSTEM_BT_2020_NC,
        .levels = PL_COLOR_LEVELS_LIMITED,
    };
    if (tm_data->el_vi) {
        el_repr.bits.sample_depth = tm_data->el_vi->format.bytesPerSample * 8;
        el_repr.bits.color_depth = tm_data->el_vi->format.bitsPerSample;
    }

    // copy so that dynamic metadata doesn't impact other threads
    struct pl_color_space *src_pl_csp = malloc(sizeof(struct pl_color_space));
    memcpy(src_pl_csp, tm_data->src_pl_csp, sizeof(struct pl_color_space));

    ok = vspl_tonemap_map_bl_frame(tm_data, frameCtx, core, vsapi, frame, el_props, &src_repr,
                                   &dst_repr, &el_repr, src_pl_csp);
    if (!ok)
        goto done;

    struct pl_plane_data plane_data[3] = {};

    for (int i = 0; i < 3; ++i) {
        plane_data[i] = (struct pl_plane_data) {
            .type = PL_FMT_UNORM,
            .width = vsapi->getFrameWidth(frame, i),
            .height = vsapi->getFrameHeight(frame, i),
            .pixel_stride = src_fmt->bytesPerSample,
            .row_stride = vsapi->getStride(frame, i),
            .pixels = vsapi->getReadPtr((VSFrame *) frame, i),
        };

        plane_data[i].component_size[0] = src_fmt->bytesPerSample * 8;
        plane_data[i].component_pad[0] = 0;
        plane_data[i].component_map[0] = i;
    }

    int dst_w = plane_data[0].width;
    int dst_h = plane_data[0].height;

    packed_dst = malloc(dst_w * dst_h * 2 * 3);

    pthread_mutex_lock(&vspl_vulkan_mutex);

    if (vspl_tonemap_reconfig(tm_data, core, vsapi, &plane_data[0])) {
        struct pl_frame src_frame = {
            .num_planes = 3,
            .repr = src_repr,
            .color = *src_pl_csp,
        };
        struct pl_frame el_pl_frame = {
            .num_planes = 3,
            .repr = el_repr,
            .color = pl_color_space_hdr10,
        };

        if (!vspl_tonemap_prepare_src_frame(tm_data, core, vsapi, plane_data, &src_frame, el_frame,
                                            &el_pl_frame)) {
            pthread_mutex_unlock(&vspl_vulkan_mutex);
            goto done;
        }

        struct pl_frame dst_frame = vspl_tonemap_create_dst_frame(tm_data, dst_repr);
        if (!vspl_tonemap_filter(tm_data, core, vsapi, packed_dst, &src_frame, &dst_frame)) {
            pthread_mutex_unlock(&vspl_vulkan_mutex);
            goto done;
        }

        dst = vsapi->newVideoFrame(dst_fmt, dst_w, dst_h, frame, core);
    }

    pthread_mutex_unlock(&vspl_vulkan_mutex);

    if (dst) {
        struct p2p_buffer_param pack_params = {
            .width = dst_w,
            .height = dst_h,
            .packing = p2p_bgr48_le,
            .src[0] = packed_dst,
            .src_stride[0] = dst_w * 2 * 3,
        };

        for (int i = 0; i < 3; ++i) {
            pack_params.dst[i] = vsapi->getWritePtr(dst, i);
            pack_params.dst_stride[i] = vsapi->getStride(dst, i);
        }

        p2p_unpack_frame(&pack_params, 0);
    }

done:
    if (packed_dst)
        free(packed_dst);
    if (src_pl_csp)
        free(src_pl_csp);

    if (src_repr.dovi)
        free((void *) src_repr.dovi);

    vsapi->freeFrame(frame);
    if (el_frame)
        vsapi->freeFrame(el_frame);

    return dst;
}

static void VS_CC VSPlaceboTMFree(void *instanceData, VSCore *core, const VSAPI *vsapi) {
    TMData *tm_data = (TMData *) instanceData;

    vspl_tonemap_free_nodes(vsapi, tm_data);

    VSPlaceboUninit(tm_data->vf);

    free((void *) tm_data->src_pl_csp);
    free((void *) tm_data->dst_pl_csp);
    free((void *) tm_data->renderParams->peak_detect_params);
    free((void *) tm_data->renderParams->color_map_params);
    free(tm_data->renderParams);

    free(tm_data);
}

void VS_CC VSPlaceboTMCreate(const VSMap *in, VSMap *out, void *userData, VSCore *core, const VSAPI *vsapi) {
    TMData d = {};
    TMData *tm_data;
    int err;

    d.node = vsapi->mapGetNode(in, "clip", 0, 0);
    d.vi = vsapi->getVideoInfo(d.node);

    if (d.vi->format.bitsPerSample != 16) {
        vsapi->mapSetError(out, "placebo.Tonemap: Input must be 16 bits per sample!");
        vspl_tonemap_free_nodes(vsapi, &d);
        return;
    }

    int src_csp = vsapi->mapGetInt(in, "src_csp", 0, &err);
    int dst_csp = vsapi->mapGetInt(in, "dst_csp", 0, &err);

    bool use_dovi = vsapi->mapGetInt(in, "use_dovi", 0, &err);
    if (err)
        use_dovi = src_csp == CSP_DOVI;

    if (use_dovi) {
#if PL_API_VER >= 369
        d.el_node = vsapi->mapGetNode(in, "dovi_el", 0, &err);
        d.el_vi = NULL;

        if (d.el_node) {
            d.el_vi = vsapi->getVideoInfo(d.el_node);

            if (d.el_vi->format.bitsPerSample != 16) {
                vsapi->mapSetError(
                    out, "placebo.Tonemap: Dolby Vision EL clip must be 16 bits per sample!");
                vspl_tonemap_free_nodes(vsapi, &d);
                return;
            }
        }
#endif
    }

    if (src_csp == CSP_DOVI &&
        (d.vi->format.colorFamily == cfRGB || (d.el_vi && d.el_vi->format.colorFamily == cfRGB))) {
        vsapi->mapSetError(out,
                           "placebo.Tonemap: Dolby Vision source colorspace must be a YUV clip!");
        vspl_tonemap_free_nodes(vsapi, &d);
        return;
    }

    d.vi_out = *d.vi;
    vsapi->getVideoFormatByID(
        &d.vi_out.format,
        d.vi->format.colorFamily == cfRGB ? pfRGB48 : pfYUV444P16,
        core
    );

    enum pl_log_level log_level = vsapi->mapGetInt(in, "log_level", 0, &err);
    if (err)
        log_level = PL_LOG_ERR;

    d.vf = VSPlaceboInit(log_level);

    struct pl_color_map_params *colorMapParams = malloc(sizeof(struct pl_color_map_params));
    *colorMapParams = pl_color_map_default_params;

    // Gamut mapping function
    int64_t gamut_map_index = vsapi->mapGetInt(in, "gamut_mapping", 0, &err);
    if (!err && gamut_map_index >= 0 && gamut_map_index < pl_num_gamut_map_functions) {
        colorMapParams->gamut_mapping = pl_gamut_map_functions[gamut_map_index];
    }

    // Tone mapping function
    int64_t function_index = vsapi->mapGetInt(in, "tone_mapping_function", 0, &err);
    if (!err && function_index >= 0 && function_index < pl_num_tone_map_functions) {
        colorMapParams->tone_mapping_function = pl_tone_map_functions[function_index];
    }

    const char *function_name = vsapi->mapGetData(in, "tone_mapping_function_s", 0, &err);
    if (function_name && !err) {
        const struct pl_tone_map_function *tm_function = pl_find_tone_map_function(function_name);
        if (tm_function)
            colorMapParams->tone_mapping_function = tm_function;
    }

    const double tone_mapping_param = vsapi->mapGetFloat(in, "tone_mapping_param", 0, &err);
    colorMapParams->tone_mapping_param = tone_mapping_param;

    if (err) {
        // Set default param from selected function
        colorMapParams->tone_mapping_param = colorMapParams->tone_mapping_function->param_def;
    }

#define COLORM_PARAM(par, type) colorMapParams->par = vsapi->mapGet##type(in, #par, 0, &err); \
        if (err) colorMapParams->par = pl_color_map_default_params.par;

    COLORM_PARAM(visualize_lut, Int)
    COLORM_PARAM(metadata, Int)
    COLORM_PARAM(show_clipping, Int)
    COLORM_PARAM(contrast_recovery, Float)

    struct pl_peak_detect_params *peakDetectParams = malloc(sizeof(struct pl_peak_detect_params));
    *peakDetectParams = pl_peak_detect_default_params;

#define PEAK_PARAM(par, type) peakDetectParams->par = vsapi->mapGet##type(in, #par, 0, &err); \
        if (err) peakDetectParams->par = pl_peak_detect_default_params.par;

    PEAK_PARAM(smoothing_period, Float)
    PEAK_PARAM(scene_threshold_low, Float)
    PEAK_PARAM(scene_threshold_high, Float)
    PEAK_PARAM(percentile, Float)

    struct pl_color_space *src_pl_csp = malloc((sizeof(struct pl_color_space)));
    struct pl_color_space *dst_pl_csp = malloc((sizeof(struct pl_color_space)));

    switch (src_csp) {
        case CSP_HDR10:
        case CSP_DOVI:
            *src_pl_csp = pl_color_space_hdr10;
            break;
        case CSP_HLG:
            *src_pl_csp = pl_color_space_bt2020_hlg;
            break;
        default:
            vsapi->mapSetError(out, "Invalid source colorspace for tonemapping.\n");
            vspl_tonemap_free_nodes(vsapi, &d);
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
            vsapi->mapSetError(out, "Invalid target colorspace for tonemapping.\n");
            vspl_tonemap_free_nodes(vsapi, &d);
            return;
    };

    const float src_max = vsapi->mapGetFloat(in, "src_max", 0, &err);
    const float src_min = vsapi->mapGetFloat(in, "src_min", 0, &err);

    src_pl_csp->hdr.max_luma = src_max;
    src_pl_csp->hdr.min_luma = src_min;

    dst_pl_csp->hdr.max_luma = vsapi->mapGetFloat(in, "dst_max", 0, &err);
    dst_pl_csp->hdr.min_luma = vsapi->mapGetFloat(in, "dst_min", 0, &err);

    int64_t dst_prim = vsapi->mapGetInt(in, "dst_prim", 0, &err);
    if (!err)
        dst_pl_csp->primaries = dst_prim;

    int peak_detection = vsapi->mapGetInt(in, "dynamic_peak_detection", 0, &err);
    if (err)
        peak_detection = 1;

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
    d.dst_csp = dst_csp;
    d.original_src_max = src_max;
    d.original_src_min = src_min;
    d.is_subsampled = d.vi->format.subSamplingW || d.vi->format.subSamplingH;
    d.use_dovi = use_dovi;

    const VSFilterDependency clip_dep = {d.node, rpStrictSpatial};
    const VSFilterDependency *deps =
        d.el_node ? (VSFilterDependency[]) {clip_dep, {d.el_node, rpStrictSpatial}}
                  : (VSFilterDependency[]) {clip_dep};
    const int num_deps = d.el_node ? 2 : 1;

    tm_data = malloc(sizeof(d));
    *tm_data = d;

    vsapi->createVideoFilter(
        out,
        "Tonemap",
        &d.vi_out,
        VSPlaceboTMGetFrame,
        VSPlaceboTMFree,
        fmParallelRequests,
        deps,
        num_deps,
        tm_data,
        core
    );
}
