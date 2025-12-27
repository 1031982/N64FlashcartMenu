/**
 * @file virtual_cpak.c
 * @brief Implementation of Virtual Controller Pak management.
 * @ingroup menu
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <libdragon.h>
#include <dir.h>

#include "virtual_cpak.h"
#include "utils/fs.h"
#include "utils/cpakfs_utils.h"

/* Global to capture last errno for debugging */
int vcpak_last_errno = 0;

/*
 * Directory Management
 */

void vcpak_get_game_directory(const char *storage_prefix, const char *game_code,
                               char *out_path, size_t out_size) {
    // Game code is only 4 bytes in ROM header, not null-terminated
    // Make a safe copy
    char safe_code[5];
    memcpy(safe_code, game_code, 4);
    safe_code[4] = '\0';

    snprintf(out_path, out_size, "%s%s/%s",
             storage_prefix ? storage_prefix : "",
             VCPAK_SAVES_BASE_DIR,
             safe_code);
}

vcpak_err_t vcpak_ensure_game_directory(const char *storage_prefix, const char *game_code) {
    char base_path[256];
    char game_path[256];

    // Create base directory first
    snprintf(base_path, sizeof(base_path), "%s%s",
             storage_prefix ? storage_prefix : "",
             VCPAK_SAVES_BASE_DIR);

    if (!directory_exists(base_path)) {
        if (directory_create(base_path)) {
            return VCPAK_ERR_DIR_CREATE;
        }
    }

    // Create game-specific directory
    vcpak_get_game_directory(storage_prefix, game_code, game_path, sizeof(game_path));

    if (!directory_exists(game_path)) {
        if (directory_create(game_path)) {
            return VCPAK_ERR_DIR_CREATE;
        }
    }

    return VCPAK_OK;
}


/*
 * Pak File Enumeration
 */

vcpak_err_t vcpak_list_paks(const char *storage_prefix, const char *game_code,
                            const char *last_used_filename, vcpak_list_t *out_list) {
    char game_dir[256];
    dir_t dir_entry;
    int count = 0;
    int capacity = 16;

    // Initialize output
    memset(out_list, 0, sizeof(*out_list));
    strncpy(out_list->game_code, game_code, sizeof(out_list->game_code) - 1);
    out_list->selected = -1;

    vcpak_get_game_directory(storage_prefix, game_code, game_dir, sizeof(game_dir));

    // Allocate initial array
    out_list->entries = malloc(capacity * sizeof(vcpak_entry_t));
    if (!out_list->entries) {
        return VCPAK_ERR_ALLOC;
    }

    // Check if directory exists
    if (!directory_exists(game_dir)) {
        out_list->count = 0;
        return VCPAK_OK;
    }

    // Enumerate .pak files
    if (dir_findfirst(game_dir, &dir_entry) >= 0) {
        do {
            // Check for .pak extension
            char *ext = strrchr(dir_entry.d_name, '.');
            if (!ext || strcasecmp(ext, ".pak") != 0) {
                continue;
            }

            // Expand array if needed
            if (count >= capacity) {
                capacity *= 2;
                vcpak_entry_t *new_entries = realloc(out_list->entries,
                                                      capacity * sizeof(vcpak_entry_t));
                if (!new_entries) {
                    free(out_list->entries);
                    out_list->entries = NULL;
                    return VCPAK_ERR_ALLOC;
                }
                out_list->entries = new_entries;
            }

            // Populate entry
            vcpak_entry_t *entry = &out_list->entries[count];
            strncpy(entry->filename, dir_entry.d_name, sizeof(entry->filename) - 1);
            entry->filename[sizeof(entry->filename) - 1] = '\0';

            snprintf(entry->full_path, sizeof(entry->full_path), "%s/%s",
                     game_dir, dir_entry.d_name);

            // Check if this is the last-used pak
            entry->is_last_used = false;
            if (last_used_filename && last_used_filename[0] != '\0') {
                if (strcmp(entry->filename, last_used_filename) == 0) {
                    entry->is_last_used = true;
                    out_list->selected = count;
                }
            }

            count++;
        } while (dir_findnext(game_dir, &dir_entry) == 0);
    }

    out_list->count = count;

    // If no last-used was found but we have entries, select the first one
    if (out_list->selected < 0 && count > 0) {
        out_list->selected = 0;
    }

    return VCPAK_OK;
}

void vcpak_list_free(vcpak_list_t *list) {
    if (list->entries) {
        free(list->entries);
        list->entries = NULL;
    }
    list->count = 0;
    list->selected = -1;
}


