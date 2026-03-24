#include "db.h"
#include "auth.h"
#include <sqlite3.h>
#include <iostream>

sqlite3* db = nullptr;   // <-- QUITAMOS "static" para que upload.cpp lo vea

bool init_db() {
    if (sqlite3_open("/mnt/xvdb1/db/files.db", &db) != SQLITE_OK) {
        std::cerr << "Error SQLite: " << sqlite3_errmsg(db) << std::endl;
        return false;
    }
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

        const char* update_sql = "UPDATE sessions SET last_used = CURRENT_TIMESTAMP WHERE token = ?";
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
    sqlite3_exec(db, "DELETE FROM sessions WHERE expires < datetime('now') OR last_used < datetime('now', '-7 days')", nullptr, nullptr, nullptr);
}

bool change_password(int user_id, const std::string& old_pass, const std::string& new_pass) { return false; }
bool create_user(const std::string& username, const std::string& password, const std::string& role) { return false; }
