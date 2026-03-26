#define CPPHTTPLIB_FORM_DATA_SUPPORT
#include "httplib.h"
#include "db.h"
#include "helpers.h"
#include "upload.h"
#include "auth.h"
#include <iostream>
#include <iostream>
#include <sqlite3.h>
#include <filesystem>
#include <mutex>
#include <condition_variable>
#include <unordered_set>

extern sqlite3* db;

std::mutex completion_mutex;
std::condition_variable completion_cv;
std::unordered_set<std::string> active_completions;

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

    // ===================== UPLOAD CHUNK CON MD5 (CORREGIDO PARA httplib 0.38.0) =====================
    svr.Post("/api/upload/chunk", [&](const httplib::Request& req, httplib::Response& res) {
        int user_id; std::string role;
        if (!check_auth(req, res, user_id, role)) return;

        // Leer campos de texto usando la API correcta de httplib 0.38.0
        std::string upload_id   = req.form.get_field("upload_id");
        std::string chunk_index_str = req.form.get_field("chunk_index");
        std::string total_chunks_str = req.form.get_field("total_chunks");
        std::string client_md5  = req.form.get_field("chunk_md5");

        std::cout << "[CHUNK DEBUG] upload_id='" << upload_id << "'"
                  << " | chunk_index='" << chunk_index_str << "'"
                  << " | total_chunks='" << total_chunks_str << "'"
                  << " | md5='" << client_md5 << "'" << std::endl;

        if (upload_id.empty() || client_md5.empty()) {
            res.status = 400;
            res.set_content(R"({"error":"Faltan datos del chunk o MD5"})", "application/json");
            std::cout << "[CHUNK ERROR] Faltan parámetros obligatorios" << std::endl;
            return;
        }

        if (!req.form.has_file("chunk")) {
            res.status = 400;
            res.set_content(R"({"error":"No se recibió el chunk"})", "application/json");
            std::cout << "[CHUNK ERROR] No se recibió el archivo 'chunk'" << std::endl;
            return;
        }

        const auto& file = req.form.get_file("chunk");
        std::string received_data(file.content.data(), file.content.size());

        std::cout << "[CHUNK] Tamaño recibido: " << received_data.size() << " bytes" << std::endl;

        std::string server_md5 = calculate_md5(received_data.data(), received_data.size());

        std::cout << "[MD5] Cliente: " << client_md5 << " | Servidor: " << server_md5 << std::endl;

        if (server_md5 != client_md5) {
            res.status = 400;
            res.set_content(R"({"error":"MD5 no coincide - chunk corrupto"})", "application/json");
            return;
        }

        int chunk_index = std::stoi(chunk_index_str);
        int total_chunks = std::stoi(total_chunks_str);

        if (save_chunk(upload_id, chunk_index, total_chunks, received_data)) {
            res.set_content(R"({"status":"ok", "chunk_index":)" + std::to_string(chunk_index) + R"(})", "application/json");
            std::cout << "[CHUNK OK] Chunk " << chunk_index << " guardado correctamente" << std::endl;
        } else {
            res.status = 500;
            res.set_content(R"({"error":"Error al guardar el chunk en disco"})", "application/json");
            std::cout << "[CHUNK ERROR 500] save_chunk() devolvió false" << std::endl;
        }
    });

    // ===================== UPLOAD COMPLETE =====================