/*
 * Pak Operations
 */

vcpak_err_t vcpak_restore_to_physical(const char *pak_path, int controller) {
    if (!has_cpak(controller)) {
        return VCPAK_ERR_NO_CPAK;
    }

    // Unmount any existing filesystem on the pak
    cpakfs_unmount(controller);

    uint8_t *data = malloc(VCPAK_BANK_SIZE);
    if (!data) {
        return VCPAK_ERR_ALLOC;
    }

    FILE *fp = fopen(pak_path, "rb");
    if (!fp) {
        free(data);
        return VCPAK_ERR_FILE_NOT_FOUND;
    }

    // Get file size
    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        free(data);
        return VCPAK_ERR_IO;
    }
    long filesize = ftell(fp);
    if (filesize < 0) {
        fclose(fp);
        free(data);
        return VCPAK_ERR_IO;
    }
    rewind(fp);

    int total_banks = (int)((filesize + VCPAK_BANK_SIZE - 1) / VCPAK_BANK_SIZE);

    // Check if file fits on physical pak
    int banks_on_device = cpak_probe_banks(controller);
    if (banks_on_device < 1) {
        fclose(fp);
        free(data);
        return VCPAK_ERR_CORRUPTED;
    }
    if (total_banks > banks_on_device) {
        fclose(fp);
        free(data);
        return VCPAK_ERR_TOO_LARGE;
    }

    // Write banks to physical pak
    for (int bank = 0; bank < total_banks; bank++) {
        size_t bytes_read = fread(data, 1, VCPAK_BANK_SIZE, fp);
        if (bytes_read == 0 && ferror(fp)) {
            fclose(fp);
            free(data);
            return VCPAK_ERR_IO;
        }
        if (bytes_read == 0 && feof(fp)) {
            break;
        }

        int written = cpak_write((joypad_port_t)controller, (uint8_t)bank, 0, data, bytes_read);
        if (written < 0 || (size_t)written != bytes_read) {
            fclose(fp);
            free(data);
            return VCPAK_ERR_IO;
        }
    }

    fclose(fp);
    free(data);
    return VCPAK_OK;
}

vcpak_err_t vcpak_backup_from_physical(const char *pak_path, int controller) {
    if (!has_cpak(controller)) {
        return VCPAK_ERR_NO_CPAK;
    }

    int banks = cpak_probe_banks(controller);
    if (banks < 1) {
        banks = 1;  // Fallback
    }

    uint8_t *data = malloc(VCPAK_BANK_SIZE);
    if (!data) {
        return VCPAK_ERR_ALLOC;
    }

    FILE *fp = fopen(pak_path, "wb");
    if (!fp) {
        free(data);
        return VCPAK_ERR_IO;
    }

    // Read banks from physical pak and write to file
    for (int bank = 0; bank < banks; bank++) {
        int bytes_read = cpak_read((joypad_port_t)controller, (uint8_t)bank, 0,
                                    data, VCPAK_BANK_SIZE);
        if (bytes_read < 0 || bytes_read != VCPAK_BANK_SIZE) {
            fclose(fp);
            free(data);
            return VCPAK_ERR_IO;
        }

        size_t written = fwrite(data, 1, VCPAK_BANK_SIZE, fp);
        if (written != VCPAK_BANK_SIZE) {
            fclose(fp);
            free(data);
            return VCPAK_ERR_IO;
        }
    }

    fclose(fp);
    free(data);
    return VCPAK_OK;
}

vcpak_err_t vcpak_create_empty(const char *pak_path) {
    // Create a properly formatted empty Controller Pak image
    // Standard pak is 32KB (1 bank) with proper filesystem structure

    uint8_t *data = malloc(VCPAK_BANK_SIZE);
    if (!data) {
        return VCPAK_ERR_ALLOC;
    }

    // Initialize with 0x00 (empty pak pattern)
    memset(data, 0x00, VCPAK_BANK_SIZE);

    // Set up the basic mempak filesystem structure
    // The mempak has a specific structure:
    // - Pages 0-4: ID area and checksums
    // - Page 5: Table of contents
    // - Pages 6-127: Data pages

    // ID area at offset 0x0000 (repeated at 0x0100)
    // Format: 0x81 repeated 32 times, then serial, then checksum areas
    for (int i = 0; i < 32; i++) {
        data[i] = 0x81;
        data[0x100 + i] = 0x81;
    }

    // Pages 1-3 contain index table copies
    // Initialize index table entries as free (0x03 = free)
    // Index table starts at page 5 (offset 0x500) with 128 entries (one per page)
    for (int i = 0; i < 128; i++) {
        // Each entry is 2 bytes: next page pointer
        // 0x0003 = free, 0x0001 = end of chain
        data[0x300 + i * 2] = 0x00;
        data[0x300 + i * 2 + 1] = 0x03;  // Free
    }

    // Copy index table to backup locations
    memcpy(&data[0x500], &data[0x300], 256);

    FILE *fp = fopen(pak_path, "wb");
    if (!fp) {
        free(data);
        return VCPAK_ERR_IO;
    }

    size_t written = fwrite(data, 1, VCPAK_BANK_SIZE, fp);
    fclose(fp);
    free(data);

    if (written != VCPAK_BANK_SIZE) {
        return VCPAK_ERR_IO;
    }

    return VCPAK_OK;
}

