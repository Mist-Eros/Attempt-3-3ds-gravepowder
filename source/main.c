/*
 * Gravepowder - a 3D zombie shooter for the Nintendo 3DS
 *
 * This is the first technology skeleton: citro3d renders a lit(ish) low-poly
 * scene on the top screen while citro2d owns the bottom touch screen.  The
 * real game will grow into the modules under source/ - this file is only here
 * to prove the 3D pipeline (shader compile -> citro3d draw -> both screens)
 * works on hardware and in Azahar before we build the game on top of it.
 *
 * Controls in this skeleton: circle pad moves the little cube, Start quits.
 */
#include <3ds.h>
#include <citro3d.h>
#include <citro2d.h>
#include <string.h>

#include "vshader_shbin.h"
#include "grave.h"

#define CLEAR_COLOR 0x0B0A10FF

#define DISPLAY_TRANSFER_FLAGS \
    (GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) | GX_TRANSFER_RAW_COPY(0) | \
    GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) | GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB8) | \
    GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO))

typedef struct { float x, y, z; } SkeletonVertex;

static const SkeletonVertex cube_vertices[] = {
    /* +z */ {-1,-1, 1},{ 1,-1, 1},{ 1, 1, 1}, {-1,-1, 1},{ 1, 1, 1},{-1, 1, 1},
    /* -z */ {-1,-1,-1},{-1, 1,-1},{ 1, 1,-1}, {-1,-1,-1},{ 1, 1,-1},{ 1,-1,-1},
    /* +x */ { 1,-1,-1},{ 1, 1,-1},{ 1, 1, 1}, { 1,-1,-1},{ 1, 1, 1},{ 1,-1, 1},
    /* -x */ {-1,-1,-1},{-1,-1, 1},{-1, 1, 1}, {-1,-1,-1},{-1, 1, 1},{-1, 1,-1},
    /* +y */ {-1, 1,-1},{-1, 1, 1},{ 1, 1, 1}, {-1, 1,-1},{ 1, 1, 1},{ 1, 1,-1},
    /* -y */ {-1,-1,-1},{ 1,-1,-1},{ 1,-1, 1}, {-1,-1,-1},{ 1,-1, 1},{-1,-1, 1},
};
#define CUBE_VERTEX_COUNT (sizeof(cube_vertices) / sizeof(cube_vertices[0]))

static const SkeletonVertex ground_vertices[] = {
    {-20.0f, 0.0f, -20.0f}, { 20.0f, 0.0f, -20.0f}, { 20.0f, 0.0f,  20.0f},
    {-20.0f, 0.0f, -20.0f}, { 20.0f, 0.0f,  20.0f}, {-20.0f, 0.0f,  20.0f},
};
#define GROUND_VERTEX_COUNT (sizeof(ground_vertices) / sizeof(ground_vertices[0]))

static DVLB_s *vshader_dvlb;
static shaderProgram_s shader_program;
static int u_loc_projection;
static int u_loc_model_view;
static C3D_Mtx projection;

static void *cube_vbo;
static void *ground_vbo;

static C3D_RenderTarget *target_top;
static C3D_RenderTarget *target_bottom;

static C2D_TextBuf hud_text_buf;
static C2D_Text hud_title;
static C2D_Text hud_hint;

static GraveInput skeleton_input;
static float cube_x = 0.0f;
static float cube_z = 0.0f;
static float spin = 0.0f;

static void *vbo_upload(const void *data, size_t size)
{
    void *vbo = linearAlloc(size);
    if (vbo != NULL)
        memcpy(vbo, data, size);
    return vbo;
}

static void scene_init(void)
{
    vshader_dvlb = DVLB_ParseFile((u32 *)vshader_shbin, vshader_shbin_size);
    shaderProgramInit(&shader_program);
    shaderProgramSetVsh(&shader_program, &vshader_dvlb->DVLE[0]);
    C3D_BindProgram(&shader_program);

    u_loc_projection = shaderInstanceGetUniformLocation(shader_program.vertexShader, "projection");
    u_loc_model_view = shaderInstanceGetUniformLocation(shader_program.vertexShader, "modelView");

    C3D_AttrInfo *attr = C3D_GetAttrInfo();
    AttrInfo_Init(attr);
    AttrInfo_AddLoader(attr, 0, GPU_FLOAT, 3); /* v0 = position */
    AttrInfo_AddFixed(attr, 1);                /* v1 = flat colour */

    cube_vbo = vbo_upload(cube_vertices, sizeof(cube_vertices));
    ground_vbo = vbo_upload(ground_vertices, sizeof(ground_vertices));

    Mtx_PerspTilt(&projection, C3D_AngleFromDegrees(60.0f), C3D_AspectRatioTop, 0.05f, 200.0f, false);

    /* The first fragment stage just outputs the vertex colour. */
    C3D_TexEnv *env = C3D_GetTexEnv(0);
    C3D_TexEnvInit(env);
    C3D_TexEnvSrc(env, C3D_Both, GPU_PRIMARY_COLOR, 0, 0);
    C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);

    C3D_DepthTest(true, GPU_GREATER, GPU_WRITE_ALL);
    C3D_CullFace(GPU_CULL_BACK_CCW);

    hud_text_buf = C2D_TextBufNew(512);
    C2D_TextParse(&hud_title, hud_text_buf, "GRAVEPOWDER");
    C2D_TextParse(&hud_hint, hud_text_buf, "3D pipeline skeleton - start = quit");
    C2D_TextOptimize(&hud_title);
    C2D_TextOptimize(&hud_hint);
}

