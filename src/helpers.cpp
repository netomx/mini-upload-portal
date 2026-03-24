#include "helpers.h"
#include <sys/statvfs.h>
#include <iomanip>
#include <sstream>
#include <unistd.h>     // para getcwd

std::string get_free_space() {
    char cwd[1024];
    if (getcwd(cwd, sizeof(cwd)) == nullptr) {
        return "Error al obtener directorio actual";
    }

    struct statvfs fs;
    if (statvfs(cwd, &fs) != 0) {   // ← Ahora usa el directorio actual
        return "Error al leer espacio";
    }

    long long free_bytes = (long long)fs.f_bsize * fs.f_bavail;
    double free_gb = free_bytes / (1024.0 * 1024 * 1024);

    std::ostringstream oss;
    oss << std::fixed << std::setprecision(2) << free_gb << " GB libres";

    return oss.str();
}
