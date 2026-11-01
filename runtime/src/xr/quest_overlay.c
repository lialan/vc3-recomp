/*
 * quest_overlay.c - the handgun, its sight, the laser and the settings
 * panel, drawn for each eye by quest_host.c into a layer of their own over
 * the game's screen (raw GLES 3, on the host's thread).
 *
 * The approach is The House of the Dead 4 VR's (lindberghrecomp's
 * xr_overlay.c): the gun on the controller's aim pose, and a sight whose
 * reticle is drawn where the shot lands on the screen but seen only through
 * the sight's glass -- like a real red dot zeroed at the screen, so it is
 * true wherever the eye is behind it. Here the gun is a handgun, modelled
 * in boxes (a polymer service pistol: slide, frame, raked grip, trigger
 * guard), with three sights:
 *
 *   iron sights    a three-dot front post and rear notch;
 *   red dot        a small reflex sight on the slide, a 3 MOA dot;
 *   holographic    a window sight, a 65 MOA ring round a 1 MOA dot.
 *
 * The aim ray quest_host.c fires along is the sight's line (the bore raised
 * by qo_sight_height), so whichever sight is on, it shows where the shot
 * goes. The slide runs back with each shot.
 *
 * Everything is in metres in the LOCAL reference space. The layer is
 * transparent: drawn with alpha written alongside, colours premultiplied.
 */
#if defined(__ANDROID__)

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifndef CHIHIRO_RENDERER_VULKAN
#include <GLES3/gl3.h>
#endif

#include "quest_overlay.h"
#include "chihiro/chihiro_profile.h"

/* ── The gun, in boxes ────────────────────────────────────────────────── */

/* Gun space: x right, y up, -z along the barrel; the bore on the aim ray. */
#include "quest_gun_geometry.inc"

float qo_sight_height(int sight)
{
    const float *p;
    if (sight < 0 || sight >= QO_SIGHT_COUNT)
        sight = 0;
    p = chihiro_profile()->vr.sight_height;     /* the game's gun, else this one */
    return p[sight] > 0.0f ? p[sight] : SIGHT_Y[sight];
}

void qo_muzzle(float out[3])
{
    out[0] = 0;
    out[1] = 0;
    out[2] = MUZZLE_Z;
}

/* ── GL ───────────────────────────────────────────────────────────────── */

#ifndef CHIHIRO_RENDERER_VULKAN
static GLuint prog_gun, prog_spot, prog_panel, vao_gun, vbo_gun, vao_spot, vbo_spot,
              vao_quad, vbo_quad, panel_tex;
static GLuint prog_ret;                  /* the sight's glass with its reticle */
static GLint u_ret_mvp, u_ret_color, u_ret_tint, u_ret_c, u_ret_r;
static GLint u_gun_mvp, u_gun_model, u_spot_mvp, u_spot_color, u_spot_shape,
             u_panel_mvp, u_panel_tex;

static GLuint shader(GLenum type, const char *src)
{
    GLuint s = glCreateShader(type);
    GLint ok = 0;
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512] = "";
        glGetShaderInfoLog(s, sizeof log, NULL, log);
        fprintf(stderr, "[overlay] shader: %s\n", log);
    }
    return s;
}

static GLuint program(const char *vs, const char *fs)
{
    GLuint p = glCreateProgram();
    GLint ok = 0;
    glAttachShader(p, shader(GL_VERTEX_SHADER, vs));
    glAttachShader(p, shader(GL_FRAGMENT_SHADER, fs));
    glLinkProgram(p);
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[512] = "";
        glGetProgramInfoLog(p, sizeof log, NULL, log);
        fprintf(stderr, "[overlay] link: %s\n", log);
        return 0;
    }
    return p;
}

