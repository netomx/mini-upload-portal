# Mini Upload Portal — endpoints y flujos

Backend C++ (httplib + SQLite). Este documento describe el contrato **tal y como está
implementado** (rutas, límites y códigos verificados con la batería de pruebas E2E),
incluidas las limitaciones conocidas.

- Base URL: el propio host. El TLS lo termina Cloudflare; el origen habla HTTP.
- Todo `/api/*` exige `Authorization: Bearer <token>` **salvo** `POST /api/login`.
- Los POST usan `application/x-www-form-urlencoded`; el único `multipart/form-data`
  es `/api/upload/chunk`.
- Todo error sale como JSON: `{"error":"motivo legible en español"}`. El frontend
  muestra ese texto directamente en el modal, así que los motivos son accionables.

---

## 1. Autenticación, tokens y sesiones

| pieza | detalle |
|---|---|
| Token de sesión | 64 caracteres hex (32 bytes aleatorios). Fila en `sessions`. |
| Validez | `expires = now + 30 días` **y** `last_used` en los últimos 7 días. Un cliente sin actividad 7 días pierde la sesión. |
| `last_used` | Se reescribe **máximo 1 vez por hora** (el auto-refresh de la UI no escribe la BD cada 8 s). |
| Limpieza | Al arrancar el proceso y cada 10 min vía `GET /api/status`. |
| Cambio/reset de contraseña | Cierra **todas** las sesiones del usuario afectado; la sesión que ejecuta el cambio se conserva. Un token robado no sobrevive a un cambio de clave. |
| `/api/logout` | Borra la fila de la sesión (el token deja de servir al instante). |
| Token de descarga | 64 hex **por archivo**. Se entrega en `/api/upload/complete` y en `/api/files`. Es el secreto de la descarga: quien lo tiene, descarga. |

### Códigos de estado

| código | significado para el cliente |
|---|---|
| `200` | OK. |
| `400` | Entrada inválida o estado imposible (nombre inválido, `chunk_index` fuera de rango, MD5 que no coincide, subida caducada, faltan chunks, sin espacio). **No reintentar igual**: cambiar algo o avisar. |
| `401` | Token ausente, inválido o caducado. La UI debe ir al login y parar el auto-refresh. |
| `403` | Rol insuficiente (no eres admin). |
| `404` | No existe (archivo, subida, usuario, ruta desconocida). |
| `409` | Otro `/complete` de esa misma subida está en curso. Reintentar tras una espera. |
| `429` | Demasiados intentos de login fallidos. Esperar; no reintentar. |
| `500` | Fallo interno (excepción capturada, fallo de disco/BD). Con motivo cuando se conoce. |

---

## 2. Endpoints

### 2.1 Autenticación

#### `POST /api/login`
Form: `username`, `password`.

```json
{ "success": true, "token": "1f4c…" }                 // 200
{ "error": "Credenciales inválidas" }                 // 401
{ "error": "demasiados intentos fallidos; espera 15 minutos" }  // 429
```

Límite anti-fuerza-bruta: 8 fallos por `IP+usuario` en 15 minutos → 429. Un login
correcto de ese par limpia el contador.

#### `POST /api/logout`
```json
{ "success": true }    // false si la fila ya no existía
```

#### `GET /api/me`
```json
{ "message": "Autenticado correctamente", "role": "admin" }   // o "user"
```
La UI lo usa para validar el token guardado **antes** de mostrar la aplicación.

#### `POST /api/change-password`
Form: `old_password`, `new_password` (mínimo 8 caracteres).

```json
{ "success": true, "message": "Contraseña cambiada correctamente" }
{ "error": "La contraseña actual es incorrecta o la nueva es inválida (mínimo 8 caracteres)" }   // 400
```
Efecto lateral: cierra las demás sesiones de ese usuario.

---

### 2.2 Estado y limpieza

#### `GET /api/status`
```json
{ "status": "ok", "free_space": "1784.41 GB libres" }
```
Además dispara, como mucho una vez cada 10 minutos, la purga de sesiones muertas y de
subidas caducadas (filas **y** sus directorios de chunks).

