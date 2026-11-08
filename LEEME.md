# Firmware del Horno PWM · v2.0 (rev D, PIC16F1827 / PIC16F1847)

Equipo Semibuhos · 6 de noviembre de 2026 · continúa el handoff `HANDOFF_Firmware_Horno_PWM.md`

## Archivos

| Archivo | Qué es |
|---|---|
| `horno_pwm_16f18x7.c` | Firmware v2 (un solo archivo, para los dos chips) |
| `hardware/` | Proyecto de Proteus (esquemático y PCB rev D) y lista de materiales |
| `original/horno_pwm_16f18x7_v1.c` | Firmware v1 tal como venía en el Apéndice A del handoff |
| `banco/` | Banco de pruebas en la PC (reemplaza al Apéndice B); también compila con XC8 si está instalado |
| `pc/horno_pc.py` | Programa de PC: telemetría, CSV, gráfica y carga de perfiles |
| `pc/perfil_ejemplo.txt` | Perfil de ejemplo para `horno_pc.py cargar` |
| `ia_arbol.h` | Árbol de decisión de la IA (generado por `ia/entrenar.py`, no editar a mano) |
| `ia/` | Generación del dataset, entrenamiento, verificación y resultados de la IA |

El mapa de pines no cambió. Ningún cambio necesita tocar la placa.

---

## 1. Qué se agregó (sección 7 del handoff)

Todo junto no cabe en los 4K del PIC16F1827, así que los **extras** solo se compilan en el **PIC16F1847** (regla 3 del handoff). Lo esencial está en los dos.

| # | Mejora | 16F1827 | 16F1847 |
|---|---|---|---|
| 7.1 | Embalamiento (PWM alto y no sube) · falla 3 | Sí | Sí |
| 7.2 | Validación cruzada (mediana) y sensor congelado · columna `desc` | Sí | Sí |
| 7.3 | Causa del reinicio · `FW 2.0;RST=...` al encender | Sí | Sí |
| 7.4 | Códigos de falla · `F;n` | Sí | Sí |
| 7.5 | Tiempo máximo de proceso · falla 4 | Sí | Sí |
| 7.6 | PID con derivada sobre la medición y antiwindup condicional | Sí | Sí |
| 7.7 | Autoajuste por relé (Tyreus-Luyben) · comando `A` | **No** | Sí |
| 7.8 | Ganancias por serial en la EEPROM · comando `K` | Sí | Sí |
| 7.9 | Modo ventana para SSR · `MODO_SALIDA` | Sí | Sí |
| 7.10 | Sensor de control seleccionable · comando `C` | Sí | Sí |
| 7.11 | 4 ranuras de perfil · `R`, `L n`, `B n`, `P n ...`, `D n` | Sí | Sí |
| 7.12 | `T s` y suma `*hh` | Sí | Sí |
| 7.12 | `?` (estado) y `H` (ayuda) | **No** | Sí |
| 7.13 | Programa de PC en Python · `pc/horno_pc.py` | Sí | Sí |
| 7.14 | Registro de las últimas 4 fallas · comando `F` | **No** | Sí |
| 7.15 | Medir el +5V con la FVR · columna `vdd` (en el 1827 sale 0) | **No** | Sí |
| 7.16 | Usar RA4 | No (requiere cambiar la placa) | No |
| 7.17 | Reloj a 32 MHz | No (el lazo de 1 s sobra a 4 MHz) | No |

En el 16F1827, los comandos que no tiene (`A`, `?`, `H`, `F`) contestan `ERR`. Cada extra se puede forzar con `-D` (`USAR_AUTOAJUSTE`, `USAR_ESTADO`, `USAR_REGISTRO`, `USAR_VDD`), pero en el 16F1827 no cabe ninguno más.

Extra:
- **Falla 5, calor con el PWM en 0:** si la temperatura sube más de 20 °C sobre el mínimo visto con la resistencia apagada (MOSFET en corto). El PIC no puede cortar la corriente en ese caso, pero avisa y lo registra.
- **Corrección de la v1:** la columna `seg` se volvía negativa a las 9.1 h (se imprimía como `int16`). Ahora es de 32 bits.
- **Renglón demasiado largo:** antes se cortaba y se ejecutaba lo que quedaba. Ahora contesta `ERR`.
- La EEPROM ya no se reescribe si el valor no cambia (menos desgaste).

