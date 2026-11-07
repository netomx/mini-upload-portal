#!/bin/sh
# Instalador de una sola pasada del watchdog del portal.
#
# Qué hace y por qué:
#  1. Baja net.ipv4.ip_unprivileged_port_start a 80 para que tc pueda bindear :80
#     sin root. Así el watchdog arranca el portal SIN sudo: dentro de `screen`,
#     `sudo` resultó poco fiable y dejaba el servicio caído.
#  2. Deja UN solo proceso escuchando (había 2 por mis pruebas: SO_REUSEPORT
#     permitía que convivieran, y cada arranque duplicado enmascaraba el fallo).
#  3. Reescribe /opt/bootlocal.sh para que en el próximo arranque: sysctl + watchdog
#     (el watchdog ya levanta el portal).
#  4. PRUEBA la recuperación: mata el portal y comprueba que renace solo.
#
# Requisitos: ejecutarlo como tc desde /mnt/xvdb1/app (usa sudo solo para sysctl,
# pkill de procesos root y escribir en /opt).

set -u
APP=/mnt/xvdb1/app
TS=$(date +%Y%m%d-%H%M%S)

echo "== 0. estado antes =="
echo "  portal pids: [$(pidof portal)]"
echo "  watchdog:    [$(cat /tmp/portal-supervisor.lock 2>/dev/null)]"
echo -n "  http: "; curl -s -m 3 -o /dev/null -w "%{http_code}\n" http://127.0.0.1/ || echo "sin respuesta"

echo "== 0.5 matar restos de mis propias pruebas (shells huérfanos de ssh) =="
# Se buscan por una cadena que SOLO aparece en aquellos comandos; el patrón no está
# en la línea de comando de este script, así que no hay riesgo de auto-mate (a
# diferencia de `pkill -f bin/portal`, que sí se mató a sí misma antes).
for d in /proc/[0-9]*; do
    p=${d#/proc/}
    [ "$p" = "$$" ] && continue
    cmd=$(tr '\0' ' ' < "$d/cmdline" 2>/dev/null)
    case "$cmd" in
        *prueba3.log*|*prueba4.log*|*estado\ actual*)
            echo "  matando residuo pid=$p: $(echo "$cmd" | cut -c1-70)…"
            kill -9 "$p" 2>/dev/null || sudo kill -9 "$p" 2>/dev/null ;;
    esac
done
sleep 1

echo "== 1. permitir bind de puertos privilegiados a usuarios normales =="
sudo sysctl -w net.ipv4.ip_unprivileged_port_start=80 2>&1 | sed 's/^/  /'
cat /proc/sys/net/ipv4/ip_unprivileged_port_start 2>/dev/null | sed 's/^/  piso actual: /'

echo "== 1.5 logs propiedad de tc (la causa de que el watchdog no arrancara el portal) =="
ls -la "$APP/portal.log" "$APP/supervisor.log" 2>/dev/null | sed 's/^/  antes| /'
sudo chown tc:staff "$APP/portal.log" "$APP/supervisor.log" 2>/dev/null
chmod 664 "$APP/portal.log" "$APP/supervisor.log" 2>/dev/null
ls -la "$APP/portal.log" "$APP/supervisor.log" 2>/dev/null | sed 's/^/  ahora| /'
if : >> "$APP/portal.log" 2>/dev/null; then echo "  portal.log escribible por tc: SI"; else echo "  portal.log escribible por tc: NO (el watchdog lo rotará al arrancar)"; fi

echo "== 2. limpiar duplicados y watchdog viejo (por PID, nunca con -f) =="
old=$(cat /tmp/portal-supervisor.lock 2>/dev/null || true)
if [ -n "$old" ] && [ -d "/proc/$old" ]; then echo "  parando watchdog $old"; kill "$old" 2>/dev/null; fi
for p in $(pidof portal); do
    echo "  matando portal $p (uid=$(cut -d' ' -f2 /proc/$p/status 2>/dev/null))"
    kill -9 "$p" 2>/dev/null || sudo kill -9 "$p" 2>/dev/null
done
screen -wipe >/dev/null 2>&1
sleep 2
echo "  portal: [$(pidof portal)]   :80 -> $(netstat -ltn 2>/dev/null | grep -c ':80 ') listener(s)"

