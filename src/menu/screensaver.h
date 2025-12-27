/**
 * @file screensaver.h
 * @brief Screensaver to prevent screen burn-in
 * @ingroup menu
 */

#ifndef SCREENSAVER_H__
#define SCREENSAVER_H__

#include <stdbool.h>
#include <libdragon.h>

/** @brief Screensaver idle timeout in seconds */
#define SCREENSAVER_TIMEOUT_SECONDS (180)  // 3 minutes

/** @brief Fade duration in frames (30fps) */
#define SCREENSAVER_FADE_FRAMES (30)  // 1 second fade

/**
 * @brief Initialize the screensaver system.
 */
void screensaver_init(void);

/**
 * @brief Deinitialize the screensaver system.
 */
void screensaver_deinit(void);

/**
 * @brief Start the screensaver immediately.
 *
 * Used for testing/preview from settings menu.
 */
void screensaver_start(void);

/**
 * @brief Request the screensaver to fade out and stop.
 *
 * Called when user input is detected.
 */
void screensaver_stop(void);

/**
 * @brief Update screensaver animation state.
 *
 * Should be called every frame.
 *
 * @param idle_seconds Number of seconds since last user input.
 */
void screensaver_update(float idle_seconds);

/**
 * @brief Draw the screensaver.
 *
 * Should be called when rdpq is already attached to a display surface.
 */
void screensaver_draw(void);

/**
 * @brief Check if screensaver is currently active or fading.
 *
 * @return true if screensaver is visible, false otherwise.
 */
bool screensaver_is_active(void);

/**
 * @brief Check if screensaver is fully active (not fading).
 *
 * @return true if screensaver is fully opaque, false otherwise.
 */
bool screensaver_is_fully_active(void);

#endif /* SCREENSAVER_H__ */