static void draw_unit_cube(const C3D_Mtx *view, float x, float y, float z,
                           float sx, float sy, float sz, float r, float g, float b)
{
    C3D_Mtx model;
    Mtx_Identity(&model);
    Mtx_Translate(&model, x, y, z, true);
    Mtx_Scale(&model, sx, sy, sz);
    Mtx_Multiply(&model, view, &model);
    C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, u_loc_model_view, &model);
    C3D_FixedAttribSet(1, r, g, b, 1.0f);
    C3D_DrawArrays(GPU_TRIANGLES, 0, CUBE_VERTEX_COUNT);
}

static void draw_ground(const C3D_Mtx *view)
{
    C3D_Mtx model;
    Mtx_Identity(&model);
    Mtx_Multiply(&model, view, &model);
    C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, u_loc_model_view, &model);
    C3D_FixedAttribSet(1, 0.13f, 0.12f, 0.16f, 1.0f);
    C3D_DrawArrays(GPU_TRIANGLES, 0, GROUND_VERTEX_COUNT);
}

/* Builds a simple third-person-ish orbit camera looking at the origin. */
static void build_view_matrix(C3D_Mtx *view)
{
    C3D_Mtx cam;
    Mtx_LookAt(&cam,
               FVec3_New(7.0f, 6.0f, 11.0f),
               FVec3_New(cube_x * 0.5f, 1.0f, cube_z * 0.5f),
               FVec3_New(0.0f, 1.0f, 0.0f),
               false);
    *view = cam;
}

static void scene_render(void)
{
    C3D_Mtx view;
    C3D_BindProgram(&shader_program);
    C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, u_loc_projection, &projection);

    build_view_matrix(&view);
    C3D_BufInfo *buf = C3D_GetBufInfo();
    BufInfo_Init(buf);

    BufInfo_Add(buf, ground_vbo, sizeof(SkeletonVertex), 1, 0x0);
    draw_ground(&view);

    BufInfo_Init(buf);
    BufInfo_Add(buf, cube_vbo, sizeof(SkeletonVertex), 1, 0x0);
    draw_unit_cube(&view, cube_x, 1.0f, cube_z, 1.0f, 1.0f, 1.0f, 0.85f, 0.62f, 0.25f);
    draw_unit_cube(&view, -3.0f, 0.6f, -2.0f, 0.6f, 1.2f, 0.6f, 0.55f, 0.22f, 0.22f);
    draw_unit_cube(&view,  3.0f, 0.9f, -4.0f, 0.9f, 1.8f, 0.9f, 0.22f, 0.34f, 0.24f);
    draw_unit_cube(&view,  5.0f, 0.5f,  2.0f, 1.4f, 1.0f, 1.4f, 0.30f, 0.30f, 0.40f);
    draw_unit_cube(&view, -6.0f, 0.5f,  4.0f, 1.0f, 1.0f, 1.0f, 0.30f, 0.30f, 0.40f);
}

static void hud_render(void)
{
    C2D_SceneTarget(target_bottom);
    C3D_RenderTargetClear(target_bottom, C3D_CLEAR_ALL, CLEAR_COLOR, 0);
    C2D_Prepare();
    C2D_DrawText(&hud_title, C2D_AlignCenter, 160.0f, 92.0f, 1.0f, 1.1f, 1.1f);
    C2D_DrawText(&hud_hint, C2D_AlignCenter, 160.0f, 124.0f, 1.0f, 0.55f, 0.55f);
    C2D_Flush();
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    gfxInitDefault();
    C3D_Init(C3D_DEFAULT_CMDBUF_SIZE);
    C2D_Init(C2D_DEFAULT_MAX_OBJECTS);

    target_top = C3D_RenderTargetCreate(240, 400, GPU_RB_RGBA8, GPU_RB_DEPTH24_STENCIL8);
    C3D_RenderTargetSetOutput(target_top, GFX_TOP, GFX_LEFT, DISPLAY_TRANSFER_FLAGS);
    target_bottom = C3D_RenderTargetCreate(240, 320, GPU_RB_RGBA8, GPU_RB_DEPTH24_STENCIL8);
    C3D_RenderTargetSetOutput(target_bottom, GFX_BOTTOM, GFX_LEFT, DISPLAY_TRANSFER_FLAGS);

    scene_init();
    grave_input_reset(&skeleton_input);

    while (aptMainLoop()) {
        hidScanInput();
        u32 kDown = hidKeysDown();
        if (kDown & KEY_START)
            break;

        circlePosition stick;
        hidCircleRead(&stick);
        grave_input_stick(&skeleton_input, stick.dx, stick.dy);

        cube_x += skeleton_input.move_x * 0.14f;
        cube_z += skeleton_input.move_y * 0.14f;
        if (cube_x < -14.0f) cube_x = -14.0f;
        if (cube_x >  14.0f) cube_x =  14.0f;
        if (cube_z < -14.0f) cube_z = -14.0f;
        if (cube_z >  14.0f) cube_z =  14.0f;
        spin += 0.01f;

        C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
        C3D_RenderTargetClear(target_top, C3D_CLEAR_ALL, CLEAR_COLOR, 0);
        C3D_FrameDrawOn(target_top);
        scene_render();
        hud_render();
        C3D_FrameEnd(0);
    }

    C2D_TextBufDelete(hud_text_buf);
    linearFree(cube_vbo);
    linearFree(ground_vbo);
    shaderProgramFree(&shader_program);
    DVLB_Free(vshader_dvlb);

    C2D_Fini();
    C3D_Fini();
    gfxExit();
    return 0;
}
