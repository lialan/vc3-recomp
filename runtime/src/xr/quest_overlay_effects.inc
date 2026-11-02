/* Shared reference geometry; requires gun geometry and overlay math. */
static int qo_beam_vertices(float q[6][5], const float eye[3], const float a[3], const float b[3], float w)
{
    float d[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] };
    float e[3] = { eye[0] - a[0], eye[1] - a[1], eye[2] - a[2] };
    float s[3] = { d[1] * e[2] - d[2] * e[1], d[2] * e[0] - d[0] * e[2], d[0] * e[1] - d[1] * e[0] };
    float l = sqrtf(s[0] * s[0] + s[1] * s[1] + s[2] * s[2]);
    int i;
    if (l < 1e-6f)
        return 0;
    for (i = 0; i < 3; i++)
        s[i] *= w / l;
    {
        float vertices[6][5] = {
            { a[0] - s[0], a[1] - s[1], a[2] - s[2], -1, 0 }, { a[0] + s[0], a[1] + s[1], a[2] + s[2], 1, 0 },
            { b[0] + s[0], b[1] + s[1], b[2] + s[2], 1, 0 }, { a[0] - s[0], a[1] - s[1], a[2] - s[2], -1, 0 },
            { b[0] + s[0], b[1] + s[1], b[2] + s[2], 1, 0 }, { b[0] - s[0], b[1] - s[1], b[2] - s[2], -1, 0 },
        };
        memcpy(q, vertices, sizeof vertices);
    }
    return 1;
}

static int qo_spot_vertices(float q[6][5], const float eye[3], const float p[3], float rad)
{
    static const float c[6][2] = { { -1, -1 }, { 1, -1 }, { 1, 1 }, { -1, -1 }, { 1, 1 }, { -1, 1 } };
    float f[3] = { eye[0] - p[0], eye[1] - p[1], eye[2] - p[2] };
    float l = sqrtf(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]), x[3], y[3], xl;
    int i, k;
    if (l < 1e-6f)
        return 0;
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
    return 1;
}

static void unapply(const float *m, const float p[3], float o[3])
{
    float d[3] = { p[0] - m[12], p[1] - m[13], p[2] - m[14] };
    int i;
    for (i = 0; i < 3; i++)
        o[i] = m[4 * i] * d[0] + m[4 * i + 1] * d[1] + m[4 * i + 2] * d[2];
}

/* The pane, its coordinates in metres from the sight's line (the reticle
 * and the edge are worked out in them); box: its x0 y0 x1 y1 there. */
static void qo_glass_vertices(float q[6][5], float center[2], float radii[3], float box[4], const float *model, int sight, float sight_height, const float eye[3], const float target[3])
{
    static const float c[6][2] = { { -1, -1 }, { 1, -1 }, { 1, 1 }, { -1, -1 }, { 1, 1 }, { -1, 1 } };
    const float *gz = GLASS[sight];
    float e[3], t[3], cx = 1e3f, cy = 1e3f;
    radii[0] = radii[1] = radii[2] = 0;
    int k;
    for (k = 0; k < 6; k++) {
        float y = c[k][1] < 0 ? gz[2] : gz[3];
        float lp[3] = { c[k][0] * gz[1], y, gz[0] };
        apply(model, lp, q[k]);
        q[k][3] = c[k][0] * gz[1];
        q[k][4] = y - sight_height;
    }
    unapply(model, eye, e);
    unapply(model, target, t);
    if (fabsf(t[2] - e[2]) > 1e-5f) {
        float sc = (gz[0] - e[2]) / (t[2] - e[2]);
        if (sc > 0) {
            float px = e[0] + sc * (t[0] - e[0]), py = e[1] + sc * (t[1] - e[1]);
            float L = sqrtf((px - e[0]) * (px - e[0]) + (py - e[1]) * (py - e[1])
                            + (gz[0] - e[2]) * (gz[0] - e[2]));
            cx = px;
            cy = py - sight_height;
            if (sight == QO_SIGHT_RED_DOT) {
                radii[0] = 0.0025f * L;                 /* a dot about 0.3 degrees across */
            } else {                                /* a ring of 1.2 degrees, a small dot */
                radii[0] = 0.0016f * L;
                radii[1] = 0.0105f * L;
                radii[2] = 0.0012f * L;
            }
        }
    }
    center[0] = cx; center[1] = cy;
    box[0] = -gz[1]; box[1] = gz[2] - sight_height;
    box[2] = gz[1]; box[3] = gz[3] - sight_height;
}
