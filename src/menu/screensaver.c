/**
 * @file screensaver.c
 * @brief Simple radial particle screensaver for N64
 * @ingroup menu
 *
 * Lightweight screensaver using minimal CPU - just a few particles
 * moving in a radial pattern from center to edges.
 */

#include <stdlib.h>

#include <libdragon.h>

#include "screensaver.h"
#include "ui_components/constants.h"

/** @brief Number of particles (keep low for performance) */
#define NUM_PARTICLES (24)

/** @brief Particle size in pixels */
#define PARTICLE_SIZE (3)

/** @brief Base speed of particles */
#define PARTICLE_SPEED (1.5f)

/** @brief Maximum brightness (dim to prevent burn-in) */
#define MAX_BRIGHTNESS (0.3f)

/** @brief Particle structure */
typedef struct {
    float x, y;         /**< Current position */
    float dx, dy;       /**< Direction vector (normalized) */
    float speed;        /**< Movement speed */
    float dist;         /**< Distance from center (for fading) */
    uint8_t r, g, b;    /**< Color */
} particle_t;

/** @brief Screensaver state */
typedef struct {
    bool active;
    bool fading_in;
    bool fading_out;
    float fade_alpha;
    particle_t particles[NUM_PARTICLES];
} screensaver_t;

static screensaver_t *ss = NULL;

/** @brief Center of screen */
#define CENTER_X (DISPLAY_WIDTH / 2)
#define CENTER_Y (DISPLAY_HEIGHT / 2)

/** @brief Max distance from center to corner */
#define MAX_DIST (400.0f)

/**
 * @brief Simple pseudo-random number generator (0.0 - 1.0)
 */
static float simple_random(unsigned int seed) {
    seed = (seed * 1103515245u + 12345u) & 0x7fffffffu;
    return (float)(seed % 1000u) / 1000.0f;
}

/**
 * @brief Initialize a particle at center with radial direction.
 */
static void init_particle(particle_t *p, int index) {
    // Start at center
    p->x = CENTER_X;
    p->y = CENTER_Y;
    p->dist = 0;

    // Direction based on index (evenly distributed around circle)
    // Pre-calculated sin/cos approximations for fixed angles
    float angle = (float)index / NUM_PARTICLES;

    // Simple angle to direction (approximate)
    // Using fixed directions to avoid runtime trig
    if (angle < 0.125f) {
        p->dx = 1.0f; p->dy = angle * 8.0f;
    } else if (angle < 0.25f) {
        p->dx = 1.0f - (angle - 0.125f) * 8.0f; p->dy = 1.0f;
    } else if (angle < 0.375f) {
        p->dx = -(angle - 0.25f) * 8.0f; p->dy = 1.0f;
    } else if (angle < 0.5f) {
        p->dx = -1.0f; p->dy = 1.0f - (angle - 0.375f) * 8.0f;
    } else if (angle < 0.625f) {
        p->dx = -1.0f; p->dy = -(angle - 0.5f) * 8.0f;
    } else if (angle < 0.75f) {
        p->dx = -1.0f + (angle - 0.625f) * 8.0f; p->dy = -1.0f;
    } else if (angle < 0.875f) {
        p->dx = (angle - 0.75f) * 8.0f; p->dy = -1.0f;
    } else {
        p->dx = 1.0f; p->dy = -1.0f + (angle - 0.875f) * 8.0f;
    }

    // Vary speed slightly
    p->speed = PARTICLE_SPEED * (0.8f + simple_random(index * 7) * 0.4f);

    // Stagger starting distance so particles don't all start at center
    p->dist = simple_random(index * 13) * MAX_DIST * 0.8f;
    p->x = CENTER_X + p->dx * p->dist;
    p->y = CENTER_Y + p->dy * p->dist;

    // Color - cycle through dim colors
    switch (index % 4) {
        case 0: p->r = 40; p->g = 80; p->b = 120; break;  // Blue
        case 1: p->r = 80; p->g = 40; p->b = 100; break;  // Purple
        case 2: p->r = 40; p->g = 90; p->b = 90; break;   // Teal
        case 3: p->r = 60; p->g = 60; p->b = 100; break;  // Slate
    }
}

/**
 * @brief Update particle position.
 */
