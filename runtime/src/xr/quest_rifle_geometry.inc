/* The sniper rifle in gun space (x right, y up, -z the barrel; the origin the
 * controller's aim pose, in the pistol grip). Always included after
 * quest_gun_geometry.inc, whose Box and colour macros it uses.
 *
 * The rifle drawn is the M700 (third_party/rifle: its mesh and texture,
 * quest_rifle_model.inc) with a scope lathed round the lens
 * (quest_rifle_scope.inc); the boxes below are the fallback when the model
 * cannot be loaded. The scope sits above the bore; its rear lens is where
 * the game's own scope picture is shown. The scope is big -- the lens 12 cm
 * across, easy to look through at arm's length -- and scales across (s:
 * XR_LENS_SCALE, the play test's tuning; 1 the default): the tube, the bells
 * and the lens; not along. Its axis -- the sight's line, qo_sight_height --
 * rises with it, as little as the M700 under it lets it. */
#define RIFLE_WOOD { 0.200f, 0.130f, 0.075f }
/* The M700, as scripts/rifle-model.py measured it: the muzzle (its bore's
 * centre at the barrel's tip; the laser leaves from here), the rail's top
 * and its ends, and how high it is ahead of the rail (the barrel, the rail's
 * front end) and behind the action (the wrist, the comb). Nothing of it is
 * higher than the rail's top. */
#define RIFLE_MUZZLE_Y 0.0464f
#define RIFLE_MUZZLE_Z (-0.7635f)
#define M700_RAIL_TOP 0.0677f
#define M700_RAIL_Z0 (-0.2471f)               /* its front end */
#define M700_RAIL_Z1 (-0.0881f)               /* its rear end */
#define M700_FORE_TOP 0.0595f                 /* z < M700_RAIL_Z0 */
#define M700_WRIST_TOP 0.0367f                /* z > -0.030 */
/* The scope's radii at scale 1 (quest_rifle_scope.inc): the tube, the
 * saddle (the widest part over the rail: the rings are under it), the
 * objective bell (ahead of the rail) and the eyepiece's housing round the
 * lens (behind the action). The rings' feet at least SCOPE_RING_FOOT under
 * the tube; everything else SCOPE_CLEAR over the M700. */
#define SCOPE_TUBE_R 0.040f
#define SCOPE_SADDLE_R 0.046f
#define SCOPE_OBJ_R 0.055f
#define RIFLE_REAR_R 0.0675f                  /* the eyepiece's housing, round the lens */
#define SCOPE_RING_FOOT 0.008f
#define SCOPE_CLEAR 0.003f
#define RIFLE_LENS_R 0.06f                    /* the lens's radius at scale 1 */
/* The box rifle (the fallback) */
#define RIFLE_BOX_MUZZLE_Z (-0.52f)
#define RIFLE_RECEIVER_TOP 0.042f             /* RIFLE_BODY[1]'s top */
#define RIFLE_SCOPE_N 6

static const Box RIFLE_BODY[] = {
    { { 0, -0.040f, 0.0f }, { 0.018f, 0.055f, 0.025f }, POLY },                    /* the pistol grip, round the origin */
    { { 0, 0.012f, -0.100f }, { 0.021f, 0.030f, 0.170f }, STEEL },                 /* the receiver: z -0.27 .. 0.07 */
    { { 0.027f, 0.030f, 0.010f }, { 0.006f, 0.004f, 0.012f }, DARK },              /* the bolt handle */
    { { 0, -0.012f, -0.140f }, { 0.012f, 0.018f, 0.045f }, DARK },                 /* the magazine */
    { { 0, 0, -0.360f }, { 0.009f, 0.009f, 0.160f }, STEEL },                      /* the barrel: z -0.52 .. -0.20 */
    { { 0, 0, RIFLE_BOX_MUZZLE_Z + 0.015f }, { 0.014f, 0.014f, 0.015f }, DARK },   /* the muzzle brake */
    { { 0, -0.017f, 0.300f }, { 0.020f, 0.050f, 0.230f }, RIFLE_WOOD },            /* the stock: z 0.07 .. 0.53 */
    { { 0, -0.060f, 0.520f }, { 0.021f, 0.035f, 0.010f }, DARK },                  /* the butt pad */
    { { 0, -0.060f, -0.280f }, { 0.012f, 0.014f, 0.140f }, DARK },                 /* the bipod stowed under the barrel */
};
#define RIFLE_RECEIVER (RIFLE_BODY[1])

static inline float rifle_maxf(float a, float b) { return a > b ? a : b; }

/* The scope's axis's height at scale s -- the sight's line
 * (qo_sight_height), the lens's centre: as low as the M700 lets the scope
 * sit. Whichever is highest of: the rings' feet on the rail, the saddle over
 * it, the objective bell over the barrel ahead of it, the eyepiece over the
 * wrist and comb behind the action. */
static inline float rifle_scope_y(float s)
{
    const float rings = M700_RAIL_TOP + SCOPE_RING_FOOT + SCOPE_TUBE_R * s;
    const float saddle = M700_RAIL_TOP + SCOPE_CLEAR + SCOPE_SADDLE_R * s;
    const float objective = M700_FORE_TOP + SCOPE_CLEAR + SCOPE_OBJ_R * s;
    const float eyepiece = M700_WRIST_TOP + SCOPE_CLEAR + RIFLE_REAR_R * s;
    return rifle_maxf(rifle_maxf(rings, saddle), rifle_maxf(objective, eyepiece));
}

/* The box scope at scale s, the tube first (rifle_lens_on_scope_line reads
 * it), its length the same at any scale; its mounts stand on the box
 * receiver, as tall as the axis needs. The axis is the M700's, lower over
 * the wrist than the box receiver allows: past s 1.56 the rear bell's bottom
 * stops on the receiver (the lens still inside it). */
static inline void rifle_scope(float s, Box out[RIFLE_SCOPE_N])
{
    const float y = rifle_scope_y(s), tube = 0.040f * s, foot = y - tube;
    const float my = (RIFLE_RECEIVER_TOP + foot) / 2, mh = (foot - RIFLE_RECEIVER_TOP) / 2;
    const float bell_top = y + RIFLE_REAR_R * s, bell_low = rifle_maxf(y - RIFLE_REAR_R * s, RIFLE_RECEIVER_TOP + 0.001f);
    const Box b[RIFLE_SCOPE_N] = {
        { { 0, y, -0.0725f }, { tube, tube, 0.1025f }, STEEL },                        /* the tube: z -0.175 .. 0.030 */
        { { 0, y, -0.200f }, { 0.055f * s, 0.055f * s, 0.025f }, OPTIC },              /* the front bell */
        { { 0, (bell_top + bell_low) / 2, 0.050f }, { RIFLE_REAR_R * s, (bell_top - bell_low) / 2, 0.020f }, OPTIC },   /* the rear bell: back face at 0.070 */
        { { 0, y + tube + 0.006f * s, -0.060f }, { 0.015f * s, 0.006f * s, 0.020f }, DARK },   /* the elevation turret */
        { { 0, my, -0.150f }, { 0.007f, mh, 0.008f }, DARK },                          /* the front mount, on the receiver */
        { { 0, my, -0.010f }, { 0.007f, mh, 0.008f }, DARK },                          /* the rear mount */
    };
    memcpy(out, b, sizeof b);
}

/* The scope picture's lens at scale s: a disc on the eyepiece's back face,
 * facing the eye (+z). */
static inline void rifle_lens(float s, float c[3], float *r)
{
    c[0] = 0;
    c[1] = rifle_scope_y(s);
    c[2] = 0.071f;
    *r = RIFLE_LENS_R * s;
}