echo "== 3. arrancar el watchdog (él levanta el portal, sin sudo) =="
cd "$APP" || exit 1
screen -dmS portal-watchdog sh -c "cd $APP && exec ./supervisor.sh"
i=1
while [ $i -le 10 ]; do
    p=$(pidof -s portal); c=$(curl -s -m 3 -o /dev/null -w '%{http_code}' http://127.0.0.1/ 2>/dev/null || echo 000)
    echo "  t+$((i*2))s portal=[$p] http=$c"
    [ -n "$p" ] && [ "$c" = "200" ] && break
    i=$((i+1)); sleep 2
done
echo "  listeners en :80: $(netstat -ltn 2>/dev/null | grep -c ':80 ')"

echo "== 4. PRUEBA DE RECUPERACION: mato el portal y espero al watchdog =="
antes=$(pidof -s portal)
echo "  matando $antes"
kill -9 "$antes" 2>/dev/null || sudo kill -9 "$antes" 2>/dev/null
sleep 1
echo -n "  tras kill: portal=[$(pidof -s portal)] http="; curl -s -m 2 -o /dev/null -w "%{http_code}\n" http://127.0.0.1/ || echo 000
i=1; recuperado=""
while [ $i -le 12 ]; do
    p=$(pidof -s portal); c=$(curl -s -m 3 -o /dev/null -w '%{http_code}' http://127.0.0.1/ 2>/dev/null || echo 000)
    if [ -n "$p" ] && [ "$p" != "$antes" ] && [ "$c" = "200" ]; then
        echo "  RECUPERADO en t+$((i*3))s: pid=$p http=$c"; recuperado=$p; break
    fi
    echo "  t+$((i*3))s portal=[$p] http=$c"; i=$((i+1)); sleep 3
done
[ -n "$recuperado" ] || echo "  *** NO se recuperó ***"

echo "== 5. persistencia en /opt/bootlocal.sh =="
sudo cp -p /opt/bootlocal.sh "/opt/bootlocal.sh.bak-$TS" 2>/dev/null || cp /opt/bootlocal.sh "/opt/bootlocal.sh.bak-$TS"
cp /opt/bootlocal.sh "$APP/bootlocal.sh.bak-$TS" 2>/dev/null
# idempotente: quita el arranque viejo del portal Y un bloque de watchdog previo,
# y vuelve a añadir uno solo
sed -e 's/.*screen -dmS portal \{1,\}sh.*/@@BORRAR@@/' -e '/--- mini-upload-portal: watchdog/,$d' \
    /opt/bootlocal.sh | grep -v '@@BORRAR@@' > /tmp/bl.new
cat >> /tmp/bl.new <<'EOF'

# --- mini-upload-portal: watchdog (levanta bin/portal si muere) ---
# tc puede bindear :80 porque el piso de puertos privilegiados se baja aqui.
sysctl -w net.ipv4.ip_unprivileged_port_start=80
su - tc -c "screen -dmS portal-watchdog sh -c 'cd /mnt/xvdb1/app && exec ./supervisor.sh'"
EOF
if sudo cp /tmp/bl.new /opt/bootlocal.sh && sudo chmod 775 /opt/bootlocal.sh; then
    echo "  bootlocal.sh actualizado (respaldos: /opt/bootlocal.sh.bak-$TS y $APP/bootlocal.sh.bak-$TS)"
    echo "  bloques de watchdog: $(grep -c 'mini-upload-portal: watchdog' /opt/bootlocal.sh)   lanzamientos de portal-watchdog: $(grep -c 'portal-watchdog' /opt/bootlocal.sh)"
else
    echo "  *** no se pudo escribir /opt/bootlocal.sh ***"
fi
echo "  contenido:"; sed 's/^/    | /' /opt/bootlocal.sh

echo "== 6. estado final =="
echo "  portal pids: [$(pidof portal)]   listeners :80: $(netstat -ltn 2>/dev/null | grep -c ':80 ')"
echo "  watchdog:    [$(cat /tmp/portal-supervisor.lock 2>/dev/null)] (uid=$(awk '/^Uid:/{print $2; exit}' /proc/$(cat /tmp/portal-supervisor.lock 2>/dev/null)/status 2>/dev/null))"
echo -n "  http local: "; curl -s -m 3 -o /dev/null -w "%{http_code}\n" http://127.0.0.1/
echo -n "  /api/files sin token: "; curl -s -m 3 -o /dev/null -w "%{http_code}\n" http://127.0.0.1/api/files
echo "  ultimos 8 del supervisor.log:"; tail -8 "$APP/supervisor.log" | sed 's/^/    /'
echo "  ultimos 4 del portal.log:"; tail -4 "$APP/portal.log" | sed 's/^/    /'

if [ -n "$recuperado" ] && [ "$(pidof portal | wc -w)" = "1" ]; then
    echo "== RESULTADO: OK (watchdog operativo y una sola instancia) =="
else
    echo "== RESULTADO: REVISAR (recuperacion=$( [ -n "$recuperado" ] && echo si || echo no ); instancias=$(pidof portal | wc -w)) =="
fi
