#include "upload.h"
#include "db.h"
#include "auth.h"
#include "helpers.h"
#include <sqlite3.h>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <map>
#include <set>
#include <vector>
#include <sys/stat.h>
#include <ctime>
#include <iostream>
#include <algorithm>
#include <mutex>
#include <cstdio>
#include <fcntl.h>
#include <unistd.h>

extern sqlite3* db;

static std::mutex db_chunk_mutex;

// Límites duros: la entrada llega del cliente y no se puede asumir razonable.
static const long long MAX_CHUNK_BYTES  = 8 * 1024 * 1024;              // el cliente manda 4 MB
static const long long MAX_TOTAL_CHUNKS = 250000;                       // 4 MB * 250k = 1 TB
static const long long MIN_FREE_BYTES   = 20LL * 1024 * 1024 * 1024;    // no agotar el volumen
static const size_t    COPY_BUFFER      = 1024 * 1024;

namespace fs = std::filesystem;

static std::string upload_dir(const std::string& upload_id) { return uploads_tmp_dir() + "/" + upload_id; }

static std::string chunk_name(long long i) {
    char b[32];
    snprintf(b, sizeof(b), "%04lld.chunk", i);
    return std::string(b);
}

// Un "chunk guardado" debe significar "chunk en disco", no "en la caché del kernel".
static bool sync_to_disk(const std::string& path) {
    int fd = open(path.c_str(), O_RDONLY);
    if (fd < 0) return false;
    bool ok = (fsync(fd) == 0);
    close(fd);
    return ok;
}

static long long dir_bytes(const std::string& dir) {
    long long total = 0;
    std::error_code ec;
    for (fs::directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec), end;
         !ec && it != end; it.increment(ec)) {
        if (it->is_regular_file(ec) && it->path().extension() == ".chunk")
            total += (long long)it->file_size(ec);
    }
    return total;
}

static long long age_hours(const std::string& path) {
    struct stat st;
    if (stat(path.c_str(), &st) != 0) return 0;
    long long diff = (long long)time(nullptr) - (long long)st.st_mtime;
    return diff > 0 ? diff / 3600 : 0;
}

static bool temp_upload_row(const std::string& upload_id, long long& total, long long& user_id) {
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "SELECT total_chunks, user_id FROM temp_uploads "
                      "WHERE upload_id = ? AND expires > datetime('now')";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_text(stmt, 1, upload_id.c_str(), -1, SQLITE_STATIC);
    bool found = false;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        total   = sqlite3_column_int64(stmt, 0);
        user_id = sqlite3_column_int64(stmt, 1);
        found = true;
    }
    sqlite3_finalize(stmt);
    return found;
}

// --------------------------- CONTROL DE ACCESO ---------------------------
bool upload_access(const std::string& upload_id, int user_id, const std::string& role, std::string& err) {
    if (!is_safe_upload_id(upload_id)) { err = "upload_id inválido"; return false; }
    long long total = 0, owner = -1;
    if (!temp_upload_row(upload_id, total, owner)) { err = "subida inexistente o caducada"; return false; }
    if (role != "admin" && owner != user_id) { err = "esa subida pertenece a otro usuario"; return false; }
    return true;
}