int qo_init(void)
{
    static const float quad[8] = { -1, -1, 1, -1, -1, 1, 1, 1 };
    int a;

    prog_gun = program(
        "#version 300 es\n"
        "layout(location=0) in vec3 p; layout(location=1) in vec3 n; layout(location=2) in vec3 c;\n"
        "uniform mat4 mvp, model; out vec3 col;\n"
        "void main() {\n"
        "  vec3 wn = normalize(mat3(model) * n);\n"
        "  float l = 0.38 + 0.62 * max(dot(wn, normalize(vec3(0.3, 1.0, 0.4))), 0.0);\n"
        "  float spec = pow(max(dot(wn, normalize(vec3(-0.2, 1.0, 0.6))), 0.0), 24.0) * 0.18;\n"
        "  col = c * l + spec; gl_Position = mvp * vec4(p, 1.0); }\n",
        "#version 300 es\nprecision mediump float; in vec3 col; out vec4 o;\n"
        "void main() { o = vec4(col, 1.0); }\n");
    /* a soft round spot, a ring, or (shape.x < 0) a flat quad */
    prog_spot = program(
        "#version 300 es\nlayout(location=0) in vec3 p; layout(location=1) in vec2 uv;\n"
        "uniform mat4 mvp; out vec2 t; void main() { t = uv; gl_Position = mvp * vec4(p, 1.0); }\n",
        "#version 300 es\nprecision mediump float; in vec2 t; uniform vec4 color; uniform vec2 shape;\n"
        "out vec4 o;\n"
        "void main() { float d = length(t);\n"
        "  float a = shape.x < 0.0 ? 1.0\n"
        "          : shape.y > 0.0 ? clamp(1.0 - abs(d - shape.x) / shape.y, 0.0, 1.0)\n"
        "          : clamp(1.0 - d, 0.0, 1.0);\n"
        "  a = color.a * (shape.x < 0.0 ? 1.0 : a * a);\n"
        "  o = vec4(color.rgb * a, a); }\n");      /* premultiplied */
    /* The glass: a faint tint, and the reticle where the eye's line to the
     * target crosses it (t: glass coordinates in metres, c: that point,
     * r: dot radius, ring radius, ring half-width). Premultiplied. */
    prog_ret = program(
        "#version 300 es\nlayout(location=0) in vec3 p; layout(location=1) in vec2 uv;\n"
        "uniform mat4 mvp; out vec2 t; void main() { t = uv; gl_Position = mvp * vec4(p, 1.0); }\n",
        "#version 300 es\nprecision highp float; in vec2 t; uniform vec4 color, tint;\n"
        "uniform vec2 c; uniform vec3 r; out vec4 o;\n"
        "void main() { float d = length(t - c);\n"
        "  float dotA = 1.0 - smoothstep(r.x * 0.55, r.x, d);\n"
        "  float ringA = r.y > 0.0 ? 1.0 - smoothstep(r.z * 0.4, r.z, abs(d - r.y)) : 0.0;\n"
        "  float a = max(dotA, ringA) * color.a;\n"
        "  float ta = tint.a * (1.0 - a);\n"
        "  o = vec4(color.rgb * a + tint.rgb * ta, a + ta); }\n");
    prog_panel = program(
        "#version 300 es\nlayout(location=0) in vec2 p; uniform mat4 mvp; out vec2 t;\n"
        "void main() { t = vec2(p.x * 0.5 + 0.5, 0.5 - p.y * 0.5);\n"
        "  gl_Position = mvp * vec4(p, 0.0, 1.0); }\n",
        "#version 300 es\nprecision mediump float; in vec2 t; uniform sampler2D tex; out vec4 o;\n"
        "void main() { vec4 c = texture(tex, t); o = vec4(c.rgb * c.a, c.a); }\n");
    if (!prog_gun || !prog_spot || !prog_panel || !prog_ret)
        return 0;
    u_ret_mvp = glGetUniformLocation(prog_ret, "mvp");
    u_ret_color = glGetUniformLocation(prog_ret, "color");
    u_ret_tint = glGetUniformLocation(prog_ret, "tint");
    u_ret_c = glGetUniformLocation(prog_ret, "c");
    u_ret_r = glGetUniformLocation(prog_ret, "r");
    u_gun_mvp = glGetUniformLocation(prog_gun, "mvp");
    u_gun_model = glGetUniformLocation(prog_gun, "model");
    u_spot_mvp = glGetUniformLocation(prog_spot, "mvp");
    u_spot_color = glGetUniformLocation(prog_spot, "color");
    u_spot_shape = glGetUniformLocation(prog_spot, "shape");
    u_panel_mvp = glGetUniformLocation(prog_panel, "mvp");
    u_panel_tex = glGetUniformLocation(prog_panel, "tex");

    build_gun(QO_SIGHT_IRON);
    glGenVertexArrays(1, &vao_gun);
    glGenBuffers(1, &vbo_gun);
    glBindVertexArray(vao_gun);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_gun);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)sizeof mesh, mesh, GL_DYNAMIC_DRAW);
    for (a = 0; a < 3; a++) {
        glVertexAttribPointer((GLuint)a, 3, GL_FLOAT, GL_FALSE, 36, (const void *)(uintptr_t)(a * 12));
        glEnableVertexAttribArray((GLuint)a);
    }
    glGenVertexArrays(1, &vao_spot);
    glGenBuffers(1, &vbo_spot);
    glBindVertexArray(vao_spot);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_spot);
    glBufferData(GL_ARRAY_BUFFER, 6 * 5 * sizeof(float), NULL, GL_DYNAMIC_DRAW);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 20, (const void *)0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 20, (const void *)12);
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glGenVertexArrays(1, &vao_quad);
    glGenBuffers(1, &vbo_quad);
    glBindVertexArray(vao_quad);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_quad);
    glBufferData(GL_ARRAY_BUFFER, sizeof quad, quad, GL_STATIC_DRAW);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 8, (const void *)0);
    glEnableVertexAttribArray(0);
    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    fprintf(stderr, "[overlay] handgun: %d triangles\n", gun_verts / 3);
    return 1;
}

