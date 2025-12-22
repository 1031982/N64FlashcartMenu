/**
 * @file bgm.h
 * @brief Background Music Player
 * @ingroup menu
 */

#ifndef BGM_H__
#define BGM_H__

#include <stdbool.h>

/** @brief BGM channel for mixer playback */
#define SOUND_BGM_CHANNEL (4)

/** @brief BGM file path relative to menu directory */
#define BGM_FILE "bg.mp3"

/**
 * @brief BGM error enumeration.
 */
typedef enum {
    BGM_OK,                 /**< No error */
    BGM_ERR_OUT_OF_MEM,     /**< Out of memory error */
    BGM_ERR_IO,             /**< Input/Output error */
    BGM_ERR_NO_FILE,        /**< No file found error */
    BGM_ERR_INVALID_FILE,   /**< Invalid file error */
} bgm_err_t;

/**
 * @brief Initialize the background music system.
 *
 * Allocates resources for BGM playback. Must be called before other BGM functions.
 *
 * @return bgm_err_t Error code indicating the result.
 */
bgm_err_t bgm_init(void);

/**
 * @brief Deinitialize the background music system.
 *
 * Stops playback and frees all resources.
 */
void bgm_deinit(void);

/**
 * @brief Load and start background music.
 *
 * Attempts to load the BGM file from the specified path and start playback.
 * The music will loop continuously until stopped.
 *
 * @param storage_prefix The storage prefix (e.g., "sd:/").
 * @return bgm_err_t Error code indicating the result.
 */
bgm_err_t bgm_load_and_play(const char *storage_prefix);

/**
 * @brief Stop and unload background music.
 *
 * Stops playback and releases the loaded file.
 */
void bgm_stop(void);

/**
 * @brief Pause background music playback.
 *
 * Temporarily stops playback without unloading the file.
 */
void bgm_pause(void);

/**
 * @brief Resume background music playback.
 *
 * Resumes playback from where it was paused.
 *
 * @return bgm_err_t Error code indicating the result.
 */
bgm_err_t bgm_resume(void);

/**
 * @brief Check if background music is currently playing.
 *
 * @return true if BGM is playing, false otherwise.
 */
bool bgm_is_playing(void);

/**
 * @brief Check if background music is loaded.
 *
 * @return true if a BGM file is loaded, false otherwise.
 */
bool bgm_is_loaded(void);

/**
 * @brief Process background music.
 *
 * Should be called regularly (e.g., each frame) to handle looping
 * and other BGM state updates.
 */
void bgm_poll(void);

#endif /* BGM_H__ */