static void update_particle(particle_t *p, int index) {
    p->x += p->dx * p->speed;
    p->y += p->dy * p->speed;
    p->dist += p->speed;

    // Reset when particle goes off screen
    if (p->x < -PARTICLE_SIZE || p->x > DISPLAY_WIDTH + PARTICLE_SIZE ||
        p->y < -PARTICLE_SIZE || p->y > DISPLAY_HEIGHT + PARTICLE_SIZE ||
        p->dist > MAX_DIST) {
        p->x = CENTER_X;
        p->y = CENTER_Y;
        p->dist = 0;
    }
}

void screensaver_init(void) {
    if (ss != NULL) {
        return;
    }

    ss = calloc(1, sizeof(screensaver_t));
    if (ss == NULL) {
        return;
    }

    ss->active = false;
    ss->fading_in = false;
    ss->fading_out = false;
    ss->fade_alpha = 0.0f;

    for (int i = 0; i < NUM_PARTICLES; i++) {
        init_particle(&ss->particles[i], i);
    }
}

void screensaver_deinit(void) {
    if (ss != NULL) {
        free(ss);
        ss = NULL;
    }
}

void screensaver_start(void) {
    if (ss == NULL) {
        screensaver_init();
    }

    if (!ss->active && !ss->fading_in) {
        ss->active = true;
        ss->fading_in = true;
        ss->fading_out = false;
    }
}

void screensaver_stop(void) {
    if (ss == NULL || !ss->active) {
        return;
    }

    if (!ss->fading_out) {
        ss->fading_out = true;
        ss->fading_in = false;
    }
}

void screensaver_update(float idle_seconds) {
    if (ss == NULL) {
        return;
    }

    if (!ss->active && idle_seconds >= SCREENSAVER_TIMEOUT_SECONDS) {
        screensaver_start();
    }

    if (!ss->active) {
        return;
    }

    // Update particles
    for (int i = 0; i < NUM_PARTICLES; i++) {
        update_particle(&ss->particles[i], i);
    }

    // Handle fade
    float fade_step = 1.0f / SCREENSAVER_FADE_FRAMES;

    if (ss->fading_in) {
        ss->fade_alpha += fade_step;
        if (ss->fade_alpha >= 1.0f) {
            ss->fade_alpha = 1.0f;
            ss->fading_in = false;
        }
    } else if (ss->fading_out) {
        ss->fade_alpha -= fade_step;
        if (ss->fade_alpha <= 0.0f) {
            ss->fade_alpha = 0.0f;
            ss->fading_out = false;
            ss->active = false;
        }
    }
}

void screensaver_draw(void) {
    if (ss == NULL || !ss->active || ss->fade_alpha <= 0.0f) {
        return;
    }

    rdpq_mode_push();
    rdpq_set_mode_standard();

    for (int i = 0; i < NUM_PARTICLES; i++) {
        particle_t *p = &ss->particles[i];

        // Fade based on distance from center (bright near center, dim at edges)
        float dist_fade = 1.0f - (p->dist / MAX_DIST);
        if (dist_fade < 0.0f) dist_fade = 0.0f;

        float brightness = dist_fade * ss->fade_alpha * MAX_BRIGHTNESS;

        uint8_t r = (uint8_t)(p->r * brightness);
        uint8_t g = (uint8_t)(p->g * brightness);
        uint8_t b = (uint8_t)(p->b * brightness);

        if (r == 0 && g == 0 && b == 0) {
            continue;
        }

        int x = (int)p->x;
        int y = (int)p->y;

        // Bounds check
        if (x < 0 || x >= DISPLAY_WIDTH - PARTICLE_SIZE ||
            y < 0 || y >= DISPLAY_HEIGHT - PARTICLE_SIZE) {
            continue;
        }

        rdpq_set_mode_fill(RGBA32(r, g, b, 0xFF));
        rdpq_fill_rectangle(x, y, x + PARTICLE_SIZE, y + PARTICLE_SIZE);
    }

    rdpq_mode_pop();
}

bool screensaver_is_active(void) {
    return ss != NULL && ss->active;
}

bool screensaver_is_fully_active(void) {
    return ss != NULL && ss->active && !ss->fading_in && !ss->fading_out;
}
