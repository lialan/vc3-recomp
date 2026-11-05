/* The sniper rifle, as boxes in gun space (x right, y up, -z the barrel; the
 * origin the controller's aim pose, in the pistol grip). Always included
 * after quest_gun_geometry.inc, whose Box and colour macros it uses. The
 * scope sits above the bore; its rear lens is where the game's own scope
 * picture is shown. The scope is big -- the lens 12 cm across, easy to look
 * through at arm's length -- and scales as a whole (s: XR_LENS_SCALE, the
 * play test's tuning; 1 the default): the tube, the bells and the lens
 * across, standing on the receiver, its axis rising with it. */
#define RIFLE_WOOD { 0.200f, 0.130f, 0.075f }
#define RIFLE_MUZZLE_Z (-0.52f)
#define RIFLE_RECEIVER_TOP 0.042f             /* RIFLE_BODY[1]'s top */
#define RIFLE_LENS_R 0.06f                    /* the lens's radius at scale 1 */
#define RIFLE_REAR_R 0.0675f                  /* the rear bell's half size at scale 1, round the lens */
#define RIFLE_SCOPE_N 6

static const Box RIFLE_BODY[] = {
    { { 0, -0.040f, 0.0f }, { 0.018f, 0.055f, 0.025f }, POLY },                    /* the pistol grip, round the origin */
    { { 0, 0.012f, -0.100f }, { 0.021f, 0.030f, 0.170f }, STEEL },                 /* the receiver: z -0.27 .. 0.07 */
    { { 0.027f, 0.030f, 0.010f }, { 0.006f, 0.004f, 0.012f }, DARK },              /* the bolt handle */
    { { 0, -0.012f, -0.140f }, { 0.012f, 0.018f, 0.045f }, DARK },                 /* the magazine */
    { { 0, 0, -0.360f }, { 0.009f, 0.009f, 0.160f }, STEEL },                      /* the barrel: z -0.52 .. -0.20 */
    { { 0, 0, RIFLE_MUZZLE_Z + 0.015f }, { 0.014f, 0.014f, 0.015f }, DARK },       /* the muzzle brake */
    { { 0, -0.017f, 0.300f }, { 0.020f, 0.050f, 0.230f }, RIFLE_WOOD },            /* the stock: z 0.07 .. 0.53 */
    { { 0, -0.060f, 0.520f }, { 0.021f, 0.035f, 0.010f }, DARK },                  /* the butt pad */
    { { 0, -0.060f, -0.280f }, { 0.012f, 0.014f, 0.140f }, DARK },                 /* the bipod stowed under the barrel */
};
#define RIFLE_RECEIVER (RIFLE_BODY[1])

/* The scope's axis above the bore at scale s: the rear bell (its widest
 * part) just clear of the receiver -- the sight's line (qo_sight_height). */
static inline float rifle_scope_y(float s)
{
    return RIFLE_RECEIVER_TOP + 0.001f + RIFLE_REAR_R * s;
}

/* The scope at scale s, the tube first (rifle_lens_on_scope_line reads it),
 * its length the same at any scale. */
static inline void rifle_scope(float s, Box out[RIFLE_SCOPE_N])
{
    const float y = rifle_scope_y(s), tube = 0.040f * s, foot = y - tube;
    const float my = (RIFLE_RECEIVER_TOP + foot) / 2, mh = (foot - RIFLE_RECEIVER_TOP) / 2;
    const Box b[RIFLE_SCOPE_N] = {
        { { 0, y, -0.0725f }, { tube, tube, 0.1025f }, STEEL },                        /* the tube: z -0.175 .. 0.030 */
        { { 0, y, -0.200f }, { 0.055f * s, 0.055f * s, 0.025f }, OPTIC },              /* the front bell */
        { { 0, y, 0.050f }, { RIFLE_REAR_R * s, RIFLE_REAR_R * s, 0.020f }, OPTIC },   /* the rear bell: back face at 0.070 */
        { { 0, y + tube + 0.006f * s, -0.060f }, { 0.015f * s, 0.006f * s, 0.020f }, DARK },   /* the elevation turret */
        { { 0, my, -0.150f }, { 0.007f, mh, 0.008f }, DARK },                          /* the front mount, on the receiver */
        { { 0, my, -0.010f }, { 0.007f, mh, 0.008f }, DARK },                          /* the rear mount */
    };
    memcpy(out, b, sizeof b);
}

/* The scope picture's lens at scale s: a disc on the rear bell's back face,
 * facing the eye (+z). */
static inline void rifle_lens(float s, float c[3], float *r)
{
    c[0] = 0;
    c[1] = rifle_scope_y(s);
    c[2] = 0.071f;
    *r = RIFLE_LENS_R * s;
}
