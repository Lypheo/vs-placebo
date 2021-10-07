#ifndef VS_PLACEBO_LIBRARY_H
#define VS_PLACEBO_LIBRARY_H

#include <libplacebo/dispatch.h>
#include <libplacebo/shaders/sampling.h>
#include <libplacebo/utils/upload.h>
#include <libplacebo/vulkan.h>

#define MAX_PLANES 4

struct priv {
    pl_log log;
    pl_vulkan vk;
    pl_gpu gpu;
    pl_dispatch dp;
    pl_shader_obj dither_state;
    pl_tex tex_in[MAX_PLANES];
    pl_tex tex_out[MAX_PLANES];
    pl_renderer rr;
};

void *init(void);
void uninit(void *priv);

#define GetIntDefault(prop, def) vsapi->mapGetInt(props, #prop, 0, &err) || (err ? (def) : 0)

int vs_to_pl_matrix(int matrix);
int vs_to_pl_trc(int trc);
int vs_to_pl_prm(int prim);


#endif //VS_PLACEBO_LIBRARY_H