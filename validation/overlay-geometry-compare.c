#include <math.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "runtime/src/xr/quest_overlay.h"
#include "runtime/src/xr/quest_gun_geometry.inc"
#include "runtime/src/xr/quest_overlay_math.inc"
#include "runtime/src/xr/quest_overlay_effects.inc"
static float saved[6][5], center[2], radii[3]; static int drawn;
static int prog_ret=1,prog_spot=2,vbo_spot,u_ret_mvp=3,u_ret_color=4,u_ret_tint=5,u_ret_c=6,u_ret_r=7;
#define GL_ARRAY_BUFFER 0
#define GL_DYNAMIC_DRAW 0
#define GL_FALSE 0
#define GL_TRIANGLES 0
float qo_sight_height(int s) { return SIGHT_Y[s]+.001f; }
static void spot_draw(const float *vp,const float q[6][5],float r,float g,float b,float a,float ring,float thick) { memcpy(saved,q,sizeof saved);drawn=1; }
static void glUseProgram(int x) {}
static void glBindBuffer(int x,int y) {}
static void glBufferData(int x,size_t n,const void *q,int y) { memcpy(saved,q,n); }
static void glUniformMatrix4fv(int x,int n,int t,const float *v) {}
static void glUniform4f(int x,float a,float b,float c,float d) {}
static void glUniform2f(int x,float a,float b) { center[0]=a;center[1]=b; }
static void glUniform3f(int x,float a,float b,float c) { radii[0]=a;radii[1]=b;radii[2]=c; }
static void glDrawArrays(int x,int a,int b) {drawn=1;}
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

/* p (LOCAL) into the gun's space through the rigid model matrix m. */
static void reference_unapply(const float *m, const float p[3], float o[3])
{
    float d[3] = { p[0] - m[12], p[1] - m[13], p[2] - m[14] };
    int i;
    for (i = 0; i < 3; i++)
        o[i] = m[4 * i] * d[0] + m[4 * i + 1] * d[1] + m[4 * i + 2] * d[2];
}

/* The sight's glass, in gun space through model, with its reticle on it:
 * where this eye's line to the target crosses the glass, sized by angle --
 * so it sits over the target from any head position (collimated), and
 * only inside the window, as part of the gun. */
static void glass_reticle(const float *vp, const float *model, int sight, const float eye[3],
                          const float target[3])
{
    static const float c[6][2] = { { -1, -1 }, { 1, -1 }, { 1, 1 }, { -1, -1 }, { 1, 1 }, { -1, 1 } };
    const float *gz = GLASS[sight];
    float q[6][5], e[3], t[3], cx = 1e3f, cy = 1e3f, r[3] = { 0, 0, 0 };
    int k;
    for (k = 0; k < 6; k++) {
        float lp[3] = { c[k][0] * gz[1], qo_sight_height(sight) + c[k][1] * gz[2], gz[0] };
        apply(model, lp, q[k]);
        q[k][3] = c[k][0] * gz[1];
        q[k][4] = c[k][1] * gz[2];
    }
    reference_unapply(model, eye, e);
    reference_unapply(model, target, t);
    if (fabsf(t[2] - e[2]) > 1e-5f) {
        float sc = (gz[0] - e[2]) / (t[2] - e[2]);
        if (sc > 0) {
            float px = e[0] + sc * (t[0] - e[0]), py = e[1] + sc * (t[1] - e[1]);
            float L = sqrtf((px - e[0]) * (px - e[0]) + (py - e[1]) * (py - e[1])
                            + (gz[0] - e[2]) * (gz[0] - e[2]));
            cx = px;
            cy = py - SIGHT_Y[sight];
            if (sight == QO_SIGHT_RED_DOT) {
                r[0] = 0.0025f * L;                 /* a dot about 0.3 degrees across */
            } else {                                /* a ring of 1.2 degrees, a small dot */
                r[0] = 0.0016f * L;
                r[1] = 0.0105f * L;
                r[2] = 0.0012f * L;
            }
        }
    }
    glUseProgram(prog_ret);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_spot);
    glBufferData(GL_ARRAY_BUFFER, 6 * 5 * sizeof(float), q, GL_DYNAMIC_DRAW);
    glUniformMatrix4fv(u_ret_mvp, 1, GL_FALSE, vp);
    glUniform4f(u_ret_color, 1.0f, 0.09f, 0.05f, 1.0f);
    glUniform4f(u_ret_tint, 0.55f, 0.75f, 0.85f, 0.10f);
    glUniform2f(u_ret_c, cx, cy);
    glUniform3f(u_ret_r, r[0], r[1], r[2]);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glUseProgram(prog_spot);
}


static float rnd(void) {return (rand()%20001-10000)/1000.f;}
int main(void) {
 for(int n=0;n<1000;n++) {
  float e[3]={rnd(),rnd(),rnd()},a[3]={rnd(),rnd(),rnd()},b[3]={rnd(),rnd(),rnd()},q[6][5],vp[16]={0},m[16],c[2],r[3];
  drawn=0; beam(vp,e,a,b,.0015f,1,1,1,1);
  int ok=qo_beam_vertices(q,e,a,b,.0015f);
  if(ok!=drawn || (ok && memcmp(q,saved,sizeof q))) return 1;
  drawn=0; spot(vp,e,a,.035f,1,1,1,1,0,0);ok=qo_spot_vertices(q,e,a,.035f);
  if(ok!=drawn || (ok && memcmp(q,saved,sizeof q))) return 2;
  float angle=rnd(),quat[4]={0,sinf(angle/2),0,cosf(angle/2)};pose_matrix(quat,a,m);
  for(int sight=1;sight<3;sight++) {
   glass_reticle(vp,m,sight,e,b);
   qo_glass_vertices(q,c,r,m,sight,qo_sight_height(sight),e,b);
   if(memcmp(q,saved,sizeof q) || memcmp(c,center,sizeof c) || memcmp(r,radii,sizeof r)) return 3;
  }
 }
 puts("PASS: 1000 deterministic reference comparisons, beam/spot and two sight-glass vertex/reticle calculations bit-identical");return 0;
}
