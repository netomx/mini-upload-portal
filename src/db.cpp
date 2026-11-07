#include "db.h"
#include "auth.h"
#include "helpers.h"
#include <sqlite3.h>
#include <iostream>
#include <cctype>

sqlite3* db = nullptr;   // <-- QUITAMOS "static" para que upload.cpp lo vea

static bool column_exists(const char* table, const char* column) {
    std::string sql = std::string("SELECT COUNT(*) FROM pragma_table_info('") + table +
                      "') WHERE name='" + column + "'";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) return false;
    bool found = (sqlite3_step(stmt) == SQLITE_ROW && sqlite3_column_int(stmt, 0) > 0);
    sqlite3_finalize(stmt);
    return found;
}

// Migraciones aditivas y idempotentes: el binario nuevo arranca sobre la BD vieja.
static void migrate_db() {
    char* err = nullptr;
    if (!column_exists("files", "md5")) {
        if (sqlite3_exec(db, "ALTER TABLE files ADD COLUMN md5 TEXT", nullptr, nullptr, &err) != SQLITE_OK) {
            std::cerr << "[MIGRACION ERROR] files.md5: " << (err ? err : "?") << std::endl;
            sqlite3_free(err);
        } else {
            std::cout << "[MIGRACION] columna files.md5 añadida" << std::endl;
        }
    }
    // speeden el control de duplicados de complete_upload y el barrido de sesiones
    sqlite3_exec(db, "CREATE INDEX IF NOT EXISTS idx_files_upload_temp ON files(upload_temp_id)", nullptr, nullptr, nullptr);
    sqlite3_exec(db, "CREATE INDEX IF NOT EXISTS idx_sessions_user ON sessions(user_id)", nullptr, nullptr, nullptr);
}

bool init_db() {
    if (sqlite3_open(db_path().c_str(), &db) != SQLITE_OK) {
        std::cerr << "Error SQLite: " << sqlite3_errmsg(db) << std::endl;
        return false;
    }
    // Menos fsync por escritura y tolerancia a dos hilos escribiendo a la vez.
    sqlite3_exec(db, "PRAGMA journal_mode=WAL", nullptr, nullptr, nullptr);
    sqlite3_exec(db, "PRAGMA synchronous=NORMAL", nullptr, nullptr, nullptr);
    sqlite3_exec(db, "PRAGMA busy_timeout=5000", nullptr, nullptr, nullptr);

    migrate_db();
    cleanup_expired_tokens();
    return true;
}

// ... (el resto del archivo db.cpp queda exactamente igual que antes)
std::optional<int> login_user(const std::string& username, const std::string& password, std::string& out_token) {
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "SELECT id, password_hash FROM users WHERE username = ?";

    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) return std::nullopt;
    sqlite3_bind_text(stmt, 1, username.c_str(), -1, SQLITE_STATIC);

    if (sqlite3_step(stmt) == SQLITE_ROW) {
        int user_id = sqlite3_column_int(stmt, 0);
        std::string hash = (const char*)sqlite3_column_text(stmt, 1);

        if (bcrypt_verify(password, hash)) {
            out_token = generate_secure_token();

            const char* insert_sql = R"(
                INSERT INTO sessions (user_id, token, expires, last_used)
                VALUES (?, ?, datetime('now', '+30 days'), CURRENT_TIMESTAMP)
            )";
            sqlite3_stmt* ins_stmt = nullptr;
            sqlite3_prepare_v2(db, insert_sql, -1, &ins_stmt, nullptr);
            sqlite3_bind_int(ins_stmt, 1, user_id);
            sqlite3_bind_text(ins_stmt, 2, out_token.c_str(), -1, SQLITE_STATIC);
            sqlite3_step(ins_stmt);
            sqlite3_finalize(ins_stmt);

            sqlite3_finalize(stmt);
            return user_id;
        }
    }
    sqlite3_finalize(stmt);
    return std::nullopt;
}

bool validate_token(const std::string& token, int& out_user_id, std::string& out_role) {
    sqlite3_stmt* stmt = nullptr;
    const char* sql = R"(
        SELECT u.id, u.role, s.last_used
        FROM sessions s 
        JOIN users u ON s.user_id = u.id 
        WHERE s.token = ? 
          AND s.expires > datetime('now')
          AND s.last_used > datetime('now', '-7 days')
    )";

    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_text(stmt, 1, token.c_str(), -1, SQLITE_STATIC);

    if (sqlite3_step(stmt) == SQLITE_ROW) {
        out_user_id = sqlite3_column_int(stmt, 0);
        out_role = (const char*)sqlite3_column_text(stmt, 1);

        // Solo tocamos last_used si ya tiene más de una hora: así el auto-refresh
        // de cada 8s no convierte cada petición en una escritura en disco.
        const char* update_sql = "UPDATE sessions SET last_used = CURRENT_TIMESTAMP "
                                 "WHERE token = ? AND last_used < datetime('now', '-1 hour')";
        sqlite3_stmt* up_stmt = nullptr;
        sqlite3_prepare_v2(db, update_sql, -1, &up_stmt, nullptr);
        sqlite3_bind_text(up_stmt, 1, token.c_str(), -1, SQLITE_STATIC);
        sqlite3_step(up_stmt);
        sqlite3_finalize(up_stmt);

        sqlite3_finalize(stmt);
        return true;
    }
    sqlite3_finalize(stmt);
    return false;
}

