#!/bin/sh
# Watchdog del mini-upload-portal.
#
# Lo arranca /opt/bootlocal.sh (en esta máquina /opt es disco real: /dev/xvda1
# montado en /opt, así que persiste entre reboots sin necesidad de filetool.sh).
#
#   · si bin/portal muere, lo vuelve a levantar en <= INTERVALO segundos
#   · si está vivo pero no contesta por HTTP, lo mata y lo levanta
#     (lazo de accept colgado: el proceso existe pero no sirve nada)
#
# Corre como tc. Para no depender de sudo dentro del bucle, bootlocal.sh baja
# net.ipv4.ip_unprivileged_port_start a 80 y así tc puede bindear :80 directamente.
# Si ese ajuste falta, el supervisor cae a `sudo` como respaldo.
#
# Registro: /mnt/xvdb1/app/supervisor.log (rota solo: conserva los últimos 256 KB)

APP=/mnt/xvdb1/app
BIN=$APP/bin/portal
PORTAL_LOG=$APP/portal.log
LOG=$APP/supervisor.log
LOCK=/tmp/portal-supervisor.lock

INTERVALO=10          # segundos entre sondeos
FALLOS_MAX=3          # ~30 s sin contestar antes de reiniciar
MAX_LOG=1048576       # 1 MB

log() {
    echo "$(date -u '+%Y-%m-%d %H:%M:%S') $*" >> "$LOG"
    if [ -f "$LOG" ] && [ "$(wc -c < "$LOG" 2>/dev/null || echo 0)" -gt "$MAX_LOG" ]; then
        tail -c 262144 "$LOG" > "$LOG.tmp" 2>/dev/null && mv -f "$LOG.tmp" "$LOG"
    fi
}

# OJO: en esta máquina `pgrep -x portal` NO encuentra el proceso (aunque comm sea
# "portal"); `pidof` sí. Usar pidof para detectar y matar, no pgrep.
portal_pid() {
    pidof -s portal 2>/dev/null || pidof portal 2>/dev/null | cut -d" " -f1
}

# --- una sola instancia (si renace, se pisa a sí misma y el portal se quedaría sin bindear) ---
if [ -f "$LOCK" ]; then
    old=$(cat "$LOCK" 2>/dev/null)
    if [ -n "$old" ] && [ -d "/proc/$old" ]; then
        echo "supervisor ya corriendo (pid $old)" >&2
        exit 0
    fi
fi
echo $$ > "$LOCK"

# --- cómo bindear :80 sin root (y con respaldo a sudo si el sysctl falta) ---
piso=$(cat /proc/sys/net/ipv4/ip_unprivileged_port_start 2>/dev/null || echo 1024)
if [ "$piso" -le 80 ] 2>/dev/null; then
    ARRANCA="$BIN"
else
    ARRANCA="sudo $BIN"
fi

levanta() {
    cd "$APP" || return 1
    # El log puede haber quedado root-owned (si alguien arrancó el portal con sudo
    # desde una shell root). Como tc no puede abrirlo para append, el arranque
    # MORIRÍA antes de ejecutar el binario y sin dejar rastro: lo rotamos si no
    # es escribible (renombrar solo necesita permiso sobre el directorio).
    if ! : >> "$PORTAL_LOG" 2>/dev/null; then
        log "AVISO: $PORTAL_LOG no es escribible por $(id -un) -> rotando a portal.log.root"
        mv -f "$PORTAL_LOG" "$PORTAL_LOG.root" 2>/dev/null || rm -f "$PORTAL_LOG" 2>/dev/null
    fi
    # </dev/null + nohup: el hijo no se queda agarrado a la tty del supervisor
    # (si no, quien lance esto por ssh cuelga esperando a que cierre el pipe).
    nohup $ARRANCA >> "$PORTAL_LOG" 2>&1 < /dev/null &
    # Sondear en vez de un sleep fijo: en frío (páginas de disco fuera de caché) el
    # binario tarda ~6-8 s en bindear, y un seul sleep 5 lo daba por muerto y provocaba
    # un segundo arranque duplicado en :80.
    p=""; i=0
    while [ $i -lt 15 ]; do
        sleep 1
        p=$(portal_pid)
        [ -n "$p" ] && break
        i=$((i + 1))
    done
    if [ -n "$p" ]; then
        n=$(pidof portal | wc -w)
        log "levantado (pid $p)$([ "$n" -gt 1 ] && echo " AVISO: $n instancias escuchando en :80")"
    else
        log "NO se pudo levantar en 15 s; revisa $PORTAL_LOG"
        tail -3 "$PORTAL_LOG" 2>/dev/null | sed 's/^/  portal.log| /' >> "$LOG"
    fi
}

fallas=0
log "supervisor iniciado (pid $$, arranca='$ARRANCA', piso_puertos=$piso), cada ${INTERVALO}s"

while :; do
    if [ -z "$(portal_pid)" ]; then
        log "portal no está corriendo -> levantando"
        levanta
        fallas=0
        sleep "$INTERVALO"
        continue
    fi

    code=$(curl -m 5 -s -o /dev/null -w '%{http_code}' http://127.0.0.1/ 2>/dev/null)
    case "$code" in
        200|302|401|404)
            fallas=0                                  # contestó: el lazo de accept está vivo
            ;;
        *)
            fallas=$((fallas + 1))
            log "sonda HTTP=$code sin respuesta ($fallas/$FALLOS_MAX)"
            if [ "$fallas" -ge "$FALLOS_MAX" ]; then
                log "portal sin responder -> reiniciando"
                p=$(portal_pid)
                if [ -n "$p" ]; then
                    kill "$p" 2>/dev/null || sudo kill "$p" 2>/dev/null
                    sleep 2
                    kill -9 "$p" 2>/dev/null || sudo kill -9 "$p" 2>/dev/null
                fi
                levanta
                fallas=0
            fi
            ;;
    esac

    # limpieza semanal de restos que ni el propio portal sabe que son suyos
    # (subidas abortadas hace días, archivos sin fila en la BD). Descomentar para activar:
    # dow=$(date +%u); hour=$(date +%H)
    # if [ "$dow" = "7" ] && [ "$hour" = "04" ]; then
    #     log "barrido semanal de huerfanos"
    #     PORTAL_SWEEP_ORPHANS=dry "$BIN" >> "$LOG" 2>&1
    #     PORTAL_SWEEP_ORPHANS=run "$BIN" >> "$LOG" 2>&1
    # fi

    sleep "$INTERVALO"
done
