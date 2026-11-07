#pragma once
#include <string>
#include <cstdint>

// ==================== RUTAS DE DATOS ====================
// Por defecto /mnt/xvdb1, reubicables con variables de entorno
// (PORTAL_DB, PORTAL_FILES_DIR, PORTAL_TMP_DIR, PORTAL_DOCROOT) para poder
// levantar una instancia de prueba sin tocar los datos reales.
const std::string& db_path();
const std::string& files_dir();
const std::string& uploads_tmp_dir();
const std::string& docroot_dir();

// ==================== ESPACIO EN DISCO ====================
long long free_bytes();                                 // bytes libres en el volumen de datos
bool enough_space(long long needed, long long margin);  // margen de seguridad antes de escribir
std::string get_free_space();                           // texto para /api/status

// ==================== MD5 ====================
std::string calculate_md5(const void* data, size_t len);

// MD5 incremental: resume archivos de varios GB sin cargarlos en memoria.
struct Md5Stream {
    uint32_t state[4];
    uint64_t bits;
    uint8_t  block[64];
    size_t   block_len;

    Md5Stream();
    void update(const void* data, size_t len);
    std::string hex_final();
};

// ==================== TEXTO SEGURO ====================
// JSON siempre válido: escapa comilla/barra/control y fuerza UTF-8 bien formado.
std::string escape_json(const std::string& s);
std::string utf8_sanitize(const std::string& in);

// Nombre de archivo sin componentes de ruta: no puede escapar del directorio.
std::string sanitize_filename(const std::string& in);

// Content-Disposition seguro (RFC 6266 + RFC 5987): sin CRLF ni comillas sueltas.
std::string content_disposition(const std::string& filename);

// Alternativa a std::stoi: nunca lanza (la entrada llega del cliente).
bool parse_ll(const std::string& s, long long& out);

// id de subida: solo hex, para poder usarlo en rutas de archivo sin más.
bool is_safe_upload_id(const std::string& id);
