# 📁 Mini Portal de Archivos

Un **mini servidor de subida y descarga de archivos** ligero, seguro y eficiente desarrollado en **C++** usando `cpp-httplib` + SQLite.

Diseñado especialmente para entornos ligeros como **TinyCore Linux**, pero también funciona perfectamente en Debian/Ubuntu.

---

## ✨ Características principales

- Subida de archivos **por chunks** (con barra de progreso en tiempo real y reintentos automáticos)
- Autenticación con **Bearer Token**
- Roles: **Administrador** y **Usuario**
- Descarga pública mediante **token** (sin necesidad de login)
- Gestión completa de usuarios (crear y resetear contraseña)
- Cambio de contraseña por parte del usuario
- Borrado de archivos (solo admin)
- Listado de archivos con auto-refresh
- Diseño moderno y responsive (HTML + Tailwind)
- Muy bajo consumo de recursos

---

## 🚀 Endpoints API (Actualizado)

### Autenticación
| Método | Ruta                    | Descripción                        | Requiere Auth | Parámetros                  |
|--------|-------------------------|------------------------------------|---------------|-----------------------------|
| POST   | `/api/login`            | Iniciar sesión                     | No            | `username`, `password`      |
| GET    | `/api/me`               | Información del usuario actual     | Sí            | —                           |

### Archivos
| Método | Ruta                        | Descripción                          | Requiere Auth | Solo Admin | Parámetros importantes |
|--------|-----------------------------|--------------------------------------|---------------|------------|------------------------|
| GET    | `/api/files`                | Listado de archivos                  | Sí            | No         | —                      |
| POST   | `/api/upload/init`          | Iniciar subida por chunks            | Sí            | No         | `filename`, `total_chunks` |
| POST   | `/api/upload/chunk`         | Subir chunk                          | Sí            | No         | `upload_id`, `chunk_index`, archivo |
| POST   | `/api/upload/complete`      | Finalizar subida                     | Sí            | No         | `upload_id`            |
| GET    | `/download?id=XX&token=YY`  | Descarga pública                     | No            | No         | `id`, `token`          |
| DELETE | `/api/files/{id}`           | Borrar archivo                       | Sí            | **Sí**     | —                      |

### Gestión de Usuarios
| Método | Ruta                              | Descripción                          | Requiere Auth | Solo Admin |
|--------|-----------------------------------|--------------------------------------|---------------|------------|
| GET    | `/api/users`                      | Listado de usuarios                  | Sí            | **Sí**     |
| POST   | `/api/users`                      | Crear nuevo usuario                  | Sí            | **Sí**     |
| POST   | `/api/users/{id}/reset-password`  | Resetear contraseña                  | Sí            | **Sí**     |
| POST   | `/api/change-password`            | Cambiar **mi** contraseña            | Sí            | No         |

---

## 🛠️ Cómo compilar

### En Debian / Ubuntu

```bash
sudo apt update
sudo apt install g++ make libsqlite3-dev

cd mini-portal-archivos
make
sudo ./bin/portal
```

### En TinyCore 64 bits

```bash
tce-load -iw compiletc sqlite3
make
sudo ./bin/portal
```