/* ── Maths (column-major) ─────────────────────────────────────────────── */

#include "quest_overlay_math.inc"

/* ── Spots, beams, glass ──────────────────────────────────────────────── */

static void spot_draw(const float *vp, const float q[6][5], float r, float g, float b, float a,
                      float ring, float thick)
{
    glBindBuffer(GL_ARRAY_BUFFER, vbo_spot);
    glBufferData(GL_ARRAY_BUFFER, 6 * 5 * sizeof(float), q, GL_DYNAMIC_DRAW);
    glUniformMatrix4fv(u_spot_mvp, 1, GL_FALSE, vp);
    glUniform4f(u_spot_color, r, g, b, a);
    glUniform2f(u_spot_shape, ring, thick);
    glDrawArrays(GL_TRIANGLES, 0, 6);
}

#include "quest_overlay_effects.inc"

static void beam(const float *vp, const float eye[3], const float a[3], const float b[3], float w,
                 float r, float g, float bl, float alpha)
{
    float q[6][5];
    if (qo_beam_vertices(q, eye, a, b, w))
        spot_draw(vp, (const float (*)[5])q, r, g, bl, alpha, 0, 0);
}

static void spot(const float *vp, const float eye[3], const float p[3], float rad,
                 float r, float g, float b, float alpha, float ring, float thick)
{
    float q[6][5];
    if (qo_spot_vertices(q, eye, p, rad))
        spot_draw(vp, (const float (*)[5])q, r, g, b, alpha, ring, thick);
}

static void glass_reticle(const float *vp, const float *model, int sight, const float eye[3],
                          const float target[3])
{
    float q[6][5], center[2], r[3];
    qo_glass_vertices(q, center, r, model, sight, qo_sight_height(sight), eye, target);
    glUseProgram(prog_ret);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_spot);
    glBufferData(GL_ARRAY_BUFFER, 6 * 5 * sizeof(float), q, GL_DYNAMIC_DRAW);
    glUniformMatrix4fv(u_ret_mvp, 1, GL_FALSE, vp);
    glUniform4f(u_ret_color, 1.0f, 0.09f, 0.05f, 1.0f);
    glUniform4f(u_ret_tint, 0.55f, 0.75f, 0.85f, 0.10f);
    glUniform2f(u_ret_c, center[0], center[1]);
    glUniform3f(u_ret_r, r[0], r[1], r[2]);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glUseProgram(prog_spot);
}

/* ── Per eye ──────────────────────────────────────────────────────────── */

