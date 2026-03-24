# 📁 Mini Portal de Archivos

Un **mini servidor de subida y descarga de archivos** ligero, seguro y de alto rendimiento desarrollado en **C++** con `cpp-httplib`.

Ideal para entornos embebidos como **TinyCore Linux**, pero también compila fácilmente en Debian/Ubuntu.

---

## ✨ Características

- Subida de archivos **por chunks** (con progreso en tiempo real y reintentos automáticos)
- Autenticación segura con **Bearer Token**
- Roles: **Admin** y **Usuario normal**
- Descarga pública mediante **token** (sin necesidad de login)
- Gestión completa de usuarios (crear y resetear contraseña)
- Listado de archivos con acciones (descargar, copiar link, borrar)
- Auto-refresh del listado de archivos
- Diseño moderno y responsive
- Muy bajo consumo de recursos (ideal para TinyCore)

---

## 🚀 Endpoints API

### Autenticación
| Método | Ruta              | Descripción                  | Parámetros                  | Respuesta                     |
|--------|-------------------|------------------------------|-----------------------------|-------------------------------|
| POST   | `/api/login`      | Iniciar sesión               | `username`, `password`      | `{ "success": true, "token": "..." }` |
| GET    | `/api/me`         | Información del usuario      | Bearer Token                | `{ "message": "...", "role": "admin/user" }` |

### Archivos
| Método | Ruta                  | Descripción                        | Parámetros                          | Respuesta |
|--------|-----------------------|------------------------------------|-------------------------------------|---------|
| GET    | `/api/files`          | Listado de archivos                | Bearer Token                        | Array de archivos |
| POST   | `/api/upload/init`    | Iniciar subida por chunks          | `filename`, `total_chunks`          | `{ "upload_id": "...", "chunk_size": 4194304 }` |
| POST   | `/api/upload/chunk`   | Subir un chunk                     | `upload_id`, `chunk_index`, `total_chunks`, archivo | `{ "status": "ok" }` |
| POST   | `/api/upload/complete`| Finalizar subida                   | `upload_id`                         | `{ "success": true, "download_token": "...", "filename": "..." }` |
| GET    | `/download?id=...&token=...` | Descarga pública (sin auth)   | `id`, `token`                       | Archivo binario |

### Gestión de Usuarios (solo Admin)
| Método | Ruta                              | Descripción                    | Parámetros                     | Respuesta |
|--------|-----------------------------------|--------------------------------|--------------------------------|---------|
| GET    | `/api/users`                      | Listado de usuarios            | Bearer Token                   | Array de usuarios |
| POST   | `/api/users`                      | Crear nuevo usuario            | `username`, `password`, `role` | `{ "success": true }` |
| POST   | `/api/users/{id}/reset-password`  | Resetear contraseña            | Bearer Token                   | `{ "success": true, "new_password": "..." }` |

---

## 🛠️ Cómo compilar

### En **Debian / Ubuntu**

```bash
sudo apt update
sudo apt install g++ make libsqlite3-dev

cd /ruta/a/tu/proyecto
make
sudo ./bin/portal

### En TinyCore 64 bits

```bash
tce-load -iw compiletc sqlite3
make
sudo ./bin/portal