---

### 2.3 Archivos

#### `GET /api/files`
Lista **todos** los archivos del portal (no por usuario): cualquier usuario autenticado
ve el listado completo. La puerta real es el `token` de descarga.

```json
[ { "id": 41,
    "filename": "Informe Q3.pdf",
    "filetype": "pdf",
    "size": 10485760,
    "token": "9f2a…",
    "md5": "c9000cbc92f0db4523cbf3abdfa7d2e9" } ]
```
`md5` viene vacío (`""`) en archivos subidos antes de la migración.

#### `GET /download?id=<n>&token=<t>`
Respuesta binaria. Sin `Content-Type` de aplicación: `application/octet-stream` forzado
a descarga.

Cabeceras emitidas:

```
Content-Length: 17536620544
Content-Disposition: attachment; filename="Zelda.xci"; filename*=UTF-8''Zelda%20Tears.xci
Accept-Ranges: bytes
X-Checksum-MD5: c9000cbc92f0db4523cbf3abdfa7d2e9     // solo si la BD lo tiene
```

- Admite `Range:` → `206 Partial Content` con el trozo exacto.
- Parámetros no numéricos o vacíos → `400` en texto plano.
- Fila existente pero archivo ausente en disco → `404` con
  `"Archivo no encontrado en disco"` (y lo deja en el log del servidor).

#### `DELETE /api/files/<id>` — solo admin
```json
{ "success": true }
{ "error": "Solo el administrador puede borrar archivos" }   // 403
{ "error": "ese archivo no existe en el registro" }          // 404
{ "error": "registro borrado pero el archivo sigue en disco" }  // 500 (queda avisado)
```
Borra las tres cosas: el archivo de `files/`, la fila de `files` y los restos de su
subida (`tmp/uploads/<upload_id>/` y su fila en `temp_uploads`).

---

### 2.4 Usuarios — solo admin

#### `GET /api/users`
```json
[ { "id": 2, "username": "marcela", "role": "user",
    "last_change": "2026-11-07 06:52:10" } ]
```

#### `POST /api/users`
Form: `username`, `password`, `role` (`user` | `admin`).

```json
{ "success": true, "message": "Usuario creado correctamente" }
{ "error": "no se pudo crear el usuario (nombre en uso, o usuario/contraseña inválidos)" }   // 400
```
`username` debe ser ASCII simple, sin espacios ni comillas; `password` ≥ 8 caracteres.

#### `POST /api/users/<id>/reset-password`
```json
{ "success": true, "new_password": "89bfe1b5120a" }
{ "error": "El usuario no existe" }                  // 404
```
La contraseña nueva **solo se ve en esta respuesta**: se genera, se hashea con bcrypt y
se devuelve en claro una vez. Cierra todas las sesiones del usuarioReset.

---

### 2.5 Subida en trozos (la parte con más reglas)

#### `POST /api/upload/init`
Form: `filename` (nombre original), `total_chunks` (1..250000).

```json
{ "upload_id": "775694f65db78301ecd91bf2f94f5880", "chunk_size": 4194304 }
{ "error": "nombre de archivo inválido" }                                  // 400
{ "error": "total_chunks fuera de rango (1..250000)" }                     // 400
{ "error": "total_chunks no es un número" }                                // 400
{ "error": "no hay suficiente espacio libre en disco" }                    // 400
```

Lo que hace el servidor: sanea el nombre (sin rutas, sin `<>"|?*`, sin control),
crea la fila en `temp_uploads` (con caducidad de **1 hora** desde el init) y crea
`tmp/uploads/<upload_id>/` con modo `0700`.

#### `POST /api/upload/chunk` — `multipart/form-data`

| campo | tipo | detalle |
|---|---|---|
| `upload_id` | texto | 32 hex. Debe existir y no haber caducado. |
| `chunk_index` | texto | `0 .. total_chunks-1`. `total_chunks` **no** se manda: el servidor lo lee de su BD. |
| `chunk_md5` | texto | MD5 hex (minúsculas) de los bytes exactos del trozo. Obligatorio. |
| `chunk` | fichero | Entre 1 y 8 MiB de datos reales. |

