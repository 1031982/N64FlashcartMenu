/**
 * @file bgm.c
 * @brief Background Music Player implementation
 * @ingroup menu
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include <libdragon.h>

#include "bgm.h"
#include "utils/fs.h"

#include <minimp3/minimp3.h>

#define SEEK_PREDECODE_FRAMES   (5)
#define MENU_DIRECTORY          "/menu"
#define MP3_BUFFER_THRESHOLD    (2048)  // Minimum buffer before refill

/**
 * @brief Skip ID3v2 tag at the beginning of an MP3 file.
 *
 * @param data Pointer to buffer data.
 * @param size Size of buffer.
 * @return Size of ID3v2 tag to skip, or 0 if no tag found.
 */
static size_t bgm_skip_id3v2(const uint8_t *data, size_t size) {
    if (size < 10) {
        return 0;
    }
    // Check for "ID3" header
    if (data[0] == 'I' && data[1] == 'D' && data[2] == '3') {
        // ID3v2 size is stored in 4 bytes, 7 bits each (syncsafe integer)
        size_t tag_size = ((data[6] & 0x7F) << 21) |
                          ((data[7] & 0x7F) << 14) |
                          ((data[8] & 0x7F) << 7) |
                          (data[9] & 0x7F);
        return tag_size + 10;  // Add 10 for header
    }
    return 0;
}

/** @brief BGM Player State Structure */
typedef struct {
    bool loaded;                    /**< Indicates if BGM file is loaded */
    bool paused;                    /**< Indicates if playback is paused */

    FILE *f;                        /**< File pointer */
    size_t file_size;               /**< Size of the file */
    size_t data_start;              /**< Start position of audio data */
    uint8_t buffer[16 * 1024];      /**< Read buffer */
    uint8_t *buffer_ptr;            /**< Current position in buffer */
    size_t buffer_left;             /**< Data remaining in buffer */

    mp3dec_t dec;                   /**< MP3 decoder state */
    mp3dec_frame_info_t info;       /**< MP3 frame information */

    int seek_predecode_frames;      /**< Frames to pre-decode after seek */

    waveform_t wave;                /**< Waveform for mixer playback */
    char *file_path;                /**< Path to loaded file */
} bgm_player_t;

static bgm_player_t *bgm = NULL;

/**
 * @brief Reset the BGM decoder state.
 */
static void bgm_reset_decoder(void) {
    mp3dec_init(&bgm->dec);
    bgm->seek_predecode_frames = 0;
    bgm->buffer_ptr = bgm->buffer;
    bgm->buffer_left = 0;
}

/**
 * @brief Fill the buffer with data from the MP3 file.
 */
static void bgm_fill_buffer(void) {
    if (feof(bgm->f)) {
        return;
    }

    if (bgm->buffer_left >= MP3_BUFFER_THRESHOLD) {
        return;
    }

    if ((bgm->buffer_ptr != bgm->buffer) && (bgm->buffer_left > 0)) {
        memmove(bgm->buffer, bgm->buffer_ptr, bgm->buffer_left);
        bgm->buffer_ptr = bgm->buffer;
    }

    bgm->buffer_left += fread(bgm->buffer + bgm->buffer_left, 1, sizeof(bgm->buffer) - bgm->buffer_left, bgm->f);
}

/**
 * @brief Check if BGM has finished playing the current loop.
 */
static bool bgm_is_finished(void) {
    return bgm->loaded && feof(bgm->f) && (bgm->buffer_left == 0);
}

/**
 * @brief Seek to the beginning of the audio data for looping.
 */
static bgm_err_t bgm_seek_to_start(void) {
    if (!bgm->loaded) {
        return BGM_ERR_NO_FILE;
    }

    if (fseek(bgm->f, bgm->data_start, SEEK_SET)) {
        return BGM_ERR_IO;
    }

    bgm_reset_decoder();
    bgm_fill_buffer();

    if (ferror(bgm->f)) {
        return BGM_ERR_IO;
    }

    return BGM_OK;
}

/**
 * @brief Waveform read callback for mixer.
 */
static void bgm_wave_read(void *ctx, samplebuffer_t *sbuf, int wpos, int wlen, bool seeking) {
    while (wlen > 0) {
        bgm_fill_buffer();

        int samples = mp3dec_decode_frame(&bgm->dec, bgm->buffer_ptr, bgm->buffer_left, NULL, &bgm->info);

        if (samples > 0) {
            short *buffer = (short *)(samplebuffer_append(sbuf, samples));

            bgm->buffer_ptr += bgm->info.frame_offset;
            bgm->buffer_left -= bgm->info.frame_offset;

            mp3dec_decode_frame(&bgm->dec, bgm->buffer_ptr, bgm->buffer_left, buffer, &bgm->info);

            if (bgm->seek_predecode_frames > 0) {
                bgm->seek_predecode_frames -= 1;
                memset(buffer, 0, samples * sizeof(short) * bgm->info.channels);
            }

            wlen -= samples;
        }

        bgm->buffer_ptr += bgm->info.frame_bytes;
        bgm->buffer_left -= bgm->info.frame_bytes;

        if (bgm->info.frame_bytes == 0) {
            short *buffer = (short *)(samplebuffer_append(sbuf, wlen));
            memset(buffer, 0, wlen * sizeof(short) * bgm->info.channels);
            wlen = 0;
        }
    }
}

/**
 * @brief Initialize the BGM mixer channel.
 */
static void bgm_mixer_init(void) {
    // Set up BGM channel with reasonable limits for 44.1kHz audio
    mixer_ch_set_limits(SOUND_BGM_CHANNEL, 16, 96000, 0);
    mixer_ch_set_vol(SOUND_BGM_CHANNEL, 0.5f, 0.5f);
}

