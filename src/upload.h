#pragma once
#include <string>

std::string init_upload(const std::string& filename, int total_chunks, int user_id);
bool save_chunk(const std::string& upload_id, int chunk_index, int total_chunks, const std::string& data);
bool complete_upload(const std::string& upload_id, std::string& out_token, std::string& out_filename);
void cleanup_old_uploads();
