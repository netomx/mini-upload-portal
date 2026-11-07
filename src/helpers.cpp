#include "helpers.h"
#include <sys/statvfs.h>
#include <iomanip>
#include <sstream>
#include <cstring>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cerrno>
#include <cctype>

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

// ==================== MD5 EN STREAMING ====================
Md5Stream::Md5Stream() : bits(0), block_len(0) {
    state[0] = 0x67452301; state[1] = 0xefcdab89; state[2] = 0x98badcfe; state[3] = 0x10325476;
    memset(block, 0, sizeof(block));
}

void Md5Stream::update(const void* data, size_t len) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    bits += (uint64_t)len * 8ULL;

    // 1) completar un bloque parcial pendiente
    if (block_len) {
        size_t need = 64 - block_len;
        if (len < need) {
            memcpy(block + block_len, p, len);
            block_len += len;
            return;
        }
        memcpy(block + block_len, p, need);
        md5_transform(state, block);
        p += need; len -= need; block_len = 0;
    }

    // 2) bloques completos
    while (len >= 64) {
        md5_transform(state, p);
        p += 64; len -= 64;
    }

    // 3) resto
    if (len) {
        memcpy(block, p, len);
        block_len = len;
    }
}

std::string Md5Stream::hex_final() {
    // padding: 0x80, ceros hasta %64==56 y longitud en bits (little-endian)
    uint8_t tail[144];
    size_t n = block_len;
    memcpy(tail, block, n);
    tail[n++] = 0x80;
    while ((n % 64) != 56) tail[n++] = 0x00;
    memcpy(tail + n, &bits, 8);
    n += 8;

    for (size_t off = 0; off < n; off += 64) md5_transform(state, tail + off);

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

std::string calculate_md5(const void* data, size_t len) {
    Md5Stream s;
    s.update(data, len);
    return s.hex_final();
}

// ==================== RUTAS ====================
static const char* env_or(const char* name, const char* def) {
    const char* v = getenv(name);
    return (v && *v) ? v : def;
}

const std::string& db_path()          { static std::string s = env_or("PORTAL_DB", "/mnt/xvdb1/db/files.db"); return s; }
const std::string& files_dir()        { static std::string s = env_or("PORTAL_FILES_DIR", "/mnt/xvdb1/files"); return s; }
const std::string& uploads_tmp_dir()  { static std::string s = env_or("PORTAL_TMP_DIR", "/mnt/xvdb1/tmp/uploads"); return s; }
const std::string& docroot_dir()      { static std::string s = env_or("PORTAL_DOCROOT", "/mnt/xvdb1/app/public"); return s; }

// ==================== ESPACIO EN DISCO ====================
long long free_bytes() {
    struct statvfs fs;
    if (statvfs(files_dir().c_str(), &fs) != 0) return -1;
    return (long long)fs.f_bsize * fs.f_bavail;
}

bool enough_space(long long needed, long long margin) {
    long long fb = free_bytes();
    return fb >= 0 && fb - margin >= needed;
}

std::string get_free_space() {
    long long fb = free_bytes();
    if (fb < 0) return "Error al leer espacio";

    double free_gb = fb / (1024.0 * 1024 * 1024);
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(2) << free_gb << " GB libres";
    return oss.str();
}

// ==================== TEXTO SEGURO ====================
// Sustituye secuencias UTF-8 inválidas por U+FFFD: sin esto, un nombre con
// bytes sueltos rompe el parseo JSON del navegador y la página entera parece
// "no poder cargar los archivos".
std::string utf8_sanitize(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    const unsigned char* p = reinterpret_cast<const unsigned char*>(in.data());
    size_t i = 0, n = in.size();

    while (i < n) {
        unsigned char c = p[i];
        if (c < 0x80) { out += (char)c; ++i; continue; }

        size_t need; unsigned int cp;
        if      ((c & 0xE0) == 0xC0) { need = 1; cp = c & 0x1F; }
        else if ((c & 0xF0) == 0xE0) { need = 2; cp = c & 0x0F; }
        else if ((c & 0xF8) == 0xF0) { need = 3; cp = c & 0x07; }
        else { out += "\xEF\xBF\xBD"; ++i; continue; }   // byte inicial imposible

        if (i + need >= n) { out += "\xEF\xBF\xBD"; ++i; continue; }

        bool ok = true;
        for (size_t k = 1; k <= need; ++k) {
            unsigned char cc = p[i + k];
            if ((cc & 0xC0) != 0x80) { ok = false; break; }
            cp = (cp << 6) | (cc & 0x3F);
        }
        // rechaza sobre-codificación, rangos reservados y > U+10FFFF
        if (!ok || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF) ||
            (need == 1 && cp < 0x80) || (need == 2 && cp < 0x800) || (need == 3 && cp < 0x10000)) {
            out += "\xEF\xBF\xBD"; ++i; continue;
        }

        out.append(reinterpret_cast<const char*>(p) + i, need + 1);
        i += need + 1;
    }
    return out;
}

