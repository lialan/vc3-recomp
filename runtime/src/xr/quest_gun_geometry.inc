/* Shared reference gun geometry; renderer-local static mesh/cache. */
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

