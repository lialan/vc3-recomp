/* The M700's mesh (third_party/rifle/m700.mesh, written by
 * scripts/rifle-model.py, in the binary by vk_rifle_assets.S) unpacked; and
 * the overlay's per-vertex light and projection for it and the scope
 * (quest_rifle_scope.inc), one pass a vertex an eye. Gun space, as
 * quest_rifle_geometry.inc. Needs <math.h>, <stdint.h>, <stdlib.h>,
 * <string.h>. */

extern const unsigned char rifle_m700_mesh[], rifle_m700_mesh_end[], rifle_m700_jpg[], rifle_m700_jpg_end[];

typedef struct {
    int nv, ni;
    float *p, *n, *uv;      /* nv positions (metres), unit normals, texture coordinates (top left 0, 0) */
    uint16_t *idx;          /* ni: triangles */
} RifleModel;

#define RIFLE_MESH_HEAD 36  /* "RFL1", vertices, indices, the positions' min[3] and step[3] */
#define RIFLE_MESH_VERTEX 14

static uint32_t rifle_le32(const unsigned char *b)
{
    return (uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24;
}

static float rifle_lef(const unsigned char *b)
{
    const uint32_t u = rifle_le32(b);
    float f;
    memcpy(&f, &u, sizeof f);
    return f;
}

static void rifle_model_free(RifleModel *m)
{
    free(m->p);
    free(m->n);
    free(m->uv);
    free(m->idx);
    memset(m, 0, sizeof *m);
}

/* 1 and the model; or 0 and nothing (m zeroed) unless b is exactly one sound
 * mesh: its magic, whole triangles, indices fitting uint16 and in range,
 * finite positions, normals that are not zero. */
static int rifle_model_unpack(const unsigned char *b, size_t size, RifleModel *m)
{
    float lo[3], step[3];
    memset(m, 0, sizeof *m);
    if (!b || size < RIFLE_MESH_HEAD || memcmp(b, "RFL1", 4))
        return 0;
    const uint32_t nv = rifle_le32(b + 4), ni = rifle_le32(b + 8);
    if (!nv || nv > 65535 || !ni || ni % 3 || ni > 3u * 1000000u
        || size != RIFLE_MESH_HEAD + (size_t)nv * RIFLE_MESH_VERTEX + (size_t)ni * 2)
        return 0;
    for (int k = 0; k < 3; k++) {
        lo[k] = rifle_lef(b + 12 + 4 * k);
        step[k] = rifle_lef(b + 24 + 4 * k);
        if (!isfinite(lo[k]) || !isfinite(step[k]) || !(step[k] > 0) || !(step[k] < 1))
            return 0;
    }
    m->p = malloc((size_t)nv * 3 * sizeof(float));
    m->n = malloc((size_t)nv * 3 * sizeof(float));
    m->uv = malloc((size_t)nv * 2 * sizeof(float));
    m->idx = malloc((size_t)ni * sizeof(uint16_t));
    if (!m->p || !m->n || !m->uv || !m->idx) {
        rifle_model_free(m);
        return 0;
    }
    for (uint32_t i = 0; i < nv; i++) {
        const unsigned char *v = b + RIFLE_MESH_HEAD + (size_t)i * RIFLE_MESH_VERTEX;
        float len = 0;
        for (int k = 0; k < 3; k++) {
            m->p[i * 3 + k] = lo[k] + (float)(v[2 * k] | v[2 * k + 1] << 8) * step[k];
            m->n[i * 3 + k] = (float)(signed char)v[10 + k];
            len += m->n[i * 3 + k] * m->n[i * 3 + k];
        }
        if (!(len > 0)) {
            rifle_model_free(m);
            return 0;
        }
        len = 1 / sqrtf(len);
        for (int k = 0; k < 3; k++)
            m->n[i * 3 + k] *= len;
        m->uv[i * 2] = (float)(v[6] | v[7] << 8) / 65535.0f;
        m->uv[i * 2 + 1] = (float)(v[8] | v[9] << 8) / 65535.0f;
    }
    const unsigned char *x = b + RIFLE_MESH_HEAD + (size_t)nv * RIFLE_MESH_VERTEX;
    for (uint32_t i = 0; i < ni; i++) {
        m->idx[i] = (uint16_t)(x[2 * i] | x[2 * i + 1] << 8);
        if (m->idx[i] >= nv) {
            rifle_model_free(m);
            return 0;
        }
    }
    m->nv = (int)nv;
    m->ni = (int)ni;
    return 1;
}

/* n vertices -- positions p and unit normals nn in gun space, colours c
 * (NULL: white) -- into an eye's: through mvp (its view-projection times
 * model) to (x, -y, (z + w) / 2, w) at out[0..3], vk_overlay.c's clip
 * convention, and lit at out[col..col+2]; every stride floats. The light is
 * the overlay's gun's (quest_overlay.c's prog_gun, vk_overlay.c's box gun):
 * 0.38 ambient, 0.62 diffuse from (0.3, 1, 0.4), a highlight from
 * (-0.2, 1, 0.6) to the 24th power at 0.18, added. In the world -- so both
 * lights are turned into gun space by model's rotation (a rotation keeps dot
 * products; normalised, as the world normal is), and x^24 is by squaring: a
 * vertex costs a matrix and two dot products. 0 if a position comes out not
 * finite or at w = 0 (as the box gun's refusal). */
static int rifle_shade(const float *p, const float *nn, const float *c, int n, const float mvp[16],
                       const float model[16], float *out, int stride, int col)
{
    static const float light[2][3] = {
        { 0.26832816f, 0.89442719f, 0.35777088f },    /* (0.3, 1, 0.4) / sqrt(1.25) */
        { -0.16903085f, 0.84515425f, 0.50709255f },   /* (-0.2, 1, 0.6) / sqrt(1.4) */
    };
    float lm[2][3], sum = 0;
    for (int l = 0; l < 2; l++) {
        float len = 0;
        for (int k = 0; k < 3; k++) {
            lm[l][k] = model[4 * k] * light[l][0] + model[4 * k + 1] * light[l][1] + model[4 * k + 2] * light[l][2];
            len += lm[l][k] * lm[l][k];
        }
        if (!(len > 0))
            return 0;
        len = 1 / sqrtf(len);
        for (int k = 0; k < 3; k++)
            lm[l][k] *= len;
    }
    for (int i = 0; i < n; i++) {
        const float x = p[i * 3], y = p[i * 3 + 1], z = p[i * 3 + 2];
        const float *nv = &nn[i * 3];
        float *o = out + (size_t)i * stride;
        const float cx = mvp[0] * x + mvp[4] * y + mvp[8] * z + mvp[12];
        const float cy = mvp[1] * x + mvp[5] * y + mvp[9] * z + mvp[13];
        const float cz = mvp[2] * x + mvp[6] * y + mvp[10] * z + mvp[14];
        const float cw = mvp[3] * x + mvp[7] * y + mvp[11] * z + mvp[15];
        if (cw == 0)
            return 0;
        sum += cx + cy + cz + cw;           /* not finite if any is not */
        o[0] = cx;
        o[1] = -cy;
        o[2] = (cz + cw) * 0.5f;
        o[3] = cw;
        const float d = nv[0] * lm[0][0] + nv[1] * lm[0][1] + nv[2] * lm[0][2];
        float h = nv[0] * lm[1][0] + nv[1] * lm[1][1] + nv[2] * lm[1][2];
        h = h > 0 ? h : 0;
        const float h2 = h * h, h4 = h2 * h2, h8 = h4 * h4, h16 = h8 * h8;
        const float diffuse = 0.38f + 0.62f * (d > 0 ? d : 0), spec = h16 * h8 * 0.18f;
        if (c) {
            o[col] = c[i * 3] * diffuse + spec;
            o[col + 1] = c[i * 3 + 1] * diffuse + spec;
            o[col + 2] = c[i * 3 + 2] * diffuse + spec;
        } else {
            o[col] = o[col + 1] = o[col + 2] = diffuse + spec;
        }
    }
    return isfinite(sum);
}
