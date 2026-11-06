/* The rifle's scope, lathed round the lens (quest_rifle_geometry.inc:
 * rifle_lens, rifle_scope_y), for the M700 (quest_rifle_model.inc): the
 * eyepiece's housing round the lens and its bell down to the tube, the power
 * ring, the tube, the turret saddle with the elevation (top) and windage
 * (right) turrets, the objective bell and its dark glass, and two rings whose
 * feet stand on the M700's rail. Smooth surfaces of revolution, SCOPE_SEG
 * round (SCOPE_SEG_SMALL the turrets and rings), hard edges where the profile
 * turns sharply. Radii scale with s; lengths along the scope do not (as the
 * box scope's). Matte black anodised: the overlay's light (rifle_shade)
 * gives it its sheen. Nothing of it is behind the lens (z > 0.0705) or below
 * the rail's top. Always included after quest_rifle_geometry.inc; needs
 * <math.h>, <stdint.h>, <string.h>. */

#define SCOPE_SEG 48
#define SCOPE_SEG_SMALL 32
#define SCOPE_MAX_V 6144
#define SCOPE_MAX_I 24576
#define SCOPE_TURRET_Z (-0.160f)              /* on the saddle, z -0.184 .. -0.136 */
#define SCOPE_RING_HALF 0.008f                /* a ring's half width, along z */
#define SCOPE_RING_R 0.0445f                  /* its outside, at scale 1 (inside the saddle's) */
#define SCOPE_FOOT_HALF_X 0.0100f             /* a foot's half width on the rail (its top 2.08 cm across) */
#define SCOPE_FOOT_HALF_Z 0.006f
static const float SCOPE_RING_Z[2] = { -0.110f, -0.222f };   /* the rear and front rings: over the rail */

typedef struct {
    int nv, ni;
    float p[SCOPE_MAX_V][3], n[SCOPE_MAX_V][3], c[SCOPE_MAX_V][3];   /* position, unit normal, colour */
    uint16_t idx[SCOPE_MAX_I];                                         /* triangles */
} RifleScopeMesh;

typedef struct { float a, r; } ScopePt;       /* a profile's point: along the axis, out from it */

static const float SCOPE_BLACK[3] = { 0.105f, 0.105f, 0.115f };
static const float SCOPE_RING_BLACK[3] = { 0.085f, 0.085f, 0.090f };
static const float SCOPE_CAP[3] = { 0.130f, 0.130f, 0.140f };
static const float SCOPE_GLASS[3] = { 0.050f, 0.080f, 0.140f };

static int scope_vertex(RifleScopeMesh *m, const float p[3], const float n[3], const float c[3])
{
    if (m->nv >= SCOPE_MAX_V)
        return -1;
    memcpy(m->p[m->nv], p, sizeof m->p[0]);
    memcpy(m->n[m->nv], n, sizeof m->n[0]);
    memcpy(m->c[m->nv], c, sizeof m->c[0]);
    return m->nv++;
}

static int scope_tri(RifleScopeMesh *m, int a, int b, int c)
{
    if (m->ni + 3 > SCOPE_MAX_I)
        return 0;
    m->idx[m->ni++] = (uint16_t)a;
    m->idx[m->ni++] = (uint16_t)b;
    m->idx[m->ni++] = (uint16_t)c;
    return 1;
}

/* A profile point's ring round the axis (o, ax; u, v across it, a right
 * angle apart): seg vertices, or one on the axis when r is 0; the profile's
 * normal there (na along the axis, nr out). Its first vertex, or -1: full. */
static int scope_ring(RifleScopeMesh *m, const float o[3], const float ax[3], const float u[3], const float v[3],
                      ScopePt pt, float na, float nr, int seg, const float rgb[3])
{
    const int first = m->nv, count = pt.r > 0 ? seg : 1;
    for (int k = 0; k < count; k++) {
        const float t = 6.28318531f * (float)k / (float)seg, cs = cosf(t), sn = sinf(t);
        float p[3], n[3];
        for (int i = 0; i < 3; i++) {
            const float d = u[i] * cs + v[i] * sn;
            p[i] = o[i] + ax[i] * pt.a + d * pt.r;
            n[i] = ax[i] * na + d * nr;
        }
        if (scope_vertex(m, p, n, rgb) < 0)
            return -1;
    }
    return first;
}

/* Triangles between two rings (seg vertices each, or one on the axis). */
static int scope_join(RifleScopeMesh *m, int a, int ca, int b, int cb, int seg)
{
    for (int k = 0; k < seg; k++) {
        const int k1 = (k + 1) % seg;
        if (ca > 1 && cb > 1) {
            if (!scope_tri(m, a + k, a + k1, b + k1) || !scope_tri(m, a + k, b + k1, b + k))
                return 0;
        } else if (ca > 1) {
            if (!scope_tri(m, a + k, a + k1, b))
                return 0;
        } else if (cb > 1) {
            if (!scope_tri(m, a, b + k1, b + k))
                return 0;
        }
    }
    return 1;
}

