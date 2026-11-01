/* Shared controller pose/recoil transforms. */
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

