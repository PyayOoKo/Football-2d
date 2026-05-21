#include <windows.h>
#include <gl/gl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define PLAYERS_PER_TEAM 11
#define TOTAL_PLAYERS    22
#define PITCH_W          2.0f
#define PITCH_H          1.35f
#define GOAL_W           0.22f
#define MATCH_MINUTES    90.0f
#define REAL_SECONDS     120.0f  /* full match in ~2 minutes */

#define PITCH_X_LIMIT    (PITCH_W * 0.48f)
#define PITCH_Y_LIMIT    (PITCH_H * 0.48f)
#define TACKLE_DIST_SQ   (0.045f * 0.045f)
#define PICKUP_DIST_SQ   (0.05f * 0.05f)
#define LEN_EPS_SQ       (0.001f * 0.001f)
#define SPEED_IDLE_SQ    (0.03f * 0.03f)
#define CIRCLE_SEG       10
#define TITLE_INTERVAL   0.25f

typedef struct {
    float x, y;
    float vx, vy;
    float disp_x, disp_y;
    float facing;
    float run_phase;
    float kick_timer;
    int team;
    int number;
    float home_x, home_y;      /* base formation slot */
    float target_x, target_y;  /* dynamic positional target */
    int role;                  /* 0=GK, 1=DEF, 2=MID, 3=ATT */
    float run_timer;           /* timing for making runs */
    int is_making_run;         /* flag for active run */
} Player;

typedef struct {
    float x, y;
    float vx, vy;
    float disp_x, disp_y;
    float height;
    float height_v;
    float spin;
    float dribble_phase;
    float kick_flash;
    int owner; /* player index, or -1 */
} Ball;

typedef struct {
    Player players[TOTAL_PLAYERS];
    Ball ball;
    int score_home;
    int score_away;
    float match_clock;   /* 0 .. MATCH_MINUTES */
    int possession_team;
    float phase_timer;
    int phase; /* 0=play, 1=goal pause, 2=kickoff */
    int last_scorer;
    char home_name[32];
    char away_name[32];
} Match;

static Match g_match;
static HWND g_hwnd = NULL;
static float g_speed = 1.0f;
static BOOL g_paused = FALSE;
static int g_chase_player = -1;
static GLuint g_pitch_list = 0;
static float g_title_timer = 0.0f;
static int g_title_clock = -1;
static int g_title_score_h = -1;
static int g_title_score_a = -1;

LRESULT CALLBACK WindowProc(HWND, UINT, WPARAM, LPARAM);
void EnableOpenGL(HWND hwnd, HDC*, HGLRC*);
void DisableOpenGL(HWND, HDC, HGLRC);
static void kickoff(void);

static float rand01(void)
{
    return (float)rand() / (float)RAND_MAX;
}

static float frand(float a, float b)
{
    return a + rand01() * (b - a);
}

