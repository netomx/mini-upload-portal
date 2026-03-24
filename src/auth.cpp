#include "auth.h"
#include <crypt.h>
#include <cstring>
#include <fstream>
#include <sstream>
#include <iomanip>

std::string generate_salt() {
    const char charset[] = "./0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
    std::string salt = "$2b$12$";
    unsigned char buf[16];
    std::ifstream urandom("/dev/urandom", std::ios::in | std::ios::binary);
    urandom.read(reinterpret_cast<char*>(buf), sizeof(buf));

    for (int i = 0; i < 22; ++i) {
        salt += charset[buf[i % 16] % (sizeof(charset)-1)];
    }
    return salt;
}

std::string bcrypt_hash(const std::string& password) {
    std::string salt = generate_salt();
    struct crypt_data data{};
    memset(&data, 0, sizeof(data));
    char* hash = crypt_r(password.c_str(), salt.c_str(), &data);
    return hash ? std::string(hash) : "";
}

bool bcrypt_verify(const std::string& password, const std::string& hash) {
    struct crypt_data data{};
    memset(&data, 0, sizeof(data));
    char* result = crypt_r(password.c_str(), hash.c_str(), &data);
    return result && std::string(result) == hash;
}

std::string generate_secure_token() {
    unsigned char buf[32];
    std::ifstream urandom("/dev/urandom", std::ios::in | std::ios::binary);
    urandom.read(reinterpret_cast<char*>(buf), sizeof(buf));

    std::ostringstream oss;
    oss << std::hex << std::setfill('0');
    for (unsigned char c : buf) {
        oss << std::setw(2) << static_cast<int>(c);
    }
    return oss.str();  // 64 caracteres hexadecimales
}