/* A surface of revolution: the profile pt[0..n) (at most 64 points) round
 * the axis. Its normal at a point is its segments' mean where they turn by
 * less than 40 degrees (smooth), else each segment's own (a hard edge). Going
 * along the profile, the outside is on the left of it: (da, dr) turned a
 * quarter: (-dr, da). */
static int scope_lathe(RifleScopeMesh *m, const float o[3], const float ax[3], const float u[3], const float v[3],
                       const ScopePt *pt, int n, int seg, const float rgb[3])
{
    float sn[64][2];
    int ok[64], in[64], out[64];
    if (n < 2 || n > 64)
        return 0;
    for (int k = 0; k + 1 < n; k++) {
        const float da = pt[k + 1].a - pt[k].a, dr = pt[k + 1].r - pt[k].r, len = sqrtf(da * da + dr * dr);
        ok[k] = len > 1e-7f;
        sn[k][0] = ok[k] ? -dr / len : 0;
        sn[k][1] = ok[k] ? da / len : 0;
    }
    for (int i = 0; i < n; i++) {
        const int prev = i > 0 && ok[i - 1], next = i + 1 < n && ok[i];
        in[i] = out[i] = -1;
        if (prev && next && sn[i - 1][0] * sn[i][0] + sn[i - 1][1] * sn[i][1] > 0.766f) {
            float na = sn[i - 1][0] + sn[i][0], nr = sn[i - 1][1] + sn[i][1], len = sqrtf(na * na + nr * nr);
            in[i] = out[i] = scope_ring(m, o, ax, u, v, pt[i], na / len, nr / len, seg, rgb);
            if (in[i] < 0)
                return 0;
            continue;
        }
        if (prev && (in[i] = scope_ring(m, o, ax, u, v, pt[i], sn[i - 1][0], sn[i - 1][1], seg, rgb)) < 0)
            return 0;
        if (next && (out[i] = scope_ring(m, o, ax, u, v, pt[i], sn[i][0], sn[i][1], seg, rgb)) < 0)
            return 0;
    }
    for (int k = 0; k + 1 < n; k++)
        if (ok[k] && !scope_join(m, out[k], pt[k].r > 0 ? seg : 1, in[k + 1], pt[k + 1].r > 0 ? seg : 1, seg))
            return 0;
    return 1;
}

/* The profile from (a0, r0) to (a1, r1) easing in and out (a cosine), its
 * steps-1 points between (r scaled by s). */
static int scope_taper(ScopePt *pt, int n, float a0, float r0, float a1, float r1, int steps, float s)
{
    for (int k = 1; k < steps; k++) {
        const float t = (float)k / (float)steps;
        pt[n].a = a0 + t * (a1 - a0);
        pt[n].r = (r0 + (r1 - r0) * 0.5f * (1 - cosf(3.14159265f * t))) * s;
        n++;
    }
    return n;
}

/* The body, front to back: the objective's glass rim and lip, its bell, the
 * front ring's tube, the saddle, the rear ring's tube, the power ring, the
 * eyepiece's bell and housing (a groove: the focus ring's edge), its rounded
 * rim, and its back face round the lens -- to just inside the lens's edge, so
 * no gap shows round the lens's 32-sided disc. */
static int scope_body(RifleScopeMesh *m, float s, float y)
{
    ScopePt pt[64];
    int n = 0;
#define P(along, out) (pt[n].a = (along), pt[n].r = (out) * s, n++)
    P(-0.2930f, 0.0490f);
    P(-0.2955f, 0.0490f);
    P(-0.2955f, 0.0528f);
    P(-0.2945f, 0.0550f);
    P(-0.2720f, 0.0550f);
    n = scope_taper(pt, n, -0.2720f, SCOPE_OBJ_R, -0.2400f, SCOPE_TUBE_R, 6, s);
    P(-0.2400f, SCOPE_TUBE_R);
    P(-0.1880f, SCOPE_TUBE_R);
    P(-0.1840f, SCOPE_SADDLE_R);
    P(-0.1360f, SCOPE_SADDLE_R);
    P(-0.1320f, SCOPE_TUBE_R);
    P(-0.0170f, SCOPE_TUBE_R);
    P(-0.0160f, 0.0428f);
    P(-0.0080f, 0.0428f);
    P(-0.0070f, 0.0410f);
    n = scope_taper(pt, n, -0.0070f, 0.0410f, 0.0360f, RIFLE_REAR_R, 7, s);
    P(0.0360f, RIFLE_REAR_R);
    P(0.0490f, RIFLE_REAR_R);
    P(0.0495f, 0.0661f);
    P(0.0525f, 0.0661f);
    P(0.0530f, RIFLE_REAR_R);
    P(0.0670f, RIFLE_REAR_R);
    P(0.0692f, 0.0668f);
    P(0.0703f, 0.0652f);
    P(0.0705f, 0.0635f);
    P(0.0705f, 0.0580f);
#undef P
    const float o[3] = { 0, y, 0 }, ax[3] = { 0, 0, 1 }, u[3] = { 1, 0, 0 }, v[3] = { 0, 1, 0 };
    const ScopePt glass[2] = { { -0.2930f, 0 }, { -0.2930f, 0.0490f * s } };     /* facing ahead */
    return scope_lathe(m, o, ax, u, v, pt, n, SCOPE_SEG, SCOPE_BLACK)
        && scope_lathe(m, o, ax, u, v, glass, 2, SCOPE_SEG, SCOPE_GLASS);
}