static float clampf(float v, float lo, float hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static float dist_sq(float x1, float y1, float x2, float y2)
{
    float dx = x2 - x1;
    float dy = y2 - y1;
    return dx * dx + dy * dy;
}

static float vec_len(float dx, float dy)
{
    return (float)sqrt(dx * dx + dy * dy);
}

static float lerp_angle(float current, float target, float t)
{
    float diff = target - current;
    while (diff > (float)M_PI)  diff -= (float)(2.0 * M_PI);
    while (diff < -(float)M_PI) diff += (float)(2.0 * M_PI);
    return current + diff * t;
}

static void player_trigger_kick(int idx)
{
    if (idx >= 0 && idx < TOTAL_PLAYERS)
        g_match.players[idx].kick_timer = 0.22f;
}

static void ball_release_from(int owner_idx, float vx, float vy, float loft)
{
    Ball* b = &g_match.ball;

    b->owner = -1;
    b->vx = vx;
    b->vy = vy;
    b->height_v = loft;
    b->kick_flash = 0.18f;
    player_trigger_kick(owner_idx);
}

static void formation_slot(int team, int idx, float* hx, float* hy, int* role)
{
    /* 4-4-2 style slots in normalized pitch space */
    static const float home[11][2] = {
        { -0.92f,  0.00f },  /* GK */
        { -0.72f, -0.55f }, { -0.72f, -0.18f }, { -0.72f,  0.18f }, { -0.72f,  0.55f },
        { -0.45f, -0.50f }, { -0.45f, -0.17f }, { -0.45f,  0.17f }, { -0.45f,  0.50f },
        { -0.18f, -0.22f }, { -0.18f,  0.22f }
    };
    static const int roles[11] = { 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3 };
    int mirror = (team == 1);
    float sx = mirror ? -home[idx][0] : home[idx][0];
    *hx = sx;
    *hy = home[idx][1];
    *role = roles[idx];
}

static void init_match(void)
{
    int i;

    srand((unsigned)time(NULL));
    memset(&g_match, 0, sizeof(g_match));
    snprintf(g_match.home_name, sizeof(g_match.home_name), "United");
    snprintf(g_match.away_name, sizeof(g_match.away_name), "City");

    for (i = 0; i < TOTAL_PLAYERS; i++)
    {
        int team = i / PLAYERS_PER_TEAM;
        int slot = i % PLAYERS_PER_TEAM;
        formation_slot(team, slot, &g_match.players[i].home_x, &g_match.players[i].home_y, &g_match.players[i].role);
        g_match.players[i].x = g_match.players[i].home_x;
        g_match.players[i].y = g_match.players[i].home_y;
        g_match.players[i].target_x = g_match.players[i].home_x;
        g_match.players[i].target_y = g_match.players[i].home_y;
        g_match.players[i].team = team;
        g_match.players[i].number = slot + 1;
        g_match.players[i].vx = 0.0f;
        g_match.players[i].vy = 0.0f;
        g_match.players[i].disp_x = g_match.players[i].x;
        g_match.players[i].disp_y = g_match.players[i].y;
        g_match.players[i].facing = (team == 0) ? 0.0f : (float)M_PI;
        g_match.players[i].run_phase = frand(0.0f, (float)(2.0 * M_PI));
        g_match.players[i].kick_timer = 0.0f;
        g_match.players[i].run_timer = frand(0.5f, 2.0f);
        g_match.players[i].is_making_run = 0;
    }

    g_match.ball.x = 0.0f;
    g_match.ball.y = 0.0f;
    g_match.ball.vx = 0.0f;
    g_match.ball.vy = 0.0f;
    g_match.ball.disp_x = 0.0f;
    g_match.ball.disp_y = 0.0f;
    g_match.ball.height = 0.0f;
    g_match.ball.height_v = 0.0f;
    g_match.ball.spin = 0.0f;
    g_match.ball.dribble_phase = 0.0f;
    g_match.ball.kick_flash = 0.0f;
    g_match.possession_team = 0;
    g_title_clock = -1;
    g_title_score_h = -1;
    g_title_score_a = -1;
    g_title_timer = TITLE_INTERVAL;
    kickoff();
}

static int nearest_player(float bx, float by, int team_filter)
{
    int best = -1;
    float best_d = 1e9f;
    int i;

    for (i = 0; i < TOTAL_PLAYERS; i++)
    {
        float d;
        if (team_filter >= 0 && g_match.players[i].team != team_filter)
            continue;
        d = dist_sq(bx, by, g_match.players[i].x, g_match.players[i].y);
        if (d < best_d)
        {
            best_d = d;
            best = i;
        }
    }
    return best;
}

static void kickoff(void)
{
    int i;
    g_match.ball.x = 0.0f;
    g_match.ball.y = 0.0f;
    g_match.ball.vx = 0.0f;
    g_match.ball.vy = 0.0f;
    g_match.ball.owner = -1;

    for (i = 0; i < TOTAL_PLAYERS; i++)
    {
        g_match.players[i].x = g_match.players[i].home_x;
        g_match.players[i].y = g_match.players[i].home_y;
        g_match.players[i].target_x = g_match.players[i].home_x;
        g_match.players[i].target_y = g_match.players[i].home_y;
        g_match.players[i].disp_x = g_match.players[i].x;
        g_match.players[i].disp_y = g_match.players[i].y;
        g_match.players[i].vx = 0.0f;
        g_match.players[i].vy = 0.0f;
        g_match.players[i].kick_timer = 0.0f;
        g_match.players[i].run_timer = frand(0.5f, 2.0f);
        g_match.players[i].is_making_run = 0;
    }

    g_match.ball.disp_x = g_match.ball.x;
    g_match.ball.disp_y = g_match.ball.y;
    g_match.ball.height = 0.0f;
    g_match.ball.height_v = 0.0f;
    g_match.ball.owner = (g_match.score_home + g_match.score_away) % 2 == 0 ? 10 : 21;
    g_match.possession_team = g_match.players[g_match.ball.owner].team;
    g_match.phase = 0;
}

static void score_goal(int team)
{
    if (team == 0)
        g_match.score_home++;
    else
        g_match.score_away++;

    g_match.last_scorer = team;
    g_match.phase = 1;
    g_match.phase_timer = 2.0f;
    g_match.ball.owner = -1;
    g_match.ball.vx = 0.0f;
    g_match.ball.vy = 0.0f;
    g_match.ball.height = 0.0f;
    g_match.ball.height_v = 0.0f;
    g_match.ball.kick_flash = 0.0f;
}

static void try_pass(int owner_idx)
{
    int team = g_match.players[owner_idx].team;
    int i, target = -1;
    float best = -1.0f;
    float ox = g_match.players[owner_idx].x;
    float oy = g_match.players[owner_idx].y;
    float attack_dir = (team == 0) ? 1.0f : -1.0f;

    for (i = 0; i < TOTAL_PLAYERS; i++)
    {
        float dx, dy, d, forward;
        if (i == owner_idx || g_match.players[i].team != team)
            continue;

        dx = g_match.players[i].x - ox;
        dy = g_match.players[i].y - oy;
        forward = dx * attack_dir;
        d = vec_len(dx, dy);

        if (d > 0.15f && d < 0.55f && forward > 0.02f)
        {
            float score = forward * 2.0f + frand(0.0f, 0.15f);
            if (score > best)
            {
                best = score;
                target = i;
            }
        }
    }

    if (target >= 0 && rand01() < 0.018f)
    {
        float dx = g_match.players[target].x - ox;
        float dy = g_match.players[target].y - oy;
        float len = vec_len(dx, dy);
        if (len > 0.001f)
        {
            float spd = frand(0.55f, 0.85f);
            ball_release_from(owner_idx,
                              (dx / len) * spd,
                              (dy / len) * spd,
                              frand(0.04f, 0.10f));
        }
    }
}

static void try_shoot(int owner_idx)
{
    int team = g_match.players[owner_idx].team;
    float px = g_match.players[owner_idx].x;
    float goal_x = (team == 0) ? (PITCH_W * 0.5f) : (-PITCH_W * 0.5f);
    float d_goal = fabsf(goal_x - px);

    if (d_goal < 0.42f && fabsf(g_match.players[owner_idx].y) < GOAL_W * 1.2f)
    {
        if (rand01() < 0.035f)
        {
            float dx = goal_x - g_match.ball.x;
            float dy = frand(-0.05f, 0.05f) - g_match.ball.y;
            float len = vec_len(dx, dy);
            if (len > 0.001f)
            {
                float spd = frand(0.9f, 1.25f);
                ball_release_from(owner_idx,
                                  (dx / len) * spd,
                                  (dy / len) * spd,
                                  frand(0.10f, 0.22f));
            }
        }
    }
}

static void calculate_positional_target(int i, float* tx, float* ty)
{
    Player* p = &g_match.players[i];
    float bx = g_match.ball.x;
    float by = g_match.ball.y;
    int ball_team = -1;
    float attack_dir = (p->team == 0) ? 1.0f : -1.0f;
    float def_x_mult, lat_shift, forward_shift;
    
    /* Determine which team has possession */
    if (g_match.ball.owner >= 0) {
        ball_team = g_match.players[g_match.ball.owner].team;
    }
    
    /* Base defensive shift based on ball position */
    def_x_mult = 0.6f + 0.25f * fabsf(bx);
    lat_shift = by * 0.15f;
    
    /* Adjust forward/backward based on possession and role */
    if (ball_team == p->team) {
        /* Attacking phase: push higher up the pitch */
        switch (p->role) {
            case 0: /* GK */
                forward_shift = 0.0f;
                break;
            case 1: /* DEF */
                forward_shift = 0.12f;
                break;
            case 2: /* MID */
                forward_shift = 0.22f;
                break;
            case 3: /* ATT */
                forward_shift = 0.30f;
                break;
            default:
                forward_shift = 0.15f;
        }
    } else {
        /* Defensive phase: drop deeper based on threat */
        float threat = fabsf(bx) * 1.5f;
        switch (p->role) {
            case 0: /* GK */
                forward_shift = -threat * 0.3f;
                break;
            case 1: /* DEF */
                forward_shift = -threat * 0.4f;
                break;
            case 2: /* MID */
                forward_shift = -0.08f - threat * 0.3f;
                break;
            case 3: /* ATT */
                forward_shift = -0.15f - threat * 0.2f;
                break;
            default:
                forward_shift = -0.1f;
        }
    }
    
    /* Calculate target position */
    *tx = p->home_x * def_x_mult + (attack_dir * forward_shift) + lat_shift;
    *ty = p->home_y * 0.75f + by * 0.12f;
    
    /* Apply lateral shift for wide players when ball is on their side */
    if ((i % 4) == 3 || (i % 4) == 0) { /* Wide players */
        float ball_side = (by > 0.0f) ? 1.0f : -1.0f;
        float player_side = (p->home_y > 0.0f) ? 1.0f : -1.0f;
        if (ball_side == player_side) {
            *ty += ball_side * 0.08f; /* Push wider when ball is on their side */
        }
    }
}

static void update_player(int i, float dt)
{
    Player* p = &g_match.players[i];
    float target_x, target_y;
    float dx, dy, len, max_speed = 0.38f;
    int has_ball = (g_match.ball.owner == i);
    
    /* Update run timer and making runs logic */
    p->run_timer -= dt;
    if (p->run_timer <= 0.0f && !has_ball) {
        p->is_making_run = (rand01() < 0.35f && p->role >= 2);
        p->run_timer = frand(1.5f, 4.0f);
    }

    if (g_match.phase != 0)
    {
        dx = p->home_x - p->x;
        dy = p->home_y - p->y;
        if (dx * dx + dy * dy > LEN_EPS_SQ)
        {
            len = vec_len(dx, dy);
            p->vx = (dx / len) * 0.12f;
            p->vy = (dy / len) * 0.12f;
        }
        p->x += p->vx * dt;
        p->y += p->vy * dt;
        return;
    }

    if (has_ball)
    {
        float attack = (p->team == 0) ? 1.0f : -1.0f;
        target_x = p->x + attack * 0.28f;
        target_y = p->y + ((i % 2) ? 0.06f : -0.06f);
        max_speed = 0.32f;
        try_pass(i);
        try_shoot(i);
    }
    else if (g_match.ball.owner >= 0)
    {
        int owner = g_match.ball.owner;
        int oteam = g_match.players[owner].team;
        if (p->team == oteam)
        {
            /* Support: positional target adjusted for supporting angle */
            calculate_positional_target(i, &target_x, &target_y);
            /* Add offset to support ahead/behind ball carrier */
            float support_offset = (oteam == 0) ? 0.15f : -0.15f;
            target_x += support_offset;
            
            /* If making a run, push forward more aggressively */
            if (p->is_making_run && p->role >= 2) {
                target_x += (oteam == 0) ? 0.20f : -0.20f;
            }
        }
        else
        {
            /* Defending: press ball carrier or mark */
            float press_dist = 0.18f;
            calculate_positional_target(i, &target_x, &target_y);
            
            /* Nearest defender presses, others hold shape */
            float d_to_owner = dist_sq(p->x, p->y, g_match.players[owner].x, g_match.players[owner].y);
            if (d_to_owner < 0.08f || (p->role == 2 && d_to_owner < 0.15f)) {
                /* Press the ball carrier */
                target_x = g_match.players[owner].x;
                target_y = g_match.players[owner].y;
                max_speed = 0.48f;
            } else {
                /* Hold defensive shape but shift toward ball */
                target_x = target_x * 0.7f + g_match.ball.x * 0.3f;
                target_y = target_y * 0.8f + g_match.ball.y * 0.2f;
            }
        }
    }
    else
    {
        if (g_chase_player == i)
        {
            target_x = g_match.ball.x;
            target_y = g_match.ball.y;
            max_speed = 0.46f;
        }
        else
        {
            /* No possession: use calculated positional target */
            calculate_positional_target(i, &target_x, &target_y);
        }
    }

    dx = target_x - p->x;
    dy = target_y - p->y;
    if (dx * dx + dy * dy > LEN_EPS_SQ)
    {
        len = vec_len(dx, dy);
        p->vx = (dx / len) * max_speed;
        p->vy = (dy / len) * max_speed;
    }
    else
    {
        p->vx *= 0.9f;
        p->vy *= 0.9f;
    }

    p->x += p->vx * dt;
    p->y += p->vy * dt;
    p->x = clampf(p->x, -PITCH_X_LIMIT, PITCH_X_LIMIT);
    p->y = clampf(p->y, -PITCH_Y_LIMIT, PITCH_Y_LIMIT);
    
    /* Store current target for debugging/visualization */
    p->target_x = target_x;
    p->target_y = target_y;
}

static void update_ball(float dt)
{
    Ball* b = &g_match.ball;
    int i;

    if (g_match.phase != 0)
        return;

    if (b->owner >= 0)
    {
        Player* o = &g_match.players[b->owner];
        b->x = o->x + ((o->team == 0) ? 0.03f : -0.03f);
        b->y = o->y;
        b->vx = o->vx;
        b->vy = o->vy;
        g_match.possession_team = o->team;

        /* tackle */
        for (i = 0; i < TOTAL_PLAYERS; i++)
        {
            if (i == b->owner || g_match.players[i].team == o->team)
                continue;
            if (dist_sq(b->x, b->y, g_match.players[i].x, g_match.players[i].y) < TACKLE_DIST_SQ)
            {
                if (rand01() < 0.06f * dt * 60.0f)
                {
                    b->owner = i;
                    g_match.possession_team = g_match.players[i].team;
                    b->height = 0.0f;
                    b->height_v = 0.0f;
                    player_trigger_kick(i);
                }
            }
        }
    }
    else
    {
        b->x += b->vx * dt;
        b->y += b->vy * dt;
        b->vx *= 0.992f;
        b->vy *= 0.992f;

        if (fabsf(b->vx) < 0.01f && fabsf(b->vy) < 0.01f)
        {
            b->vx = 0.0f;
            b->vy = 0.0f;
        }

        /* pickup */
        if (fabsf(b->vx) + fabsf(b->vy) < 0.35f)
        {
            int pick = nearest_player(b->x, b->y, -1);
            if (pick >= 0 &&
                dist_sq(b->x, b->y,
                        g_match.players[pick].x,
                        g_match.players[pick].y) < PICKUP_DIST_SQ)
            {
                b->owner = pick;
                b->vx = 0.0f;
                b->vy = 0.0f;
            }
        }

        /* bounds */
        if (b->x < -PITCH_W * 0.5f || b->x > PITCH_W * 0.5f)
        {
            if (fabsf(b->y) < GOAL_W)
            {
                score_goal(b->x > 0.0f ? 1 : 0);
            }
            else
            {
                b->x = clampf(b->x, -PITCH_W * 0.49f, PITCH_W * 0.49f);
                b->vx *= -0.55f;
            }
        }
        if (b->y < -PITCH_H * 0.5f || b->y > PITCH_H * 0.5f)
        {
            b->y = clampf(b->y, -PITCH_H * 0.49f, PITCH_H * 0.49f);
            b->vy *= -0.55f;
        }
    }
}

static void update_match(float dt)
{
    int i;
    char title[128];

    if (g_paused)
        return;

    dt *= g_speed;

    if (g_match.phase == 1 || g_match.phase == 2)
    {
        g_match.phase_timer -= dt;
        if (g_match.phase_timer <= 0.0f)
            kickoff();
    }
    else
    {
        g_match.match_clock += dt * (MATCH_MINUTES / REAL_SECONDS);
        if (g_match.match_clock > MATCH_MINUTES)
            g_match.match_clock = MATCH_MINUTES;

        if (g_match.ball.owner < 0)
            g_chase_player = nearest_player(g_match.ball.x, g_match.ball.y, -1);
        else
            g_chase_player = -1;

        for (i = 0; i < TOTAL_PLAYERS; i++)
            update_player(i, dt);

        update_ball(dt);
    }

    g_title_timer += dt;
    if (g_hwnd && g_title_timer >= TITLE_INTERVAL)
    {
        int clock_min = (int)g_match.match_clock;
        if (g_match.score_home != g_title_score_h ||
            g_match.score_away != g_title_score_a ||
            clock_min != g_title_clock)
        {
            g_title_score_h = g_match.score_home;
            g_title_score_a = g_match.score_away;
            g_title_clock = clock_min;
            snprintf(title, sizeof(title),
                     "2D Football - %s %d - %d %s  |  %d'  |  Space=pause  +/-=speed",
                     g_match.home_name, g_match.score_home, g_match.score_away,
                     g_match.away_name, clock_min);
            SetWindowTextA(g_hwnd, title);
        }
        g_title_timer = 0.0f;
    }
}

static void update_animations(float dt)
{
    int i;
    Ball* b = &g_match.ball;
    float ball_speed;
    float lerp_p, lerp_b, lerp_face;

    lerp_p = 1.0f - (float)exp(-14.0f * dt);
    lerp_b = 1.0f - (float)exp(-18.0f * dt);
    lerp_face = 1.0f - (float)exp(-10.0f * dt);

    for (i = 0; i < TOTAL_PLAYERS; i++)
    {
        Player* p = &g_match.players[i];
        float speed_sq = p->vx * p->vx + p->vy * p->vy;
        float speed;
        float target_face;

        p->disp_x += (p->x - p->disp_x) * lerp_p;
        p->disp_y += (p->y - p->disp_y) * lerp_p;

        if (speed_sq > SPEED_IDLE_SQ)
        {
            speed = (float)sqrt(speed_sq);
            target_face = atan2f(p->vy, p->vx);
        }
        else
        {
            speed = 0.0f;
            target_face = (p->team == 0) ? 0.0f : (float)M_PI;
        }

        p->facing = lerp_angle(p->facing, target_face, lerp_face);
        p->run_phase += (0.8f + speed * 22.0f) * dt;
        if (p->kick_timer > 0.0f)
            p->kick_timer -= dt;
    }

    b->disp_x += (b->x - b->disp_x) * lerp_b;
    b->disp_y += (b->y - b->disp_y) * lerp_b;

    ball_speed = vec_len(b->vx, b->vy);
    if (ball_speed > 0.02f)
        b->spin += ball_speed * dt * 9.0f;

    if (b->kick_flash > 0.0f)
        b->kick_flash -= dt;

    if (b->owner >= 0)
    {
        b->dribble_phase += dt * (9.0f + ball_speed * 8.0f);
        b->height_v = 0.0f;
        b->height = (float)fabs(sinf(b->dribble_phase)) * 0.012f;
    }
    else
    {
        b->height += b->height_v * dt;
        b->height_v -= 2.8f * dt;
        if (b->height < 0.0f)
        {
            if (b->height_v < -0.08f)
                b->height_v *= -0.38f;
            else
                b->height_v = 0.0f;
            b->height = 0.0f;
        }
    }
}

static void draw_circle(float cx, float cy, float r, int segments)
{
    int i;
    glBegin(GL_TRIANGLE_FAN);
    glVertex2f(cx, cy);
    for (i = 0; i <= segments; i++)
    {
        float a = (float)(2.0 * M_PI * i / segments);
        glVertex2f(cx + cosf(a) * r, cy + sinf(a) * r);
    }
    glEnd();
}

static void draw_pitch_geometry(void)
{
    float hw = PITCH_W * 0.5f;
    float hh = PITCH_H * 0.5f;

    /* grass */
    glColor3f(0.18f, 0.55f, 0.22f);
    glBegin(GL_QUADS);
    glVertex2f(-hw, -hh);
    glVertex2f( hw, -hh);
    glVertex2f( hw,  hh);
    glVertex2f(-hw,  hh);
    glEnd();

    /* stripes */
    glColor3f(0.16f, 0.50f, 0.20f);
    {
        int i;
        for (i = -4; i < 4; i++)
        {
            if (i % 2 == 0)
            {
                float x0 = -hw + (hw * 2.0f) * (i + 4) / 8.0f;
                float x1 = -hw + (hw * 2.0f) * (i + 5) / 8.0f;
                glBegin(GL_QUADS);
                glVertex2f(x0, -hh);
                glVertex2f(x1, -hh);
                glVertex2f(x1,  hh);
                glVertex2f(x0,  hh);
                glEnd();
            }
        }
    }

    glColor3f(0.92f, 0.92f, 0.92f);
    glLineWidth(2.0f);
    glBegin(GL_LINE_LOOP);
    glVertex2f(-hw, -hh);
    glVertex2f( hw, -hh);
    glVertex2f( hw,  hh);
    glVertex2f(-hw,  hh);
    glEnd();

    /* halfway */
    glBegin(GL_LINES);
    glVertex2f(0.0f, -hh);
    glVertex2f(0.0f,  hh);
    glEnd();

    /* centre circle */
    glBegin(GL_LINE_LOOP);
    {
        int i;
        for (i = 0; i < 32; i++)
        {
            float a = (float)(2.0 * M_PI * i / 32);
            glVertex2f(cosf(a) * 0.14f, sinf(a) * 0.14f);
        }
    }
    glEnd();

    /* penalty boxes (simplified) */
    glBegin(GL_LINE_LOOP);
    glVertex2f(-hw, -0.32f);
    glVertex2f(-hw + 0.28f, -0.32f);
    glVertex2f(-hw + 0.28f,  0.32f);
    glVertex2f(-hw,  0.32f);
    glEnd();
    glBegin(GL_LINE_LOOP);
    glVertex2f( hw, -0.32f);
    glVertex2f( hw - 0.28f, -0.32f);
    glVertex2f( hw - 0.28f,  0.32f);
    glVertex2f( hw,  0.32f);
    glEnd();

    /* goals */
    glColor3f(0.95f, 0.95f, 0.95f);
    glLineWidth(3.0f);
    glBegin(GL_LINES);
    glVertex2f(-hw, -GOAL_W); glVertex2f(-hw, GOAL_W);
    glVertex2f( hw, -GOAL_W); glVertex2f( hw, GOAL_W);
    glEnd();
}

static void draw_pitch(void)
{
    if (!g_pitch_list)
    {
        g_pitch_list = glGenLists(1);
        glNewList(g_pitch_list, GL_COMPILE);
        draw_pitch_geometry();
        glEndList();
    }
    glCallList(g_pitch_list);
}

static void draw_player_sprite(const Player* p, int has_ball)
{
    float speed_sq = p->vx * p->vx + p->vy * p->vy;
    float speed = (speed_sq > SPEED_IDLE_SQ) ? (float)sqrt(speed_sq) : 0.0f;
    float run = p->run_phase;
    float leg_sw = (float)sin(run) * (0.012f + speed * 0.035f);
    float leg_sw2 = (float)sin(run + (float)M_PI) * (0.012f + speed * 0.035f);
    float kick_ext = (p->kick_timer > 0.0f) ? (p->kick_timer / 0.22f) * 0.03f : 0.0f;
    float cr, cg, cb;
    float skin_r = 0.94f, skin_g = 0.78f, skin_b = 0.62f;

    /* shadow */
    glColor4f(0.0f, 0.0f, 0.0f, 0.28f);
    draw_circle(p->disp_x, p->disp_y - 0.006f, 0.024f, CIRCLE_SEG);

    glPushMatrix();
    glTranslatef(p->disp_x, p->disp_y, 0.0f);
    glRotatef(p->facing * 57.2958f, 0.0f, 0.0f, 1.0f);

    if (has_ball)
    {
        cr = 1.0f; cg = 0.95f; cb = 0.35f;
    }
    else if (p->team == 0)
    {
        cr = 0.95f; cg = 0.25f; cb = 0.20f;
    }
    else
    {
        cr = 0.20f; cg = 0.45f; cb = 0.95f;
    }

    /* shorts (darker) */
    glColor3f(cr * 0.55f, cg * 0.55f, cb * 0.55f);
    glBegin(GL_QUADS);
    glVertex2f(-0.010f, -0.004f);
    glVertex2f( 0.010f, -0.004f);
    glVertex2f( 0.009f, -0.014f);
    glVertex2f(-0.009f, -0.014f);
    glEnd();

    /* legs */
    glColor3f(0.12f, 0.12f, 0.14f);
    glLineWidth(2.5f);
    glBegin(GL_LINES);
    glVertex2f(-0.005f, -0.014f);
    glVertex2f(-0.005f + leg_sw, -0.028f - kick_ext * 0.2f);
    glVertex2f( 0.005f, -0.014f);
    glVertex2f( 0.005f + leg_sw2, -0.028f);
    glEnd();

    /* kick follow-through */
    if (kick_ext > 0.0f)
    {
        glBegin(GL_LINES);
        glVertex2f(0.012f + kick_ext, -0.010f);
        glVertex2f(0.022f + kick_ext * 1.5f, -0.006f);
        glEnd();
    }

    /* torso */
    glColor3f(cr, cg, cb);
    draw_circle(0.0f, 0.002f, 0.016f, CIRCLE_SEG);

    /* head */
    glColor3f(skin_r, skin_g, skin_b);
    draw_circle(0.0f, 0.020f, 0.009f, 8);

    /* direction nib */
    glColor4f(1.0f, 1.0f, 1.0f, 0.55f);
    glBegin(GL_LINES);
    glVertex2f(0.0f, 0.020f);
    glVertex2f(0.018f, 0.020f);
    glEnd();

    glPopMatrix();
}

static void draw_ball_sprite(void)
{
    Ball* b = &g_match.ball;
    float bx = b->disp_x;
    float by = b->disp_y + b->height;
    float spd = vec_len(b->vx, b->vy);
    float pulse = (b->kick_flash > 0.0f) ? (b->kick_flash / 0.18f) : 0.0f;
    float r = 0.014f + pulse * 0.004f;
    float shadow_a = 0.35f - b->height * 8.0f;
    int i;
    float patch_x, patch_y;

    if (shadow_a < 0.08f) shadow_a = 0.08f;

    /* ground shadow */
    glColor4f(0.0f, 0.0f, 0.0f, shadow_a);
    draw_circle(b->disp_x, b->disp_y, 0.012f + b->height * 0.4f, CIRCLE_SEG);

    /* motion streak when fast */
    if (b->owner < 0 && spd > 0.45f)
    {
        float dx = -b->vx / spd * 0.03f;
        float dy = -b->vy / spd * 0.03f;
        glColor4f(1.0f, 1.0f, 1.0f, 0.25f);
        glLineWidth(2.0f);
        glBegin(GL_LINES);
        glVertex2f(bx, by);
        glVertex2f(bx + dx, by + dy);
        glEnd();
    }

    glPushMatrix();
    glTranslatef(bx, by, 0.0f);
    glRotatef(b->spin * 57.2958f, 0.0f, 0.0f, 1.0f);

    /* ball body */
    glColor3f(0.98f, 0.98f, 0.98f);
    draw_circle(0.0f, 0.0f, r, CIRCLE_SEG);

    /* rotating patches (rolling look) */
    glColor3f(0.12f, 0.12f, 0.12f);
    for (i = 0; i < 3; i++)
    {
        float a = (float)(2.0 * M_PI * i / 3.0);
        patch_x = cosf(a) * r * 0.55f;
        patch_y = sinf(a) * r * 0.55f;
        draw_circle(patch_x, patch_y, r * 0.28f, 6);
    }

    glPopMatrix();

    /* kick ring */
    if (pulse > 0.0f)
    {
        glColor4f(1.0f, 1.0f, 0.5f, pulse * 0.5f);
        glLineWidth(2.0f);
        glBegin(GL_LINE_LOOP);
        for (i = 0; i < 16; i++)
        {
            float a = (float)(2.0 * M_PI * i / 16);
            glVertex2f(bx + cosf(a) * (r + 0.008f + pulse * 0.012f),
                       by + sinf(a) * (r + 0.008f + pulse * 0.012f));
        }
        glEnd();
    }
}

static void draw_scene(void)
{
    int i;

    glClearColor(0.08f, 0.10f, 0.12f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(-1.15, 1.15, -0.85, 0.85, -1.0, 1.0);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();

    draw_pitch();

    /* players (drawn before ball so ball renders on top) */
    for (i = 0; i < TOTAL_PLAYERS; i++) {
        draw_player_sprite(&g_match.players[i], g_match.ball.owner == i);
        
        /* Draw positional target indicator (small dot showing where player wants to go) */
        if (!g_paused && g_match.phase == 0) {
            float tx = g_match.players[i].target_x;
            float ty = g_match.players[i].target_y;
            glColor4f(1.0f, 1.0f, 0.0f, 0.25f);
            draw_circle(tx, ty, 0.012f, 6);
        }
    }

    draw_ball_sprite();

    /* possession hint (small bar at top of pitch in world space) */
    {
        float bar = (g_match.possession_team == 0) ? -0.35f : 0.35f;
        glColor3f(1.0f, 1.0f, 1.0f);
        glLineWidth(4.0f);
        glBegin(GL_LINES);
        glVertex2f(bar, 0.62f);
        glVertex2f(bar + (g_match.possession_team == 0 ? 0.08f : -0.08f), 0.62f);
        glEnd();
    }
}

int WINAPI WinMain(HINSTANCE hInstance,
                   HINSTANCE hPrevInstance,
                   LPSTR lpCmdLine,
                   int nCmdShow)
{
    WNDCLASSEX wcex;
    HWND hwnd;
    HDC hDC;
    HGLRC hRC;
    MSG msg;
    BOOL bQuit = FALSE;
    DWORD last_tick = GetTickCount();

    wcex.cbSize = sizeof(WNDCLASSEX);
    wcex.style = CS_OWNDC;
    wcex.lpfnWndProc = WindowProc;
    wcex.cbClsExtra = 0;
    wcex.cbWndExtra = 0;
    wcex.hInstance = hInstance;
    wcex.hIcon = LoadIcon(NULL, IDI_APPLICATION);
    wcex.hCursor = LoadCursor(NULL, IDC_ARROW);
    wcex.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wcex.lpszMenuName = NULL;
    wcex.lpszClassName = "Football2D";
    wcex.hIconSm = LoadIcon(NULL, IDI_APPLICATION);

    if (!RegisterClassEx(&wcex))
        return 0;

    hwnd = CreateWindowEx(0,
                          "Football2D",
                          "2D Football Match",
                          WS_OVERLAPPEDWINDOW,
                          CW_USEDEFAULT,
                          CW_USEDEFAULT,
                          900,
                          650,
                          NULL,
                          NULL,
                          hInstance,
                          NULL);

    g_hwnd = hwnd;
    ShowWindow(hwnd, nCmdShow);
    EnableOpenGL(hwnd, &hDC, &hRC);
    init_match();

    while (!bQuit)
    {
        if (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE))
        {
            if (msg.message == WM_QUIT)
                bQuit = TRUE;
            else
            {
                TranslateMessage(&msg);
                DispatchMessage(&msg);
            }
        }
        else
        {
            DWORD now = GetTickCount();
            float dt = (float)(now - last_tick) / 1000.0f;
            if (dt > 0.05f) dt = 0.05f;
            last_tick = now;

            update_match(dt);
            update_animations(dt);
            draw_scene();
            SwapBuffers(hDC);
            Sleep(0);
        }
    }

    DisableOpenGL(hwnd, hDC, hRC);
    DestroyWindow(hwnd);
    return (int)msg.wParam;
}

LRESULT CALLBACK WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    switch (uMsg)
    {
        case WM_CLOSE:
            PostQuitMessage(0);
            break;

        case WM_DESTROY:
            return 0;

        case WM_KEYDOWN:
            switch (wParam)
            {
                case VK_ESCAPE:
                    PostQuitMessage(0);
                    break;
                case VK_SPACE:
                    g_paused = !g_paused;
                    break;
                case VK_ADD:
                case VK_OEM_PLUS:
                    g_speed += 0.25f;
                    if (g_speed > 4.0f) g_speed = 4.0f;
                    break;
                case VK_SUBTRACT:
                case VK_OEM_MINUS:
                    g_speed -= 0.25f;
                    if (g_speed < 0.25f) g_speed = 0.25f;
                    break;
                case 'R':
                    init_match();
                    break;
            }
            break;

        default:
            return DefWindowProc(hwnd, uMsg, wParam, lParam);
    }
    return 0;
}

void EnableOpenGL(HWND hwnd, HDC* hDC, HGLRC* hRC)
{
    PIXELFORMATDESCRIPTOR pfd;
    int iFormat;

    *hDC = GetDC(hwnd);
    ZeroMemory(&pfd, sizeof(pfd));
    pfd.nSize = sizeof(pfd);
    pfd.nVersion = 1;
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = 24;
    pfd.cDepthBits = 16;
    pfd.iLayerType = PFD_MAIN_PLANE;

    iFormat = ChoosePixelFormat(*hDC, &pfd);
    SetPixelFormat(*hDC, iFormat, &pfd);
    *hRC = wglCreateContext(*hDC);
    wglMakeCurrent(*hDC, *hRC);

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
}

void DisableOpenGL(HWND hwnd, HDC hDC, HGLRC hRC)
{
    if (g_pitch_list)
    {
        glDeleteLists(g_pitch_list, 1);
        g_pitch_list = 0;
    }
    wglMakeCurrent(NULL, NULL);
    wglDeleteContext(hRC);
    ReleaseDC(hwnd, hDC);
}
