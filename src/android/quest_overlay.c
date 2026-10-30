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

#include <GLES3/gl3.h>

#include "quest_overlay.h"

/* ── The gun, in boxes ────────────────────────────────────────────────── */

/* Gun space: x right, y up, -z along the barrel; the bore on the aim ray. */
typedef struct { float c[3], h[3], rgb[3]; } Box;

#define POLY   { 0.150f, 0.150f, 0.160f }     /* the frame's polymer */
#define STEEL  { 0.240f, 0.240f, 0.255f }     /* the slide */
#define DARK   { 0.070f, 0.070f, 0.075f }
#define WHITE  { 0.92f, 0.92f, 0.88f }
#define OPTIC  { 0.200f, 0.200f, 0.215f }

#define MUZZLE_Z (-0.066f)

static const Box SLIDE[] = {
    { { 0, 0.009f, 0.027f }, { 0.0128f, 0.015f, 0.093f }, STEEL },          /* the slide */
    { { 0.004f, 0.0242f, -0.005f }, { 0.0065f, 0.0006f, 0.016f }, DARK },   /* ejection port */
    { { 0, 0.0f, MUZZLE_Z - 0.0005f }, { 0.0048f, 0.0048f, 0.0008f }, { 0.02f, 0.02f, 0.02f } },  /* the bore */
    /* rear serrations, both sides */
    { {  0.0129f, 0.010f, 0.092f }, { 0.0004f, 0.010f, 0.0014f }, DARK },
    { {  0.0129f, 0.010f, 0.099f }, { 0.0004f, 0.010f, 0.0014f }, DARK },
    { {  0.0129f, 0.010f, 0.106f }, { 0.0004f, 0.010f, 0.0014f }, DARK },
    { { -0.0129f, 0.010f, 0.092f }, { 0.0004f, 0.010f, 0.0014f }, DARK },
    { { -0.0129f, 0.010f, 0.099f }, { 0.0004f, 0.010f, 0.0014f }, DARK },
    { { -0.0129f, 0.010f, 0.106f }, { 0.0004f, 0.010f, 0.0014f }, DARK },
    /* the front sight: a post with a white dot facing the eye */
    { { 0, 0.0275f, -0.058f }, { 0.0016f, 0.0035f, 0.003f }, DARK },
    { { 0, 0.0285f, -0.0548f }, { 0.0010f, 0.0010f, 0.0003f }, WHITE },
};
static const Box REAR_SIGHT[] = {   /* the notch, white dots either side */
    { { 0, 0.0250f, 0.105f }, { 0.011f, 0.0020f, 0.004f }, DARK },
    { {  0.0075f, 0.0285f, 0.105f }, { 0.0035f, 0.0025f, 0.004f }, DARK },
    { { -0.0075f, 0.0285f, 0.105f }, { 0.0035f, 0.0025f, 0.004f }, DARK },
    { {  0.0075f, 0.0285f, 0.1092f }, { 0.0010f, 0.0010f, 0.0003f }, WHITE },
    { { -0.0075f, 0.0285f, 0.1092f }, { 0.0010f, 0.0010f, 0.0003f }, WHITE },
};
static const Box RED_DOT[] = {      /* a reflex sight, like an RMR, where the rear sight was */
    { { 0, 0.0275f, 0.082f }, { 0.0125f, 0.0035f, 0.022f }, OPTIC },        /* body */
    { {  0.0115f, 0.0385f, 0.066f }, { 0.0015f, 0.0075f, 0.0055f }, OPTIC },/* hood */
    { { -0.0115f, 0.0385f, 0.066f }, { 0.0015f, 0.0075f, 0.0055f }, OPTIC },
    { { 0, 0.0475f, 0.066f }, { 0.013f, 0.0015f, 0.0055f }, OPTIC },
    { { 0, 0.0330f, 0.097f }, { 0.007f, 0.0025f, 0.006f }, OPTIC },         /* emitter */
    { { 0.0125f, 0.0300f, 0.084f }, { 0.0012f, 0.0020f, 0.003f }, DARK },   /* button */
};
static const Box HOLO[] = {         /* a window sight, like an EOTech */
    { { 0, 0.0300f, 0.072f }, { 0.0150f, 0.0060f, 0.040f }, OPTIC },        /* body */
    { {  0.0170f, 0.0500f, 0.050f }, { 0.0025f, 0.0140f, 0.020f }, { 0.14f, 0.14f, 0.15f } },
    { { -0.0170f, 0.0500f, 0.050f }, { 0.0025f, 0.0140f, 0.020f }, { 0.14f, 0.14f, 0.15f } },
    { { 0, 0.0655f, 0.050f }, { 0.0195f, 0.0025f, 0.020f }, { 0.14f, 0.14f, 0.15f } },
    { { 0.0070f, 0.0365f, 0.100f }, { 0.0050f, 0.0008f, 0.006f }, DARK },   /* buttons */
    { { -0.0070f, 0.0365f, 0.100f }, { 0.0050f, 0.0008f, 0.006f }, DARK },
};
static const Box FRAME[] = {
    { { 0, -0.014f, 0.027f }, { 0.0115f, 0.008f, 0.092f }, POLY },          /* dust cover to tang */
    { { 0, -0.0235f, -0.040f }, { 0.0105f, 0.0015f, 0.022f }, DARK },       /* accessory rail */
    { { 0, -0.032f, 0.020f }, { 0.0060f, 0.010f, 0.003f }, POLY },          /* trigger guard */
    { { 0, -0.0425f, 0.040f }, { 0.0060f, 0.0025f, 0.023f }, POLY },
    { { 0, -0.030f, 0.050f }, { 0.0035f, 0.007f, 0.0025f }, DARK },         /* trigger */
    { { 0, -0.016f, 0.122f }, { 0.011f, 0.006f, 0.008f }, POLY },           /* beavertail */
    /* the grip, raked back, with stippled panels */
    { { 0, -0.0355f, 0.090f }, { 0.0145f, 0.0135f, 0.025f }, POLY },
    { { 0, -0.0615f, 0.097f }, { 0.0145f, 0.0135f, 0.025f }, POLY },
    { { 0, -0.0875f, 0.104f }, { 0.0145f, 0.0135f, 0.025f }, POLY },
    { { 0, -0.1135f, 0.111f }, { 0.0145f, 0.0135f, 0.025f }, POLY },
    { {  0.0148f, -0.062f, 0.099f }, { 0.0004f, 0.024f, 0.017f }, DARK },
    { { -0.0148f, -0.062f, 0.099f }, { 0.0004f, 0.024f, 0.017f }, DARK },
    { {  0.0148f, -0.104f, 0.110f }, { 0.0004f, 0.016f, 0.017f }, DARK },
    { { -0.0148f, -0.104f, 0.110f }, { 0.0004f, 0.016f, 0.017f }, DARK },
    { { 0, -0.1290f, 0.112f }, { 0.0155f, 0.0025f, 0.026f }, DARK },        /* magazine base */
};

