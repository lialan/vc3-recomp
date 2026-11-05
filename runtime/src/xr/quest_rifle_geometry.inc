/* The sniper rifle, as boxes in gun space (x right, y up, -z the barrel; the
 * origin the controller's aim pose, in the pistol grip). Always included
 * after quest_gun_geometry.inc, whose Box and colour macros it uses. The
 * scope sits above the bore; its rear lens is where the game's own scope
 * picture is shown. */
#define RIFLE_WOOD { 0.200f, 0.130f, 0.075f }
#define RIFLE_SCOPE_Y 0.065f                  /* the scope's axis above the bore */
#define RIFLE_MUZZLE_Z (-0.52f)
#define RIFLE_LENS_R 0.024f

/* the scope tube first (rifle_lens_on_scope_line reads it) */
static const Box RIFLE_SCOPE[] = {
    { { 0, RIFLE_SCOPE_Y, -0.0725f }, { 0.016f, 0.016f, 0.1025f }, STEEL },        /* the tube: z -0.175 .. 0.030 */
    { { 0, RIFLE_SCOPE_Y, -0.200f }, { 0.022f, 0.022f, 0.025f }, OPTIC },          /* the front bell */
    { { 0, RIFLE_SCOPE_Y, 0.050f }, { 0.027f, 0.027f, 0.020f }, OPTIC },           /* the rear bell: back face at 0.070 */
    { { 0, RIFLE_SCOPE_Y + 0.0285f, -0.060f }, { 0.006f, 0.0025f, 0.008f }, DARK },/* the elevation turret */
    { { 0, 0.0475f, -0.150f }, { 0.007f, 0.016f, 0.008f }, DARK },                 /* the front mount */
    { { 0, 0.0475f, -0.010f }, { 0.007f, 0.016f, 0.008f }, DARK },                 /* the rear mount */
};
static const Box RIFLE_BODY[] = {
    { { 0, -0.040f, 0.0f }, { 0.018f, 0.055f, 0.025f }, POLY },                    /* the pistol grip, round the origin */
    { { 0, 0.012f, -0.100f }, { 0.021f, 0.030f, 0.170f }, STEEL },                 /* the receiver: z -0.27 .. 0.07 */
    { { 0.027f, 0.030f, 0.010f }, { 0.006f, 0.004f, 0.012f }, DARK },              /* the bolt handle */
    { { 0, -0.012f, -0.140f }, { 0.012f, 0.018f, 0.045f }, DARK },                 /* the magazine */
    { { 0, 0, -0.360f }, { 0.009f, 0.009f, 0.160f }, STEEL },                      /* the barrel: z -0.52 .. -0.20 */
    { { 0, 0, RIFLE_MUZZLE_Z + 0.015f }, { 0.014f, 0.014f, 0.015f }, DARK },       /* the muzzle brake */
    { { 0, -0.002f, 0.300f }, { 0.020f, 0.050f, 0.230f }, RIFLE_WOOD },            /* the stock: z 0.07 .. 0.53 */
    { { 0, -0.060f, 0.520f }, { 0.021f, 0.035f, 0.010f }, DARK },                  /* the butt pad */
    { { 0, -0.060f, -0.280f }, { 0.012f, 0.014f, 0.140f }, DARK },                 /* the bipod stowed under the barrel */
};
#define RIFLE_RECEIVER (RIFLE_BODY[1])
/* The scope picture's lens: a disc on the rear bell's back face. */
static const float RIFLE_LENS_C[3] = { 0, RIFLE_SCOPE_Y, 0.0705f };