```json
{ "status": "ok", "chunk_index": 3 }
{ "error": "chunk_index fuera de rango (0..2)" }                             // 400
{ "error": "MD5 no coincide - chunk corrupto en tránsito" }                  // 400
{ "error": "tamaño de chunk inválido (1..8388608 bytes)" }                    // 400
{ "error": "subida inexistente o caducada" }                                  // 400
{ "error": "esa subida pertenece a otro usuario" }                             // 400
{ "error": "fallo de escritura al guardar el chunk (¿disco lleno?)" }         // 500
```

Cómo se escribe un chunk para que "guardado" signifique "en disco":
`.part` → `fsync` → `rename()` atómico. Si algo falla a mitad, no queda chunk
a medias. Un `chunk_index` repetido se responde `ok` pero **no** infla el contador.

#### `GET /api/upload/status/<upload_id>`
Para reanudar: qué falta realmente.

```json
{ "upload_id": "f8784d8b4118f05b6a9c877927b8fe3b",
  "total": 2, "present": 1, "missing": [1], "missing_truncated": false }
```
`missing` trae como máximo 5000 índices; si se corta, `missing_truncated=true`
(y entonces la UI no reanuda: vuelve a empezar).

#### `POST /api/upload/complete`
Form: `upload_id`.

```json
{ "status": "ok",
  "download_token": "e8ddae23…",
  "filename": "prueba grande.bin",
  "md5": "c9000cbc92f0db4523cbf3abdfa7d2e9",
  "size": 10485760 }

{ "status": "ok", "download_token": "cc2a1f92…", "duplicado": true }   // ya estaba hecho

{ "error": "faltan 1 chunk(s) (ej. 1)" }                                // 400
{ "error": "tamaño ensamblado no coincide (1048576 != 10485760)" }      // 400
{ "error": "el archivo final quedó truncado" }                          // 400
{ "error": "no hay espacio suficiente para ensamblar" }                  // 400
{ "error": "otro completo de esa subida sigue en curso" }                // 409
```

Orden interno (importante para entender los reintentos):
1. Si ya hay una fila en `files` para ese `upload_id` → responde el token anterior
   con `duplicado:true` **sin volver a ensamblar**. Idempotente: el cliente puede
   reintentar este endpoint sin miedo a duplicar archivos.
2. Exige el conjunto **exacto** `0..total-1` de chunks en disco. "Hay N archivos" no
   basta: un hueco ensamblaría un archivo truncado y eso es exactamente como se
   corrompían los archivos antes.
3. Ensambla a `files/<token>_<nombre>.ensamblando`, contando bytes leídos y escritos
   y calculando el MD5 en streaming; `fsync`; `rename` atómico; vuelve a verificar el
   tamaño final en disco.
4. `INSERT` en `files`. **Solo después** borra los chunks y la fila de `temp_uploads`.
   Si el INSERT falla, los chunks siguen ahí y se puede reintentar (antes se perdían).

---

## 3. Flujos de ejemplo

### 3.1 Login y listado

```bash
TOKEN=*** -s -X POST -d "username=marcela&password=****" http://HOST/api/login \
       | sed 's/.*"token":"\([^"]*\)".*/\1/')

curl -s -H "Authorization: Bearer $TOKEN" http://HOST/api/me
curl -s -H "Authorization: Bearer $TOKEN" http://HOST/api/files | sed 's/},{/}\n{/g'
```

```js
const r = await fetch('/api/login', {
  method: 'POST',
  headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
  body: new URLSearchParams({ username, password })
});
if (r.status === 429) throw new Error('espera 15 minutos');
const { token } = await r.json();
localStorage.setItem('token', token);
```

### 3.2 Subida completa con MD5 por trozo