// ------------------------------- INIT -------------------------------
std::string init_upload(const std::string& filename, long long total_chunks, int user_id, std::string& err) {
    std::string safe_name = sanitize_filename(filename);
    if (safe_name.empty()) { err = "nombre de archivo inválido"; return ""; }
    if (total_chunks < 1 || total_chunks > MAX_TOTAL_CHUNKS) {
        err = "total_chunks fuera de rango (1.." + std::to_string(MAX_TOTAL_CHUNKS) + ")";
        return "";
    }
    if (!enough_space(0, MIN_FREE_BYTES)) { err = "no hay suficiente espacio libre en disco"; return ""; }

    std::string upload_id = generate_secure_token().substr(0, 32);
    std::cout << "[INIT] upload_id=" << upload_id << " | filename=" << safe_name
              << " | chunks=" << total_chunks << std::endl;

    const char* sql = "INSERT INTO temp_uploads (upload_id, original_filename, total_chunks, user_id, chunks_received) "
                      "VALUES (?, ?, ?, ?, 0)";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        err = std::string("error interno: ") + sqlite3_errmsg(db);
        return "";
    }
    sqlite3_bind_text(stmt, 1, upload_id.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, safe_name.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_int64(stmt, 3, total_chunks);
    sqlite3_bind_int(stmt, 4, user_id);
    if (sqlite3_step(stmt) != SQLITE_DONE) {
        std::cerr << "[INIT ERROR] INSERT temp_uploads: " << sqlite3_errmsg(db) << std::endl;
        sqlite3_finalize(stmt);
        err = "no se pudo registrar la subida";
        return "";
    }
    sqlite3_finalize(stmt);

    std::string dir = upload_dir(upload_id);
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) {
        std::cerr << "[INIT ERROR] mkdir " << dir << ": " << ec.message() << std::endl;
        err = "no se pudo crear el directorio temporal";
        return "";
    }
    chmod(dir.c_str(), 0700);

    return upload_id;
}

// ------------------------------- CHUNK -------------------------------
bool save_chunk(const std::string& upload_id, long long chunk_index,
                const char* data, size_t len, std::string& err) {
    if (!is_safe_upload_id(upload_id)) { err = "upload_id inválido"; return false; }

    long long total = 0, owner = 0;
    if (!temp_upload_row(upload_id, total, owner)) { err = "subida inexistente o caducada"; return false; }

    if (chunk_index < 0 || chunk_index >= total) {
        err = "chunk_index fuera de rango (0.." + std::to_string(total - 1) + ")";
        return false;
    }
    if (len == 0 || (long long)len > MAX_CHUNK_BYTES) {
        err = "tamaño de chunk inválido (1.." + std::to_string(MAX_CHUNK_BYTES) + " bytes)";
        return false;
    }
    if (!enough_space((long long)len, MIN_FREE_BYTES)) { err = "no hay suficiente espacio libre en disco"; return false; }

    std::string dir = upload_dir(upload_id);
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) { err = "subida inexistente o caducada"; return false; }

    std::string final_path = dir + "/" + chunk_name(chunk_index);
    std::string part_path  = final_path + ".part";

    // escribir en .part -> fsync -> rename: o queda el chunk completo o no queda nada
    {
        std::ofstream f(part_path, std::ios::binary | std::ios::trunc);
        if (!f) {
            std::cerr << "[CHUNK ERROR] no se pudo abrir " << part_path << std::endl;
            err = "no se pudo abrir el chunk para escritura";
            return false;
        }
        f.write(data, (std::streamsize)len);
        f.flush();
        if (!f.good()) {
            f.close();
            fs::remove(part_path, ec);
            std::cerr << "[CHUNK ERROR] escritura falló: " << part_path << std::endl;
            err = "fallo de escritura al guardar el chunk (¿disco lleno?)";
            return false;
        }
        f.close();
    }
    if (!sync_to_disk(part_path)) {
        fs::remove(part_path, ec);
        err = "no se pudo confirmar el chunk en disco";
        return false;
    }
    fs::rename(part_path, final_path, ec);
    if (ec) {
        fs::remove(part_path, ec);
        std::cerr << "[CHUNK ERROR] rename: " << ec.message() << std::endl;
        err = "no se pudo publicar el chunk";
        return false;
    }

    // Contar el chunk solo si realmente falta en la BD. Comparar contra el número de
    // archivos en disco (bajo el mutex) hace que un retry concurrente no infle el contador.
    bool es_repetido = false;
    {
        std::lock_guard<std::mutex> lock(db_chunk_mutex);
        int en_disco = 0, en_bd = 0;
        std::error_code ec2;
        for (fs::directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec2), end;
             !ec2 && it != end; it.increment(ec2)) {
            if (it->is_regular_file(ec2) && it->path().extension() == ".chunk") en_disco++;
        }
        sqlite3_stmt* chk = nullptr;
        if (sqlite3_prepare_v2(db, "SELECT chunks_received FROM temp_uploads WHERE upload_id = ?", -1, &chk, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(chk, 1, upload_id.c_str(), -1, SQLITE_STATIC);
            if (sqlite3_step(chk) == SQLITE_ROW) en_bd = sqlite3_column_int(chk, 0);
            sqlite3_finalize(chk);
        }
        es_repetido = (en_disco <= en_bd);

        if (!es_repetido) {
            sqlite3_stmt* stmt = nullptr;
            if (sqlite3_prepare_v2(db, "UPDATE temp_uploads SET chunks_received = chunks_received + 1 WHERE upload_id = ?",
                                   -1, &stmt, nullptr) == SQLITE_OK) {
                sqlite3_bind_text(stmt, 1, upload_id.c_str(), -1, SQLITE_STATIC);
                if (sqlite3_step(stmt) != SQLITE_DONE)
                    std::cerr << "[CHUNK DB ERROR] UPDATE: " << sqlite3_errmsg(db) << std::endl;
                sqlite3_finalize(stmt);
            }
        }
    }

    std::cout << "[CHUNK OK] " << upload_id << " chunk " << chunk_index << " (" << len << " bytes)"
              << (es_repetido ? " [repetido]" : "") << std::endl;
    return true;
}