std::string escape_json(const std::string& s) {
    static const char* hexd = "0123456789abcdef";
    std::string result;
    result.reserve(s.size() + 8);

    for (unsigned char c : utf8_sanitize(s)) {
        switch (c) {
            case '"':  result += "\\\""; break;
            case '\\': result += "\\\\"; break;
            case '\n': result += "\\n";  break;
            case '\r': result += "\\r";  break;
            case '\t': result += "\\t";  break;
            case '\b': result += "\\b";  break;
            case '\f': result += "\\f";  break;
            default:
                if (c < 32 || c == 0x7f) {
                    result += "\\u00";
                    result += hexd[(c >> 4) & 0xf];
                    result += hexd[c & 0xf];
                } else {
                    result += (char)c;
                }
        }
    }
    return result;
}

// Deja solo el nombre: sin '/', '\\', sin control y sin caracteres problemáticos.
// Así "/mnt/xvdb1/files/<token>_<nombre>" nunca puede salir del directorio.
std::string sanitize_filename(const std::string& in_raw) {
    std::string in = utf8_sanitize(in_raw);

    size_t sep = in.find_last_of("/\\");
    if (sep != std::string::npos) in = in.substr(sep + 1);

    std::string out;
    out.reserve(in.size());
    for (unsigned char c : in) {
        if (c < 32 || c == 0x7f) continue;
        if (c == '"' || c == '<' || c == '>' || c == '|' || c == '?' || c == '*') continue;
        out += (char)c;
    }

    while (!out.empty() && (out.back() == '.' || out.back() == ' ')) out.pop_back();
    if (out.empty() || out.find_first_not_of(". ") == std::string::npos) out = "archivo";
    if (out.size() > 200) out = out.substr(out.size() - 200);   // conserva la extensión
    return out;
}

std::string content_disposition(const std::string& filename) {
    std::string base = utf8_sanitize(filename);

    // fallback ASCII: sin comillas, sin barra, sin control (nunca CRLF)
    std::string ascii;
    for (unsigned char c : base) {
        if (c >= 32 && c < 127 && c != '"' && c != '\\') ascii += (char)c;
    }
    if (ascii.empty()) ascii = "archivo";
    if (ascii.size() > 120) ascii = ascii.substr(0, 120);

    // nombre real en UTF-8 (RFC 5987)
    static const char* hexd = "0123456789ABCDEF";
    std::string enc;
    for (unsigned char c : base) {
        bool safe = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                    c == '!' || c == '-' || c == '_' || c == '.' || c == '\'' ||
                    c == '(' || c == ')' || c == '~';
        if (safe) enc += (char)c;
        else { enc += '%'; enc += hexd[c >> 4]; enc += hexd[c & 0xf]; }
    }

    return "attachment; filename=\"" + ascii + "\"; filename*=UTF-8''" + enc;
}

bool parse_ll(const std::string& s, long long& out) {
    if (s.empty()) return false;
    errno = 0;
    char* end = nullptr;
    long long v = strtoll(s.c_str(), &end, 10);
    if (errno != 0 || end == s.c_str()) return false;
    while (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n') ++end;
    if (*end != '\0') return false;
    out = v;
    return true;
}

bool is_safe_upload_id(const std::string& id) {
    if (id.empty() || id.size() > 64) return false;
    for (char c : id) if (!isxdigit((unsigned char)c)) return false;
    return true;
}