## 2. Cambios que hay que tomar en cuenta

1. **9 tramos por ranura** (antes 40 en una sola). Una EEPROM de la v1 con 9 tramos o menos se conserva como ranura 1. Con más de 9 se carga el perfil de fábrica.
2. **Telemetría:** se agregaron 4 columnas al final: `...;est;ctl;desc;cod;vdd`. Las 10 primeras son iguales; un programa de la v1 que lea por posición sigue funcionando.
3. **Al encender** hay un renglón nuevo después del nombre: `FW 2.0;RST=POR`.
4. **Después de un reinicio por watchdog o por la pila no arranca solo** aunque `AUTOARRANQUE = 1`: un reinicio así indica que el programa falló, así que espera el comando `I`. Con POR, BOR o MCLR sí arranca.
5. **`prom` es el promedio de los sensores aceptados** (sin los descartados). La seguridad (sobretemperatura) sigue usando **todas** las lecturas válidas, aunque estén descartadas.
6. **Estado nuevo `A`** (autoajuste) en la columna `est`.
7. Las ganancias de fábrica (`KP=32, KI=16, KD=0`) dan **exactamente el mismo PI de la v1** (`KP=8, KI=1`), con otras unidades más finas. Lo único distinto es el antiwindup (condicional en vez de solo recorte).

## 3. Protocolo (9600 8N1, comando + Enter, acepta minúsculas)

`[n]` = ranura 1–4; sin ella se usa la activa.

| Comando | Acción | Respuesta |
|---|---|---|
| `I` | Iniciar el perfil de la ranura activa | OK / ERR |
| `S` | Detener (no borra una falla; también detiene el autoajuste) | OK |
| `L [n]` | Listar: `N=n` y renglones `i;temp;rampa;meseta` | lista |
| `B [n]` | Borrar un perfil | OK / ERR |
| `P [n] t,r,m` | Agregar un tramo (0 ≤ t ≤ T_MAX−10, 0 ≤ r, m ≤ 1000) | OK / ERR |
| `D [n]` | Perfil de fábrica en la ranura | OK / ERR |
| `R n` | Ranura activa (se guarda) | OK / ERR |
| `K` | Ver las ganancias | `K;kp;ki;kd` |
| `K kp,ki,kd` | Cambiar las ganancias (kp, ki ≤ 1000; kd ≤ 30000), se guardan | OK / ERR |
| `C m` | Control con: 0 promedio, 1–5 un sensor, 6 máximo, 7 mediana | OK / ERR |
| `A t[,0]` | Autoajuste alrededor de t °C (`,0` = solo PI) | OK, y al final `A;OK;kp;ki;kd;Tu;pp` o `A;ERR` |
| `T s` | Telemetría cada s segundos (0 = apagada) | OK / ERR |
| `?` | Estado: `EST=..;COD=..;R=..;N=..;TR=..;FASE=R/M;KP;KI;KD;CTL;TEL;VDD;RST` | renglón |
| `F` | Registro de fallas: `REG` y renglones `i;código;segundo;temp_máx` | lista |
| `F 0` | Borrar el registro | OK |
| `H` | Ayuda corta | renglón |

- `B`, `P`, `D`, `R` y `A` se rechazan mientras corre un perfil o el autoajuste (manda `S` primero). `K`, `C` y `T` sí se aceptan corriendo.
- **Suma de verificación opcional:** `comando*hh`, con hh = XOR en hexadecimal de los caracteres antes del `*`. Ejemplo `L*4C`. Si no cuadra, `ERR`. `horno_pc.py` la usa siempre.
- Si un sensor elegido con `C 1`–`C 5` falla o se descarta, el control usa el promedio.

**Telemetría:** `seg;sp;prom;pwm;T1;T2;T3;T4;T5;est;ctl;desc;cod;vdd`
`ctl` = temperatura de control (°C), `desc` = sensores descartados (bit 0 = T1 … bit 4 = T5), `cod` = código de falla, `vdd` = +5V en mV.

### Códigos de falla (`F;n`, la falla se queda hasta apagar y prender)