bgm_err_t bgm_init(void) {
    if (bgm != NULL) {
        return BGM_OK;
    }

    bgm = calloc(1, sizeof(bgm_player_t));
    if (bgm == NULL) {
        return BGM_ERR_OUT_OF_MEM;
    }

    bgm_reset_decoder();
    bgm->loaded = false;
    bgm->paused = false;
    bgm->file_path = NULL;

    bgm->wave = (waveform_t){
        .name = "bgm",
        .bits = 16,
        .channels = 2,
        .frequency = 44100,
        .len = WAVEFORM_MAX_LEN - 1,
        .loop_len = WAVEFORM_MAX_LEN - 1,
        .read = bgm_wave_read,
        .ctx = bgm,
    };

    bgm_mixer_init();

    return BGM_OK;
}

void bgm_deinit(void) {
    if (bgm == NULL) {
        return;
    }

    bgm_stop();
    free(bgm->file_path);
    free(bgm);
    bgm = NULL;
}

/**
 * @brief Load a BGM file.
 */
static bgm_err_t bgm_load(const char *path) {
    if (bgm == NULL) {
        return BGM_ERR_OUT_OF_MEM;
    }

    if (bgm->loaded) {
        bgm_stop();
    }

    if ((bgm->f = fopen(path, "rb")) == NULL) {
        return BGM_ERR_NO_FILE;
    }
    setbuf(bgm->f, NULL);

    struct stat st;
    if (fstat(fileno(bgm->f), &st)) {
        fclose(bgm->f);
        return BGM_ERR_IO;
    }
    bgm->file_size = st.st_size;

    bgm_reset_decoder();

    // Parse the MP3 file to find the first audio frame
    while (!(feof(bgm->f) && bgm->buffer_left == 0)) {
        bgm_fill_buffer();

        if (ferror(bgm->f)) {
            fclose(bgm->f);
            return BGM_ERR_IO;
        }

        // Skip ID3v2 tags
        size_t id3v2_skip = bgm_skip_id3v2((const uint8_t *)(bgm->buffer_ptr), bgm->buffer_left);
        if (id3v2_skip > 0) {
            if (fseek(bgm->f, (-bgm->buffer_left) + id3v2_skip, SEEK_CUR)) {
                fclose(bgm->f);
                return BGM_ERR_IO;
            }
            bgm_reset_decoder();
            continue;
        }

        int samples = mp3dec_decode_frame(&bgm->dec, bgm->buffer_ptr, bgm->buffer_left, NULL, &bgm->info);
        if (samples > 0) {
            bgm->loaded = true;
            bgm->data_start = ftell(bgm->f) - bgm->buffer_left + bgm->info.frame_offset;

            bgm->buffer_ptr += bgm->info.frame_offset;
            bgm->buffer_left -= bgm->info.frame_offset;

            bgm->wave.channels = bgm->info.channels;
            bgm->wave.frequency = bgm->info.hz;

            // Store file path for potential reload
            free(bgm->file_path);
            bgm->file_path = strdup(path);

            return BGM_OK;
        }

        bgm->buffer_ptr += bgm->info.frame_bytes;
        bgm->buffer_left -= bgm->info.frame_bytes;
    }

    fclose(bgm->f);
    return BGM_ERR_INVALID_FILE;
}

bgm_err_t bgm_load_and_play(const char *storage_prefix) {
    if (bgm == NULL) {
        bgm_err_t err = bgm_init();
        if (err != BGM_OK) {
            return err;
        }
    }

    // Construct path to bg.mp3 in menu directory
    char path[256];
    snprintf(path, sizeof(path), "%s%s/%s", storage_prefix, MENU_DIRECTORY, BGM_FILE);

    // Check if file exists
    if (!file_exists(path)) {
        return BGM_ERR_NO_FILE;
    }

    bgm_err_t err = bgm_load(path);
    if (err != BGM_OK) {
        return err;
    }

    // Start playback
    bgm->paused = false;
    mixer_ch_play(SOUND_BGM_CHANNEL, &bgm->wave);

    return BGM_OK;
}

void bgm_stop(void) {
    if (bgm == NULL) {
        return;
    }

    if (mixer_ch_playing(SOUND_BGM_CHANNEL)) {
        mixer_ch_stop(SOUND_BGM_CHANNEL);
    }

    if (bgm->loaded) {
        bgm->loaded = false;
        bgm->paused = false;
        fclose(bgm->f);
    }
}

void bgm_pause(void) {
    if (bgm == NULL || !bgm->loaded) {
        return;
    }

    if (mixer_ch_playing(SOUND_BGM_CHANNEL)) {
        mixer_ch_stop(SOUND_BGM_CHANNEL);
        bgm->paused = true;
    }
}

bgm_err_t bgm_resume(void) {
    if (bgm == NULL || !bgm->loaded) {
        return BGM_ERR_NO_FILE;
    }

    if (bgm->paused && !mixer_ch_playing(SOUND_BGM_CHANNEL)) {
        mixer_ch_play(SOUND_BGM_CHANNEL, &bgm->wave);
        bgm->paused = false;
    }

    return BGM_OK;
}

bool bgm_is_playing(void) {
    if (bgm == NULL) {
        return false;
    }
    return mixer_ch_playing(SOUND_BGM_CHANNEL);
}

bool bgm_is_loaded(void) {
    if (bgm == NULL) {
        return false;
    }
    return bgm->loaded;
}

void bgm_poll(void) {
    if (bgm == NULL || !bgm->loaded || bgm->paused) {
        return;
    }

    // Check for I/O errors
    if (ferror(bgm->f)) {
        bgm_stop();
        return;
    }

    // Handle looping: if finished, seek back to start and continue
    if (bgm_is_finished()) {
        bgm_seek_to_start();
    }
}
