#include "helpers.h"
#include <sys/statvfs.h>
#include <iomanip>
#include <sstream>
#include <cstring>
#include <cstdint>

// ==================== MD5 ====================
static uint32_t rotate_left(uint32_t x, int n) {
    return (x << n) | (x >> (32 - n));
}

static void md5_transform(uint32_t state[4], const uint8_t block[64]) {
    uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
    uint32_t x[16];
    memcpy(x, block, 64);

    // Round 1
    a = rotate_left(a + ((b & c) | (~b & d)) + x[0]  + 0xd76aa478, 7) + b;
    d = rotate_left(d + ((a & b) | (~a & c)) + x[1]  + 0xe8c7b756, 12) + a;
    c = rotate_left(c + ((d & a) | (~d & b)) + x[2]  + 0x242070db, 17) + d;
    b = rotate_left(b + ((c & d) | (~c & a)) + x[3]  + 0xc1bdceee, 22) + c;
    a = rotate_left(a + ((b & c) | (~b & d)) + x[4]  + 0xf57c0faf, 7) + b;
    d = rotate_left(d + ((a & b) | (~a & c)) + x[5]  + 0x4787c62a, 12) + a;
    c = rotate_left(c + ((d & a) | (~d & b)) + x[6]  + 0xa8304613, 17) + d;
    b = rotate_left(b + ((c & d) | (~c & a)) + x[7]  + 0xfd469501, 22) + c;
    a = rotate_left(a + ((b & c) | (~b & d)) + x[8]  + 0x698098d8, 7) + b;
    d = rotate_left(d + ((a & b) | (~a & c)) + x[9]  + 0x8b44f7af, 12) + a;
    c = rotate_left(c + ((d & a) | (~d & b)) + x[10] + 0xffff5bb1, 17) + d;
    b = rotate_left(b + ((c & d) | (~c & a)) + x[11] + 0x895cd7be, 22) + c;
    a = rotate_left(a + ((b & c) | (~b & d)) + x[12] + 0x6b901122, 7) + b;
    d = rotate_left(d + ((a & b) | (~a & c)) + x[13] + 0xfd987193, 12) + a;
    c = rotate_left(c + ((d & a) | (~d & b)) + x[14] + 0xa679438e, 17) + d;
    b = rotate_left(b + ((c & d) | (~c & a)) + x[15] + 0x49b40821, 22) + c;

    // Round 2
    a = rotate_left(a + ((b & d) | (c & ~d)) + x[1]  + 0xf61e2562, 5) + b;
    d = rotate_left(d + ((a & c) | (b & ~c)) + x[6]  + 0xc040b340, 9) + a;
    c = rotate_left(c + ((d & b) | (a & ~b)) + x[11] + 0x265e5a51, 14) + d;
    b = rotate_left(b + ((c & a) | (d & ~a)) + x[0]  + 0xe9b6c7aa, 20) + c;
    a = rotate_left(a + ((b & d) | (c & ~d)) + x[5]  + 0xd62f105d, 5) + b;
    d = rotate_left(d + ((a & c) | (b & ~c)) + x[10] + 0x02441453, 9) + a;
    c = rotate_left(c + ((d & b) | (a & ~b)) + x[15] + 0xd8a1e681, 14) + d;
    b = rotate_left(b + ((c & a) | (d & ~a)) + x[4]  + 0xe7d3fbc8, 20) + c;
    a = rotate_left(a + ((b & d) | (c & ~d)) + x[9]  + 0x21e1cde6, 5) + b;
    d = rotate_left(d + ((a & c) | (b & ~c)) + x[14] + 0xc33707d6, 9) + a;
    c = rotate_left(c + ((d & b) | (a & ~b)) + x[3]  + 0xf4d50d87, 14) + d;
    b = rotate_left(b + ((c & a) | (d & ~a)) + x[8]  + 0x455a14ed, 20) + c;
    a = rotate_left(a + ((b & d) | (c & ~d)) + x[13] + 0xa9e3e905, 5) + b;
    d = rotate_left(d + ((a & c) | (b & ~c)) + x[2]  + 0xfcefa3f8, 9) + a;
    c = rotate_left(c + ((d & b) | (a & ~b)) + x[7]  + 0x676f02d9, 14) + d;
    b = rotate_left(b + ((c & a) | (d & ~a)) + x[12] + 0x8d2a4c8a, 20) + c;

    // Round 3
    a = rotate_left(a + (b ^ c ^ d) + x[5]  + 0xfffa3942, 4) + b;
    d = rotate_left(d + (a ^ b ^ c) + x[8]  + 0x8771f681, 11) + a;
    c = rotate_left(c + (d ^ a ^ b) + x[11] + 0x6d9d6122, 16) + d;
    b = rotate_left(b + (c ^ d ^ a) + x[14] + 0xfde5380c, 23) + c;
    a = rotate_left(a + (b ^ c ^ d) + x[1]  + 0xa4beea44, 4) + b;
    d = rotate_left(d + (a ^ b ^ c) + x[4]  + 0x4bdecfa9, 11) + a;
    c = rotate_left(c + (d ^ a ^ b) + x[7]  + 0xf6bb4b60, 16) + d;
    b = rotate_left(b + (c ^ d ^ a) + x[10] + 0xbebfbc70, 23) + c;
    a = rotate_left(a + (b ^ c ^ d) + x[13] + 0x289b7ec6, 4) + b;
    d = rotate_left(d + (a ^ b ^ c) + x[0]  + 0xeaa127fa, 11) + a;
    c = rotate_left(c + (d ^ a ^ b) + x[3]  + 0xd4ef3085, 16) + d;
    b = rotate_left(b + (c ^ d ^ a) + x[6]  + 0x04881d05, 23) + c;
    a = rotate_left(a + (b ^ c ^ d) + x[9]  + 0xd9d4d039, 4) + b;
    d = rotate_left(d + (a ^ b ^ c) + x[12] + 0xe6db99e5, 11) + a;
    c = rotate_left(c + (d ^ a ^ b) + x[15] + 0x1fa27cf8, 16) + d;
    b = rotate_left(b + (c ^ d ^ a) + x[2]  + 0xc4ac5665, 23) + c;

    // Round 4
    a = rotate_left(a + (c ^ (b | ~d)) + x[0]  + 0xf4292244, 6) + b;
    d = rotate_left(d + (b ^ (a | ~c)) + x[7]  + 0x432aff97, 10) + a;
    c = rotate_left(c + (a ^ (d | ~b)) + x[14] + 0xab9423a7, 15) + d;
    b = rotate_left(b + (d ^ (c | ~a)) + x[5]  + 0xfc93a039, 21) + c;
    a = rotate_left(a + (c ^ (b | ~d)) + x[12] + 0x655b59c3, 6) + b;
    d = rotate_left(d + (b ^ (a | ~c)) + x[3]  + 0x8f0ccc92, 10) + a;
    c = rotate_left(c + (a ^ (d | ~b)) + x[10] + 0xffeff47d, 15) + d;
    b = rotate_left(b + (d ^ (c | ~a)) + x[1]  + 0x85845dd1, 21) + c;
    a = rotate_left(a + (c ^ (b | ~d)) + x[8]  + 0x6fa87e4f, 6) + b;
    d = rotate_left(d + (b ^ (a | ~c)) + x[15] + 0xfe2ce6e0, 10) + a;
    c = rotate_left(c + (a ^ (d | ~b)) + x[6]  + 0xa3014314, 15) + d;
    b = rotate_left(b + (d ^ (c | ~a)) + x[13] + 0x4e0811a1, 21) + c;
    a = rotate_left(a + (c ^ (b | ~d)) + x[4]  + 0xf7537e82, 6) + b;
    d = rotate_left(d + (b ^ (a | ~c)) + x[11] + 0xbd3af235, 10) + a;
    c = rotate_left(c + (a ^ (d | ~b)) + x[2]  + 0x2ad7d2bb, 15) + d;
    b = rotate_left(b + (d ^ (c | ~a)) + x[9]  + 0xeb86d391, 21) + c;

    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
}