void cleanup_expired_tokens() {
    sqlite3_exec(db, "DELETE FROM sessions WHERE expires < datetime('now') OR last_used IS NULL OR last_used < datetime('now', '-7 days')", nullptr, nullptr, nullptr);
}

bool delete_session(const std::string& token) {
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, "DELETE FROM sessions WHERE token = ?", -1, &stmt, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_text(stmt, 1, token.c_str(), -1, SQLITE_STATIC);
    bool ok = (sqlite3_step(stmt) == SQLITE_DONE);
    sqlite3_finalize(stmt);
    return ok;
}

int invalidate_sessions(int user_id, const std::string& keep_token) {
    sqlite3_stmt* stmt = nullptr;
    const char* sql = keep_token.empty()
        ? "DELETE FROM sessions WHERE user_id = ?"
        : "DELETE FROM sessions WHERE user_id = ? AND token <> ?";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) return 0;
    sqlite3_bind_int(stmt, 1, user_id);
    if (!keep_token.empty()) sqlite3_bind_text(stmt, 2, keep_token.c_str(), -1, SQLITE_STATIC);
    bool ok = (sqlite3_step(stmt) == SQLITE_DONE);
    sqlite3_finalize(stmt);
    if (!ok) return 0;
    return sqlite3_changes(db);
}

bool valid_username(const std::string& username) {
    if (username.size() < 3 || username.size() > 32) return false;
    for (unsigned char c : username) {
        if (!isalnum(c) && c != '.' && c != '-' && c != '_') return false;
    }
    return true;
}

bool change_password(int user_id, const std::string& old_pass, const std::string& new_pass,
                     const std::string& keep_token) {
    if (old_pass.empty() || new_pass.empty() || new_pass.length() < 6) {
        return false;
    }

    // 1. Verificar que la contraseña antigua sea correcta
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "SELECT password_hash FROM users WHERE id = ?";
    sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr);
    sqlite3_bind_int(stmt, 1, user_id);

    if (sqlite3_step(stmt) != SQLITE_ROW) {
        sqlite3_finalize(stmt);
        return false;
    }

    std::string current_hash = (const char*)sqlite3_column_text(stmt, 0);
    sqlite3_finalize(stmt);

    if (!bcrypt_verify(old_pass, current_hash)) {
        return false; // Contraseña antigua incorrecta
    }

    // 2. Hashear la nueva contraseña
    std::string new_hash = bcrypt_hash(new_pass);
    if (new_hash.empty()) return false;

    // 3. Actualizar en la base de datos
    const char* update_sql = "UPDATE users SET password_hash = ?, last_password_change = CURRENT_TIMESTAMP WHERE id = ?";
    sqlite3_prepare_v2(db, update_sql, -1, &stmt, nullptr);
    sqlite3_bind_text(stmt, 1, new_hash.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt, 2, user_id);

    int result = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    if (result != SQLITE_DONE) return false;

    // Cambiar la contraseña cierra las demás sesiones del usuario (la actual sigue viva).
    int cerradas = invalidate_sessions(user_id, keep_token);
    if (cerradas > 0)
        std::cout << "[SESIONES] " << cerradas << " sesión(es) cerradas al cambiar contraseña (user_id=" << user_id << ")" << std::endl;
    return true;
}

// ===================== CREAR USUARIO  =====================
bool create_user(const std::string& username, const std::string& password, const std::string& role) {
    if (username.empty() || password.empty() || (role != "user" && role != "admin")) {
        return false;
    }
    // Sin esta cota, un usuario llamado  a"b  romperse el JSON de /api/users.
    if (!valid_username(username)) {
        std::cout << "[CREATE USER] nombre rechazado: '" << username << "'" << std::endl;
        return false;
    }
    if (password.size() < 6) {
        return false;
    }

    // Verificar si el usuario ya existe
    sqlite3_stmt* stmt = nullptr;
    const char* check_sql = "SELECT id FROM users WHERE username = ?";
    sqlite3_prepare_v2(db, check_sql, -1, &stmt, nullptr);
    sqlite3_bind_text(stmt, 1, username.c_str(), -1, SQLITE_STATIC);

    if (sqlite3_step(stmt) == SQLITE_ROW) {
        sqlite3_finalize(stmt);
        return false; // Usuario ya existe
    }
    sqlite3_finalize(stmt);

    // Hashear la contraseña
    std::string hash = bcrypt_hash(password);
    if (hash.empty()) return false;

    // Insertar nuevo usuario
    const char* insert_sql = "INSERT INTO users (username, password_hash, role) VALUES (?, ?, ?)";
    sqlite3_prepare_v2(db, insert_sql, -1, &stmt, nullptr);
    sqlite3_bind_text(stmt, 1, username.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, hash.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 3, role.c_str(), -1, SQLITE_STATIC);

    int result = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    return (result == SQLITE_DONE);
}