| n | Causa | Cuándo |
|---|---|---|
| 1 | Sobretemperatura | Cualquier lectura válida > `T_MAX_SEGURIDAD_C` (250 °C), en cualquier estado |
| 2 | Sensores insuficientes | Menos de `MIN_SENSORES_OK` aceptados en C o A |
| 3 | Embalamiento | PWM ≥ 80 %, ≥ 2 °C bajo el setpoint por 180 s y sube < 5 °C |
| 4 | Tiempo máximo | El perfil pasa de su duración + 1/4 + 30 min |
| 5 | Calor sin PWM | PWM en 0 y sube > 20 °C sobre el mínimo visto |
| 6 | Autoajuste | La temperatura pasa del objetivo + 25 °C |

Solo se guarda la primera causa. Todos los umbrales están en la sección de configuración del `.c`.

## 4. Mapa de la EEPROM (256 B)

| Dirección | Contenido |
|---|---|
| 0, 57, 114, 171 | Ranuras 1–4: `0xA5`, n.º de tramos, suma de 8 bits, 9 tramos × 6 B (`temp`, `rampa`, `meseta`, int16 little-endian) |
| 228 | Configuración: `0x5A`, KP, KI, KD (int16), modo de control, ranura activa, telemetría (s), suma |
| 239 | Registro: siguiente entrada, y 4 × (código, segundo uint16, temp. máx. °C) |

La ranura 1 tiene el mismo formato y dirección que la v1. Si la configuración no es válida se cargan los valores de fábrica y se borra el registro.

## 5. Compilar en MPLAB X

Probado con **MPLAB X v6.35, XC8 v4.00** y el paquete PIC12-16F1xxx_DFP 1.9.258. Compila sin errores para los dos chips.

| Compilación (optimización `s`) | Memoria de programa | RAM |
|---|---|---|
| PIC16F1827 | 3881 / 4096 palabras (94.8 %) | 226 / 384 B |
| PIC16F1827 + `SIMULAR_SENSORES=1` (Proteus) | 4005 / 4096 palabras (97.8 %) | 236 / 384 B |
| PIC16F1847 (todo, con IA) | 6621 / 8192 palabras (80.8 %) | |
| PIC16F1847 + `SIMULAR_SENSORES=1` | 6757 / 8192 palabras (82.5 %) | |

1. Proyecto con el dispositivo **PIC16F1827** (para Proteus) o **PIC16F1847**, compilador **XC8**.
2. Agregar solo `horno_pwm_16f18x7.c` en *Source Files*. **No** definir `BANCO_PC`.
3. **Obligatorio para el 16F1827:** *Project Properties → XC8 Global Options → XC8 Compiler → Optimizations → Optimization level = **s***. Con 0, 1 o 2 **no cabe**. El 16F1847 cabe con cualquier nivel.
4. Para Proteus: *XC8 Compiler → Define macros* → `SIMULAR_SENSORES=1`.
5. El 16F1827 para Proteus queda con ~90 palabras libres: casi cualquier función nueva tendrá que ir dentro de `#if USAR_...` y solo en el 16F1847.
6. Si al reiniciar por watchdog sale `RST=POR` o `RST=?` en lugar de `RST=WDT`, activa en XC8 *Linker → Runtime → "Backup reset condition flags"* y lee `__timeout` en lugar de `STATUSbits.nTO`.

Todas las macros de configuración se pueden cambiar en el archivo o con `-D`.

## 6. Banco de pruebas en la PC

```bash
powershell -ExecutionPolicy Bypass -File banco\correr_banco.ps1
```

(con gcc: `bash banco/correr_banco.sh`). Usa gcc si está en el PATH; si no, Visual Studio. Los resultados quedan en `banco/salida/` (`*.log` con las revisiones, `*.txt` con la salida serial completa).

A diferencia del Apéndice B, el banco **no** usa `SIMULAR_SENSORES`: modela los MAX6675 **bit a bit en la pata SO**, toma la potencia de los registros `CCPR1L/CCP1CON` (o de `LATB3` en modo ventana) y exige la secuencia `0x55/0xAA` con `GIE = 0` para escribir la EEPROM. El horno es el modelo del handoff (325 °C al 100 %, τ = 600 s) más 20 s de retraso del termopar y ±0.25 °C de ruido.

La compilación `real` es la del 16F1847 (todo); `r1827` es la del 16F1827 (sin extras) y corre los mismos escenarios, esperando `ERR` en los comandos que no tiene. Si XC8 está instalado, el script también compila para los dos chips reales y revisa que quepan.