El MD5 por chunk protege la integridad **en tránsito** (proxy, red, Cloudflare).
El MD5 del archivo completo lo calcula el servidor al ensamblar y es el que viaja
en `X-Checksum-MD5` al descargar: así se comprueba que lo que baja es lo que subió.

```bash
FILE="video.mp4"
CHUNK=$((4*1024*1024))
SIZE=$(wc -c < "$FILE")
N=$(( (SIZE + CHUNK - 1) / CHUNK ))

UP=$(curl -s -H "Authorization: Bearer $TOKEN" -X POST \
       --data-urlencode "filename=$(basename "$FILE")" \
       -d "total_chunks=$N" http://HOST/api/upload/init \
     | sed 's/.*"upload_id":"\([^"]*\)".*/\1/')
echo "upload_id=$UP de $N trozos"

i=0
while [ $i -lt $N ]; do
  dd if="$FILE" of=/tmp/part bs=$CHUNK skip=$i count=1 2>/dev/null
  M=$(md5sum /tmp/part | cut -d' ' -f1)
  R=$(curl -s -H "Authorization: Bearer $TOKEN" \
        -F "upload_id=$UP" -F "chunk_index=$i" -F "chunk_md5=$M" \
        -F "chunk=@/tmp/part" http://HOST/api/upload/chunk)
  echo "  trozo $i -> $R"
  case "$R" in *'"ok"'*) ;; *) echo "  abortando: $R"; exit 1;; esac
  i=$((i+1))
done
rm -f /tmp/part

curl -s -H "Authorization: Bearer $TOKEN" -X POST -d "upload_id=$UP" \
     http://HOST/api/upload/complete
# -> {"status":"ok","download_token":"…","filename":"video.mp4",
#     "md5":"…","size":10485760}
```

### 3.3 Reanudar una subida interrumpida

El servidor sabe qué trozos tiene en disco. La UI guarda en `localStorage` la clave
`nombre|tamaño|fecha_modificación` → `upload_id`, y al volver a elegir el mismo
archivo pregunta antes de subir nada.

```bash
curl -s -H "Authorization: Bearer $TOKEN" http://HOST/api/upload/status/$UP
# {"upload_id":"f8784d8b…","total":512,"present":371,"missing":[371,372,…],…}

# solo se reenvían los índices de "missing"
for i in 371 372 373; do
  dd if="$FILE" of=/tmp/part bs=$CHUNK skip=$i count=1 2>/dev/null
  M=$(md5sum /tmp/part | cut -d' ' -f1)
  curl -s -H "Authorization: Bearer $TOKEN" \
       -F "upload_id=$UP" -F "chunk_index=$i" -F "chunk_md5=$M" \
       -F "chunk=@/tmp/part" http://HOST/api/upload/chunk
done

curl -s -H "Authorization: Bearer $TOKEN" -X POST -d "upload_id=$UP" \
     http://HOST/api/upload/complete     # devuelve duplicado:true si ya se había completado
```

### 3.4 Descarga con verificación de integridad y reanudación

```bash
URL="http://HOST/download?id=$ID&token=$DTOK"

curl -s -D - -o /dev/null "$URL" | grep -i -E 'content-length|x-checksum|accept-ranges'
curl -s -o video.mp4 "$URL"
md5sum video.mp4           # debe coincidir con X-Checksum-MD5

# trozo concreto (o continuar una descarga cortada)
curl -s -r 1000000000-1999999999 -o pedazo.bin "$URL"     # 206, exactamente 1 GB
curl -s -C - -o video.mp4 "$URL"                          # reanuda donde quedó
```

### 3.5 Admin: alta y reset de usuario

```bash
curl -s -H "Authorization: Bearer $ADMIN" -X POST \
     --data-urlencode "username=embarques" \
     --data-urlencode "password=clave-larga-1" \
     -d "role=user" http://HOST/api/users

ID=$(curl -s -H "Authorization: Bearer $ADMIN" http://HOST/api/users \
     | sed 's/},{/}\n{/g' | grep embarques | sed 's/.*"id":\([0-9]*\).*/\1/')

curl -s -H "Authorization: Bearer $ADMIN" -X POST http://HOST/api/users/$ID/reset-password
# {"success":true,"new_password":"89bfe1b5120a"}   <- único momento en que se ve
```

