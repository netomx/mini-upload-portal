#pragma once
#include <string>

std::string bcrypt_hash(const std::string& password);
bool bcrypt_verify(const std::string& password, const std::string& hash);
std::string generate_secure_token();
