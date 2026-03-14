#!/bin/bash
# =============================================================================
# stress_validator.sh
# Orquesta servidor + N clientes en paralelo y valida los resultados.
#
# Uso: ./stress_validator.sh [opciones]
#   --modo       local|dist      (default: dist)
#   --clientes   N               (default: 10)
#   --cliente    ./ejecutable    (default: ./cliente_local)
#   --servidor   ./ejecutable    (default: ./servidor_mq)
#   --timeout    segundos        (default: 30)
#   --limpiar                    Borra /dev/mqueue antes de empezar
#   --no-servidor                No arranca servidor (modo local)
# =============================================================================

# ── Colores ──────────────────────────────────────────────────────────────────
GREEN="\033[32m"; RED="\033[31m"; YELLOW="\033[33m"
CYAN="\033[36m";  BOLD="\033[1m"; RESET="\033[0m"

ok()   { echo -e "${GREEN}✔ $*${RESET}"; }
fail() { echo -e "${RED}✖ $*${RESET}"; }
warn() { echo -e "${YELLOW}⚠ $*${RESET}"; }
info() { echo -e "${CYAN}ℹ $*${RESET}"; }
bold() { echo -e "${BOLD}$*${RESET}"; }

# ── Defaults ─────────────────────────────────────────────────────────────────
MODO="dist"
N_CLIENTES=50
CLIENTE_EXE="./cliente_local"
SERVIDOR_EXE="./servidor_mq"
TIMEOUT=10000
LIMPIAR=0
NO_SERVIDOR=0
TMPDIR_RES="/tmp/stress_validator_$$"

# ── Parseo de argumentos ──────────────────────────────────────────────────────
while [[ $# -gt 0 ]]; do
    case "$1" in
        --modo)       MODO="$2";        shift 2 ;;
        --clientes)   N_CLIENTES="$2";  shift 2 ;;
        --cliente)    CLIENTE_EXE="$2"; shift 2 ;;
        --servidor)   SERVIDOR_EXE="$2";shift 2 ;;
        --timeout)    TIMEOUT="$2";     shift 2 ;;
        --limpiar)    LIMPIAR=1;        shift   ;;
        --no-servidor)NO_SERVIDOR=1;    shift   ;;
        *) warn "Argumento desconocido: $1"; shift ;;
    esac
done

mkdir -p "$TMPDIR_RES"
trap "rm -rf $TMPDIR_RES" EXIT

# ── Cabecera ──────────────────────────────────────────────────────────────────
bold "══════════════════════════════════════════════════════════════"
bold "  Stress Test Validator  |  modo=$MODO  clientes=$N_CLIENTES"
bold "══════════════════════════════════════════════════════════════"
echo