// Índice -> tamaño, leyendo el directorio una sola vez (sin N stat()).
static std::map<long long, long long> chunks_on_disk(const std::string& dir) {
    std::map<long long, long long> out;
    std::error_code ec;
    for (fs::directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec), end;
         !ec && it != end; it.increment(ec)) {
        const fs::path& p = it->path();
        if (p.extension() != ".chunk") continue;
        long long idx = 0;
        if (!parse_ll(p.stem().string(), idx)) continue;
        out[idx] = (long long)it->file_size(ec);
    }
    return out;
}

bool upload_status(const std::string& upload_id, long long& total, long long& present,
                   std::vector<long long>& missing) {
    missing.clear();
    present = 0;
    total = 0;
    long long owner = 0;
    if (!is_safe_upload_id(upload_id)) return false;
    if (!temp_upload_row(upload_id, total, owner)) return false;

    std::map<long long, long long> have = chunks_on_disk(upload_dir(upload_id));
    for (long long i = 0; i < total; ++i) {
        if (have.count(i)) present++;
        else if (missing.size() < 5000) missing.push_back(i);
    }
    return true;
}

// ------------------------------- COMPLETE -------------------------------
bool find_completed_upload(const std::string& upload_id, std::string& out_token, long long& out_owner) {
    out_owner = -1;
    if (!is_safe_upload_id(upload_id)) return false;
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, "SELECT token, uploader_id FROM files WHERE upload_temp_id = ?", -1, &stmt, nullptr) != SQLITE_OK)
        return false;
    sqlite3_bind_text(stmt, 1, upload_id.c_str(), -1, SQLITE_STATIC);
    bool found = false;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const unsigned char* t = sqlite3_column_text(stmt, 0);
        if (t) out_token = std::string((const char*)t);
        out_owner = sqlite3_column_int64(stmt, 1);
        found = !out_token.empty();
    }
    sqlite3_finalize(stmt);
    return found;
}