### 3.6 Qué hace el cliente según el error

| respuesta | acción del cliente |
|---|---|
| `401` en cualquier endpoint | Borrar token, parar auto-refresh, volver al login (una sola vez). |
| `400` con `"faltan N chunk(s) (ej. …)"` | Pedir `/api/upload/status/<id>` y reenviar **solo** los que faltan, luego `complete`. |
| `400` con `"subida inexistente o caducada"` | Volver a `/init` y subir desde cero. |
| `400` con `"MD5 no coincide …"` | Reenviar ese mismo trozo (corrupción en tránsito). |
| `409` en `complete` | Esperar y reintentar: hay otro ensamblado en curso para ese `upload_id`. |
| `400` con `"no hay suficiente espacio libre en disco"` | Avisar al usuario; no reintentar. |
| `429` en login | No reintentar hasta pasar la ventana. |
| `500` | Reintentar con pausa; si persiste, mostrar el motivo. |

---

## 4. Límites y comportamiento del servidor

| concepto | valor | dónde |
|---|---|---|
| Tamaño de trozo recomendado | 4 MiB (`chunk_size` devuelto por `/init`) | `main.cpp` |
| Máximo por petición (cuerpo) | 32 MiB | `set_payload_max_length` |
| Máximo por chunk | 8 MiB | `MAX_CHUNK_BYTES` |
| Máximo de chunks por subida | 250 000 (≈1 TB con 4 MiB) | `MAX_TOTAL_CHUNKS` |
| Margen mínimo de disco libre | 20 GiB para escribir/ensamblar | `MIN_FREE_BYTES` |
| Caducidad de una subida en curso | 1 hora desde `/init` | `temp_uploads.expires` |
| Espera máxima en `complete` concurrente | 300 s | `wait_for` |
| Freno de login | 8 fallos / 15 min por IP+usuario | `main.cpp` |
| Auto-limpieza | cada 10 min (sesiones + subidas caducadas) | `/api/status` |

---

## 5. Limitaciones conocidas (para decidir, no para tapar)

1. **Una subida debe terminar en 1 hora.** `temp_uploads.expires` se fija en `/init`
   y **no se renueva** al recibir trozos: una subida muy lenta o una pausa larga >1 h
   hace que la fila caduque y los chunks se purguen; el cliente lo ve como
   `"subida inexistente o caducada"`. Arreglo de una línea (renovar `expires` en cada
   chunk) si molesta.
2. **Cualquier usuario autenticado ve la lista completa de archivos** (`/api/files`).
   El aislamiento real está en la descarga por token y en que solo el admin borra.
   Si se quiere visibilidad por usuario, hay que filtrar por `uploader_id`.
3. **El token de descarga va en la URL** (`/download?id&token`): queda en el historial
   del navegador y en logs intermedios. Alternativa: token en cabecera o URL firmada
   con caducidad.
4. **MD5 no es resistente a manipulación**, solo a corrupción accidentales. Para
   "nadie lo alteró" haría falta SHA-256.
5. **El watchdog no tiene quién le haga watchdog a él**: si `supervisor.sh` muere
   (reboot aparte, que lo relanza desde `bootlocal.sh`), nada levanta el portal hasta
   el siguiente arranque. La cobertura extra sería un cron que compruebe
   `/tmp/portal-supervisor.lock` (el PID del guion) y relance el `screen`; en esta
   imagen no hay `cron` en ejecución, así que haría falta instalarlo.

---

## 6 bis. Watchdog (`deploy/supervisor.sh`)

Instalado en `/mnt/xvdb1/app/supervisor.sh`, arrancado desde `/opt/bootlocal.sh`
(disco real, persiste). Cada 10 s:

| condición | acción |
|---|---|
| No hay proceso `portal` | Lo levanta y sondea hasta 15 s (en frío tarda ~6-8 s en bindear). |
| El proceso existe pero HTTP no contesta 3 veces seguidas | Lo mata (`kill`, luego `-9`) y lo levanta. Cubre el lazo de accept colgado. |
| `portal.log` no es escribible (quedó `root:root`) | Lo rota a `portal.log.root` y crea uno nuevo. Sin esto el arranque fallaba **antes** de ejecutar el binario y sin dejar rastro. |

Detalles que importan si se toca el guion:

- **Sin `sudo` en el bucle**: `bootlocal.sh` baja `net.ipv4.ip_unprivileged_port_start`
  a 80 para que `tc` bindee :80 directamente. `sudo` dentro de `screen` con stdout
  redirigido se colgaba. (Si falta el sysctl, el guion cae a `sudo` como respaldo.)
- **Detección con `pidof`, nunca `pgrep -x`**: en esta imagen `pgrep -x portal` no
  encuentra el proceso aunque su `comm` sea `portal`, y eso provocaba reintentos
  infinitos.
- **Nunca `pkill -f bin/portal`** desde un script: el patrón coincide con la propia
  línea de comando de quien lo ejecuta y se auto-mata a mitad de camino. Matar por PID.
- El hijo se lanza con `nohup … </dev/null`: si hereda la tty, quien lance esto por
  ssh se queda colgado esperando a que cierre el pipe.
- Registro en `/mnt/xvdb1/app/supervisor.log` (rota a 1 MB conservando 256 KB).

Comprobado en producción (2026-11-07): `kill -9` al portal → renace en ~12 s;
`kill -STOP` (proceso vivo que no contesta) → reemplazado en ~33 s. En ambos casos,
un solo listener en :80 y HTTP 200.

Instalación/actualización: `sh /mnt/xvdb1/app/install-supervisor.sh` (idempotente:
deja una sola instancia, arregla permisos de los logs y reescribe el bloque de
`/opt/bootlocal.sh`, con respaldo previo).

---

## 6. Operación

| cosa | valor |
|---|---|
| Código en producción | `/mnt/xvdb1/app` (`src/`, `bin/portal`, `public/`) |
| Base de datos | `/mnt/xvdb1/db/files.db` (SQLite en modo WAL tras la primera ejecución del binario nuevo) |
| Archivos | `/mnt/xvdb1/files/<token>_<nombre sanificado>` |
| Trozos en curso | `/mnt/xvdb1/tmp/uploads/<upload_id>/0000.chunk …` |
| Logs | `/mnt/xvdb1/app/portal.log`, `/mnt/xvdb1/app/supervisor.log` |
| Arranque | `/opt/bootlocal.sh` (`/opt` es disco real: `/dev/xvda1`) |
| Migración | El binario nuevo añade `files.md5` y dos índices, y activa WAL. Aditiva e idempotente: arranca sobre la BD vieja sin script manual. |
| Barrido de huérfanos | `PORTAL_SWEEP_ORPHANS=dry ./bin/portal` (informa) y `=run` (borra). Saca filas caducadas con sus directorios, directorios de chunks sin fila y archivos en `files/` sin referencia en la BD. |

Variables de entorno (todas opcionales):

| variable | por defecto |
|---|---|
| `PORTAL_DB` | `/mnt/xvdb1/db/files.db` |
| `PORTAL_FILES_DIR` | `/mnt/xvdb1/files` |
| `PORTAL_TMP_DIR` | `/mnt/xvdb1/tmp/uploads` |
| `PORTAL_DOCROOT` | `/mnt/xvdb1/app/public` |
| `PORTAL_PORT` | `80` |
| `PORTAL_UID` / `PORTAL_GID` | `1001` / `50` (a quién se despoja de privilegios si arranca como root) |

Reinicio manual:

```bash
cd /mnt/xvdb1/app
sudo ./bin/portal &                      # o el watchdog, si está activo
curl -s -o /dev/null -w "%{http_code}\n" http://127.0.0.1/
tail -f portal.log
```