**Resultado: 36 corridas en 6 compilaciones de PC + 4 compilaciones con XC8, todo pasa, sin advertencias con `/W4`.**

| Escenario | Qué prueba | Resultado clave |
|---|---|---|
| perfil (1847, 1827, ventana, sim) | Perfil de fábrica completo | Mesetas ±1 °C; sobrepaso 5 °C (100 °C) y 3 °C (180 °C) con las ganancias de fábrica |
| comandos | Todos los comandos, ranuras, sumas, renglón largo | 56 revisiones bien |
| persistencia | Ganancias, ranura activa y telemetría sobreviven al reinicio | Bien |
| sobretemp | TC4 a 260 °C | `F;1` en 1 s, PWM 0, `I` rechazado, registro |
| sin_sensores | 4 de 5 fallan y luego el 5.º | Sigue con 1; `F;2` al perder el último |
| embalamiento | Resistencia quemada | `F;3` a los 253 s |
| corto | MOSFET en corto con el horno detenido | `F;5` a los 65 s |
| atipico | TC2 marca +60 °C | Se descarta, el promedio no se mueve |
| congelado1 / congelado5 | TC3 de control congelado / todos | Se descarta TC3 y sigue / `F;2` |
| autoajuste (solo 1847) | `A 150` y luego el perfil | KP=142, KI=54, KD=1707; sobrepaso 2 °C |
| wdt, bor, mclr, pila | Causa del reinicio | Se informa bien; tras WDT/pila no arranca |
| migracion / corrupto | EEPROM de la v1 / con suma mala | Se conserva / perfil de fábrica |
| vdd (solo 1847) | +5V a 4.5 V y 5.4 V | `AVISO;VDD;...` |
| prueba, simfalla | `MODO_PRUEBA`, `SIMULAR_SENSORES` con falla | PWM 512; `F;2` en el segundo 600 |

**Hallazgo de control:** con las ganancias de fábrica (las de la v1), una rampa muy rápida (`P 120,5,10`: 95 °C en 5 min, el horno casi al 100 %) **se pasa 11 °C** antes de asentarse. Las ganancias del autoajuste dejan el perfil de fábrica en 1–2 °C. En el horno real conviene correr `A` (o ajustar con `K`) antes de usarlo.

## 7. Probar en Proteus 9.02 (PIC16F1827)

1. El firmware ya compilado para Proteus está en `hardware/horno_proteus.hex` (16F1827, `SIMULAR_SENSORES=1`, `AUTOARRANQUE=1`, `-Os`). Para cambiar opciones, recompilar con `SIMULAR_SENSORES=1` y reemplazar ese archivo.
2. U2 = PIC16F1827, **Program File** = `horno_proteus.hex` (solo el nombre, sin carpeta: Proteus lo busca junto al `.pdsprj`), **Clock** = 4 MHz. No usar una ruta absoluta ni la carpeta temporal de VSM Studio (`AppData\Local\Temp\VSM Studio\...`): en otra computadora no existe y la simulación no arranca.
3. **VIRTUAL TERMINAL:** RXD al pin 8 (TX), TXD al pin 7 (RX), 9600 baudios.
4. Al arrancar debe salir `HORNO PWM rev D - PIC16F1827`, `FW 2.0;RST=POR`, el perfil y el encabezado de 14 columnas.
5. **Osciloscopio** en el pin 9: PWM de 0 a 5 V a 244 Hz.
6. Prueba los comandos `K`, `L`, `L 2`, `C 7`, `T 5`, `S`, `I` (en el 16F1827 `?`, `H`, `F` y `A` contestan `ERR`, es normal). El modelo corre ~50 veces más rápido: el perfil de fábrica (100 min) dura unos 2 min.
7. Fallas simuladas: `SIM_FALLA_SEG = 600` con `SIM_FALLA_MODO` = 0 (sensores fuera → `F;2`), 1 (TC3 congelado), 2 (TC2 +50 °C → `desc` = 2), 3 (resistencia quemada → `F;3`), 4 (MOSFET en corto).
8. En el 16F1827 la columna `vdd` siempre es 0 (la medición solo está en el 16F1847).
9. J9–J14 y U4 en **Exclude from Simulation**.

## 8. Probar en la placa real

**Recomendado: montar el PIC16F1847** en la placa, porque trae el autoajuste, el registro de fallas, `?` y la medición de +5V. Con el 16F1827 funciona todo lo esencial, pero las ganancias se ajustan a mano con `K`.