bool complete_upload(const std::string& upload_id, std::string& out_token, std::string& out_filename,
                     std::string& out_md5, long long& out_size, std::string& err) {
    out_size = 0;
    err.clear();
    if (!is_safe_upload_id(upload_id)) { err = "upload_id inválido"; return false; }

    std::string orig_name;
    long long total = 0, user_id = 0;
    {
        sqlite3_stmt* stmt = nullptr;
        const char* sql = "SELECT original_filename, total_chunks, user_id FROM temp_uploads "
                          "WHERE upload_id = ? AND expires > datetime('now')";
        if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
            err = std::string("error interno: ") + sqlite3_errmsg(db);
            return false;
        }
        sqlite3_bind_text(stmt, 1, upload_id.c_str(), -1, SQLITE_STATIC);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            const unsigned char* nm = sqlite3_column_text(stmt, 0);
            orig_name = nm ? std::string((const char*)nm) : std::string();
            total   = sqlite3_column_int64(stmt, 1);
            user_id = sqlite3_column_int64(stmt, 2);
        }
        sqlite3_finalize(stmt);
    }
    if (orig_name.empty()) { err = "subida inexistente o caducada"; return false; }
    if (total < 1 || total > MAX_TOTAL_CHUNKS) { err = "total_chunks inválido en la BD"; return false; }

    std::string dir = upload_dir(upload_id);
    std::map<long long, long long> have = chunks_on_disk(dir);

    // Tiene que estar el conjunto EXACTO 0..total-1; "hay N archivos" no basta
    // (permitía un hueco y ensamblaba un archivo truncado sin avisar).
    long long expected_bytes = 0;
    std::vector<long long> faltan;
    for (long long i = 0; i < total; ++i) {
        auto it = have.find(i);
        if (it == have.end()) { if (faltan.size() < 20) faltan.push_back(i); continue; }
        expected_bytes += it->second;
    }
    if (!faltan.empty()) {
        std::string lista;
        for (size_t i = 0; i < faltan.size(); ++i) { if (i) lista += ","; lista += std::to_string(faltan[i]); }
        err = "faltan " + std::to_string(total - (long long)have.size()) + " chunk(s) (ej. " + lista + ")";
        std::cout << "[ERROR] upload incompleto | total=" << total << " en_disco=" << have.size() << std::endl;
        return false;
    }
    if (expected_bytes <= 0) { err = "la subida no contiene datos"; return false; }
    if (!enough_space(expected_bytes, MIN_FREE_BYTES)) { err = "no hay espacio suficiente para ensamblar"; return false; }

    out_token = generate_secure_token();
    std::string internal_name = out_token + "_" + sanitize_filename(orig_name);
    std::string final_path = files_dir() + "/" + internal_name;
    std::string part_path  = final_path + ".ensamblando";

    // ---------- ensamblar contando bytes y calculando MD5 sobre la marcha ----------
    std::string md5;
    long long written = 0;
    bool ok = true;
    {
        std::ofstream out(part_path, std::ios::binary | std::ios::trunc);
        if (!out) {
            std::cerr << "[COMPLETE ERROR] no se pudo crear " << part_path << std::endl;
            err = "no se pudo crear el archivo final";
            return false;
        }
        Md5Stream digest;
        std::vector<char> buf(COPY_BUFFER);

        for (long long i = 0; i < total && ok; ++i) {
            std::ifstream in(dir + "/" + chunk_name(i), std::ios::binary);
            if (!in) { err = "no se pudo leer el chunk " + std::to_string(i); ok = false; break; }
            long long chunk_left = have[i];
            while (chunk_left > 0) {
                size_t want = (size_t)std::min<long long>(chunk_left, (long long)buf.size());
                in.read(buf.data(), (std::streamsize)want);
                std::streamsize got = in.gcount();
                if (got <= 0) { err = "lectura corta en el chunk " + std::to_string(i); ok = false; break; }
                out.write(buf.data(), got);
                if (!out.good()) { err = "fallo de escritura al ensamblar (¿disco lleno?)"; ok = false; break; }
                digest.update(buf.data(), (size_t)got);
                written += got;
                chunk_left -= got;
            }
        }
        out.flush();
        if (ok && !out.good()) err = "fallo al cerrar el archivo final";
        out.close();

        if (ok && written != expected_bytes)
            err = "tamaño ensamblado no coincide (" + std::to_string(written) + " != " + std::to_string(expected_bytes) + ")";
        if (ok && !sync_to_disk(part_path))
            err = "no se pudo confirmar el archivo en disco";
        if (ok) md5 = digest.hex_final();

        if (!ok || !err.empty()) {
            std::error_code ec2;
            fs::remove(part_path, ec2);
            std::cerr << "[COMPLETE ERROR] " << err << std::endl;
            return false;
        }
    }

    std::error_code ec;
    fs::rename(part_path, final_path, ec);
    if (ec) {
        fs::remove(part_path, ec);
        std::cerr << "[COMPLETE ERROR] rename: " << ec.message() << std::endl;
        err = "no se pudo publicar el archivo final";
        return false;
    }

    long long final_size = (long long)fs::file_size(final_path, ec);
    if (ec || final_size != expected_bytes) {
        fs::remove(final_path, ec);
        err = "el archivo final quedó truncado";
        return false;
    }

    // extensión limpia
    std::string ext = "bin";
    size_t dot = orig_name.find_last_of('.');
    if (dot != std::string::npos && dot + 1 < orig_name.size()) {
        ext = orig_name.substr(dot + 1);
        ext.erase(std::remove_if(ext.begin(), ext.end(), [](unsigned char c) { return c < 32 || c > 126; }), ext.end());
        if (ext.empty()) ext = "bin";
    }
    if (ext.size() > 16) ext = ext.substr(0, 16);

    // ---------- INSERT antes de borrar los chunks: si falla, se puede reintentar ----------
    {
        const char* ins_sql = "INSERT INTO files (filename, internal_filename, filetype, size, token, md5, uploader_id, upload_temp_id) "
                              "VALUES (?, ?, ?, ?, ?, ?, ?, ?)";
        sqlite3_stmt* ins = nullptr;
        if (sqlite3_prepare_v2(db, ins_sql, -1, &ins, nullptr) != SQLITE_OK) {
            err = std::string("error interno: ") + sqlite3_errmsg(db);
            fs::remove(final_path, ec);
            return false;
        }
        sqlite3_bind_text (ins, 1, orig_name.c_str(),      -1, SQLITE_STATIC);
        sqlite3_bind_text (ins, 2, internal_name.c_str(),  -1, SQLITE_STATIC);
        sqlite3_bind_text (ins, 3, ext.c_str(),            -1, SQLITE_STATIC);
        sqlite3_bind_int64(ins, 4, final_size);
        sqlite3_bind_text (ins, 5, out_token.c_str(),      -1, SQLITE_STATIC);
        sqlite3_bind_text (ins, 6, md5.c_str(),            -1, SQLITE_STATIC);
        sqlite3_bind_int64(ins, 7, user_id);
        sqlite3_bind_text (ins, 8, upload_id.c_str(),      -1, SQLITE_STATIC);

        bool insert_ok = (sqlite3_step(ins) == SQLITE_DONE);
        if (!insert_ok) std::cerr << "[COMPLETE ERROR] INSERT files: " << sqlite3_errmsg(db) << std::endl;
        sqlite3_finalize(ins);

        if (!insert_ok) {
            fs::remove(final_path, ec);
            err = "no se pudo registrar el archivo (los chunks se conservan para reintentar)";
            return false;
        }
    }

    out_md5 = md5;
    out_size = final_size;
    out_filename = orig_name;

    fs::remove_all(dir, ec);
    if (ec) std::cerr << "[COMPLETE WARN] no se pudo borrar " << dir << ": " << ec.message() << std::endl;
    {
        sqlite3_stmt* del = nullptr;
        if (sqlite3_prepare_v2(db, "DELETE FROM temp_uploads WHERE upload_id = ?", -1, &del, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(del, 1, upload_id.c_str(), -1, SQLITE_STATIC);
            sqlite3_step(del);
            sqlite3_finalize(del);
        }
    }

    std::cout << "[SUCCESS] completado: " << orig_name << " | " << final_size << " bytes | md5=" << md5 << std::endl;
    return true;
}

