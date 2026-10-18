#include "config_vsplacebo.h"

#include <libplacebo/colorspace.h>

#ifdef HAVE_DOVI
#include <libdovi/rpu_parser.h>

static struct pl_dovi_metadata *create_dovi_meta(DoviRpuOpaque *rpu, const DoviRpuDataHeader *hdr,
                                                 const bool el_present)
{
    static struct pl_dovi_metadata dovi_meta; // persist state
    if (hdr->use_prev_vdr_rpu_flag)
        goto done;

    const DoviRpuDataMapping *mapping = dovi_rpu_get_data_mapping(rpu);
    if (!mapping)
        goto skip_mapping;

    const uint8_t bl_bit_depth = hdr->bl_bit_depth_minus8 + 8;
    const float coef_scale = 1.0f / (1ULL << hdr->coefficient_log2_denom);

    for (int c = 0; c < 3; c++) {
        const DoviReshapingCurve curve = mapping->curves[c];

        struct pl_reshape_data *cmp = &dovi_meta.comp[c];
        cmp->num_pivots = curve.pivots.len;
        memset(cmp->method, curve.mapping_idc, sizeof(cmp->method));

        uint16_t pivot = 0;
        for (int pivot_idx = 0; pivot_idx < cmp->num_pivots; pivot_idx++) {
            pivot += curve.pivots.data[pivot_idx];
#if PL_API_VER >= 387
            cmp->pivots[pivot_idx] = (float) pivot / (1 << bl_bit_depth);
#else
            cmp->pivots[pivot_idx] = (float) pivot / ((1 << bl_bit_depth) - 1);
#endif
        }

        for (int i = 0; i < cmp->num_pivots - 1; i++) {
            memset(cmp->poly_coeffs[i], 0, sizeof(cmp->poly_coeffs[i]));

            if (curve.polynomial) {
                const DoviPolynomialCurve *poly_curve = curve.polynomial;

                for (int k = 0; k <= poly_curve->poly_order_minus1.data[i] + 1; k++) {
                    int64_t ipart = poly_curve->poly_coef_int.list[i]->data[k];
                    uint64_t fpart = poly_curve->poly_coef.list[i]->data[k];
                    cmp->poly_coeffs[i][k] = ipart + coef_scale * fpart;
                }
            } else if (curve.mmr) {
                const DoviMMRCurve *mmr_curve = curve.mmr;

                int64_t ipart = mmr_curve->mmr_constant_int.data[i];
                uint64_t fpart = mmr_curve->mmr_constant.data[i];
                cmp->mmr_constant[i] = ipart + coef_scale * fpart;
                cmp->mmr_order[i] = mmr_curve->mmr_order_minus1.data[i] + 1;

                for (int j = 0; j < cmp->mmr_order[i]; j++) {
                    for (int k = 0; k < 7; k++) {
                        ipart = mmr_curve->mmr_coef_int.list[i]->list[j]->data[k];
                        fpart = mmr_curve->mmr_coef.list[i]->list[j]->data[k];
                        cmp->mmr_coeffs[i][j][k] = ipart + coef_scale * fpart;
                    }
                }
            }
        }
    }

#if PL_API_VER >= 369
    // See `pl_map_dovi_metadata` for documentation
    // Re-implemented using libdovi
    dovi_meta.nlq_active = el_present && !hdr->disable_residual_flag &&
                           strcmp(hdr->el_type, "FEL") == 0 &&
                           mapping->nlq_method_idc == 0; // LINEAR_DZ NLQ

    if (dovi_meta.nlq_active && mapping->nlq) {
        const DoviRpuDataNlq *src = mapping->nlq;

#if PL_API_VER < 387
        const uint8_t el_bit_depth = hdr->el_bit_depth_minus8 + 8;
        const float el_scale = 1.0f / ((1 << el_bit_depth) - 1);
        const double slope_scale = ((1ULL << el_bit_depth) - 1) * coef_scale;
#endif

        for (int c = 0; c < 3; c++) {
            struct pl_dovi_nlq_data *dst = &dovi_meta.nlq[c];
#if PL_API_VER < 387
            const double S = (src->linear_deadzone_slope_int[c] << hdr->coefficient_log2_denom) |
                             src->linear_deadzone_slope[c];
            const double T = (src->linear_deadzone_threshold_int[c] << hdr->coefficient_log2_denom) |
                             src->linear_deadzone_threshold[c];
            dst->offset = el_scale * src->nlq_offset[c];
            dst->deadzone_slope = slope_scale * S;
            dst->deadzone_threshold = coef_scale * (T - 0.5 * S);
#else
            dst->offset = src->nlq_offset[c];

            int64_t ipart = src->vdr_in_max_int[c];
            uint64_t fpart = src->vdr_in_max[c];
            dst->hdr_in_max = ipart + coef_scale * fpart;

            ipart = src->linear_deadzone_slope_int[c];
            fpart = src->linear_deadzone_slope[c];
            dst->deadzone_slope = ipart + coef_scale * fpart;
            
            ipart = src->linear_deadzone_threshold_int[c];
            fpart = src->linear_deadzone_threshold[c];
            dst->deadzone_threshold = ipart + coef_scale * fpart;
#endif
        }
    }
#endif // PL_API_VER >= 369

    dovi_rpu_free_data_mapping(mapping);
skip_mapping:

    if (hdr->vdr_dm_metadata_present_flag) {
        const DoviVdrDmData *dm_data = dovi_rpu_get_vdr_dm_data(rpu);
        if (!dm_data)
            goto done;

        const uint32_t *off = &dm_data->ycc_to_rgb_offset0;
        for (int i = 0; i < 3; i++)
            dovi_meta.nonlinear_offset[i] = (float) off[i] / (1 << 28);

        const int16_t *src = &dm_data->ycc_to_rgb_coef0;
        float *dst = &dovi_meta.nonlinear.m[0][0];
        for (int i = 0; i < 9; i++)
            dst[i] = src[i] / 8192.0;

        src = &dm_data->rgb_to_lms_coef0;
        dst = &dovi_meta.linear.m[0][0];
        for (int i = 0; i < 9; i++)
            dst[i] = src[i] / 16384.0;

        dovi_rpu_free_vdr_dm_data(dm_data);
    }

done:
    struct pl_dovi_metadata *ret = malloc(sizeof(dovi_meta));
    memcpy(ret, &dovi_meta, sizeof(dovi_meta));
    return ret;
}

#endif // HAVE_DOVI
