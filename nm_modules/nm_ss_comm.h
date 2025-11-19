#ifndef NM_SS_COMM_H
#define NM_SS_COMM_H

#include "nm_types.h"

/*
 * Storage Server Communication Module
 * Handles all communication between Name Server and Storage Servers
 */

// Forward commands to SS
int forward_create_to_ss(const char* ss_ip, int ss_nm_port, const char* filename);
int forward_delete_to_ss(const char* ss_ip, int ss_nm_port, const char* filename);
int forward_undo_to_ss(const char* ss_ip, int ss_nm_port, const char* filename);
int forward_create_folder_to_ss(const char* ss_ip, int ss_nm_port, const char* folder_path);
int forward_move_to_ss(const char* ss_ip, int ss_nm_port, const char* filename, const char* folder_path);

// Checkpoint operations
int forward_checkpoint_to_ss(const char* ss_ip, int ss_nm_port, const char* filename, 
                              const char* checkpoint_tag, char* out_response, int out_len);
int forward_listcheckpoints_to_ss(const char* ss_ip, int ss_nm_port, const char* filename, 
                                   char* out_buffer, int out_len);
int forward_viewcheckpoint_to_ss(const char* ss_ip, int ss_nm_port, const char* filename, 
                                  const char* checkpoint_tag, char* out_buffer, int out_len);
int forward_revert_to_ss(const char* ss_ip, int ss_nm_port, const char* filename, 
                          const char* checkpoint_tag, char* out_response, int out_len);

// Get data from SS
int get_file_content_from_ss(const char* ss_ip, int ss_client_port, const char* filename, 
                              char* out_buffer, int out_len);
int get_metadata_from_ss(const char* ss_ip, int ss_nm_port, const char* filename,
                          char* word_count_buf, char* char_count_buf);
int get_folder_listing_from_ss(const char* ss_ip, int ss_nm_port, const char* folder_path, 
                                char* out_buffer, int out_len);

// File operations
int validate_sentence_index(const char* ss_ip, int ss_client_port, const char* filename, int target_sentence);
int copy_file_between_ss(const char* src_ip, int src_client_port, 
                         const char* dest_ip, int dest_nm_port, 
                         const char* filename);

// External dependencies (these are defined in nameserver.c)
extern StorageServer server_list[MAX_SERVERS];
extern int g_num_servers;

#endif // NM_SS_COMM_H
