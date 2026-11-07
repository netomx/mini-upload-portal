#define CPPHTTPLIB_FORM_DATA_SUPPORT
#include "httplib.h"
#include "db.h"
#include "helpers.h"
#include "upload.h"
#include "auth.h"
#include <iostream>
#include <sqlite3.h>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <condition_variable>
#include <unordered_set>
#include <unordered_map>
#include <string>
#include <vector>
#include <cstdlib>
#include <chrono>

namespace fs = std::filesystem;

extern sqlite3* db;

std::mutex completion_mutex;
std::condition_variable completion_cv;
std::unordered_set<std::string> active_completions;

// Limpieza periódica (sesiones muertas + subidas caducadas), a tope de una vez cada 10 min.
std::mutex cleanup_mu;
std::chrono::steady_clock::time_point last_cleanup{};

// Error en JSON: el frontend hace res.json() y muestra el texto.
static void json_error(httplib::Response& res, int status, const std::string& msg) {
    res.status = status;
    res.set_content("{\"error\":\"" + escape_json(msg) + "\"}", "application/json");
}

static std::string bearer_token(const httplib::Request& req) {
    auto auth = req.get_header_value("Authorization");
    if (auth.find("Bearer ") != 0) return std::string();
    return auth.substr(7);
}

// Freno de fuerza bruta en /api/login (el portal está expuesto en internet).
namespace {
    const int LOGIN_MAX_FAILS = 8;
    const auto LOGIN_WINDOW = std::chrono::minutes(15);
    std::mutex login_mu;
    struct LoginState { int fails = 0; std::chrono::steady_clock::time_point since{}; };
    std::unordered_map<std::string, LoginState> login_fails;

    bool login_blocked(const std::string& key) {
        std::lock_guard<std::mutex> lk(login_mu);
        auto it = login_fails.find(key);
        if (it == login_fails.end()) return false;
        if (std::chrono::steady_clock::now() - it->second.since > LOGIN_WINDOW) { login_fails.erase(it); return false; }
        return it->second.fails >= LOGIN_MAX_FAILS;
    }
    void login_failed(const std::string& key) {
        std::lock_guard<std::mutex> lk(login_mu);
        if (login_fails.size() > 10000) login_fails.clear();      // cota de memoria
        auto& st = login_fails[key];
        if (st.fails == 0) st.since = std::chrono::steady_clock::now();
        st.fails++;
    }
    void login_succeeded(const std::string& key) {
        std::lock_guard<std::mutex> lk(login_mu);
        login_fails.erase(key);
    }
}

bool check_auth(const httplib::Request& req, httplib::Response& res, int& out_user_id, std::string& out_role) {
    auto auth = req.get_header_value("Authorization");
    if (auth.find("Bearer ") != 0) {
        res.status = 401; res.set_content(R"({"error":"Unauthorized"})", "application/json"); return false;
    }
    std::string token = auth.substr(7);
    if (!validate_token(token, out_user_id, out_role)) {
        res.status = 401; res.set_content(R"({"error":"Invalid token"})", "application/json"); return false;
    }
    return true;
}

// Modo mantenimiento: PORTAL_SWEEP_ORPHANS=dry|run  limpia y sale (sin subir el portal).
static int run_sweep_mode(const char* mode) {
    bool dry = (std::string(mode) != "run");
    std::string report;
    long long freed = sweep_abandoned(dry, report);
    std::cout << "================ BARRIDO " << (dry ? "(dry-run: no borra nada)" : "(ejecutado)") << " ================\n"
              << report
              << "total " << (freed / (1024.0 * 1024.0 * 1024.0)) << " GB" << std::endl;
    if (dry && report.empty()) std::cout << "(sin huérfanos detectados)" << std::endl;
    else if (dry) std::cout << "Repite con PORTAL_SWEEP_ORPHANS=run para borrar lo listado." << std::endl;
    return 0;
}

