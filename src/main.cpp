#include "httplib.h"
#include "db.h"
#include "helpers.h"
#include "upload.h"
#include "auth.h"
#include <iostream>
#include <iostream>
#include <sqlite3.h>
#include <filesystem>

extern sqlite3* db;

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

int main() {
    if (!init_db()) { std::cerr << "DB error\n"; return 1; }

    httplib::Server svr;

    // ===================== LOGIN =====================
    svr.Post("/api/login", [&](const httplib::Request& req, httplib::Response& res) {
        std::string username = req.get_param_value("username");
        std::string password = req.get_param_value("password");
        std::string token;
        auto user_id_opt = login_user(username, password, token);
        if (user_id_opt.has_value()) {
            res.set_content("{\"success\":true, \"token\":\"" + token + "\"}", "application/json");
        } else {
            res.status = 401; res.set_content(R"({"error":"Credenciales inválidas"})", "application/json");
        }
    });

    // ===================== UPLOAD INIT (CORREGIDO) =====================
    svr.Post("/api/upload/init", [&](const httplib::Request& req, httplib::Response& res) {
        int user_id; std::string role;
        if (!check_auth(req, res, user_id, role)) return;

        std::string filename = req.get_param_value("filename");
        std::string total_str = req.get_param_value("total_chunks");
        int total_chunks = total_str.empty() ? 1 : std::stoi(total_str);

        std::cout << "[DEBUG INIT] filename='" << filename << "' | total_chunks=" << total_chunks << std::endl;

        cleanup_old_uploads();
        std::string upload_id = init_upload(filename, total_chunks, user_id);
        res.set_content("{\"upload_id\":\"" + upload_id + "\", \"chunk_size\":4194304}", "application/json");
    });

    // ===================== UPLOAD CHUNK (ya estaba bien) =====================
    svr.Post("/api/upload/chunk", [&](const httplib::Request& req, httplib::Response& res) {
        int user_id; std::string role;
        if (!check_auth(req, res, user_id, role)) return;

        std::string upload_id = req.form.get_field("upload_id");
        std::string idx_str = req.form.get_field("chunk_index");
        std::string total_str = req.form.get_field("total_chunks");
        int chunk_index = idx_str.empty() ? 0 : std::stoi(idx_str);
        int total_chunks = total_str.empty() ? 1 : std::stoi(total_str);

        std::cout << "[DEBUG CHUNK] upload_id recibido = '" << upload_id << "'" << std::endl;

        if (req.form.has_file("chunk")) {
            const auto& file = req.form.get_file("chunk");
            std::string data(file.content.data(), file.content.size());
            if (save_chunk(upload_id, chunk_index, total_chunks, data)) {
                res.set_content("{\"status\":\"ok\"}", "application/json");
                return;
            }
        }
        res.status = 400;
        res.set_content("{\"error\":\"Error al guardar chunk\"}", "application/json");
    });

    // ===================== UPLOAD COMPLETE (CORREGIDO) =====================
    svr.Post("/api/upload/complete", [&](const httplib::Request& req, httplib::Response& res) {
        int user_id; std::string role;
        if (!check_auth(req, res, user_id, role)) return;

        std::string upload_id = req.get_param_value("upload_id");
        std::cout << "[DEBUG COMPLETE] upload_id recibido = '" << upload_id << "'" << std::endl;

        std::string token, filename;
        if (complete_upload(upload_id, token, filename)) {
            res.set_content("{\"success\":true, \"download_token\":\"" + token + "\", \"filename\":\"" + filename + "\"}", "application/json");
        } else {
            res.status = 400;
            res.set_content("{\"error\":\"Upload incompleto\"}", "application/json");
        }
    });

    // ===================== LISTADO DE ARCHIVOS (CORREGIDO) =====================
    svr.Get("/api/files", [&](const httplib::Request& req, httplib::Response& res) {
        int user_id; std::string role;
        if (!check_auth(req, res, user_id, role)) return;

        sqlite3_stmt* stmt = nullptr;
        const char* sql = "SELECT id, filename, filetype, size, token FROM files ORDER BY upload_date DESC";
        sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr);

        std::string json = "[";
        bool first = true;
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            if (!first) json += ",";
            std::string filetype = (const char*)sqlite3_column_text(stmt, 2);
            // Limpiar caracteres de control inválidos
            filetype.erase(std::remove_if(filetype.begin(), filetype.end(), [](unsigned char c){ return c < 32; }), filetype.end());

            json += "{";
            json += "\"id\":" + std::to_string(sqlite3_column_int(stmt, 0)) + ",";
            json += "\"filename\":\"" + std::string((const char*)sqlite3_column_text(stmt, 1)) + "\",";
            json += "\"filetype\":\"" + filetype + "\",";
            json += "\"size\":" + std::to_string(sqlite3_column_int64(stmt, 3)) + ",";
            json += "\"token\":\"" + std::string((const char*)sqlite3_column_text(stmt, 4)) + "\"";
            json += "}";
            first = false;
        }
        json += "]";
        sqlite3_finalize(stmt);
        res.set_content(json, "application/json");
    });

    // ===================== DESCARGA EN STREAMING (SEGURA - sin segfault) =====================
    svr.Get("/download", [&](const httplib::Request& req, httplib::Response& res) {
        std::string id_str = req.get_param_value("id");
        std::string token = req.get_param_value("token");

        if (id_str.empty() || token.empty()) {
            res.status = 400;
            res.set_content("Faltan parámetros", "text/plain");
            return;
        }

        sqlite3_stmt* stmt = nullptr;
        sqlite3_prepare_v2(db, "SELECT filename FROM files WHERE id = ? AND token = ?", -1, &stmt, nullptr);
        sqlite3_bind_int(stmt, 1, std::stoi(id_str));
        sqlite3_bind_text(stmt, 2, token.c_str(), -1, SQLITE_STATIC);

        if (sqlite3_step(stmt) == SQLITE_ROW) {
            std::string filename = (const char*)sqlite3_column_text(stmt, 0);
            std::string filepath = "/mnt/xvdb1/files/" + filename;

            if (std::filesystem::exists(filepath)) {
                auto file_size = std::filesystem::file_size(filepath);

                res.set_content_provider(
                    file_size,
                    "application/octet-stream",
                    [filepath](size_t offset, size_t length, httplib::DataSink& sink) {
                        std::ifstream file(filepath, std::ios::binary);
                        if (!file) return false;

                        file.seekg(offset);
                        char buffer[65536];
                        size_t to_read = std::min(length, sizeof(buffer));
                        file.read(buffer, to_read);
                        size_t read_bytes = file.gcount();

                        if (read_bytes > 0) {
                            sink.write(buffer, read_bytes);
                        }
                        if (read_bytes < to_read) {
                            sink.done();
                        }
                        return true;
                    }
                );

                res.set_header("Content-Disposition", "attachment; filename=\"" + filename + "\"");
            } else {
                res.status = 404;
                res.set_content("Archivo no encontrado", "text/plain");
            }
        } else {
            res.status = 404;
            res.set_content("Token inválido", "text/plain");
        }
        sqlite3_finalize(stmt);
    });

    // ===================== BORRAR ARCHIVO (SOLO ADMIN) =====================
    svr.Delete(R"(/api/files/(\d+))", [&](const httplib::Request& req, httplib::Response& res) {
        int user_id; std::string role;
        if (!check_auth(req, res, user_id, role)) return;

        if (role != "admin") {
            res.status = 403;
            res.set_content(R"({"error":"Solo el administrador puede borrar archivos"})", "application/json");
            return;
        }

        int file_id = std::stoi(req.matches[1]);

        // Obtener nombre del archivo
        sqlite3_stmt* stmt = nullptr;
        sqlite3_prepare_v2(db, "SELECT filename FROM files WHERE id = ?", -1, &stmt, nullptr);
        sqlite3_bind_int(stmt, 1, file_id);
        std::string filename;
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            filename = (const char*)sqlite3_column_text(stmt, 0);
        }
        sqlite3_finalize(stmt);

        if (filename.empty()) {
            res.status = 404;
            res.set_content(R"({"error":"Archivo no encontrado"})", "application/json");
            return;
        }

        // Borrar del disco
        std::string path = "/mnt/xvdb1/files/" + filename;
        if (std::filesystem::exists(path)) {
            std::filesystem::remove(path);
        }

        // Borrar de la BD
        sqlite3_exec(db, ("DELETE FROM files WHERE id = " + std::to_string(file_id)).c_str(), nullptr, nullptr, nullptr);

        std::cout << "[DELETE] Archivo borrado por admin - ID: " << file_id << " | Archivo: " << filename << std::endl;
        res.set_content(R"({"success":true})", "application/json");
    });

    // ===================== GESTIÓN DE USUARIOS (SOLO ADMIN) =====================
    svr.Get("/api/users", [&](const httplib::Request& req, httplib::Response& res) {
        int user_id; std::string role;
        if (!check_auth(req, res, user_id, role)) return;
        if (role != "admin") { res.status = 403; res.set_content(R"({"error":"Acceso denegado"})", "application/json"); return; }

        sqlite3_stmt* stmt = nullptr;
        sqlite3_prepare_v2(db, "SELECT id, username, role, last_password_change FROM users ORDER BY username", -1, &stmt, nullptr);

        std::string json = "[";
        bool first = true;
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            if (!first) json += ",";
            json += "{";
            json += "\"id\":" + std::to_string(sqlite3_column_int(stmt, 0)) + ",";
            json += "\"username\":\"" + std::string((const char*)sqlite3_column_text(stmt, 1)) + "\",";
            json += "\"role\":\"" + std::string((const char*)sqlite3_column_text(stmt, 2)) + "\",";
            json += "\"last_change\":\"" + std::string((const char*)sqlite3_column_text(stmt, 3)) + "\"";
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
        if (role != "admin") { res.status = 403; res.set_content(R"({"error":"Acceso denegado"})", "application/json"); return; }

        std::string username = req.get_param_value("username");
        std::string password = req.get_param_value("password");
        std::string new_role = req.get_param_value("role");

        if (create_user(username, password, new_role)) {
            res.set_content(R"({"success":true, "message":"Usuario creado correctamente"})", "application/json");
        } else {
            res.status = 400;
            res.set_content(R"({"error":"No se pudo crear el usuario"})", "application/json");
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
            std::cout << "[RESET SUCCESS] Usuario " << target_id << " - Nueva contraseña: " << new_password << std::endl;
            res.set_content("{\"success\":true, \"new_password\":\"" + new_password + "\"}", "application/json");
        } else {
            std::cout << "[RESET ERROR] Falló el UPDATE - result: " << result << std::endl;
            res.status = 500;
            res.set_content(R"({"error":"No se pudo resetear la contraseña"})", "application/json");
        }
    });

    // ===================== CAMBIO DE CONTRASEÑA (cualquier usuario logueado) =====================
    svr.Post("/api/change-password", [&](const httplib::Request& req, httplib::Response& res) {
        int user_id; std::string role;
        if (!check_auth(req, res, user_id, role)) return;

        std::string old_password = req.get_param_value("old_password");
        std::string new_password = req.get_param_value("new_password");

        if (change_password(user_id, old_password, new_password)) {
            res.set_content(R"({"success":true, "message":"Contraseña cambiada correctamente"})", "application/json");
        } else {
            res.status = 400;
            res.set_content(R"({"error":"La contraseña actual es incorrecta o la nueva es inválida"})", "application/json");
        }
    });

    // ===================== RUTAS ANTERIORES =====================
    svr.Get("/api/status", [&](const httplib::Request& req, httplib::Response& res) {
        int user_id; std::string role;
        if (!check_auth(req, res, user_id, role)) return;
        res.set_content("{\"status\":\"ok\", \"free_space\":\"" + get_free_space() + "\"}", "application/json");
    });

    svr.Get("/api/me", [&](const httplib::Request& req, httplib::Response& res) {
        int user_id; std::string role;
        if (!check_auth(req, res, user_id, role)) return;
        //res.set_content(R"({"message":"Autenticado correctamente"})", "application/json");
        //std::string json = R"({\"message\":\"Autenticado correctamente\", \"role\":\")" + role + R"(\"})";
	std::string json = R"({"message":"Autenticado correctamente", "role":")" + role + R"("})";
        res.set_content(json, "application/json");
    });

    svr.set_mount_point("/", "/mnt/xvdb1/app/public");
    svr.Get("/", [](const httplib::Request&, httplib::Response& res) {
        res.set_redirect("/index.html", 302);
    });

    //std::cout << "🚀 Servidor corriendo en http://0.0.0.0:80\n";
    //svr.listen("0.0.0.0", 80);
    std::cout << "🚀 Iniciando servidor en puerto 80...\n";

    // 1. Bindear el socket mientras aún somos root
    if (!svr.bind_to_port("0.0.0.0", 80)) {
        std::cerr << "❌ ERROR: No se pudo bindear el puerto 80." << std::endl;
        std::cerr << "   Asegúrate de ejecutar con sudo." << std::endl;
        return 1;
    }

    // 2. Dropear privilegios (cambiar a usuario tc)
    if (geteuid() == 0) {
        if (setgid(50) != 0 || setuid(1001) != 0) {
            std::cerr << "❌ No se pudo dropear privilegios" << std::endl;
        } else {
            std::cout << "✅ Privilegios dropeados correctamente (ahora corre como tc)" << std::endl;
        }
    }

    // 3. Iniciar el loop de escucha
    std::cout << "🚀 Servidor escuchando en http://0.0.0.0:80" << std::endl;
    svr.listen_after_bind();
}
