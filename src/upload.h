#pragma once
#include <string>
#include <vector>
#include <cstddef>

// Ninguna función lanza: validan la entrada y devuelven ""/false con el motivo en `err`.
std::string init_upload(const std::string& filename, long long total_chunks, int user_id, std::string& err);

// El chunk llega como puntero+tamaño para no copiar el buffer de httplib.
// total_chunks NO viene del cliente: se lee de la BD dentro de save_chunk.
bool save_chunk(const std::string& upload_id, long long chunk_index,
                const char* data, size_t len, std::string& err);

bool complete_upload(const std::string& upload_id, std::string& out_token, std::string& out_filename,
                     std::string& out_md5, long long& out_size, std::string& err);

// ¿El upload_id existe y es del usuario indicado (o es admin)?
bool upload_access(const std::string& upload_id, int user_id, const std::string& role, std::string& err);

// Estado de una subida, para poder reanudarla.
bool upload_status(const std::string& upload_id, long long& total, long long& present,
                   std::vector<long long>& missing);

// ¿Ya se completó antes? Devuelve el token existente y su dueño (evita duplicados).
bool find_completed_upload(const std::string& upload_id, std::string& out_token, long long& out_owner);

// Borra filas caducadas de temp_uploads y SUS directorios.
void cleanup_old_uploads();

// Autolimpieza: filas caducadas, directorios de chunks sin fila y archivos de
// files/ sin referencia en la BD. Con dry_run solo informa. Devuelve bytes liberados.
long long sweep_abandoned(bool dry_run, std::string& report);