/* Each sight's line above the bore, and its glass: centre z, half width,
 * half height (around the line). */
static const float SIGHT_Y[QO_SIGHT_COUNT] = { 0.0285f, 0.0385f, 0.0500f };
static const float GLASS[QO_SIGHT_COUNT][3] = {
    { 0, 0, 0 }, { 0.066f, 0.0100f, 0.0075f }, { 0.050f, 0.0145f, 0.0125f },
};

#define MAX_BOXES 64
static float mesh[MAX_BOXES * 36 * 9];
static int slide_verts, gun_verts, cur_sight = -1;

static void add_boxes(int *n, const Box *bx, int count)
{
    static const int face[6][4] = { { 1, 3, 7, 5 }, { 0, 4, 6, 2 }, { 2, 6, 7, 3 },
                                    { 0, 1, 5, 4 }, { 4, 5, 7, 6 }, { 0, 2, 3, 1 } };
    static const float nrm[6][3] = { { 1, 0, 0 }, { -1, 0, 0 }, { 0, 1, 0 },
                                     { 0, -1, 0 }, { 0, 0, 1 }, { 0, 0, -1 } };
    static const int tri[6] = { 0, 1, 2, 0, 2, 3 };
    int p, k, a, f, t;

    for (p = 0; p < count && *n + 36 <= MAX_BOXES * 36; p++) {
        float v[8][3];
        for (k = 0; k < 8; k++)
            for (a = 0; a < 3; a++)
                v[k][a] = bx[p].c[a] + (((k >> a) & 1) ? bx[p].h[a] : -bx[p].h[a]);
        for (f = 0; f < 6; f++)
            for (t = 0; t < 6; t++) {
                float *o = &mesh[(*n)++ * 9];
                memcpy(o, v[face[f][tri[t]]], 12);
                memcpy(o + 3, nrm[f], 12);
                memcpy(o + 6, bx[p].rgb, 12);
            }
    }
}
#define ADD(n, list) add_boxes(n, list, (int)(sizeof list / sizeof *list))

