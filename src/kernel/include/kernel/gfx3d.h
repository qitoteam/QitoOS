/*
 * QitoOS - Software 3D rasterizer
 * Depth buffer, perspective-correct textured triangles, frustum culling, gfx3d API.
 * Prerequisite for Minecraft – chunked voxel renderer needs this.
 */

#ifndef QITO_GFX3D_H
#define QITO_GFX3D_H

#include <kernel/types.h>

typedef struct {
    float x, y, z, w;
} gfx3d_vec4;

typedef struct {
    float x, y, z;
} gfx3d_vec3;

typedef struct {
    float u, v;
} gfx3d_vec2;

typedef struct {
    gfx3d_vec3 position;
    gfx3d_vec2 texcoord;
    uint32_t color;
    float    w;
} gfx3d_vertex;

typedef struct {
    int width, height;
    uint32_t *color_buffer;
    float *depth_buffer;
} gfx3d_context;

typedef struct {
    float m[4][4];
} gfx3d_mat4;

void gfx3d_init(void);
gfx3d_context *gfx3d_create_context(int width, int height);
void gfx3d_destroy_context(gfx3d_context *ctx);
void gfx3d_clear(gfx3d_context *ctx, uint32_t color, float depth);
void gfx3d_draw_triangle(gfx3d_context *ctx,
                         gfx3d_vertex v0, gfx3d_vertex v1, gfx3d_vertex v2,
                         const uint32_t *texture, int tex_w, int tex_h);
void gfx3d_present(gfx3d_context *ctx, int dst_x, int dst_y);
void gfx3d_set_modelview(gfx3d_mat4 mat);
void gfx3d_set_projection(gfx3d_mat4 mat);

gfx3d_mat4 gfx3d_mat4_identity(void);
gfx3d_mat4 gfx3d_mat4_perspective(float fov, float aspect, float near, float far);
gfx3d_mat4 gfx3d_mat4_translate(float x, float y, float z);
gfx3d_mat4 gfx3d_mat4_rotate_y(float angle);

#endif /* QITO_GFX3D_H */
