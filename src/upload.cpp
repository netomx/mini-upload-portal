#include "upload.h"
#include "db.h"
#include "auth.h"
#include <sqlite3.h>
#include <fstream>
#include <filesystem>
#include <sys/stat.h>
#include <iostream>
#include <algorithm>   // <--- NECESARIO para std::remove_if

extern sqlite3* db;

std::string init_upload(const std::string& filename, int total_chunks, int user_id) {
    std::string upload_id = generate_secure_token().substr(0, 32);
    std::cout << "[DEBUG INIT] upload_id=" << upload_id << " | filename=" << filename << " | chunks=" << total_chunks << std::endl;

    const char* sql = R"(INSERT INTO temp_uploads (upload_id, original_filename, total_chunks, user_id, chunks_received) VALUES (?, ?, ?, ?, 0))";
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr);
    sqlite3_bind_text(stmt, 1, upload_id.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, filename.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt, 3, total_chunks);
    sqlite3_bind_int(stmt, 4, user_id);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    std::string dir = "/mnt/xvdb1/tmp/uploads/" + upload_id;
    mkdir(dir.c_str(), 0777);
    return upload_id;
}

bool save_chunk(const std::string& upload_id, int chunk_index, int total_chunks, const std::string& data) {
    std::cout << "[DEBUG CHUNK] upload=" << upload_id << " chunk=" << chunk_index << "/" << total_chunks << std::endl;

    std::string dir = "/mnt/xvdb1/tmp/uploads/" + upload_id;
    if (!std::filesystem::exists(dir)) return false;

    char fname[32];
    snprintf(fname, sizeof(fname), "%04d.chunk", chunk_index);
    std::ofstream f(dir + "/" + fname, std::ios::binary);
    f.write(data.data(), data.size());
    f.close();

    const char* sql = "UPDATE temp_uploads SET chunks_received = chunks_received + 1 WHERE upload_id = ?";
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr);
    sqlite3_bind_text(stmt, 1, upload_id.c_str(), -1, SQLITE_STATIC);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    return true;
}

bool complete_upload(const std::string& upload_id, std::string& out_token, std::string& out_filename) {
    std::cout << "[DEBUG COMPLETE] upload_id recibido = '" << upload_id << "'" << std::endl;

    sqlite3_stmt* stmt = nullptr;
    const char* sql = "SELECT original_filename, total_chunks, chunks_received, user_id FROM temp_uploads WHERE upload_id = ? AND expires > datetime('now')";
    sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr);
    sqlite3_bind_text(stmt, 1, upload_id.c_str(), -1, SQLITE_STATIC);

    std::string orig_name;
    int total = 0, received = 0, user_id = 0;

    if (sqlite3_step(stmt) == SQLITE_ROW) {
        orig_name = (const char*)sqlite3_column_text(stmt, 0);
        total = sqlite3_column_int(stmt, 1);
        received = sqlite3_column_int(stmt, 2);
        user_id = sqlite3_column_int(stmt, 3);
    }
    sqlite3_finalize(stmt);

    std::cout << "[DEBUG BD] total=" << total << " | received=" << received << std::endl;

    std::string dir = "/mnt/xvdb1/tmp/uploads/" + upload_id;
    int files_on_disk = 0;
    if (std::filesystem::exists(dir)) {
        for (const auto& entry : std::filesystem::directory_iterator(dir)) {
            if (entry.path().extension() == ".chunk") files_on_disk++;
        }
    }
    std::cout << "[DEBUG DISCO] Archivos .chunk encontrados: " << files_on_disk << std::endl;

    if (files_on_disk > received) received = files_on_disk;

    if (received != total) {
        std::cout << "[ERROR] Upload incompleto - total=" << total << " | received=" << received << std::endl;
        return false;
    }

	/*
    std::string final_path = "/mnt/xvdb1/files/" + orig_name;

    std::ofstream final_file(final_path, std::ios::binary);
    for (int i = 0; i < total; ++i) {
        char cname[32];
        snprintf(cname, sizeof(cname), "%04d.chunk", i);
        std::ifstream chunk(dir + "/" + cname, std::ios::binary);
        final_file << chunk.rdbuf();
        chunk.close();
    }
    final_file.close();
	*/
	
	out_token = generate_secure_token();
    out_filename = orig_name;
	
	std::string internal_name = out_token + "_" + orig_name;
    std::string final_path = "/mnt/xvdb1/files/" + internal_name;
	
	std::ofstream final_file(final_path, std::ios::binary);
    for (int i = 0; i < total; ++i) {
        char cname[32];
        snprintf(cname, sizeof(cname), "%04d.chunk", i);
        std::ifstream chunk(dir + "/" + cname, std::ios::binary);
        final_file << chunk.rdbuf();
        chunk.close();
    }
    final_file.close();
	
	
    // === EXTENSIÓN LIMPIA ===
    std::string ext = "bin";
    size_t dot = orig_name.find_last_of('.');
    if (dot != std::string::npos && dot + 1 < orig_name.size()) {
        ext = orig_name.substr(dot + 1);
        ext.erase(std::remove_if(ext.begin(), ext.end(), [](unsigned char c){ return c < 32 || c > 126; }), ext.end());
        if (ext.empty()) ext = "bin";
    }

    
	/*
    const char* ins_sql = R"(INSERT INTO files (filename, filetype, size, token, uploader_id, upload_temp_id) VALUES (?, ?, ?, ?, ?, ?))";
    sqlite3_stmt* ins = nullptr;
    sqlite3_prepare_v2(db, ins_sql, -1, &ins, nullptr);
    sqlite3_bind_text(ins, 1, orig_name.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(ins, 2, ext.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_int64(ins, 3, std::filesystem::file_size(final_path));
    sqlite3_bind_text(ins, 4, out_token.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_int(ins, 5, user_id);
	sqlite3_bind_text(ins, 6, upload_id.c_str(), -1, SQLITE_STATIC);
    sqlite3_step(ins);
    sqlite3_finalize(ins);
	*/
	const char* ins_sql = R"(
        INSERT INTO files (filename, internal_filename, filetype, size, token, uploader_id, upload_temp_id) 
        VALUES (?, ?, ?, ?, ?, ?, ?)
    )";
    sqlite3_stmt* ins = nullptr;
    sqlite3_prepare_v2(db, ins_sql, -1, &ins, nullptr);
    
    sqlite3_bind_text(ins, 1, orig_name.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(ins, 2, internal_name.c_str(), -1, SQLITE_STATIC); // <--- Guardamos el nombre real
    sqlite3_bind_text(ins, 3, ext.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_int64(ins, 4, std::filesystem::file_size(final_path));
    sqlite3_bind_text(ins, 5, out_token.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_int(ins, 6, user_id);
    sqlite3_bind_text(ins, 7, upload_id.c_str(), -1, SQLITE_STATIC);
    
    sqlite3_step(ins);
    sqlite3_finalize(ins);

    std::filesystem::remove_all(dir);
    std::string del_sql = "DELETE FROM temp_uploads WHERE upload_id = '" + upload_id + "'";
	sqlite3_exec(db, del_sql.c_str(), nullptr, nullptr, nullptr);

    std::cout << "[SUCCESS] ¡Upload completado! Archivo: " << orig_name << " | Token: " << out_token << std::endl;
    return true;
}

void cleanup_old_uploads() {
    sqlite3_exec(db, "DELETE FROM temp_uploads WHERE expires < datetime('now')", nullptr, nullptr, nullptr);
}