void qo_draw(const QoFrame *f, const float viewproj[16], const float eye[3])
{
    float model[16], slide[16], kick[16], mvp[16], muzzle[3];
    float dx, dy, dz, dist;
    int sight = f->sight;

    if (!prog_gun)
        return;
    if (sight < 0 || sight >= QO_SIGHT_COUNT)
        sight = QO_SIGHT_IRON;
    if (sight != cur_sight) {
        build_gun(sight);
        glBindBuffer(GL_ARRAY_BUFFER, vbo_gun);
        glBufferSubData(GL_ARRAY_BUFFER, 0, (GLsizeiptr)(gun_verts * 9 * sizeof(float)), mesh);
    }
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glDisable(GL_CULL_FACE);
    glDisable(GL_SCISSOR_TEST);
    pose_matrix(f->gun_q, f->gun_p, model);
    kick_matrix(f->kick, kick);
    mul(model, model, kick);
    {   /* the slide runs back and returns */
        float back[16];
        memset(back, 0, 64);
        back[0] = back[5] = back[10] = back[15] = 1;
        back[14] = 0.022f * (f->kick > 0.6f ? (1.0f - f->kick) / 0.4f : f->kick / 0.6f);
        mul(slide, model, back);
    }
    {
        float m0[3] = { 0, 0, MUZZLE_Z };
        apply(model, m0, muzzle);
    }

    if (f->gun_valid && f->show_gun) {
        glClearDepthf(1);
        glDepthMask(GL_TRUE);
        glClear(GL_DEPTH_BUFFER_BIT);
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(GL_LESS);
        glDisable(GL_BLEND);
        glUseProgram(prog_gun);
        glBindVertexArray(vao_gun);
        mul(mvp, viewproj, slide);
        glUniformMatrix4fv(u_gun_mvp, 1, GL_FALSE, mvp);
        glUniformMatrix4fv(u_gun_model, 1, GL_FALSE, slide);
        glDrawArrays(GL_TRIANGLES, 0, slide_verts);
        mul(mvp, viewproj, model);
        glUniformMatrix4fv(u_gun_mvp, 1, GL_FALSE, mvp);
        glUniformMatrix4fv(u_gun_model, 1, GL_FALSE, model);
        glDrawArrays(GL_TRIANGLES, slide_verts, gun_verts - slide_verts);
    }

    dx = f->end[0] - eye[0];
    dy = f->end[1] - eye[1];
    dz = f->end[2] - eye[2];
    dist = sqrtf(dx * dx + dy * dy + dz * dz);
    glUseProgram(prog_spot);
    glBindVertexArray(vao_spot);
    /* premultiplied over: colour and alpha alike */
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

    /* The optic's glass with its reticle, part of the gun: depth-tested
     * against the gun (the hood in front of it hides it), not written. */
    if (f->gun_valid && f->show_gun && sight != QO_SIGHT_IRON) {
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(GL_LEQUAL);
        glDepthMask(GL_FALSE);
        glass_reticle(viewproj, slide, sight, eye, f->end);
    }

    /* laser, dot and flash over everything */
    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    if (f->gun_valid && f->beam)
        beam(viewproj, eye, muzzle, f->end, 0.0015f, 1.0f, 0.12f, 0.08f, 0.85f);
    if (f->gun_valid && f->dot)
        spot(viewproj, eye, f->end, 0.006f * dist, 1.0f, 0.15f, 0.1f, 1.0f, 0, 0);
    if (f->gun_valid && f->flash)
        spot(viewproj, eye, muzzle, 0.035f, 1.0f, 0.75f, 0.3f, 0.9f, 0, 0);
    glDisable(GL_BLEND);
    glDepthMask(GL_TRUE);
    glBindVertexArray(0);
}

/* ── The settings panel ───────────────────────────────────────────────── */

void qo_panel_upload(const unsigned char *rgba, int w, int h)
{
    if (!panel_tex) {
        glGenTextures(1, &panel_tex);
        glBindTexture(GL_TEXTURE_2D, panel_tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }
    glBindTexture(GL_TEXTURE_2D, panel_tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    glBindTexture(GL_TEXTURE_2D, 0);
}

void qo_draw_panel(const float mvp[16])
{
    if (!prog_panel || !panel_tex)
        return;
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glUseProgram(prog_panel);
    glUniformMatrix4fv(u_panel_mvp, 1, GL_FALSE, mvp);
    glUniform1i(u_panel_tex, 0);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, panel_tex);
    glBindVertexArray(vao_quad);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glBindVertexArray(0);
    glBindTexture(GL_TEXTURE_2D, 0);
    glDisable(GL_BLEND);
}

#endif /* OpenGL renderer */
#endif /* __ANDROID__ */