/* The slide (and what rides on it) first, then the frame. */
static void build_gun(int sight)
{
    int n = 0;
    ADD(&n, SLIDE);
    if (sight == QO_SIGHT_IRON) ADD(&n, REAR_SIGHT);
    if (sight == QO_SIGHT_RED_DOT) ADD(&n, RED_DOT);
    if (sight == QO_SIGHT_HOLO) ADD(&n, HOLO);
    slide_verts = n;
    ADD(&n, FRAME);
    gun_verts = n;
    cur_sight = sight;
}

float qo_sight_height(int sight)
{
    return sight >= 0 && sight < QO_SIGHT_COUNT ? SIGHT_Y[sight] : SIGHT_Y[0];
}

void qo_muzzle(float out[3])
{
    out[0] = 0;
    out[1] = 0;
    out[2] = MUZZLE_Z;
}

/* ── GL ───────────────────────────────────────────────────────────────── */

static GLuint prog_gun, prog_spot, prog_panel, vao_gun, vbo_gun, vao_spot, vbo_spot,
              vao_quad, vbo_quad, panel_tex;
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
    prog_panel = program(
        "#version 300 es\nlayout(location=0) in vec2 p; uniform mat4 mvp; out vec2 t;\n"
        "void main() { t = vec2(p.x * 0.5 + 0.5, 0.5 - p.y * 0.5);\n"
        "  gl_Position = mvp * vec4(p, 0.0, 1.0); }\n",
        "#version 300 es\nprecision mediump float; in vec2 t; uniform sampler2D tex; out vec4 o;\n"
        "void main() { vec4 c = texture(tex, t); o = vec4(c.rgb * c.a, c.a); }\n");
    if (!prog_gun || !prog_spot || !prog_panel)
        return 0;
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

static void mul(float *o, const float *a, const float *b)
{
    float r[16];
    int c, k;
    for (c = 0; c < 4; c++)
        for (k = 0; k < 4; k++)
            r[c * 4 + k] = a[k] * b[c * 4] + a[4 + k] * b[c * 4 + 1] + a[8 + k] * b[c * 4 + 2]
                         + a[12 + k] * b[c * 4 + 3];
    memcpy(o, r, 64);
}

static void pose_matrix(const float q[4], const float p[3], float *m)
{
    float x = q[0], y = q[1], z = q[2], w = q[3];
    m[0] = 1 - 2 * (y * y + z * z); m[1] = 2 * (x * y + z * w);     m[2] = 2 * (x * z - y * w);
    m[4] = 2 * (x * y - z * w);     m[5] = 1 - 2 * (x * x + z * z); m[6] = 2 * (y * z + x * w);
    m[8] = 2 * (x * z + y * w);     m[9] = 2 * (y * z - x * w);     m[10] = 1 - 2 * (x * x + y * y);
    m[3] = m[7] = m[11] = 0;
    m[12] = p[0]; m[13] = p[1]; m[14] = p[2]; m[15] = 1;
}

/* the kick: the gun turned muzzle-up about the grip and pushed back */
static void kick_matrix(float k, float *m)
{
    float a = 0.10f * k, c = cosf(a), s = sinf(a);
    const float py = -0.06f, pz = 0.10f;        /* the pivot: the hand */
    memset(m, 0, 64);
    m[0] = 1;
    m[5] = c; m[6] = s;                         /* about +x: -z (the muzzle) rises */
    m[9] = -s; m[10] = c;
    m[15] = 1;
    /* rotate about the pivot, then back along the barrel */
    m[13] = py - (c * py - s * pz);
    m[14] = pz - (s * py + c * pz) + 0.012f * k;
}

static void apply(const float *m, const float p[3], float o[3])
{
    int i;
    for (i = 0; i < 3; i++)
        o[i] = m[i] * p[0] + m[4 + i] * p[1] + m[8 + i] * p[2] + m[12 + i];
}

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