std::string calculate_md5(const void* data, size_t len) {
    const uint8_t* bytes = static_cast<const uint8_t*>(data);
    uint32_t state[4] = {0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476};
    uint64_t bit_len = len * 8ULL;
    uint8_t block[64] = {0};
    size_t i = 0;

    while (len - i >= 64) {
        md5_transform(state, bytes + i);
        i += 64;
    }

    memcpy(block, bytes + i, len - i);
    block[len - i] = 0x80;

    if (len - i >= 56) {
        md5_transform(state, block);
        memset(block, 0, 64);
    }

    memcpy(block + 56, &bit_len, 8);
    md5_transform(state, block);

    char buf[33];
    for (int j = 0; j < 4; ++j) {
        sprintf(buf + j*8, "%02x%02x%02x%02x",
                (state[j] & 0xff),
                (state[j] >> 8) & 0xff,
                (state[j] >> 16) & 0xff,
                (state[j] >> 24) & 0xff);
    }
    return std::string(buf);
}

// ==================== ESPACIO EN DISCO ====================
std::string get_free_space() {
    struct statvfs fs;
    if (statvfs("/mnt/xvdb1", &fs) != 0) {
        return "Error al leer espacio";
    }

    long long free_bytes = (long long)fs.f_bsize * fs.f_bavail;
    double free_gb = free_bytes / (1024.0 * 1024 * 1024);

    std::ostringstream oss;
    oss << std::fixed << std::setprecision(2) << free_gb << " GB libres";
    return oss.str();
}

std::string escape_json(const std::string& s) {
    std::string result;
    for (char c : s) {
        if (c == '"') {
            result += "\\\"";
        } else if (c == '\\') {
            result += "\\\\";
        } else if (c >= 0 && c <= 31) {
            // Ignorar caracteres de control invisibles
        } else {
            result += c;
        }
    }
    return result;
}