// ------------------------------- LIMPIEZA -------------------------------
void cleanup_old_uploads() {
    // primero los datos de las filas que vamos a borrar (si no, quedan huérfanos para siempre)
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, "SELECT upload_id FROM temp_uploads WHERE expires < datetime('now')", -1, &stmt, nullptr) == SQLITE_OK) {
        std::vector<std::string> a_borrar;
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            const unsigned char* id = sqlite3_column_text(stmt, 0);
            if (id) a_borrar.push_back(std::string((const char*)id));
        }
        sqlite3_finalize(stmt);
        std::error_code ec;
        for (const auto& id : a_borrar) {
            if (!is_safe_upload_id(id)) continue;
            fs::remove_all(upload_dir(id), ec);
        }
    }
    sqlite3_exec(db, "DELETE FROM temp_uploads WHERE expires < datetime('now')", nullptr, nullptr, nullptr);
}

long long sweep_abandoned(bool dry_run, std::string& report) {
    long long freed = 0;
    std::ostringstream log;
    std::error_code ec;

    cleanup_old_uploads();

    // 1) directorios de chunks que ya no tienen fila en temp_uploads
    std::set<std::string> vivos;
    {
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db, "SELECT upload_id FROM temp_uploads", -1, &stmt, nullptr) == SQLITE_OK) {
            while (sqlite3_step(stmt) == SQLITE_ROW) {
                const unsigned char* id = sqlite3_column_text(stmt, 0);
                if (id) vivos.insert(std::string((const char*)id));
            }
            sqlite3_finalize(stmt);
        }
    }
    if (fs::is_directory(uploads_tmp_dir(), ec)) {
        for (fs::directory_iterator it(uploads_tmp_dir(), fs::directory_options::skip_permission_denied, ec), end;
             !ec && it != end; it.increment(ec)) {
            std::error_code e2;
            if (!it->is_directory(e2)) continue;
            std::string name = it->path().filename().string();
            if (vivos.count(name) || !is_safe_upload_id(name)) continue;

            long long b = dir_bytes(it->path().string());
            long long h = age_hours(it->path().string());
            log << "chunks huerfanos: " << name << " (" << (b / (1024 * 1024)) << " MB, " << h << " h)\n";
            if (!dry_run && h >= 1) {                 // nunca tocar una subida en curso
                fs::remove_all(it->path(), e2);
                if (!e2) freed += b; else log << "  ! no se pudo borrar: " << e2.message() << "\n";
            }
        }
    }

    // 2) archivos en files/ que no referencia ninguna fila de la BD
    std::set<std::string> referenciados;
    {
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db, "SELECT internal_filename FROM files WHERE internal_filename IS NOT NULL "
                                   "UNION SELECT filename FROM files WHERE filename IS NOT NULL", -1, &stmt, nullptr) == SQLITE_OK) {
            while (sqlite3_step(stmt) == SQLITE_ROW) {
                const unsigned char* v = sqlite3_column_text(stmt, 0);
                if (v) referenciados.insert(std::string((const char*)v));
            }
            sqlite3_finalize(stmt);
        }
    }
    if (fs::is_directory(files_dir(), ec)) {
        for (fs::directory_iterator it(files_dir(), fs::directory_options::skip_permission_denied, ec), end;
             !ec && it != end; it.increment(ec)) {
            std::error_code e2;
            if (!it->is_regular_file(e2)) continue;
            std::string name = it->path().filename().string();

            // residuo de un ensamblado interrumpido
            if (name.size() > 12 && name.compare(name.size() - 12, 12, ".ensamblando") == 0) {
                long long b = (long long)it->file_size(e2);
                log << "residuo de ensamblado: " << name << " (" << (b / (1024 * 1024)) << " MB)\n";
                if (!dry_run) {
                    fs::remove(it->path(), e2);
                    if (!e2) freed += b;
                }
                continue;
            }
            if (referenciados.count(name)) continue;

            long long b = (long long)it->file_size(e2);
            log << "archivo huerfano: " << name << " (" << (b / (1024 * 1024)) << " MB)\n";
            if (!dry_run) {
                fs::remove(it->path(), e2);
                if (!e2) freed += b; else log << "  ! no se pudo borrar: " << e2.message() << "\n";
            }
        }
    }

    report = log.str();
    std::cout << "[SWEEP " << (dry_run ? "dry-run" : "ejecutado") << "] " << (freed / (1024 * 1024)) << " MB\n"
              << report << std::endl;
    return freed;
}