vcpak_err_t vcpak_generate_filename(const char *storage_prefix, const char *game_code,
                                    const char *game_title, char *out_filename, size_t out_size) {
    char game_dir[256];
    char test_path[256];
    char clean_title[32];

    vcpak_get_game_directory(storage_prefix, game_code, game_dir, sizeof(game_dir));

    // Clean up the game title for use in filename
    // Remove spaces and special characters
    // Note: ROM titles are fixed 20-byte fields, not always null-terminated
    int j = 0;
    for (int i = 0; i < 20 && game_title[i] != '\0' && j < 20; i++) {
        char c = game_title[i];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9')) {
            clean_title[j++] = c;
        }
    }
    clean_title[j] = '\0';

    // If title is empty, use game code
    if (clean_title[0] == '\0') {
        strncpy(clean_title, game_code, sizeof(clean_title) - 1);
        clean_title[sizeof(clean_title) - 1] = '\0';
    }

    // Find a unique filename
    for (int i = 1; i <= 999; i++) {
        snprintf(out_filename, out_size, "%s_%03d.pak", clean_title, i);
        snprintf(test_path, sizeof(test_path), "%s/%s", game_dir, out_filename);

        if (!file_exists(test_path)) {
            return VCPAK_OK;
        }
    }

    // Fallback with timestamp if somehow we have 999+ paks
    time_t now = time(NULL);
    snprintf(out_filename, out_size, "%s_%ld.pak", clean_title, (long)now);
    return VCPAK_OK;
}


/*
 * Dirty State Management
 */

static void vcpak_get_state_path(const char *storage_prefix, char *out_path, size_t out_size) {
    snprintf(out_path, out_size, "%s/menu/%s",
             storage_prefix ? storage_prefix : "",
             VCPAK_STATE_FILENAME);
}

vcpak_err_t vcpak_state_save(const char *storage_prefix, const vcpak_state_t *state) {
    char state_path[256];
    vcpak_get_state_path(storage_prefix, state_path, sizeof(state_path));

    FILE *fp = fopen(state_path, "wb");
    if (!fp) {
        return VCPAK_ERR_IO;
    }

    size_t written = fwrite(state, sizeof(*state), 1, fp);
    fclose(fp);

    return (written == 1) ? VCPAK_OK : VCPAK_ERR_IO;
}

vcpak_err_t vcpak_state_load(const char *storage_prefix, vcpak_state_t *state) {
    char state_path[256];
    vcpak_get_state_path(storage_prefix, state_path, sizeof(state_path));

    if (!file_exists(state_path)) {
        return VCPAK_ERR_FILE_NOT_FOUND;
    }

    FILE *fp = fopen(state_path, "rb");
    if (!fp) {
        return VCPAK_ERR_IO;
    }

    size_t read_count = fread(state, sizeof(*state), 1, fp);
    fclose(fp);

    if (read_count != 1) {
        return VCPAK_ERR_IO;
    }

    // Validate magic number
    if (state->magic != VCPAK_STATE_MAGIC) {
        return VCPAK_ERR_CORRUPTED;
    }

    return VCPAK_OK;
}

vcpak_err_t vcpak_state_clear(const char *storage_prefix) {
    char state_path[256];
    vcpak_get_state_path(storage_prefix, state_path, sizeof(state_path));

    if (file_exists(state_path)) {
        if (remove(state_path) != 0) {
            return VCPAK_ERR_IO;
        }
    }

    return VCPAK_OK;
}

bool vcpak_state_is_dirty(const char *storage_prefix) {
    vcpak_state_t state;

    if (vcpak_state_load(storage_prefix, &state) != VCPAK_OK) {
        return false;
    }

    return (state.is_dirty != 0);
}
