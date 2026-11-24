/*
 * PLANEO - the flight (see vu_flight.h)
 */
#include "vu_flight.h"

#include <math.h>
#include <string.h>

#define G           9.81f
#define V_STALL     27.0f
#define V_MAX       95.0f
#define THRUST      15.0f       /* m/s2 at full throttle */
#define DRAG        (THRUST / (V_MAX * V_MAX))
#define ROLL_RATE   2.4f        /* rad/s at full stick */
#define PITCH_RATE  1.15f
#define RUDDER_RATE 0.55f       /* rad/s at full pedal: about 30 degrees a second */
#define CEILING     560.0f
#define CAM_BACK    15.0f
#define CAM_UP      4.6f

static inline float dot3(const float *a, const float *b)
{
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

static void norm3(float *a)
{
    float l = sqrtf(dot3(a, a));
    if (l < 1e-9f) return;
    a[0] /= l;
    a[1] /= l;
    a[2] /= l;
}

static void cross3(const float *a, const float *b, float *o)
{
    o[0] = a[1] * b[2] - a[2] * b[1];
    o[1] = a[2] * b[0] - a[0] * b[2];
    o[2] = a[0] * b[1] - a[1] * b[0];
}

/* a towards b by angle t: a' = a cos t + b sin t, b' = b cos t - a sin t */
static void turn2(float *a, float *b, float t)
{
    float c = cosf(t), s = sinf(t);
    for (int k = 0; k < 3; k++) {
        float A = a[k], B = b[k];
        a[k] = A * c + B * s;
        b[k] = B * c - A * s;
    }
}

/* every axis round the world's vertical by t (counter-clockwise from above) */
static void yaw_world(float *v, float t)
{
    float c = cosf(t), s = sinf(t);
    float x = v[0], y = v[1];
    v[0] = x * c - y * s;
    v[1] = x * s + y * c;
}

static void reortho(vf_plane_t *pl)
{
    norm3(pl->f);
    float d = dot3(pl->r, pl->f);
    for (int k = 0; k < 3; k++) pl->r[k] -= pl->f[k] * d;
    norm3(pl->r);
    cross3(pl->r, pl->f, pl->u);
}

static void derive(vf_plane_t *pl)
{
    pl->pitch = asinf(pl->f[2] < -1 ? -1 : pl->f[2] > 1 ? 1 : pl->f[2]);
    pl->bank = atan2f(-pl->r[2], pl->u[2]);
    pl->heading = atan2f(pl->f[1], pl->f[0]);
}

void vf_reset(vf_plane_t *pl, const vw_world_t *w)
{
    int view = pl->view;
    memset(pl, 0, sizeof *pl);
    pl->view = view;
    pl->p[0] = w->start[0];
    pl->p[1] = w->start[1];
    pl->p[2] = w->start[2];
    float c = cosf(w->start_yaw), s = sinf(w->start_yaw);
    pl->f[0] = c;
    pl->f[1] = s;
    pl->r[0] = s;
    pl->r[1] = -c;
    pl->u[2] = 1;
    pl->v = 52.0f;
    pl->thr = pl->in_thr = 0.6f;
    memcpy(pl->cam_d, pl->f, sizeof pl->cam_d);
    derive(pl);
    pl->ground = vw_height(w, pl->p[0], pl->p[1]);
    pl->agl = pl->p[2] - pl->ground;
}

void vf_step(vf_plane_t *pl, const vw_world_t *w, float dt)
{
    if (pl->crashed) {
        pl->crash_t += dt;
        return;
    }
    /* the engine spools towards the throttle */
    pl->thr += (pl->in_thr - pl->thr) * (dt * 2.5f > 1 ? 1 : dt * 2.5f);
    float auth = pl->v / 45.0f;            /* the controls bite with speed */
    if (auth > 1) auth = 1;
    if (auth < 0.35f) auth = 0.35f;
    float roll = pl->in_x * ROLL_RATE * auth;
    float pitch = pl->in_y * PITCH_RATE * auth;
    /* hands off: the wings come level (unless it is on its back: then a roll) */
    if (fabsf(pl->in_x) < 0.08f && fabsf(pl->bank) < 1.4f) roll -= pl->bank * 1.6f;
    /* the stall: the nose drops */
    pl->stall = pl->v < V_STALL ? (V_STALL - pl->v) / 10.0f : 0;
    if (pl->stall > 1) pl->stall = 1;
    if (pl->stall > 0) pitch -= pl->stall * 1.4f * (pl->u[2] > 0 ? 1.0f : -1.0f);
    /* the ceiling: the air too thin to climb */
    if (pl->p[2] > CEILING && pl->f[2] > 0) pitch -= (pl->p[2] - CEILING) * 0.04f;
    turn2(pl->r, pl->u, -roll * dt);       /* the right wing towards down */
    /* the rudder: the nose towards the right wing, round the plane's up */
    turn2(pl->f, pl->r, pl->in_rud * RUDDER_RATE * auth * dt);
    turn2(pl->f, pl->u, pitch * dt);       /* the nose towards up */
    /* the bank turns it: the lift leans with the wings */
    /* tan(bank) from the axes, kept finite near the vertical; an arcade's
     * quick turn: about 30 degrees a second at 60 of bank (a real plane's
     * physics gave 7, and circles of 750 m) */
    float v = pl->v < 25.0f ? 25.0f : pl->v;
    float uz = pl->u[2] > 0.35f ? pl->u[2] : 0.35f;
    float tb = -pl->r[2] / uz;
    float turn = G * tb / v * 2.6f;
    if (turn > 0.9f) turn = 0.9f;
    if (turn < -0.9f) turn = -0.9f;
    float ff = sqrtf(pl->f[0] * pl->f[0] + pl->f[1] * pl->f[1]);
    turn *= ff;
    yaw_world(pl->f, -turn * dt);
    yaw_world(pl->r, -turn * dt);
    yaw_world(pl->u, -turn * dt);
    reortho(pl);
    derive(pl);
    /* speed: thrust, drag, and gravity along the path */
    float acc = THRUST * (0.15f + 0.85f * pl->thr) - DRAG * pl->v * pl->v - G * pl->f[2];
    acc -= fabsf(pl->in_rud) * 2.5f;        /* skidding sideways costs speed */
    pl->v += acc * dt;
    if (pl->v < 8.0f) pl->v = 8.0f;
    for (int k = 0; k < 3; k++) pl->p[k] += pl->f[k] * pl->v * dt;
    /* sinking in the stall */
    if (pl->stall > 0) pl->p[2] -= pl->stall * 6.0f * dt;
    pl->p[0] = vw_wrap(pl->p[0]);
    pl->p[1] = vw_wrap(pl->p[1]);
    pl->ground = vw_height(w, pl->p[0], pl->p[1]);
    pl->agl = pl->p[2] - pl->ground;
    if (pl->agl < 0.9f) {
        pl->crashed = true;
        pl->crash_t = 0;
        pl->p[2] = pl->ground + 0.9f;
    }
}

void vf_pose(const vf_plane_t *pl, vp_pose_t *pose)
{
    memcpy(pose->p, pl->p, sizeof pose->p);
    memcpy(pose->f, pl->f, sizeof pose->f);
    memcpy(pose->r, pl->r, sizeof pose->r);
    memcpy(pose->u, pl->u, sizeof pose->u);
    pose->prop = 0.3f + 0.7f * pl->thr;
    pose->ground = pl->ground;
}

void vf_camera(vf_plane_t *pl, const vw_world_t *w, float dt, float time, vr_cam_t *cam)
{
    cam->time = time;
    cam->tanfov = 0.78f;
    float k = dt * 3.2f;
    if (k > 1 || !pl->cam_ok) k = 1;
    float rk = dt * 4.0f;
    if (rk > 1 || !pl->cam_ok) rk = 1;
    pl->cam_ok = true;
    if (pl->view == VF_COCKPIT) {
        float eye[3];
        for (int i = 0; i < 3; i++) eye[i] = pl->p[i] + pl->f[i] * 0.4f + pl->u[i] * 1.05f;
        cam->x = eye[0];
        cam->y = eye[1];
        cam->z = eye[2];
        cam->yaw = pl->heading;
        cam->pitch = pl->pitch;
        cam->roll = pl->bank;
        cam->tanfov = 0.9f;
        memcpy(pl->cam_d, pl->f, sizeof pl->cam_d);
        pl->cam_roll = pl->bank;
    } else {
        /* behind and a little above, lagging the turns */
        for (int i = 0; i < 3; i++) pl->cam_d[i] += (pl->f[i] - pl->cam_d[i]) * k;
        norm3(pl->cam_d);
        float eye[3], tgt[3];
        for (int i = 0; i < 3; i++) {
            eye[i] = pl->p[i] - pl->cam_d[i] * CAM_BACK;
            tgt[i] = pl->p[i] + pl->cam_d[i] * 30.0f;
        }
        eye[2] += CAM_UP;
        tgt[2] += 1.2f;
        float g = vw_height(w, vw_wrap(eye[0]), vw_wrap(eye[1])) + 2.5f;
        if (eye[2] < g) eye[2] = g;
        float d[3] = { tgt[0] - eye[0], tgt[1] - eye[1], tgt[2] - eye[2] };
        norm3(d);
        cam->x = vw_wrap(eye[0]);
        cam->y = vw_wrap(eye[1]);
        cam->z = eye[2];
        cam->yaw = atan2f(d[1], d[0]);
        cam->pitch = asinf(d[2]);
        /* the camera banks with the plane, a little less */
        float want = pl->bank * 0.8f;
        float diff = want - pl->cam_roll;
        while (diff > 3.14159f) diff -= 6.28318f;
        while (diff < -3.14159f) diff += 6.28318f;
        pl->cam_roll += diff * rk;
        cam->roll = pl->cam_roll;
    }
    if (cam->pitch > 0.95f) cam->pitch = 0.95f;
    if (cam->pitch < -0.95f) cam->pitch = -0.95f;
    pl->cam_eye[0] = cam->x;
    pl->cam_eye[1] = cam->y;
    pl->cam_eye[2] = cam->z;
}

void vf_autopilot(vf_plane_t *pl, const vw_world_t *w, float t)
{
    /* a heading that wanders, and the ground ahead at a height */
    float want_h = pl->heading + 0.5f * sinf(t * 0.13f) + 0.35f * sinf(t * 0.31f + 1.0f);
    float dh = want_h - pl->heading;
    while (dh > 3.14159f) dh -= 6.28318f;
    while (dh < -3.14159f) dh += 6.28318f;
    float want_bank = dh * 1.4f;
    if (want_bank > 0.8f) want_bank = 0.8f;
    if (want_bank < -0.8f) want_bank = -0.8f;
    /* negative heading change is a right turn here: bank right for it */
    want_bank = -want_bank;
    pl->in_x = (want_bank - pl->bank) * 1.8f;
    float ahead = 0;
    for (int i = 1; i <= 4; i++) {
        float d = (float)i * 60.0f;
        float h = vw_height(w, vw_wrap(pl->p[0] + pl->f[0] * d), vw_wrap(pl->p[1] + pl->f[1] * d));
        if (h > ahead) ahead = h;
    }
    float want_z = ahead + 55.0f + 25.0f * sinf(t * 0.21f);
    float want_pitch = (want_z - pl->p[2]) * 0.012f;
    if (want_pitch > 0.45f) want_pitch = 0.45f;
    if (want_pitch < -0.35f) want_pitch = -0.35f;
    float cb = cosf(pl->bank);
    pl->in_y = (want_pitch - pl->pitch) * 2.5f / (cb > 0.3f ? cb : 0.3f) + fabsf(sinf(pl->bank)) * 0.35f;
    if (pl->in_x > 1) pl->in_x = 1;
    if (pl->in_x < -1) pl->in_x = -1;
    if (pl->in_y > 1) pl->in_y = 1;
    if (pl->in_y < -1) pl->in_y = -1;
    pl->in_thr = pl->pitch > 0.15f ? 0.9f : 0.62f;
    pl->in_rud = 0;
}

void vf_autopilot_to(vf_plane_t *pl, const vw_world_t *w, const float target[3])
{
    float dx = target[0] - pl->p[0], dy = target[1] - pl->p[1];
    if (dx > VW_SIZE / 2) dx -= VW_SIZE;
    if (dx < -VW_SIZE / 2) dx += VW_SIZE;
    if (dy > VW_SIZE / 2) dy -= VW_SIZE;
    if (dy < -VW_SIZE / 2) dy += VW_SIZE;
    float dh = atan2f(dy, dx) - pl->heading;
    while (dh > 3.14159f) dh -= 6.28318f;
    while (dh < -3.14159f) dh += 6.28318f;
    float want_bank = -dh * 2.2f;
    if (want_bank > 1.0f) want_bank = 1.0f;
    if (want_bank < -1.0f) want_bank = -1.0f;
    float dist = sqrtf(dx * dx + dy * dy);
    /* inside the turn's circle: straight on until there is room to turn */
    float radius = pl->v * pl->v / (9.81f * 2.6f * 1.4f) + 20.0f;
    if (fabsf(dh) > 0.6f && dist < radius * 1.6f * sinf(fabsf(dh) > 1.5f ? 1.5f : fabsf(dh))) want_bank = 0;
    pl->in_x = (want_bank - pl->bank) * 2.0f;
    float want_z = target[2];
    for (int i = 1; i <= 3; i++) {
        float d = (float)i * 40.0f;
        float h = vw_height(w, vw_wrap(pl->p[0] + pl->f[0] * d), vw_wrap(pl->p[1] + pl->f[1] * d)) + 18.0f;
        if (h > want_z) want_z = h;
    }
    float want_pitch = atan2f(want_z - pl->p[2], dist > 60.0f ? dist : 60.0f);
    if (want_pitch > 0.5f) want_pitch = 0.5f;
    if (want_pitch < -0.45f) want_pitch = -0.45f;
    float cb = cosf(pl->bank);
    pl->in_y = (want_pitch - pl->pitch) * 3.0f / (cb > 0.3f ? cb : 0.3f) + fabsf(sinf(pl->bank)) * 0.4f;
    if (pl->in_x > 1) pl->in_x = 1;
    if (pl->in_x < -1) pl->in_x = -1;
    if (pl->in_y > 1) pl->in_y = 1;
    if (pl->in_y < -1) pl->in_y = -1;
    pl->in_thr = 0.85f;
    /* near the ring the pedals line it up */
    pl->in_rud = dist < 160.0f ? -dh * 3.0f : 0;
    if (pl->in_rud > 1) pl->in_rud = 1;
    if (pl->in_rud < -1) pl->in_rud = -1;
}
