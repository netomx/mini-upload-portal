#pragma once
#include <string>
#include <optional>

bool init_db();
std::optional<int> login_user(const std::string& username, const std::string& password, std::string& out_token);
bool validate_token(const std::string& token, int& out_user_id, std::string& out_role);
bool change_password(int user_id, const std::string& old_pass, const std::string& new_pass,
                     const std::string& keep_token);
bool create_user(const std::string& username, const std::string& password, const std::string& role);
void cleanup_expired_tokens();

// Cierra la sesión de un token concreto (/api/logout).
bool delete_session(const std::string& token);

// Cierra todas las sesiones de un usuario, salvo keep_token si se indica.
// Al cambiar/resetear la contraseña, un token robado no debe sobrevivir.
int invalidate_sessions(int user_id, const std::string& keep_token);

// true si el nombre de usuario es aceptable (ASCII simple, sin espacios ni comillas).
bool valid_username(const std::string& username);