int main() {
    if (!init_db()) { std::cerr << "DB error\n"; return 1; }

    const char* sweep = getenv("PORTAL_SWEEP_ORPHANS");
    if (sweep && *sweep) return run_sweep_mode(sweep);

    httplib::Server svr;

    // Cualquier excepción en un handler devuelve 500 con motivo (antes: 500 vacío).
    svr.set_exception_handler([](const httplib::Request& req, httplib::Response& res, std::exception_ptr ep) {
        std::string what = "excepción no capturada";
        try { std::rethrow_exception(ep); }
        catch (const std::exception& e) { what = e.what(); }
        catch (...) {}
        std::cerr << "[EXCEPTION] " << req.method << " " << req.path << " -> " << what << std::endl;
        json_error(res, 500, "error interno del servidor");
    });

    // Techo de cuerpo de petición: un chunk de 4 MB + cabecera multipart.
    svr.set_payload_max_length(32 * 1024 * 1024);

    // ===================== LOGIN =====================
    svr.Post("/api/login", [&](const httplib::Request& req, httplib::Response& res) {
        std::string username = req.get_param_value("username");
        std::string password = req.get_param_value("password");
        std::string key = req.remote_addr + "|" + username;

        if (login_blocked(key)) {
            json_error(res, 429, "demasiados intentos fallidos; espera 15 minutos");
            std::cerr << "[LOGIN] bloqueado por intentos: " << key << std::endl;
            return;
        }

        std::string token;
        auto user_id_opt = login_user(username, password, token);
        if (user_id_opt.has_value()) {
            login_succeeded(key);
            res.set_content("{\"success\":true, \"token\":\"" + escape_json(token) + "\"}", "application/json");
        } else {
            login_failed(key);
            json_error(res, 401, "Credenciales inválidas");
        }
    });

    // ===================== UPLOAD INIT =====================
    svr.Post("/api/upload/init", [&](const httplib::Request& req, httplib::Response& res) {
        int user_id; std::string role;
        if (!check_auth(req, res, user_id, role)) return;

        std::string filename = req.get_param_value("filename");
        std::string total_str = req.get_param_value("total_chunks");
        long long total_chunks = 1;
        if (!total_str.empty() && !parse_ll(total_str, total_chunks)) {
            json_error(res, 400, "total_chunks no es un número");
            return;
        }

        cleanup_old_uploads();

        std::string err;
        std::string upload_id = init_upload(filename, total_chunks, user_id, err);
        if (upload_id.empty()) {
            json_error(res, 400, err.empty() ? "no se pudo iniciar la subida" : err);
            return;
        }
        res.set_content("{\"upload_id\":\"" + escape_json(upload_id) + "\", \"chunk_size\":4194304}", "application/json");
    });

    // ===================== UPLOAD CHUNK =====================
    svr.Post("/api/upload/chunk", [&](const httplib::Request& req, httplib::Response& res) {
        int user_id; std::string role;
        if (!check_auth(req, res, user_id, role)) return;

        std::string upload_id       = req.form.get_field("upload_id");
        std::string chunk_index_str = req.form.get_field("chunk_index");
        std::string client_md5      = req.form.get_field("chunk_md5");

        std::string err;
        if (!upload_access(upload_id, user_id, role, err)) { json_error(res, 400, err); return; }
        if (client_md5.empty()) { json_error(res, 400, "falta chunk_md5"); return; }
        if (!req.form.has_file("chunk")) { json_error(res, 400, "no se recibió el chunk"); return; }

        const std::string& content = req.form.get_file("chunk").content;

        // Integridad del transporte: el cliente manda su MD5 en la misma petición.
        std::string server_md5 = calculate_md5(content.data(), content.size());
        if (server_md5 != client_md5) {
            std::cout << "[MD5 DIFF] cliente=" << client_md5 << " servidor=" << server_md5 << std::endl;
            json_error(res, 400, "MD5 no coincide - chunk corrupto en tránsito");
            return;
        }

        long long chunk_index = -1;
        if (!parse_ll(chunk_index_str, chunk_index)) { json_error(res, 400, "chunk_index no es un número"); return; }

        // total_chunks NO se toma del cliente: save_chunk lo lee de la BD.
        if (save_chunk(upload_id, chunk_index, content.data(), content.size(), err)) {
            res.set_content("{\"status\":\"ok\", \"chunk_index\":" + std::to_string(chunk_index) + "}", "application/json");
        } else {
            json_error(res, 500, err.empty() ? "error al guardar el chunk" : err);
        }
    });

    // ===================== UPLOAD COMPLETE =====================
svr.Post("/api/upload/complete", [&](const httplib::Request& req, httplib::Response& res) {
        int user_id; std::string role;
        if (!check_auth(req, res, user_id, role)) return;

        std::string upload_id = req.get_param_value("upload_id");

        std::cout << "[COMPLETE] Recibido para upload_id = " << upload_id << std::endl;

        std::string err;

        // 1. ¿Ya se completó antes? Se comprueba ANTES de exigir la fila temporal
        //    (que /complete borra al triunfar): así el reintento del cliente es idempotente.
        std::string previo;
        long long dueno = -1;
        if (find_completed_upload(upload_id, previo, dueno)) {
            if (role != "admin" && dueno != user_id) { json_error(res, 400, "esa subida pertenece a otro usuario"); return; }
            res.set_content("{\"status\":\"ok\", \"download_token\":\"" + escape_json(previo) + "\", \"duplicado\":true}", "application/json");
            std::cout << "[COMPLETE] ya estaba completado anteriormente" << std::endl;
            return;
        }

        if (!upload_access(upload_id, user_id, role, err)) { json_error(res, 400, err); return; }

        if (upload_id.empty()) {
            res.status = 400;
            res.set_content(R"({"error":"Falta upload_id"})", "application/json");
            return;
        }
		
		{
            std::unique_lock<std::mutex> lock(completion_mutex);
            // Un solo ensamblado por upload_id, y esperando CON TOPE: el wait() sin límite
            // enredaba un hilo del servidor por cada retry del cliente.
            bool libre = completion_cv.wait_for(lock, std::chrono::seconds(300), [&]{
                return active_completions.find(upload_id) == active_completions.end();
            });
            if (!libre) {
                json_error(res, 409, "otro completo de esa subida sigue en curso");
                return;
            }
            
            // Cuando es nuestro turno, lo marcamos como activo para bloquear a otros.
            active_completions.insert(upload_id);
        }
		
		struct CompletionGuard {
            std::string id;
            ~CompletionGuard() {
                std::lock_guard<std::mutex> lock(completion_mutex);
                active_completions.erase(id);
                completion_cv.notify_all(); // Despierta a los hilos que estaban pausados
            }
        } guard{upload_id};

        // Ensamblado real: valida set exacto de chunks, tamaño, fsync y luego INSERT.
        std::string filename, download_token, md5;
        long long size = 0;
        if (complete_upload(upload_id, download_token, filename, md5, size, err)) {
            res.set_content("{\"status\":\"ok\", \"download_token\":\"" + escape_json(download_token) +
                            "\", \"filename\":\"" + escape_json(filename) +
                            "\", \"md5\":\"" + escape_json(md5) +
                            "\", \"size\":" + std::to_string(size) + "}", "application/json");
            std::cout << "[COMPLETE SUCCESS] " << filename << " | " << size << " bytes" << std::endl;
        } else {
            std::cout << "[COMPLETE FAILED] " << err << std::endl;
            json_error(res, 400, err.empty() ? "no se pudo completar el upload" : err);
        }
    });

    // ===================== ESTADO DE SUBIDA (permite reanudar) =====================
    svr.Get(R"(/api/upload/status/([0-9a-fA-F]+))", [&](const httplib::Request& req, httplib::Response& res) {
        int user_id; std::string role;
        if (!check_auth(req, res, user_id, role)) return;

        std::string upload_id = req.matches[1];
        std::string err;
        if (!upload_access(upload_id, user_id, role, err)) { json_error(res, 400, err); return; }

        long long total = 0, present = 0;
        std::vector<long long> missing;
        if (!upload_status(upload_id, total, present, missing)) { json_error(res, 404, "subida inexistente o caducada"); return; }

        std::string json = "{\"upload_id\":\"" + escape_json(upload_id) + "\",\"total\":" + std::to_string(total) +
                           ",\"present\":" + std::to_string(present) + ",\"missing\":[";
        for (size_t i = 0; i < missing.size(); ++i) { if (i) json += ","; json += std::to_string(missing[i]); }
        json += "],\"missing_truncated\":" + std::string(missing.size() >= 5000 ? "true" : "false") + "}";
        res.set_content(json, "application/json");
    });

    // ===================== LOGOUT =====================
    svr.Post("/api/logout", [&](const httplib::Request& req, httplib::Response& res) {
        int user_id; std::string role;
        if (!check_auth(req, res, user_id, role)) return;

        bool ok = delete_session(bearer_token(req));
        res.set_content(std::string("{\"success\":") + (ok ? "true" : "false") + "}", "application/json");
    });

    // ===================== LISTADO DE ARCHIVOS =====================
    svr.Get("/api/files", [&](const httplib::Request& req, httplib::Response& res) {
        int user_id; std::string role;
        if (!check_auth(req, res, user_id, role)) return;

        auto text = [](sqlite3_stmt* s, int i) -> std::string {
            const unsigned char* v = sqlite3_column_text(s, i);
            return v ? std::string((const char*)v) : std::string();
        };

        sqlite3_stmt* stmt = nullptr;
        const char* sql = "SELECT id, filename, filetype, size, token, COALESCE(md5,'') FROM files ORDER BY upload_date DESC";
        if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) { json_error(res, 500, "error interno"); return; }

        std::string json = "[";
        bool first = true;
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            if (!first) json += ",";
            std::string filetype = text(stmt, 2);
            filetype.erase(std::remove_if(filetype.begin(), filetype.end(),
                                          [](unsigned char c){ return c < 32 || c > 126; }), filetype.end());

            json += "{";
            json += "\"id\":"     + std::to_string(sqlite3_column_int(stmt, 0)) + ",";
            json += "\"filename\":\"" + escape_json(text(stmt, 1)) + "\",";
            json += "\"filetype\":\"" + escape_json(filetype) + "\",";
            json += "\"size\":"    + std::to_string(sqlite3_column_int64(stmt, 3)) + ",";
            json += "\"token\":\""  + escape_json(text(stmt, 4)) + "\",";
            json += "\"md5\":\""    + escape_json(text(stmt, 5)) + "\"";
            json += "}";
            first = false;
        }
        json += "]";
        sqlite3_finalize(stmt);
        res.set_content(json, "application/json");
    });

    // ===================== DESCARGA EN STREAMING =====================
    svr.Get("/download", [&](const httplib::Request& req, httplib::Response& res) {
        std::string id_str  = req.get_param_value("id");
        std::string token   = req.get_param_value("token");
        std::string dl_name = req.get_param_value("name");   // opcional: nombre a mostrar

        long long id_num = -1;
        if (id_str.empty() || token.empty() || !parse_ll(id_str, id_num)) {
            res.status = 400;
            res.set_content("Faltan parámetros", "text/plain");
            return;
        }

        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db, "SELECT internal_filename, filename, filetype, COALESCE(md5,'') FROM files WHERE id = ? AND token = ?",
                               -1, &stmt, nullptr) != SQLITE_OK) {
            res.status = 500; res.set_content("error interno", "text/plain"); return;
        }
        sqlite3_bind_int64(stmt, 1, id_num);
        sqlite3_bind_text(stmt, 2, token.c_str(), -1, SQLITE_STATIC);

        if (sqlite3_step(stmt) != SQLITE_ROW) {
            sqlite3_finalize(stmt);
            res.status = 404;
            res.set_content("Token inválido", "text/plain");
            return;
        }
        auto col = [&](int i) -> std::string {
            const unsigned char* v = sqlite3_column_text(stmt, i);
            return v ? std::string((const char*)v) : std::string();
        };
        std::string internal_name = col(0);
        std::string display_name  = dl_name.empty() ? col(1) : dl_name;
        std::string ext           = col(2);
        std::string md5           = col(3);
        sqlite3_finalize(stmt);

        // El nombre viene de la BD; aún así no dejamos escapar la ruta fuera de files/
        if (internal_name.find('/') != std::string::npos ||
            internal_name.find('\\') != std::string::npos ||
            internal_name.find("..") != std::string::npos) {
            res.status = 400; res.set_content("nombre interno inválido", "text/plain"); return;
        }
        std::string filepath = files_dir() + "/" + internal_name;

        std::error_code fec;
        if (!fs::is_regular_file(filepath, fec)) {
            std::cerr << "[DOWNLOAD ERROR] falta el archivo en disco: " << filepath << std::endl;
            res.status = 404;
            res.set_content("Archivo no encontrado en disco", "text/plain");
            return;
        }
        auto file_size = fs::file_size(filepath, fec);
        if (fec) { res.status = 500; res.set_content("no se pudo leer el tamaño", "text/plain"); return; }

        // El archivo se abre UNA sola vez para toda la descarga. Antes cada bloque de 64 KB
        // reabría el archivo: ~265.000 aperturas para 16 GB.
        struct Stream {
            std::ifstream in;
            std::vector<char> buf;
            long long pos = 0;
        };
        auto st = std::make_shared<Stream>();
        st->buf.resize(256 * 1024);
        st->in.open(filepath, std::ios::binary);
        if (!st->in) {
            std::cerr << "[DOWNLOAD ERROR] no se pudo abrir " << filepath << std::endl;
            res.status = 500;
            res.set_content("no se pudo abrir el archivo", "text/plain");
            return;
        }

        res.set_content_provider(
            (size_t)file_size, "application/octet-stream",
            [st](size_t offset, size_t length, httplib::DataSink& sink) -> bool {
                if ((long long)offset != st->pos) {
                    st->in.seekg((std::streamoff)offset);
                    st->pos = (long long)offset;
                }
                size_t left = length;
                while (left > 0) {
                    size_t want = std::min(left, st->buf.size());
                    st->in.read(st->buf.data(), (std::streamsize)want);
                    std::streamsize got = st->in.gcount();
                    if (got <= 0) return false;            // truncado: mejor cortar que mandar huecos
                    if (!sink.write(st->buf.data(), (size_t)got)) return false;
                    st->pos += got;
                    left -= (size_t)got;
                }
                return true;
            });

        res.set_header("Accept-Ranges", "bytes");
        if (!md5.empty()) res.set_header("X-Checksum-MD5", md5);

        // Content-Disposition saneado (RFC 6266/5987): antes el nombre del archivo
        // se inyectaba crudo en la cabecera.
        res.set_header("Content-Disposition", content_disposition(display_name.empty() ? internal_name : display_name));
    });

    // ===================== BORRAR ARCHIVO (SOLO ADMIN) =====================
    svr.Delete(R"(/api/files/(\d+))", [&](const httplib::Request& req, httplib::Response& res) {
        int user_id; std::string role;
        if (!check_auth(req, res, user_id, role)) return;

        if (role != "admin") { json_error(res, 403, "Solo el administrador puede borrar archivos"); return; }

        long long file_id = 0;
        parse_ll(req.matches[1], file_id);          // el regex de la ruta ya garantiza dígitos
        std::cout << "[DELETE] ID " << file_id << std::endl;

        std::string internal_name;
        bool row_exists = false;
        {
            sqlite3_stmt* stmt = nullptr;
            if (sqlite3_prepare_v2(db, "SELECT internal_filename FROM files WHERE id = ?", -1, &stmt, nullptr) == SQLITE_OK) {
                sqlite3_bind_int64(stmt, 1, file_id);
                if (sqlite3_step(stmt) == SQLITE_ROW) {
                    row_exists = true;
                    const unsigned char* v = sqlite3_column_text(stmt, 0);
                    if (v) internal_name = (const char*)v;
                }
                sqlite3_finalize(stmt);
            }
        }
        if (!row_exists) { json_error(res, 404, "ese archivo no existe en el registro"); return; }

        std::error_code ec;
        bool file_gone = true;
        if (!internal_name.empty() &&
            internal_name.find('/') == std::string::npos && internal_name.find("..") == std::string::npos) {
            std::string path = files_dir() + "/" + internal_name;
            if (fs::exists(path, ec)) {
                fs::remove(path, ec);
                file_gone = !ec;
                if (ec) std::cerr << "[DELETE WARN] no se pudo borrar " << path << ": " << ec.message() << std::endl;
            }
        }

        // restos de la subida, si los hubiera (antes quedaban colgados en tmp/uploads)
        {
            sqlite3_stmt* q = nullptr;
            if (sqlite3_prepare_v2(db, "SELECT upload_temp_id FROM files WHERE id = ?", -1, &q, nullptr) == SQLITE_OK) {
                sqlite3_bind_int64(q, 1, file_id);
                if (sqlite3_step(q) == SQLITE_ROW) {
                    const unsigned char* v = sqlite3_column_text(q, 0);
                    if (v) {
                        std::string uid = (const char*)v;
                        if (is_safe_upload_id(uid)) fs::remove_all(uploads_tmp_dir() + "/" + uid, ec);
                    }
                }
                sqlite3_finalize(q);
            }
            sqlite3_stmt* d = nullptr;
            if (sqlite3_prepare_v2(db, "DELETE FROM temp_uploads WHERE upload_id IN "
                                       "(SELECT upload_temp_id FROM files WHERE id = ?)", -1, &d, nullptr) == SQLITE_OK) {
                sqlite3_bind_int64(d, 1, file_id);
                sqlite3_step(d);
                sqlite3_finalize(d);
            }
        }

        bool db_ok = false;
        {
            sqlite3_stmt* del = nullptr;
            if (sqlite3_prepare_v2(db, "DELETE FROM files WHERE id = ?", -1, &del, nullptr) == SQLITE_OK) {
                sqlite3_bind_int64(del, 1, file_id);
                db_ok = (sqlite3_step(del) == SQLITE_DONE);
                sqlite3_finalize(del);
            }
        }
        if (!db_ok)     { json_error(res, 500, "no se pudo borrar el registro"); return; }
        if (!file_gone) { json_error(res, 500, "registro borrado pero el archivo sigue en disco"); return; }

        res.set_content(R"({"success":true})", "application/json");
    });

    // ===================== GESTIÓN DE USUARIOS (SOLO ADMIN) =====================
    svr.Get("/api/users", [&](const httplib::Request& req, httplib::Response& res) {
        int user_id; std::string role;
        if (!check_auth(req, res, user_id, role)) return;
        if (role != "admin") { json_error(res, 403, "Acceso denegado"); return; }

        auto text = [](sqlite3_stmt* s, int i) -> std::string {
            const unsigned char* v = sqlite3_column_text(s, i);
            return v ? std::string((const char*)v) : std::string();
        };

        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db, "SELECT id, username, role, last_password_change FROM users ORDER BY username", -1, &stmt, nullptr) != SQLITE_OK) {
            json_error(res, 500, "error interno"); return;
        }

        std::string json = "[";
        bool first = true;
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            if (!first) json += ",";
            json += "{";
            json += "\"id\":" + std::to_string(sqlite3_column_int(stmt, 0)) + ",";
            json += "\"username\":\"" + escape_json(text(stmt, 1)) + "\",";
            json += "\"role\":\""      + escape_json(text(stmt, 2)) + "\",";
            json += "\"last_change\":\"" + escape_json(text(stmt, 3)) + "\"";
            json += "}";
            first = false;
        }
        json += "]";
        sqlite3_finalize(stmt);
        res.set_content(json, "application/json");
    });

    svr.Post("/api/users", [&](const httplib::Request& req, httplib::Response& res) {
        int user_id; std::string role;
        if (!check_auth(req, res, user_id, role)) return;
        if (role != "admin") { json_error(res, 403, "Acceso denegado"); return; }

        std::string username = req.get_param_value("username");
        std::string password = req.get_param_value("password");
        std::string new_role = req.get_param_value("role");

        if (create_user(username, password, new_role)) {
            res.set_content(R"({"success":true, "message":"Usuario creado correctamente"})", "application/json");
        } else {
            json_error(res, 400, "no se pudo crear el usuario (nombre en uso, o usuario/contraseña inválidos)");
        }
    });

    // ===================== RESET PASSWORD (VERSIÓN MEJORADA) =====================
    svr.Post(R"(/api/users/(\d+)/reset-password)", [&](const httplib::Request& req, httplib::Response& res) {
        int caller_id; std::string role;
        if (!check_auth(req, res, caller_id, role)) return;

        if (role != "admin") {
            res.status = 403;
            res.set_content(R"({"error":"Solo el administrador puede resetear contraseñas"})", "application/json");
            return;
        }

        int target_id = std::stoi(req.matches[1]);
        std::cout << "[RESET] Intentando resetear contraseña del usuario ID: " << target_id << std::endl;

        // Generar contraseña temporal
        std::string new_password = generate_secure_token().substr(0, 12);
        std::string hash = bcrypt_hash(new_password);

        // Verificar que el usuario exista
        sqlite3_stmt* check = nullptr;
        sqlite3_prepare_v2(db, "SELECT id FROM users WHERE id = ?", -1, &check, nullptr);
        sqlite3_bind_int(check, 1, target_id);
        if (sqlite3_step(check) != SQLITE_ROW) {
            sqlite3_finalize(check);
            res.status = 404;
            res.set_content(R"({"error":"El usuario no existe"})", "application/json");
            std::cout << "[RESET] ERROR: Usuario ID " << target_id << " no encontrado" << std::endl;
            return;
        }
        sqlite3_finalize(check);

        // Actualizar contraseña
        sqlite3_stmt* stmt = nullptr;
        const char* sql = "UPDATE users SET password_hash = ?, last_password_change = CURRENT_TIMESTAMP WHERE id = ?";
        sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr);
        sqlite3_bind_text(stmt, 1, hash.c_str(), -1, SQLITE_STATIC);
        sqlite3_bind_int(stmt, 2, target_id);

        int result = sqlite3_step(stmt);
        sqlite3_finalize(stmt);

        if (result == SQLITE_DONE) {
            // Un token robado no puede sobrevivir al reset: se cierran las sesiones del usuario.
            invalidate_sessions(target_id, "");
            std::cout << "[RESET SUCCESS] Usuario " << target_id << " - Nueva contraseña: " << new_password << std::endl;
            res.set_content("{\"success\":true, \"new_password\":\"" + escape_json(new_password) + "\"}", "application/json");
        } else {
            std::cout << "[RESET ERROR] Falló el UPDATE - result: " << result << std::endl;
            json_error(res, 500, "No se pudo resetear la contraseña");
        }
    });

    // ===================== CAMBIO DE CONTRASEÑA (cualquier usuario logueado) =====================
    svr.Post("/api/change-password", [&](const httplib::Request& req, httplib::Response& res) {
        int user_id; std::string role;
        if (!check_auth(req, res, user_id, role)) return;

        std::string old_password = req.get_param_value("old_password");
        std::string new_password = req.get_param_value("new_password");

        // keep_token: esta sesión sigue viva; las demás (incluida una robada) se cierran.
        if (change_password(user_id, old_password, new_password, bearer_token(req))) {
            res.set_content(R"({"success":true, "message":"Contraseña cambiada correctamente"})", "application/json");
        } else {
            json_error(res, 400, "La contraseña actual es incorrecta o la nueva es inválida (mínimo 8 caracteres)");
        }
    });

    // ===================== RUTAS ANTERIORES =====================
    svr.Get("/api/status", [&](const httplib::Request& req, httplib::Response& res) {
        int user_id; std::string role;
        if (!check_auth(req, res, user_id, role)) return;

        // Limpieza periódica: antes solo se hacía al arrancar, así que las sesiones
        // muertas y las subidas caducadas se acumularan para siempre.
        std::lock_guard<std::mutex> lk(cleanup_mu);
        auto now = std::chrono::steady_clock::now();
        if (now - last_cleanup > std::chrono::minutes(10)) {
            last_cleanup = now;
            cleanup_expired_tokens();
            cleanup_old_uploads();
        }

        res.set_content("{\"status\":\"ok\", \"free_space\":\"" + escape_json(get_free_space()) + "\"}", "application/json");
    });

    svr.Get("/api/me", [&](const httplib::Request& req, httplib::Response& res) {
        int user_id; std::string role;
        if (!check_auth(req, res, user_id, role)) return;
        res.set_content("{\"message\":\"Autenticado correctamente\", \"role\":\"" + escape_json(role) + "\"}", "application/json");
    });

    svr.set_mount_point("/", docroot_dir());
    svr.Get("/", [](const httplib::Request&, httplib::Response& res) {
        res.set_redirect("/index.html", 302);
    });

    int port = 80;
    if (const char* p = getenv("PORTAL_PORT")) { long long v = 0; if (parse_ll(p, v) && v > 0 && v < 65536) port = (int)v; }
    std::cout << "🚀 Iniciando servidor en puerto " << port << "...\n";

    // 1. Bindear el socket mientras aún somos root
    if (!svr.bind_to_port("0.0.0.0", port)) {
        std::cerr << "❌ ERROR: No se pudo bindear el puerto " << port << "." << std::endl;
        std::cerr << "   Asegúrate de ejecutar con sudo." << std::endl;
        return 1;
    }

    // 2. Dropear privilegios (cambiar a usuario tc); PORTAL_UID/PORTAL_GID para pruebas
    if (geteuid() == 0) {
        long long gid = 50, uid = 1001;
        if (const char* g = getenv("PORTAL_GID")) parse_ll(g, gid);
        if (const char* u = getenv("PORTAL_UID")) parse_ll(u, uid);
        if (setgid((gid_t)gid) != 0 || setuid((uid_t)uid) != 0) {
            std::cerr << "❌ No se pudo dropear privilegios" << std::endl;
        } else {
            std::cout << "✅ Privilegios dropeados correctamente (ahora corre como uid=" << uid << ")" << std::endl;
        }
    }

    // 3. Iniciar el loop de escucha
    std::cout << "🚀 Servidor escuchando en http://0.0.0.0:" << port << std::endl;
    svr.listen_after_bind();
}
