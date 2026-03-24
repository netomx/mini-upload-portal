#pragma once
#include <string>
#include <optional>

bool init_db();
std::optional<int> login_user(const std::string& username, const std::string& password, std::string& out_token);
bool validate_token(const std::string& token, int& out_user_id, std::string& out_role);
bool change_password(int user_id, const std::string& old_pass, const std::string& new_pass);
bool create_user(const std::string& username, const std::string& password, const std::string& role);
void cleanup_expired_tokens();
