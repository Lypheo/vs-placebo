#include "config_vsplacebo.h"
#include <libplacebo/colorspace.h>

#if PL_API_VER >= 185
    #ifdef HAVE_DOVI
        #include "libdovi/rpu_parser.h"

        static struct pl_dovi_metadata *create_dovi_meta(DoviRpuOpaque *rpu,
                                                        const DoviRpuDataHeader *hdr)
        {
            static struct pl_dovi_metadata dovi_meta; // persist state
            if (hdr->use_prev_vdr_rpu_flag)
                goto done;

            const DoviRpuDataMapping *vdm = dovi_rpu_get_data_mapping(rpu);
            if (!vdm)
                goto skip_vdm;

            const uint8_t bits = hdr->bl_bit_depth_minus8 + 8;
            const float scale = 1.0f / (1 << hdr->coefficient_log2_denom);

            for (int c = 0; c < 3; c++) {
                struct pl_reshape_data *cmp = &dovi_meta.comp[c];
                uint16_t pivot = 0;
                cmp->num_pivots = hdr->num_pivots_minus_2[c] + 2;
                for (int pivot_idx = 0; pivot_idx < cmp->num_pivots; pivot_idx++) {
                    pivot += hdr->pred_pivot_value[c].data[pivot_idx];
                    cmp->pivots[pivot_idx] = (float) pivot / ((1 << bits) - 1);
                }

                for (int i = 0; i < cmp->num_pivots - 1; i++) {
                    memset(cmp->poly_coeffs[i], 0, sizeof(cmp->poly_coeffs[i]));
                    cmp->method[i] = vdm->mapping_idc[c].data[i];

                    switch (cmp->method[i]) {
                    case 0: // polynomial
                        for (int k = 0; k <= vdm->poly_order_minus1[c].data[i] + 1; k++) {
                            int64_t ipart = vdm->poly_coef_int[c].list[i]->data[k];
                            uint64_t fpart = vdm->poly_coef[c].list[i]->data[k];
                            cmp->poly_coeffs[i][k] = ipart + scale * fpart;
                        }
                        break;
                    case 1: // MMR
                        int64_t ipart = vdm->mmr_constant_int[c].data[i];
                        uint64_t fpart = vdm->mmr_constant[c].data[i];
                        cmp->mmr_constant[i] = ipart + scale * fpart;
                        cmp->mmr_order[i] = vdm->mmr_order_minus1[c].data[i] + 1;
                        for (int j = 1; j <= cmp->mmr_order[i]; j++) {
                            for (int k = 0; k < 7; k++) {
                                ipart = vdm->mmr_coef_int[c].list[i]->list[j]->data[k];
                                fpart = vdm->mmr_coef[c].list[i]->list[j]->data[k];
                                cmp->mmr_coeffs[i][j - 1][k] = ipart + scale * fpart;
                            }
                        }
                        break;
                    }
                }
            }

            dovi_rpu_free_data_mapping(vdm);
        skip_vdm:

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

        done: ;
            struct pl_dovi_metadata *ret = malloc(sizeof(dovi_meta));
            memcpy(ret, &dovi_meta, sizeof(dovi_meta));
            return ret;
        }
    #endif
#endif
