#ifndef VS_PLACEBO_LIBRARY_H
#define VS_PLACEBO_LIBRARY_H

#include <libplacebo/dispatch.h>
#include <libplacebo/shaders/sampling.h>
#include <libplacebo/utils/upload.h>
#include <libplacebo/vulkan.h>

#include "config_vsplacebo.h"

struct format {
    int num_comps;
    int bitdepth;
};

struct plane {
    int subx, suby; // subsampling shift
    struct format fmt;
    size_t stride;
    void *data;
};

#define MAX_PLANES 4

struct image {
    int width, height;
    int num_planes;
    struct plane planes[MAX_PLANES];
};

struct priv {
    pl_log log;
    pl_vulkan vk;
    pl_gpu gpu;
    pl_dispatch dp;
    pl_shader_obj dither_state;

    pl_renderer rr;
    pl_tex tex_in[MAX_PLANES];
    pl_tex tex_out[MAX_PLANES];
};

void *VSPlaceboInit(enum pl_log_level log_level);
void VSPlaceboUninit(void *priv);

#endif //VS_PLACEBO_LIBRARY_H