/* A turret on the saddle along ax from the scope's axis: its foot inside the
 * saddle, a base, a gap, the cap with a bevelled top. */
static int scope_turret(RifleScopeMesh *m, float s, float y, const float ax[3], const float u[3], const float v[3])
{
    const float rt = 0.017f * s < 0.021f ? 0.017f * s : 0.021f;     /* within the saddle's length */
    const ScopePt pt[8] = {
        { 0.040f * s, rt }, { 0.052f * s, rt }, { 0.052f * s, rt * 0.90f }, { 0.0545f * s, rt * 0.90f },
        { 0.0545f * s, rt * 0.97f }, { 0.067f * s, rt * 0.97f }, { 0.070f * s, rt * 0.82f }, { 0.070f * s, 0 },
    };
    const float o[3] = { 0, y, SCOPE_TURRET_Z };
    return scope_lathe(m, o, ax, u, v, pt, 8, SCOPE_SEG_SMALL, SCOPE_CAP);
}

/* A ring round the tube at zc, and its foot: from the rail's top (a hair
 * above it) up into the tube, flaring out under it. */
static int scope_ring_and_foot(RifleScopeMesh *m, float s, float y, float zc)
{
    const ScopePt band[4] = {
        { zc - SCOPE_RING_HALF, (SCOPE_TUBE_R - 0.0004f) * s }, { zc - SCOPE_RING_HALF, SCOPE_RING_R * s },
        { zc + SCOPE_RING_HALF, SCOPE_RING_R * s }, { zc + SCOPE_RING_HALF, (SCOPE_TUBE_R - 0.0004f) * s },
    };
    const float o[3] = { 0, y, 0 }, ax[3] = { 0, 0, 1 }, u[3] = { 1, 0, 0 }, v[3] = { 0, 1, 0 };
    if (!scope_lathe(m, o, ax, u, v, band, 4, SCOPE_SEG_SMALL, SCOPE_RING_BLACK))
        return 0;
    /* the foot: bottom y0 half width hb, top y1 (inside the tube) half width ht */
    const float y0 = M700_RAIL_TOP + 0.0003f, y1 = y - 0.5f * SCOPE_TUBE_R * s;
    const float hb = SCOPE_FOOT_HALF_X, ht = 0.8f * SCOPE_TUBE_R * s, z0 = zc - SCOPE_FOOT_HALF_Z, z1 = zc + SCOPE_FOOT_HALF_Z;
    const float sx = y1 - y0, sy = -(ht - hb), sl = sqrtf(sx * sx + sy * sy);
    const float c[8][3] = {   /* bottom: -x -z, +x -z, +x +z, -x +z; then the top's */
        { -hb, y0, z0 }, { hb, y0, z0 }, { hb, y0, z1 }, { -hb, y0, z1 },
        { -ht, y1, z0 }, { ht, y1, z0 }, { ht, y1, z1 }, { -ht, y1, z1 },
    };
    static const int face[5][4] = { { 0, 1, 2, 3 }, { 0, 1, 5, 4 }, { 3, 2, 6, 7 }, { 1, 2, 6, 5 }, { 0, 3, 7, 4 } };
    const float nrm[5][3] = { { 0, -1, 0 }, { 0, 0, -1 }, { 0, 0, 1 }, { sx / sl, sy / sl, 0 }, { -sx / sl, sy / sl, 0 } };
    for (int f = 0; f < 5; f++) {
        int b = -1;
        for (int k = 0; k < 4; k++) {
            const int i = scope_vertex(m, c[face[f][k]], nrm[f], SCOPE_RING_BLACK);
            if (i < 0)
                return 0;
            if (!k)
                b = i;
        }
        if (!scope_tri(m, b, b + 1, b + 2) || !scope_tri(m, b, b + 2, b + 3))
            return 0;
    }
    return 1;
}

/* The scope at scale s into m: 1, or 0 (and an empty or partial mesh) if s
 * is out of reason or it would not fit. */
static int rifle_scope_mesh(float s, RifleScopeMesh *m)
{
    static const float X[3] = { 1, 0, 0 }, Y[3] = { 0, 1, 0 }, Z[3] = { 0, 0, 1 };
    const float y = rifle_scope_y(s);
    m->nv = m->ni = 0;
    if (!(s > 0.05f && s < 10))
        return 0;
    return scope_body(m, s, y)
        && scope_turret(m, s, y, Y, Z, X)                     /* elevation: up */
        && scope_turret(m, s, y, X, Y, Z)                     /* windage: right */
        && scope_ring_and_foot(m, s, y, SCOPE_RING_Z[0])
        && scope_ring_and_foot(m, s, y, SCOPE_RING_Z[1]);
}
