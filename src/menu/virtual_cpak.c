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

/**
 * @brief Calculate the ID sector checksum (matches libdragon's __cpakfs_fsid_checksum)
 *
 * The checksum is computed over the first 14 big-endian words (28 bytes) of the ID sector.
 */
static void vcpak_calc_id_checksum(const uint8_t *id_sector, uint16_t *checksum1, uint16_t *checksum2) {
    uint32_t sum = 0;
    for (int i = 0; i < 14; i++) {
        // Read as big-endian 16-bit value
        sum += ((uint16_t)id_sector[i * 2] << 8) | id_sector[i * 2 + 1];
    }
    *checksum1 = sum & 0xFFFF;
    *checksum2 = 0xFFF2 - *checksum1;
}

/**
 * @brief Calculate the FAT page checksum (matches libdragon's __cpakfs_fat_checksum)
 *
 * The checksum is the sum of all bytes in entries starting from start_idx.
 */
static uint8_t vcpak_calc_fat_checksum(const uint8_t *fat_page, int start_idx) {
    uint8_t checksum = 0;
    for (int i = start_idx; i < 128; i++) {
        checksum += fat_page[i * 2];      // bank byte
        checksum += fat_page[i * 2 + 1];  // page byte
    }
    return checksum;
}

vcpak_err_t vcpak_create_empty(const char *pak_path) {
    // Create a properly formatted empty Controller Pak image
    // Standard pak is 32KB (1 bank) with proper filesystem structure
    //
    // Structure (32KB = 128 pages of 256 bytes):
    // - Page 0 (0x000-0x0FF): ID area with checksums at specific blocks
    // - Pages 1-2 (0x100-0x2FF): FAT (File Allocation Table) and backup
    // - Pages 3-4 (0x300-0x4FF): Note table and backup
    // - Pages 5-127 (0x500-0x7FFF): Data pages (available for saves)

    uint8_t *data = malloc(VCPAK_BANK_SIZE);
    if (!data) {
        return VCPAK_ERR_ALLOC;
    }

    // Initialize entire pak with 0x00
    memset(data, 0x00, VCPAK_BANK_SIZE);

    // === PAGE 0: ID Area ===
    // The ID sector (32 bytes) is written at 4 locations within page 0:
    // blocks 1, 3, 4, 6 (offsets 0x20, 0x60, 0x80, 0xC0)
    // This matches libdragon's pattern: 0x5A = 01011010 binary

    // Build the ID sector (32 bytes)
    uint8_t id_sector[32];
    memset(id_sector, 0, 32);

    // Bytes 0-23: Serial number (can be zeros or random, we use a simple pattern)
    // Using "N64MENU" as identifier in the serial area
    memcpy(&id_sector[0], "N64MENUVPAK", 11);
    // Rest of serial is zeros

    // Bytes 24-25: device_id_lsb (big-endian, value 0x0001)
    id_sector[24] = 0x00;
    id_sector[25] = 0x01;

    // Bytes 26-27: bank_size_msb (big-endian, value 0x0100 for 1 bank = 256 FAT entries)
    id_sector[26] = 0x01;
    id_sector[27] = 0x00;

    // Bytes 28-31: checksums (calculated below)
    uint16_t checksum1, checksum2;
    vcpak_calc_id_checksum(id_sector, &checksum1, &checksum2);

    // Store checksums in big-endian
    id_sector[28] = (checksum1 >> 8) & 0xFF;
    id_sector[29] = checksum1 & 0xFF;
    id_sector[30] = (checksum2 >> 8) & 0xFF;
    id_sector[31] = checksum2 & 0xFF;

    // Write ID sector to all 4 required locations (pattern 0x5A)
    // Bit positions: 1, 3, 4, 6 -> offsets 0x20, 0x60, 0x80, 0xC0
    memcpy(&data[0x20], id_sector, 32);
    memcpy(&data[0x60], id_sector, 32);
    memcpy(&data[0x80], id_sector, 32);
    memcpy(&data[0xC0], id_sector, 32);

    // === PAGES 1-2: FAT (File Allocation Table) ===
    // For a 1-bank pak, the reserved pages are:
    // Page 0: ID sector
    // Pages 1-2: FAT (2 copies)
    // Pages 3-4: Note table (2 copies)
    // Total reserved: 5 pages (indices 0-4)
    //
    // FAT entry format: 2 bytes (bank, page)
    // - 0x00, 0x00 = Reserved (system page)
    // - 0x00, 0x01 = End of chain (terminator)
    // - 0x00, 0x03 = Unused (free page)

    uint8_t fat_page[256];
    memset(fat_page, 0, 256);

    // Entry 0: will hold checksum (set after calculating)
    fat_page[0] = 0x00;
    fat_page[1] = 0x00;  // placeholder for checksum

    // Entries 1-4: Reserved (system pages)
    for (int i = 1; i <= 4; i++) {
        fat_page[i * 2] = 0x00;      // bank = 0
        fat_page[i * 2 + 1] = 0x00;  // page = 0 (reserved marker)
    }

    // Entries 5-127: Unused (free pages available for saves)
    for (int i = 5; i < 128; i++) {
        fat_page[i * 2] = 0x00;      // bank = 0
        fat_page[i * 2 + 1] = 0x03;  // page = 3 (unused marker)
    }

    // Calculate and store FAT checksum
    // For the first FAT page, checksum starts from index 5 (after reserved entries)
    // Actually, libdragon uses: int reserved = 1 + (fat_size >> 8) * 2 + 2 = 5
    // And checksum is calculated from index 1 for simplicity (matching __cpakfs_fat_checksum behavior)
    uint8_t fat_checksum = vcpak_calc_fat_checksum(fat_page, 5);
    fat_page[1] = fat_checksum;

    // Write FAT to page 1 (0x100) and backup to page 2 (0x200)
    memcpy(&data[0x100], fat_page, 256);
    memcpy(&data[0x200], fat_page, 256);

    // === PAGES 3-4: Note Table ===
    // The note table holds 16 notes of 32 bytes each = 512 bytes total
    // For an empty pak, all notes are zeroed (empty)
    // Pages 3-4 (0x300-0x4FF) are already zeroed from memset

    // === PAGES 5-127: Data Pages ===
    // Already zeroed from memset, available for game saves

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