1. Igual que antes: 12 V en J8–J7, 5 V en el pin 14, cristal en el pin 15. Con el 16F1847, revisa que `vdd` diga ~5000.
2. `MODO_PRUEBA = 1` con la carga de prueba (PWM en el pin 9, compuerta J4–J7, drenador J5–J7).
3. Modo normal con un termopar: desconéctalo durante un perfil → `F;2`. Revisa `F` (registro).
4. **Autoajuste:** con el horno frío, `S` y luego `A 150` (o la temperatura de trabajo). En el modelo del banco tardó 12 min (calentar y 5 ciclos); en el horno real depende de su inercia, con un máximo de 2 h. Al final sale `A;OK;kp;ki;kd;Tu;pp` y las ganancias quedan guardadas. Si sale `A;ERR`, usa `A 150,0` (solo PI) o ajusta con `K`.
5. Embalamiento: con el perfil corriendo, desconecta la resistencia (J3) → `F;3` en unos 4 min (en el banco: 253 s).
6. Si el horno real no llega a alguna temperatura del perfil (PWM al 100 % sin alcanzarla), saltará `F;3`. Es a propósito; baja esa temperatura o sube `RUNAWAY_SEG`.

Nunca pongas pinzas de tierra en GND y GND_P al mismo tiempo.

## 9. Programa de PC (`pc/horno_pc.py`)

```bash
pip install pyserial matplotlib
python pc/horno_pc.py monitor COM5 --csv corrida.csv --graf
python pc/horno_pc.py cargar COM5 pc/perfil_ejemplo.txt --ranura 2
python pc/horno_pc.py cmd COM5 "A 150"
python pc/horno_pc.py archivo banco/salida/banco_real_perfil.txt --csv x.csv --graf
```

Lee la telemetría de la v1 y de la v2. `cargar` borra la ranura, manda cada tramo con suma de verificación y verifica con `L`. Probado sin puerto (modo `archivo` y funciones del protocolo); falta probarlo con el adaptador USB real.

## 10. IA de diagnóstico (solo PIC16F1847)

Cada 10 s el firmware calcula 10 características enteras y las pasa por un árbol de decisión (`ia_arbol.h`). Si la misma clase sale 6 veces seguidas (60 s) manda `IA;n` por serial: 0 normal, 1 resistencia abierta, 2 MOSFET en corto, 4 termopar congelado, 5 termopar con deriva. Es solo un aviso; la protección siguen siendo las reglas. `?` muestra la clase actual (`IA=n`).

Para regenerar todo (necesita Python 3, sin librerías extra, y el banco compilado):

```bash
python ia/generar_datos.py 900 2026
```
```bash
python ia/entrenar.py
```
```bash
python ia/verificar.py
```

`generar_datos.py` corre el firmware real contra 900 hornos simulados distintos con fallas al azar; `entrenar.py` entrena el árbol, lo compara con las reglas y escribe `ia_arbol.h` y `ia/resultados.md`; `verificar.py` comprueba que el árbol dentro del firmware da la misma predicción que Python en todas las evaluaciones. Después de reentrenar, vuelve a compilar el firmware.

Resultados (270 corridas de prueba no vistas): resistencia abierta detectada en 137 s (reglas: 297 s), termopar congelado en 141 s (reglas: 789 s), termopar con deriva en 129 s (reglas: 372 s), 1 falsa alarma en 270 corridas. Limitaciones: datos solo simulados; la resistencia que se degrada poco a poco no está modelada; el corto lo detecta la IA en 62 % de los casos (las reglas en 100 %).

## 11. Pendiente y riesgos

- **Proteus** con el PIC16F1827 y **hardware real** (lo único que falta probar).
- El 16F1827 está casi lleno (94.8 % normal, 97.8 % para Proteus). Lo nuevo debe ir a los extras del 16F1847.
- Las ganancias de fábrica son las de la v1; en el horno real hay que correr el autoajuste.
- La FVR del PIC tiene una tolerancia de unos cuantos %, así que `vdd` es aproximado: el aviso de 4.75–5.25 V puede saltar de más o de menos. Es solo un aviso, no una falla.
- El registro guarda el segundo como `uint16` (se satura a 65535 s ≈ 18 h).
