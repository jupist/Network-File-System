#include "nm_persistence.h"
#include "nm_hashmap.h"
#include "nm_logging.h"
#include "nm_types.h"
#include <stdio.h>

/*
 * Saves the current file directory state to disk.
 * "unsafe" - g_system_mutex must be held.
 */
void save_registry_to_disk_unsafe() {
    FILE* file = fopen(NM_REGISTRY_FILE, "wb"); // "wb" = Write Binary
    if (file == NULL) {
        perror("NM: (Persistence) Failed to open registry for saving");
        log_to_file("Internal", "NameServer", "CRITICAL: (Persistence) Failed to open registry for saving: %m");
        return;
    }

    // 1. Count the number of files
    int g_num_files = 0;
    for (int i = 0; i < HASH_MAP_SIZE; i++) {
        HashNode* node = g_file_hash_map[i];
        while (node != NULL) {
            g_num_files++;
            node = node->next;
        }
    }

    // 2. Write the count
    if (fwrite(&g_num_files, sizeof(int), 1, file) != 1) {
        perror("NM: (Persistence) Failed to write file count to registry");
        log_to_file("Internal", "NameServer", "CRITICAL: (Persistence) Failed to write file count to registry: %m");
        fclose(file);
        return;
    }

    // 3. Write each FileLocation struct
    if (g_num_files > 0) {
        for (int i = 0; i < HASH_MAP_SIZE; i++) {
            HashNode* node = g_file_hash_map[i];
            while (node != NULL) {
                if (fwrite(&node->file, sizeof(FileLocation), 1, file) != 1) {
                    perror("NM: (Persistence) Failed to write file data to registry");
                    log_to_file("Internal", "NameServer", "CRITICAL: (Persistence) Failed to write file data to registry: %m");
                    fclose(file);
                    return;
                }
                node = node->next;
            }
        }
    }

    fclose(file);
    printf("NM: (Persistence) System state saved to disk (%d files).\n", g_num_files);
    log_to_file("Internal", "NameServer", "INFO: (Persistence) System state saved to disk (%d files).", g_num_files);
}

/*
 * Loads the file directory state from disk on startup.
 * Runs before any threads, so no mutex is needed.
 */
void load_registry_from_disk() {
    FILE* file = fopen(NM_REGISTRY_FILE, "rb"); // "rb" = Read Binary
    if (file == NULL) {
        printf("NM: (Persistence) No existing registry file found. Starting fresh.\n");
        log_to_file("Internal", "NameServer", "INFO: (Persistence) No existing registry file found. Starting fresh.");
        return;
    }

    int g_num_files = 0;
    if (fread(&g_num_files, sizeof(int), 1, file) != 1) {
        perror("NM: (Persistence) Failed to read file count. Starting fresh");
        log_to_file("Internal", "NameServer", "ERROR: (Persistence) Failed to read file count. Starting fresh.");
        fclose(file);
        return;
    }

    if (g_num_files < 0) {
        printf("NM: (Persistence) Registry file corrupted (count=%d). Starting fresh.\n", g_num_files);
        log_to_file("Internal", "NameServer", "ERROR: (Persistence) Registry file corrupted (count=%d). Starting fresh.", g_num_files);
        fclose(file);
        return;
    }

    // Read each FileLocation and insert it into the hash map
    for (int i = 0; i < g_num_files; i++) {
        FileLocation temp_file;
        if (fread(&temp_file, sizeof(FileLocation), 1, file) != 1) {
            perror("NM: (Persistence) Failed to read file data. Halting load");
            log_to_file("Internal", "NameServer", "ERROR: (Persistence) Failed to read file data. Halting load.");
            break; // Stop loading, but keep what we have
        }
        
        // Initialize ss_index if it wasn't saved (backwards compatibility)
        if (temp_file.ss_index < 0 || temp_file.ss_index >= MAX_SERVERS) {
            temp_file.ss_index = 0; // Default to SS0
        }
        
        hash_map_insert_unsafe(temp_file);
    }

    fclose(file);
    printf("NM: (Persistence) Successfully loaded %d files from registry.\n", g_num_files);
    log_to_file("Internal", "NameServer", "INFO: (Persistence) Successfully loaded %d files from registry.", g_num_files);
}