svr.Post("/api/upload/complete", [&](const httplib::Request& req, httplib::Response& res) {
        int user_id; std::string role;
        if (!check_auth(req, res, user_id, role)) return;

        std::string upload_id = req.get_param_value("upload_id");

        std::cout << "[COMPLETE] Recibido para upload_id = " << upload_id << std::endl;

        if (upload_id.empty()) {
            res.status = 400;
            res.set_content(R"({"error":"Falta upload_id"})", "application/json");
            return;
        }
		
		{
            std::unique_lock<std::mutex> lock(completion_mutex);
            // Si el upload_id ya está en la lista de "activos", el hilo se pausa aquí y espera.
            completion_cv.wait(lock, [&]{
                return active_completions.find(upload_id) == active_completions.end();
            });
            
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

        // 1. Verificar si ya fue completado antes (evita duplicados)
        sqlite3_stmt* check = nullptr;
        sqlite3_prepare_v2(db, "SELECT id, token FROM files WHERE upload_temp_id = ?", -1, &check, nullptr);
        sqlite3_bind_text(check, 1, upload_id.c_str(), -1, SQLITE_STATIC);

        if (sqlite3_step(check) == SQLITE_ROW) {
            // Ya existía → devolver el token anterior
            std::string existing_token = (const char*)sqlite3_column_text(check, 1);
            sqlite3_finalize(check);

            res.set_content("{\"status\":\"ok\", \"download_token\":\"" + existing_token + "\"}", "application/json");
            std::cout << "[COMPLETE] Ya estaba completado anteriormente" << std::endl;
            return;
        }
        sqlite3_finalize(check);

        // 2. No existía → completarlo normalmente
        std::string filename, download_token;
        if (complete_upload(upload_id, filename, download_token)) {
            res.set_content("{\"status\":\"ok\", \"download_token\":\"" + download_token + "\", \"filename\":\"" + filename + "\"}", "application/json");
            std::cout << "[COMPLETE SUCCESS] Archivo finalizado: " << filename << std::endl;
        } else {
            res.status = 500;
            res.set_content(R"({"error":"No se pudo completar el upload"})", "application/json");
            std::cout << "[COMPLETE FAILED]" << std::endl;
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
			
			std::string raw_filename = (const char*)sqlite3_column_text(stmt, 1);
			std::string safe_json_filename = escape_json(raw_filename); // <--- Escapamos para el JSON

            json += "{";
            json += "\"id\":" + std::to_string(sqlite3_column_int(stmt, 0)) + ",";
            //json += "\"filename\":\"" + std::string((const char*)sqlite3_column_text(stmt, 1)) + "\",";
			json += "\"filename\":\"" + safe_json_filename + "\",";
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
        //sqlite3_prepare_v2(db, "SELECT filename FROM files WHERE id = ? AND token = ?", -1, &stmt, nullptr);
		sqlite3_prepare_v2(db, "SELECT internal_filename, filename FROM files WHERE id = ? AND token = ?", -1, &stmt, nullptr);
        sqlite3_bind_int(stmt, 1, std::stoi(id_str));
        sqlite3_bind_text(stmt, 2, token.c_str(), -1, SQLITE_STATIC);

        if (sqlite3_step(stmt) == SQLITE_ROW) {
			std::string internal_name = (const char*)sqlite3_column_text(stmt, 0);
			std::string display_name  = (const char*)sqlite3_column_text(stmt, 1);
			std::string filepath = "/mnt/xvdb1/files/" + internal_name;
            //std::string filename = (const char*)sqlite3_column_text(stmt, 0);
            //std::string filepath = "/mnt/xvdb1/files/" + filename;

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

                //res.set_header("Content-Disposition", "attachment; filename=\"" + filename + "\"");
				res.set_header("Content-Disposition", "attachment; filename=\"" + display_name + "\"");
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
		std::cout << "[DELETE] Inicio de tratar de borrar ID: " << file_id << std::endl;

        // Obtener nombre del archivo
        sqlite3_stmt* stmt = nullptr;
        sqlite3_prepare_v2(db, "SELECT internal_filename FROM files WHERE id = ?", -1, &stmt, nullptr);
        sqlite3_bind_int(stmt, 1, file_id);
        //std::string filename;
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            std::string internal_name = (const char*)sqlite3_column_text(stmt, 0);
			std::string path = "/mnt/xvdb1/files/" + internal_name;
			if (std::filesystem::exists(path)) {
				std::filesystem::remove(path);
				std::cout << "[DELETE] Borrado archivo: " << internal_name << std::endl;
			} else {
				std::cout << "[DELETE] No se encuentra el archivo: " << internal_name << std::endl;
				res.status = 404;
				res.set_content(R"({"error":"Archivo no encontrado"})", "application/json");
				return;
			}
        }
        sqlite3_finalize(stmt);
		
		/*
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
		*/

        // Borrar de la BD
        sqlite3_exec(db, ("DELETE FROM files WHERE id = " + std::to_string(file_id)).c_str(), nullptr, nullptr, nullptr);

        //std::cout << "[DELETE] Archivo borrado por admin - ID: " << file_id << " | Archivo: " << internal_name << std::endl;
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
