#!/usr/bin/env bash
# correr_banco.sh - Compila y corre el banco de pruebas con gcc (Linux, macOS, MSYS2).
#   bash banco/correr_banco.sh
set -u
cd "$(dirname "$0")"
mkdir -p salida
CC="gcc -std=c99 -Wall -Wextra -Wno-unknown-pragmas -DBANCO_PC -I."

compilar() {  # nombre, defines...
    local n=$1; shift
    local d=""; for x in "$@"; do d="$d -D$x"; done
    $CC $d banco.c -lm -o "salida/$n" || { echo "Fallo la compilacion de $n"; exit 1; }
    echo "  $n ($*)"
}

echo "=== Compilando ==="
compilar banco_real     _16F1847                 # todo
compilar banco_r1827    _16F1827                 # sin extras
compilar banco_ventana  _16F1827 MODO_SALIDA=1
compilar banco_sim      _16F1827 SIMULAR_SENSORES=1
compilar banco_simfalla _16F1827 SIMULAR_SENSORES=1 SIM_FALLA_SEG=600 SIM_FALLA_MODO=0
compilar banco_prueba   _16F1827 MODO_PRUEBA=1

malas=0
correr() {  # exe, escenario, args extra...
    local exe=$1 esc=$2; shift 2
    local tag="${exe}_${esc}"
    ./salida/$exe "$esc" -o "salida/$tag.txt" "$@" > "salida/$tag.log"
    local r=$?
    grep -E '\[FALLA\]|\[nota\]|Resultado|^==' "salida/$tag.log"
    if [ $r -ne 0 ] || ! grep -q 'Resultado \[' "salida/$tag.log"; then malas=$((malas + 1)); echo "  *** $tag: MAL"; fi
}

for b in banco_real banco_r1827; do
    correr $b perfil
    correr $b comandos -ee_out salida/ee_$b.bin
    correr $b persistencia -ee_in salida/ee_$b.bin
    for e in sobretemp sin_sensores embalamiento corto atipico congelado1 congelado5 wdt bor migracion corrupto; do
        correr $b $e
    done
done
for e in autoajuste ia mclr pila vdd; do correr banco_real $e; done
correr banco_ventana perfil
correr banco_sim perfil
correr banco_simfalla simfalla
correr banco_prueba prueba

echo
if [ $malas -ne 0 ]; then echo "$malas corridas con fallas (ver banco/salida/*.log)"; exit 1; fi
echo "Todas las corridas pasaron."