# ── 1. Limpiar colas POSIX ────────────────────────────────────────────────────
if [[ $LIMPIAR -eq 1 ]]; then
    if [[ -d /dev/mqueue ]]; then
        COUNT=$(ls /dev/mqueue | wc -l)
        rm -f /dev/mqueue/*
        info "Eliminadas $COUNT colas de /dev/mqueue"
    fi
fi

# ── 2. Arrancar servidor ──────────────────────────────────────────────────────
SERVER_PID=""
if [[ $MODO == "dist" && $NO_SERVIDOR -eq 0 ]]; then
    if [[ ! -x "$SERVIDOR_EXE" ]]; then
        warn "Servidor '$SERVIDOR_EXE' no encontrado o no ejecutable — saltando"
    else
        info "Arrancando servidor: $SERVIDOR_EXE"
        "$SERVIDOR_EXE" > "$TMPDIR_RES/servidor.log" 2>&1 &
        SERVER_PID=$!
        sleep 1
        if kill -0 "$SERVER_PID" 2>/dev/null; then
            ok "Servidor activo (PID=$SERVER_PID)"
        else
            fail "El servidor terminó inesperadamente"
            cat "$TMPDIR_RES/servidor.log"
        fi
    fi
fi

# ── 3. Lanzar clientes en paralelo ────────────────────────────────────────────
if [[ ! -x "$CLIENTE_EXE" ]]; then
    fail "Cliente '$CLIENTE_EXE' no encontrado o no ejecutable"
    exit 1
fi

info "Lanzando $N_CLIENTES clientes en paralelo…"
echo
T_START=$(date +%s%N)
PIDS=()

for (( i=0; i<N_CLIENTES; i++ )); do
    OUT="$TMPDIR_RES/cliente_${i}.out"
    ERR="$TMPDIR_RES/cliente_${i}.err"
    RET="$TMPDIR_RES/cliente_${i}.ret"

    (
        timeout "$TIMEOUT" "$CLIENTE_EXE" "$i" \
            > "$OUT" 2> "$ERR"
        echo $? > "$RET"
    ) &
    PIDS+=($!)
done

# Esperar a todos
for pid in "${PIDS[@]}"; do
    wait "$pid"
done

T_END=$(date +%s%N)
ELAPSED=$(( (T_END - T_START) / 1000000 ))
ok "Todos los clientes terminaron en ${ELAPSED}ms"
echo

# ── 4. Parar servidor ─────────────────────────────────────────────────────────
if [[ -n "$SERVER_PID" ]]; then
    kill -SIGINT "$SERVER_PID" 2>/dev/null
    wait "$SERVER_PID" 2>/dev/null
    ok "Servidor detenido"
fi

# ── 5. Parsear resultados y validar ──────────────────────────────────────────
SEP="──────────────────────────────────────────────────────────────────────"
bold "$SEP"
printf "${BOLD}%-8s %6s %6s %7s %10s %11s %7s %8s${RESET}\n" \
    "CLIENTE" "OK" "ERR" "TOTAL" "ASSERT_OK" "ASSERT_ERR" "T(ms)" "ESTADO"
bold "$SEP"

GLOBAL_OK=0
GLOBAL_ERR=0
GLOBAL_ASSERT_ERR=0
CLIENTS_PASSED=0
CLIENTS_TIMEOUT=0
EXPECTED_OPS=$(( 5 * 100 ))   # 5 ops x N_OPS (ajusta si cambias N_OPS en el .c)

for (( i=0; i<N_CLIENTES; i++ )); do
    OUT="$TMPDIR_RES/cliente_${i}.out"
    ERR_F="$TMPDIR_RES/cliente_${i}.err"
    RET_F="$TMPDIR_RES/cliente_${i}.ret"

    RETCODE=0
    [[ -f "$RET_F" ]] && RETCODE=$(cat "$RET_F")

    # Detectar timeout (timeout devuelve 124)
    TIMED_OUT=0
    [[ "$RETCODE" -eq 124 ]] && TIMED_OUT=1

    # Parsear línea resumen: [C3] Done. OK=500 ERR=0 TOTAL=500
    OPS_OK=0; OPS_ERR=0; OPS_TOTAL=0
    if [[ -f "$OUT" ]]; then
        SUMMARY=$(grep -oE 'OK=[0-9]+ ERR=[0-9]+ TOTAL=[0-9]+' "$OUT" | tail -1)
        if [[ -n "$SUMMARY" ]]; then
            OPS_OK=$(echo    "$SUMMARY" | grep -oE 'OK=[0-9]+'    | grep -oE '[0-9]+')
            OPS_ERR=$(echo   "$SUMMARY" | grep -oE 'ERR=[0-9]+'   | grep -oE '[0-9]+' | head -1)
            OPS_TOTAL=$(echo "$SUMMARY" | grep -oE 'TOTAL=[0-9]+' | grep -oE '[0-9]+')
        fi
    fi

    # Parsear asserts: [C3]  asserts    : OK=490 ERR=10
    ASSERT_OK=0; ASSERT_ERR=0
    if [[ -f "$OUT" ]]; then
        ASSERT_LINE=$(grep -i 'assert' "$OUT" | tail -1)
        if [[ -n "$ASSERT_LINE" ]]; then
            ASSERT_OK=$(echo  "$ASSERT_LINE" | grep -oE 'OK=[0-9]+'  | grep -oE '[0-9]+')
            ASSERT_ERR=$(echo "$ASSERT_LINE" | grep -oE 'ERR=[0-9]+' | grep -oE '[0-9]+' | head -1)
        fi
    fi

    # Estado del cliente
    PASSED=1
    [[ $TIMED_OUT -eq 1 ]]       && PASSED=0
    [[ "$RETCODE" -ne 0 ]]       && PASSED=0
    [[ "${OPS_ERR:-0}" -ne 0 ]]  && PASSED=0
    [[ "${ASSERT_ERR:-0}" -ne 0 ]]&& PASSED=0

    if [[ $PASSED -eq 1 ]]; then
        ESTADO=$(echo -e "${GREEN}PASS${RESET}")
        CLIENTS_PASSED=$(( CLIENTS_PASSED + 1 ))
    elif [[ $TIMED_OUT -eq 1 ]]; then
        ESTADO=$(echo -e "${RED}TIMEOUT${RESET}")
        CLIENTS_TIMEOUT=$(( CLIENTS_TIMEOUT + 1 ))
    else
        ESTADO=$(echo -e "${RED}FAIL${RESET}")
    fi

    GLOBAL_OK=$(( GLOBAL_OK + ${OPS_OK:-0} ))
    GLOBAL_ERR=$(( GLOBAL_ERR + ${OPS_ERR:-0} ))
    GLOBAL_ASSERT_ERR=$(( GLOBAL_ASSERT_ERR + ${ASSERT_ERR:-0} ))

    printf "  C%-5d %6s %6s %7s %10s %11s %7s %s\n" \
        "$i" \
        "${OPS_OK:-?}" \
        "${OPS_ERR:-?}" \
        "${OPS_TOTAL:-?}" \
        "${ASSERT_OK:-?}" \
        "${ASSERT_ERR:-?}" \
        "$ELAPSED" \
        "$ESTADO"
done

bold "$SEP"
printf "${BOLD}  %-6s %6d %6d %7d${RESET}\n" \
    "TOTAL" "$GLOBAL_OK" "$GLOBAL_ERR" "$(( GLOBAL_OK + GLOBAL_ERR ))"
bold "$SEP"
echo

# ── 6. Checks globales ────────────────────────────────────────────────────────
ALL_PASS=1

check() {
    local desc="$1" cond="$2"
    if [[ "$cond" -eq 1 ]]; then
        ok "$desc"
    else
        fail "$desc"
        ALL_PASS=0
    fi
}

check "Todos los clientes terminaron (${CLIENTS_PASSED}/${N_CLIENTES})" \
    $(( CLIENTS_PASSED == N_CLIENTES ))

check "Cero errores de operación globales (ERR=$GLOBAL_ERR)" \
    $(( GLOBAL_ERR == 0 ))

check "Cero fallos de asserts globales (ASSERT_ERR=$GLOBAL_ASSERT_ERR)" \
    $(( GLOBAL_ASSERT_ERR == 0 ))

check "Ningún cliente superó el timeout (timeouts=$CLIENTS_TIMEOUT)" \
    $(( CLIENTS_TIMEOUT == 0 ))

echo

if [[ $ALL_PASS -eq 1 ]]; then
    bold "$(ok "═══  STRESS TEST COMPLETO — ${N_CLIENTES} clientes OK  ═══")"
else
    bold "$(fail "═══  HAY FALLOS — revisa los clientes en rojo  ═══")"
fi

# ── 7. Mostrar stderr si hay errores ─────────────────────────────────────────
for (( i=0; i<N_CLIENTES; i++ )); do
    ERR_F="$TMPDIR_RES/cliente_${i}.err"
    if [[ -s "$ERR_F" ]]; then
        warn "Stderr C${i}:"
        head -10 "$ERR_F"
    fi
done

echo
[[ $ALL_PASS -eq 1 ]] && exit 0 || exit 1