/* A strip from a to b facing the eye, half-width w. */
static void beam(const float *vp, const float eye[3], const float a[3], const float b[3], float w,
                 float r, float g, float bl, float alpha)
{
    float d[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] };
    float e[3] = { eye[0] - a[0], eye[1] - a[1], eye[2] - a[2] };
    float s[3] = { d[1] * e[2] - d[2] * e[1], d[2] * e[0] - d[0] * e[2], d[0] * e[1] - d[1] * e[0] };
    float l = sqrtf(s[0] * s[0] + s[1] * s[1] + s[2] * s[2]);
    int i;
    if (l < 1e-6f)
        return;
    for (i = 0; i < 3; i++)
        s[i] *= w / l;
    {
        float q[6][5] = {
            { a[0] - s[0], a[1] - s[1], a[2] - s[2], -1, 0 }, { a[0] + s[0], a[1] + s[1], a[2] + s[2], 1, 0 },
            { b[0] + s[0], b[1] + s[1], b[2] + s[2], 1, 0 }, { a[0] - s[0], a[1] - s[1], a[2] - s[2], -1, 0 },
            { b[0] + s[0], b[1] + s[1], b[2] + s[2], 1, 0 }, { b[0] - s[0], b[1] - s[1], b[2] - s[2], -1, 0 },
        };
        spot_draw(vp, (const float (*)[5])q, r, g, bl, alpha, 0, 0);
    }
}

/* A round spot at p facing the eye, radius rad; thick > 0: a ring at ring. */
static void spot(const float *vp, const float eye[3], const float p[3], float rad,
                 float r, float g, float b, float alpha, float ring, float thick)
{
    static const float c[6][2] = { { -1, -1 }, { 1, -1 }, { 1, 1 }, { -1, -1 }, { 1, 1 }, { -1, 1 } };
    float f[3] = { eye[0] - p[0], eye[1] - p[1], eye[2] - p[2] };
    float l = sqrtf(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]), x[3], y[3], xl, q[6][5];
    int i, k;
    if (l < 1e-6f)
        return;
    for (i = 0; i < 3; i++)
        f[i] /= l;
    x[0] = f[2]; x[1] = 0; x[2] = -f[0];        /* up x f */
    xl = sqrtf(x[0] * x[0] + x[2] * x[2]);
    if (xl < 1e-6f) { x[0] = 1; x[2] = 0; xl = 1; }
    for (i = 0; i < 3; i++)
        x[i] *= rad / xl;
    y[0] = f[1] * x[2] - f[2] * x[1];
    y[1] = f[2] * x[0] - f[0] * x[2];
    y[2] = f[0] * x[1] - f[1] * x[0];
    for (k = 0; k < 6; k++) {
        for (i = 0; i < 3; i++)
            q[k][i] = p[i] + c[k][0] * x[i] + c[k][1] * y[i];
        q[k][3] = c[k][0];
        q[k][4] = c[k][1];
    }
    spot_draw(vp, (const float (*)[5])q, r, g, b, alpha, ring, thick);
}

/* The sight's glass, in gun space through model. */
static void glass(const float *vp, const float *model, int sight, float r, float g, float b, float a)
{
    static const float c[6][2] = { { -1, -1 }, { 1, -1 }, { 1, 1 }, { -1, -1 }, { 1, 1 }, { -1, 1 } };
    const float *gz = GLASS[sight];
    float q[6][5];
    int k;
    for (k = 0; k < 6; k++) {
        float lp[3] = { c[k][0] * gz[1], SIGHT_Y[sight] + c[k][1] * gz[2], gz[0] };
        apply(model, lp, q[k]);
        q[k][3] = q[k][4] = 0;
    }
    spot_draw(vp, (const float (*)[5])q, r, g, b, a, -1, 0);
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

    /* The optic's reticle: where the shot lands, seen only through the
     * glass. The glass marks depth 1 where all else was cleared to 0; the
     * reticle is drawn where the depth is 1. */
    if (f->gun_valid && f->show_gun && sight != QO_SIGHT_IRON) {
        glClearDepthf(0);
        glDepthMask(GL_TRUE);
        glClear(GL_DEPTH_BUFFER_BIT);
        glEnable(GL_DEPTH_TEST);
        glDepthRangef(1, 1);
        glDepthFunc(GL_ALWAYS);
        glass(viewproj, slide, sight, 0.55f, 0.75f, 0.85f, 0.10f);    /* a faint tint */
        glDepthMask(GL_FALSE);
        glDepthFunc(GL_EQUAL);
        if (sight == QO_SIGHT_RED_DOT)                  /* a 3 MOA dot */
            spot(viewproj, eye, f->end, 0.0018f * dist, 1.0f, 0.08f, 0.05f, 1.0f, 0, 0);
        else {                                          /* 65 MOA ring, 1 MOA dot */
            spot(viewproj, eye, f->end, 0.0105f * dist, 1.0f, 0.10f, 0.06f, 0.95f, 0.90f, 0.08f);
            spot(viewproj, eye, f->end, 0.0010f * dist, 1.0f, 0.10f, 0.06f, 1.0f, 0, 0);
        }
        glDepthRangef(0, 1);
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

#endif /* __ANDROID__ */
