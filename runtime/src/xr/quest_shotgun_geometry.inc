/* The pump-action shotgun in gun space (x right, y up, -z the barrel; the
 * origin the controller's aim pose). Always included after
 * quest_gun_geometry.inc, whose Box, DARK, MOUNT_H and QO_SIGHT_* it uses.
 *
 * The shotgun drawn is the Remington 870 (third_party/shotgun: its body, its
 * pump -- the fore-end, which slides -- and their texture;
 * quest_rifle_model.inc unpacks them), held as the handgun is: its trigger
 * where the handgun's is, so the hand holds the wrist of its stock where it
 * held the handgun's grip and the stock runs back along the forearm. As
 * scripts/rifle-model.py --shotgun measured it: the muzzle (the bore's
 * centre at the barrel's tip; the laser and the flash leave from here), the
 * bead's top, the rib's highest (by the receiver), the receiver's flat top
 * (z -0.135 .. 0.04) and how far the pump slides back before it meets the
 * receiver (0.048 m). */
#define R870_MUZZLE_Y 0.0240f
#define R870_MUZZLE_Z (-0.8073f)
#define R870_BEAD_TOP 0.0440f
#define R870_RIB_TOP 0.0447f
#define R870_RECEIVER_TOP 0.0432f
#define R870_STROKE 0.045f

/* Each sight's line above the origin (qo_sight_height) and its glass, as
 * the handgun's (SIGHT_Y, GLASS): the iron sight the line along the rib's
 * top, the bead's top a hair under it at the muzzle; the optics bare panes
 * on mount strips on the receiver's rear, as high over it as the handgun's
 * over its slide. */
static const float R870_SIGHT_Y[QO_SIGHT_COUNT] = {
    R870_RIB_TOP, R870_RECEIVER_TOP + 0.0095f, R870_RECEIVER_TOP + 0.0160f };
static const float R870_GLASS[QO_SIGHT_COUNT][4] = {
    { 0, 0, 0, 0 },
    { 0.020f, 0.0100f, R870_RECEIVER_TOP + MOUNT_H, R870_RECEIVER_TOP + 0.0180f },
    { 0.005f, 0.0125f, R870_RECEIVER_TOP + MOUNT_H, R870_RECEIVER_TOP + 0.0300f },
};
static const Box R870_MOUNT[QO_SIGHT_COUNT] = {
    { { 0, 0, 0 }, { 0, 0, 0 }, { 0, 0, 0 } },
    { { 0, R870_RECEIVER_TOP + MOUNT_H / 2, 0.020f }, { 0.0100f, MOUNT_H / 2, 0.0025f }, DARK },
    { { 0, R870_RECEIVER_TOP + MOUNT_H / 2, 0.005f }, { 0.0125f, MOUNT_H / 2, 0.0030f }, DARK },
};
#define R870_BOX_V 36

/* A box's twelve triangles (add_boxes's corners, faces and order) as
 * separate positions, normals and colours, for rifle_shade. */
static inline void r870_box(const Box *b, float p[R870_BOX_V][3], float n[R870_BOX_V][3], float c[R870_BOX_V][3])
{
    static const int face[6][4] = { { 1, 3, 7, 5 }, { 0, 4, 6, 2 }, { 2, 6, 7, 3 },
                                    { 0, 1, 5, 4 }, { 4, 5, 7, 6 }, { 0, 2, 3, 1 } };
    static const float nrm[6][3] = { { 1, 0, 0 }, { -1, 0, 0 }, { 0, 1, 0 },
                                     { 0, -1, 0 }, { 0, 0, 1 }, { 0, 0, -1 } };
    static const int tri[6] = { 0, 1, 2, 0, 2, 3 };
    for (int f = 0; f < 6; f++)
        for (int t = 0; t < 6; t++) {
            const int k = face[f][tri[t]], i = f * 6 + t;
            for (int a = 0; a < 3; a++) {
                p[i][a] = b->c[a] + (((k >> a) & 1) ? b->h[a] : -b->h[a]);
                n[i][a] = nrm[f][a];
                c[i][a] = b->rgb[a];
            }
        }
}
