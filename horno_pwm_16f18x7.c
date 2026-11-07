/*
 * ============================================================================
 *  Controlador Horno PWM  -  rev D (con puerto serial)  -  Semibuhos
 *  Firmware v2.0
 *  Microcontrolador: PIC16F1827 o PIC16F1847, cristal de 4 MHz
 *  (reemplazo directo del PIC16F628A: mismas 18 patas, misma placa)
 *
 *  El mismo archivo sirve para los dos chips: solo elige el dispositivo
 *  en MPLAB X. Usa el PIC16F1827 para simular en Proteus (el 16F1847 no
 *  tiene modelo de simulación); en la placa real puedes montar cualquiera.
 *  Compilador: MPLAB XC8
 * ============================================================================
 *
 *  Qué hace:
 *   - Lee hasta 5 termopares tipo K con módulos MAX6675 (TC1 a TC5).
 *     Descarta el sensor que se aleja de la mediana o que se queda
 *     "congelado" mientras se calienta.
 *   - Sigue un perfil térmico (rampas y mesetas) guardado en la EEPROM.
 *     Hay 4 ranuras de perfil de 9 tramos cada una.
 *   - Controla la resistencia con PWM en RB3 (CCP1) y un control PID
 *     (o con ventana de tiempo para un relé de estado sólido).
 *   - Autoajuste del PID por relé (comando A).
 *   - Manda la temperatura por serial cada 10 s (configurable).
 *   - Apaga la resistencia (estado F, con código) si:
 *       1 algún sensor pasa la temperatura máxima
 *       2 no quedan sensores válidos
 *       3 el PWM está alto y la temperatura no sube (embalamiento)
 *       4 el perfil dura más de lo esperado
 *       5 la temperatura sube con el PWM en 0 (MOSFET en corto)
 *       6 el autoajuste se pasa de su margen de temperatura
 *     El watchdog reinicia el PIC si el programa se traba.
 *
 *  Conexiones (rev D):
 *   RA0 (pin 17) -> SCK  (común a los 5 MAX6675)
 *   RA2 (pin 1)  <- SO   (común a los 5 MAX6675)
 *   RA1 (pin 18) -> CS1  (TC1, J9)
 *   RA3 (pin 2)  -> CS2  (TC2, J10)
 *   RB0 (pin 6)  -> CS3  (TC3, J11)
 *   RB4 (pin 10) -> CS4  (TC4, J12)
 *   RB5 (pin 11) -> CS5  (TC5, J13)
 *   RB1 (pin 7)  <- RX   (J14 pin 3, viene del TX del adaptador USB)
 *   RB2 (pin 8)  -> TX   (J14 pin 2, va al RX del adaptador USB)
 *   RB3 (pin 9)  -> PWM_PIC (R1 -> LED del PC817)
 *   RB6, RB7 (pines 12 y 13): conector de programación J15
 *
 *  Puerto serial: 9600 baudios, 8 bits, sin paridad, 1 bit de parada.
 *  Escribe un comando y presiona Enter (acepta minúsculas).
 *  [n] = ranura 1-4 opcional; sin ella se usa la ranura activa.
 *
 *    I              Iniciar el perfil de la ranura activa
 *    S              Detener (resistencia apagada; no borra una falla)
 *    L [n]          Mostrar un perfil
 *    B [n]          Borrar un perfil
 *    P [n] t,r,m    Agregar un tramo: llegar a t °C en r minutos y
 *                   mantener m minutos. Ejemplo:  P 100,10,15
 *    D [n]          Restaurar el perfil de fábrica en la ranura
 *    R n            Elegir la ranura activa (1-4)
 *    K              Mostrar las ganancias:  K;kp;ki;kd
 *    K kp,ki,kd     Cambiar las ganancias (se guardan en la EEPROM)
 *    C m            Temperatura de control: 0 promedio, 1-5 un sensor,
 *                   6 máximo, 7 mediana
 *    T s            Telemetría cada s segundos (0 = apagada)
 *  Solo en el PIC16F1847 (en el 16F1827 no caben y contestan ERR):
 *    A t[,0]        Autoajuste alrededor de t °C (,0 = solo PI)
 *    ?              Estado completo
 *    F              Registro de las últimas 4 fallas   (F 0 = borrarlo)
 *    H              Ayuda corta
 *
 *  Suma de verificación opcional (para programas): agrega *hh al final,
 *  con hh = XOR en hexadecimal de todos los caracteres antes del '*'.
 *  Ejemplo:  L*4C
 *
 *  Telemetría, un renglón cada 10 s:
 *    seg;sp;prom;pwm;T1;T2;T3;T4;T5;est;ctl;desc;cod;vdd
 *    (temperaturas en °C, pwm de 0 a 1023, X = sensor sin lectura)
 *    est:  E = en espera, C = siguiendo perfil, F = falla de seguridad,
 *          T = perfil terminado, A = autoajuste
 *    ctl:  temperatura de control (°C)
 *    desc: sensores descartados (bit 0 = TC1 ... bit 4 = TC5)
 *    cod:  código de falla (0 = ninguna)
 *    vdd:  voltaje de +5V medido por el PIC, en mV
 * ============================================================================
 */

// ---------------- Bits de configuración (PIC16F1827 / PIC16F1847) ----------------
// CONFIG1
#pragma config FOSC     = XT     // Cristal de 4 MHz
#pragma config WDTE     = ON     // Watchdog activado (seguridad)
#pragma config PWRTE    = ON     // Espera de arranque
#pragma config MCLRE    = ON     // Pin 4 como MCLR (R9 a +5V)
#pragma config CP       = OFF
#pragma config CPD      = OFF
#pragma config BOREN    = ON     // Reinicio por bajo voltaje
#pragma config CLKOUTEN = OFF    // RA6 queda para el cristal
#pragma config IESO     = OFF
#pragma config FCMEN    = ON     // Si el cristal falla, sigue con el reloj interno
// CONFIG2
#pragma config WRT      = OFF
#pragma config PLLEN    = OFF    // Sin PLL: el PIC corre a 4 MHz
#pragma config STVREN   = ON     // Reinicio si la pila se desborda
#pragma config BORV     = HI     // Voltaje alto de reinicio por bajo voltaje
#pragma config LVP      = OFF    // Libera RB4 (lo usamos para CS4)

#ifdef BANCO_PC
#include "xc_pc.h"               // banco de pruebas en la PC (carpeta banco/)
#else
#include <xc.h>
#endif
#include <stdint.h>

#define _XTAL_FREQ 4000000UL
#define VERSION_FW "2.0"

#if defined(_16F1827)
#define NOMBRE_CHIP "PIC16F1827"
#else
#define NOMBRE_CHIP "PIC16F1847"
#endif

// ============================================================================
//  CONFIGURACIÓN DEL USUARIO
//  (cada valor se puede cambiar aquí o con -D al compilar)
// ============================================================================

// 1 = al encender, arranca el perfil solo (sin computadora conectada).
// 0 = espera el comando I por el serial.
// Después de un reinicio por watchdog o por la pila NO arranca solo.
#ifndef AUTOARRANQUE
#define AUTOARRANQUE       1
#endif

// MODO DE PRUEBA: 1 = PWM fijo sin leer sensores (para el osciloscopio).
// ¡Regrésalo a 0 antes de conectar la resistencia del horno!
#ifndef MODO_PRUEBA
#define MODO_PRUEBA        0
#endif
#ifndef PWM_PRUEBA
#define PWM_PRUEBA       512    // 0-1023 (512 = 50 %)
#endif

// SIMULACIÓN EN PROTEUS: 1 = no lee los MAX6675; usa un modelo térmico del
// horno y corre unas 50 veces más rápido. ¡Ponlo en 0 para el PIC real!
#ifndef SIMULAR_SENSORES
#define SIMULAR_SENSORES   0
#endif
#ifndef SIM_FALLA_SEG
#define SIM_FALLA_SEG      0    // 0 = sin falla; N = falla simulada desde el segundo N
#endif
#ifndef SIM_FALLA_MODO
#define SIM_FALLA_MODO     0    // 0 todos los sensores desconectados, 1 TC3 congelado,
#endif                          // 2 TC2 descalibrado +50 °C, 3 resistencia quemada,
                                // 4 MOSFET en corto
#ifndef SIM_TAU_SENSOR_S
#define SIM_TAU_SENSOR_S  20    // retraso del termopar en el modelo (0 = sin retraso)
#endif

// Salida de potencia:
//   SALIDA_PWM     = PWM de 244 Hz por CCP1 (resistencia de 12 V DC, como la placa)
//   SALIDA_VENTANA = ventana de 2 s encendida una fracción (relé de estado sólido)
#define SALIDA_PWM         0
#define SALIDA_VENTANA     1
#ifndef MODO_SALIDA
#define MODO_SALIDA        SALIDA_PWM
#endif

typedef struct {
    int16_t  temp_c;      // temperatura objetivo en °C
    uint16_t rampa_min;   // minutos para llegar a esa temperatura
    uint16_t meseta_min;  // minutos que se mantiene ahí
} Segmento;

// Perfil de fábrica: se usa si la EEPROM está vacía o con el comando D.
static const Segmento PERFIL_DEFECTO[] = {
    { 100,  10, 15 },     // Precalentado: a 100 °C en 10 min, mantener 15 min
    { 180,  15, 30 },     // Proceso: a 180 °C en 15 min, mantener 30 min
    {  60,  30,  0 },     // Enfriamiento controlado hasta 60 °C en 30 min
};

// Sensores conectados: 1 = se usa, 0 = no está conectado (se ignora).
static const uint8_t USAR_SENSOR[5] = { 1, 1, 1, 1, 1 };

#ifndef T_MAX_SEGURIDAD_C
#define T_MAX_SEGURIDAD_C  250   // Si CUALQUIER sensor pasa de aquí, se apaga todo
#endif
#ifndef MIN_SENSORES_OK
#define MIN_SENSORES_OK      1   // Mínimo de sensores aceptados para seguir calentando
#endif

// Ganancias de fábrica del PID (se cambian con K o con el autoajuste A).
//   KP: cuentas de PWM por °C de error
//   KI: 1/64 de cuenta de PWM por °C·s
//   KD: cuentas de PWM por °C/s (derivada sobre la medición)
// Con 32/16/0 el control es idéntico al PI de la versión 1 (KP=8, KI=1).
#ifndef KP_DEF
#define KP_DEF   32
#endif
#ifndef KI_DEF
#define KI_DEF   16
#endif
#ifndef KD_DEF
#define KD_DEF    0
#endif
#define D_FILTRO  8              // filtro de la derivada: constante de ~8 s

#ifndef CTL_DEF
#define CTL_DEF   0              // temperatura de control: 0 = promedio
#endif
#ifndef TELE_DEF
#define TELE_DEF 10              // telemetría cada 10 s
#endif

// Embalamiento: PWM >= RUNAWAY_PWM y la temperatura de control al menos
// RUNAWAY_ERR_C por debajo del setpoint durante RUNAWAY_SEG segundos,
// y en ese tiempo sube menos de RUNAWAY_DELTA_C  ->  falla 3.
// (Resistencia quemada, termopar fuera del horno.) RUNAWAY_SEG 0 = apagado.
#ifndef RUNAWAY_PWM
#define RUNAWAY_PWM      819     // 80 %
#endif
#ifndef RUNAWAY_SEG
#define RUNAWAY_SEG      180
#endif
#ifndef RUNAWAY_DELTA_C
#define RUNAWAY_DELTA_C    5
#endif
#ifndef RUNAWAY_ERR_C
#define RUNAWAY_ERR_C      2
#endif

// Calor sin PWM: con el PWM en 0, si la temperatura sube más de
// CALOR_SIN_PWM_C sobre el mínimo visto  ->  falla 5 (MOSFET en corto).
// 0 = apagado.
#ifndef CALOR_SIN_PWM_C
#define CALOR_SIN_PWM_C   20
#endif

// Validación cruzada: con 3 o más sensores, se descarta el que se aleje más
// de SENSOR_DELTA_C de la mediana. Un sensor es "congelado" si su lectura no
// cambia en CONGELADO_SEG segundos de calentamiento fuerte (máx. 255, 0 = apagado).
#ifndef SENSOR_DELTA_C
#define SENSOR_DELTA_C    25
#endif
#ifndef CONGELADO_SEG
#define CONGELADO_SEG    120
#endif

// Tiempo máximo de proceso: duración del perfil + 1/4 + MARGEN_MIN minutos.
#ifndef MARGEN_MIN
#define MARGEN_MIN        30
#endif

// Funciones extra. Todo junto ocupa ~5400 palabras: cabe en el PIC16F1847
// (8K) pero no en el PIC16F1827 (4K). Por eso en el 16F1827 (el de Proteus)
// los extras vienen apagados; lo esencial (seguridad, PID, perfiles, ranuras,
// K, C, T, suma de verificación) está en los dos. Se pueden forzar con -D.
#if defined(_16F1827)
#define EXTRAS_DEF 0
#else
#define EXTRAS_DEF 1
#endif
#ifndef USAR_AUTOAJUSTE
#define USAR_AUTOAJUSTE   EXTRAS_DEF   // autoajuste por relé (comando A)
#endif
#ifndef USAR_REGISTRO
#define USAR_REGISTRO     EXTRAS_DEF   // registro de fallas en la EEPROM (comando F)
#endif
#ifndef USAR_VDD
#define USAR_VDD          EXTRAS_DEF   // medir el +5V con la FVR (columna vdd)
#endif
#ifndef USAR_ESTADO
#define USAR_ESTADO       EXTRAS_DEF   // comandos ? y H
#endif
#ifndef USAR_IA
#define USAR_IA           EXTRAS_DEF   // diagnóstico con IA (árbol de decisión de ia_arbol.h)
#endif
#define AT_HIST_Q          4     // histéresis del relé en 0.25 °C (4 = 1 °C)
#define AT_CICLOS          3     // ciclos que se miden (después de descartar uno)
#define AT_MAX_SEG      7200     // tiempo máximo del autoajuste
#define AT_MARGEN_C       25     // falla 6 si pasa del objetivo + este margen

// Aviso si el +5V medido sale de este rango (mV)
#define VDD_MIN_MV      4750
#define VDD_MAX_MV      5250

// ============================================================================
//  Fin de la configuración. No necesitas cambiar lo de abajo.
// ============================================================================

#define N_DEFECTO    (sizeof(PERFIL_DEFECTO) / sizeof(PERFIL_DEFECTO[0]))
#define PWM_MAX      1023
#define SENSOR_FALLA (-1)

// ---------------- Mapa de la EEPROM (256 bytes) ----------------
// Ranura r (0-3) en r*57:  [0] = 0xA5, [1] = número de tramos,
//                          [2] = suma de 8 bits de los tramos, [3..56] = 9 tramos
//                          de 6 bytes (temp, rampa, meseta; int16 little-endian).
//   La ranura 0 tiene el mismo formato que la versión 1 (direcciones 0, 1, 2, 3...).
// Configuración en 228:   [0] = 0x5A, [1-2] KP, [3-4] KI, [5-6] KD, [7] modo de control,
//                          [8] ranura activa, [9] telemetría (s), [10] suma de [1..9]
// Registro de fallas en 239: [0] = siguiente entrada (0-3), después 4 entradas de
//                          4 bytes: código, segundo (uint16), temperatura máxima (°C)
#define N_RANURAS       4
#define SEG_POR_RANURA  9
#define EE_MAGIA        0xA5
#define EE_RAN_TAM      (3 + SEG_POR_RANURA * 6)
#define EE_CFG          (N_RANURAS * EE_RAN_TAM)
#define EE_CFG_MAGIA    0x5A
#define EE_CFG_TAM      11
#define EE_LOG          (EE_CFG + EE_CFG_TAM)
#define N_LOG           4
#define EE_LOG_TAM      (1 + N_LOG * 4)

#if (EE_LOG + EE_LOG_TAM) > 256
#error "El mapa de la EEPROM no cabe en 256 bytes"
#endif
#if CONGELADO_SEG > 255
#error "CONGELADO_SEG debe ser 255 o menos"
#endif
// Si el perfil de fábrica tiene más tramos de los que caben en una ranura,
// esta línea da error de compilación (arreglo de tamaño negativo).
typedef char perfil_defecto_cabe_en_ranura[(N_DEFECTO <= SEG_POR_RANURA) ? 1 : -1];

// Pines
// Las salidas usan los registros LAT (evita errores de lectura-modificación-
// escritura). La entrada SO se lee del puerto.
#define SCK   LATAbits.LATA0
#define SO    PORTAbits.RA2
#define CS1   LATAbits.LATA1
#define CS2   LATAbits.LATA3
#define CS3   LATBbits.LATB0
#define CS4   LATBbits.LATB4
#define CS5   LATBbits.LATB5

// Estados
#define EST_ESPERA   'E'
#define EST_PERFIL   'C'
#define EST_FALLA    'F'
#define EST_FIN      'T'
#define EST_AUTO     'A'

// Códigos de falla
#define F_SOBRETEMP     1
#define F_SENSORES      2
#define F_EMBALAMIENTO  3
#define F_TIEMPO        4
#define F_CALOR         5
#define F_AUTOAJUSTE    6

// Causas de reinicio
#define RST_POR    0
#define RST_BOR    1
#define RST_WDT    2
#define RST_MCLR   3
#define RST_PILA   4
#define RST_INSTR  5
#define RST_OTRO   6
static const char * const RST_TXT[] = { "POR", "BOR", "WDT", "MCLR", "PILA", "RESET", "?" };

#if MODO_SALIDA == SALIDA_VENTANA
#define VENT_TICKS 200           // 200 x 10 ms = ventana de 2 s
static volatile uint8_t vent_on = 0;
static volatile uint8_t vent_tick = 0;
#endif

static volatile uint8_t medios_segundos = 0;
static volatile char    rx_buf[32];
static volatile uint8_t rx_n = 0;
static volatile uint8_t rx_listo = 0;
static volatile uint8_t rx_desborde = 0;

// Sensores
static int16_t  temp_q[5];        // temperaturas en 0.25 °C (o SENSOR_FALLA)
static int16_t  ult_q[5];         // última lectura (detección de congelado)
static uint8_t  cong_s[5];        // segundos sin cambio calentando fuerte
static int16_t  prom_q, max_q, med_q, tctl_q;
static uint8_t  validos = 0;      // sensores con lectura
static uint8_t  desc_mask = 0;    // sensores descartados

// Estado y perfil
static uint8_t  estado = EST_ESPERA;
static uint8_t  cod_falla = 0;
static uint8_t  rst_causa = RST_OTRO;
static uint8_t  ranura = 0;
static uint8_t  n_seg = 0;
static uint8_t  segmento = 0;
static uint8_t  en_rampa = 1;
static uint16_t seg_fase = 0;
static uint32_t reloj_s = 0;
static int16_t  sp_inicio_q;
static int16_t  setpoint_q = 0;
static Segmento seg_act;          // tramo en curso (leído de la EEPROM)
static uint8_t  proc_s = 0;       // tiempo de proceso (s y min)
static uint16_t proc_min = 0, proc_lim = 0;

// Control
static int16_t  kp = KP_DEF, ki = KI_DEF, kd = KD_DEF;
static uint8_t  ctl_modo = CTL_DEF;
static uint8_t  tele_s = TELE_DEF, tele_cnt = 0;
static int32_t  integral = 0;     // en 1/256 de cuenta de PWM
static int16_t  dfilt = 0;        // derivada filtrada (x16, en 0.25 °C/s)
static int16_t  t_prev_q = 0;
static uint16_t duty_actual = 0;

// Seguridad
static uint8_t  pot_alta = 0;     // PWM alto y lejos del setpoint (calentando fuerte)
static uint16_t run_s = 0;
static int16_t  run_ref_q = 0;
static int16_t  tmin0_q = 0x7FFF;

// Voltaje
static uint16_t vdd_mv = 0;       // queda en 0 si USAR_VDD = 0
#if USAR_VDD
static uint8_t  vdd_aviso = 0;
#endif

// ---------------- Interrupciones ----------------
void __interrupt() isr(void)
{
    if (PIR1bits.TMR1IF) {
        TMR1H = 0x0B;            // 0.5 s
        TMR1L = 0xDC;
        PIR1bits.TMR1IF = 0;
        medios_segundos++;
    }
#if MODO_SALIDA == SALIDA_VENTANA
    if (PIR1bits.TMR2IF) {       // cada 10 ms
        PIR1bits.TMR2IF = 0;
        if (++vent_tick >= VENT_TICKS) vent_tick = 0;
        LATBbits.LATB3 = (vent_tick < vent_on) ? 1 : 0;
    }
#endif
    if (PIR1bits.RCIF) {
        char c = RCREG;
        if (RCSTAbits.OERR) { RCSTAbits.CREN = 0; RCSTAbits.CREN = 1; }
        if (!rx_listo) {
            if (c == '\r' || c == '\n') {
                if (rx_n) { rx_buf[rx_n] = 0; rx_listo = 1; }
            } else if (rx_n < sizeof(rx_buf) - 1) {
                rx_buf[rx_n++] = c;
            } else {
                rx_desborde = 1;  // renglón demasiado largo: se contesta ERR
            }
        }
    }
}

// ---------------- Salida de potencia ----------------
static void pwm_set(uint16_t d)
{
    if (d > PWM_MAX) d = PWM_MAX;
    duty_actual = d;
#if MODO_SALIDA == SALIDA_VENTANA
    vent_on = (uint8_t)(((uint16_t)(d * 25u) + 64u) >> 7);   // d * 200 / 1023
    if (!d) LATBbits.LATB3 = 0;
#else
    CCPR1L = (uint8_t)(d >> 2);
    CCP1CON = (uint8_t)(0x0C | ((d & 0x03) << 4));
#endif
}

// ---------------- Serial ----------------
static void uart_char(char c)
{
    while (!TXSTAbits.TRMT) { CLRWDT(); }
    TXREG = c;
}

static void uart_texto(const char *t)
{
    while (*t) uart_char(*t++);
}

static void uart_num(int32_t n)
{
    char buf[10];
    uint8_t i = 0;
    uint32_t u, q;
    if (n < 0) { uart_char('-'); n = -n; }
    u = (uint32_t)n;
    do {                                 // sin %: ahorra la rutina de módulo de 32 bits
        q = u / 10;
        buf[i++] = (char)('0' + (uint8_t)(u - q * 10));
        u = q;
    } while (u && i < 10);
    while (i) uart_char(buf[--i]);
}

static void uart_fin(void)  { uart_texto("\r\n"); }
static void uart_ok(void)   { uart_texto("OK\r\n"); }
static void uart_err(void)  { uart_texto("ERR\r\n"); }
static void uart_sep(void)  { uart_char(';'); }

// ---------------- EEPROM ----------------
static uint8_t ee_leer(uint8_t a)
{
    EEADRL = a;
    EECON1bits.CFGS = 0;         // memoria de datos EEPROM
    EECON1bits.EEPGD = 0;
    EECON1bits.RD = 1;
    return EEDATL;
}

static void ee_escribir(uint8_t a, uint8_t d)
{
    uint8_t gie;
    if (ee_leer(a) == d) return; // no gasta la EEPROM si el valor ya está
    while (EECON1bits.WR) { CLRWDT(); }
    EEADRL = a;
    EEDATL = d;
    EECON1bits.CFGS = 0;
    EECON1bits.EEPGD = 0;
    EECON1bits.WREN = 1;
    gie = INTCONbits.GIE;
    INTCONbits.GIE = 0;
    EECON2 = 0x55;               // secuencia obligatoria del fabricante
    EECON2 = 0xAA;
    EECON1bits.WR = 1;
    if (gie) INTCONbits.GIE = 1;
    EECON1bits.WREN = 0;
    while (EECON1bits.WR) { CLRWDT(); }
}

static uint16_t ee_leer16(uint8_t a)
{
    return (uint16_t)(ee_leer(a) | ((uint16_t)ee_leer((uint8_t)(a + 1)) << 8));
}

static void ee_escribir16(uint8_t a, uint16_t v)
{
    ee_escribir(a, (uint8_t)v);
    ee_escribir((uint8_t)(a + 1), (uint8_t)(v >> 8));
}

static uint8_t ee_suma(uint8_t a, uint8_t n)
{
    uint8_t s = 0;
    while (n--) s += ee_leer(a++);
    return s;
}

// ---------------- Ranuras de perfil ----------------
static uint8_t ran_base(uint8_t r)
{
    return (uint8_t)(r * EE_RAN_TAM);
}

static uint8_t ran_n(uint8_t r)
{
    uint8_t n = ee_leer((uint8_t)(ran_base(r) + 1));
    return n > SEG_POR_RANURA ? 0 : n;
}

static uint8_t ran_valida(uint8_t r)
{
    uint8_t b = ran_base(r);
    uint8_t n = ee_leer((uint8_t)(b + 1));
    return ee_leer(b) == EE_MAGIA && n <= SEG_POR_RANURA &&
           ee_suma((uint8_t)(b + 3), (uint8_t)(n * 6)) == ee_leer((uint8_t)(b + 2));
}

static void ee_leer_seg(uint8_t r, uint8_t i, Segmento *s)
{
    uint8_t a = (uint8_t)(ran_base(r) + 3 + i * 6);
    s->temp_c     = (int16_t)ee_leer16(a);
    s->rampa_min  = ee_leer16((uint8_t)(a + 2));
    s->meseta_min = ee_leer16((uint8_t)(a + 4));
}

static void ran_borrar(uint8_t r)
{
    uint8_t b = ran_base(r);
    ee_escribir(b, EE_MAGIA);
    ee_escribir((uint8_t)(b + 1), 0);
    ee_escribir((uint8_t)(b + 2), 0);
    if (r == ranura) n_seg = 0;
}

static uint8_t ran_agregar(uint8_t r, const Segmento *s)
{
    uint8_t b = ran_base(r);
    uint8_t n = ran_n(r);
    uint8_t a;
    if (n >= SEG_POR_RANURA) return 0;
    a = (uint8_t)(b + 3 + n * 6);
    ee_escribir16(a, (uint16_t)s->temp_c);
    ee_escribir16((uint8_t)(a + 2), s->rampa_min);
    ee_escribir16((uint8_t)(a + 4), s->meseta_min);
    n++;
    ee_escribir((uint8_t)(b + 1), n);
    ee_escribir((uint8_t)(b + 2), ee_suma((uint8_t)(b + 3), (uint8_t)(n * 6)));
    if (r == ranura) n_seg = n;
    return 1;
}

static void ran_defecto(uint8_t r)
{
    ran_borrar(r);
    for (uint8_t i = 0; i < N_DEFECTO; i++) ran_agregar(r, &PERFIL_DEFECTO[i]);
}

// ---------------- Configuración en la EEPROM ----------------
static void cfg_guardar(void)
{
    ee_escribir16(EE_CFG + 1, (uint16_t)kp);
    ee_escribir16(EE_CFG + 3, (uint16_t)ki);
    ee_escribir16(EE_CFG + 5, (uint16_t)kd);
    ee_escribir(EE_CFG + 7, ctl_modo);
    ee_escribir(EE_CFG + 8, ranura);
    ee_escribir(EE_CFG + 9, tele_s);
    ee_escribir(EE_CFG + 10, ee_suma(EE_CFG + 1, 9));
    ee_escribir(EE_CFG, EE_CFG_MAGIA);
}

#if USAR_REGISTRO
static void log_borrar(void)
{
    ee_escribir(EE_LOG, 0);
    for (uint8_t i = 0; i < N_LOG; i++) ee_escribir((uint8_t)(EE_LOG + 1 + i * 4), 0);
}
#endif

// Al encender: revisa las ranuras, la configuración y el registro.
static void ee_revisar(void)
{
    for (uint8_t r = 0; r < N_RANURAS; r++) {
        if (!ran_valida(r)) {
            if (r == 0) ran_defecto(0);
            else ran_borrar(r);
        }
    }
    if (ee_leer(EE_CFG) == EE_CFG_MAGIA &&
        ee_suma(EE_CFG + 1, 9) == ee_leer(EE_CFG + 10)) {
        kp = (int16_t)ee_leer16(EE_CFG + 1);
        ki = (int16_t)ee_leer16(EE_CFG + 3);
        kd = (int16_t)ee_leer16(EE_CFG + 5);
        ctl_modo = ee_leer(EE_CFG + 7);
        ranura = ee_leer(EE_CFG + 8);
        tele_s = ee_leer(EE_CFG + 9);
        if (ranura >= N_RANURAS) ranura = 0;
        if (ctl_modo > 7) ctl_modo = 0;
    } else {
        cfg_guardar();           // valores de fábrica
#if USAR_REGISTRO
        log_borrar();
#endif
    }
    n_seg = ran_n(ranura);
}

// ---------------- Registro de fallas ----------------
#if USAR_REGISTRO
static void log_agregar(uint8_t cod)
{
    uint8_t i = ee_leer(EE_LOG);
    uint8_t a;
    uint16_t s = reloj_s > 0xFFFF ? 0xFFFF : (uint16_t)reloj_s;
    int16_t t = max_q < 0 ? 0 : max_q / 4;
    if (i >= N_LOG) i = 0;
    a = (uint8_t)(EE_LOG + 1 + i * 4);
    ee_escribir(a, cod);
    ee_escribir16((uint8_t)(a + 1), s);
    ee_escribir((uint8_t)(a + 3), (uint8_t)(t > 255 ? 255 : t));
    ee_escribir(EE_LOG, (uint8_t)((i + 1) % N_LOG));
}

static void log_mostrar(void)
{
    uint8_t i = ee_leer(EE_LOG), k, a, c, n = 0;
    uart_texto("REG\r\n");
    for (k = 0; k < N_LOG; k++) {                 // de la más vieja a la más nueva
        a = (uint8_t)(EE_LOG + 1 + ((i + k) % N_LOG) * 4);
        c = ee_leer(a);
        if (c == 0 || c == 0xFF) continue;
        uart_num(++n);                               uart_sep();
        uart_num(c);                                 uart_sep();
        uart_num(ee_leer16((uint8_t)(a + 1)));       uart_sep();
        uart_num(ee_leer((uint8_t)(a + 3)));
        uart_fin();
    }
}
#endif

// ---------------- Falla de seguridad ----------------
// Apaga la resistencia y se queda en F hasta apagar y prender.
static void falla(uint8_t cod)
{
    pwm_set(0);
    integral = 0;
    if (estado == EST_FALLA) return;  // se conserva la primera causa
    estado = EST_FALLA;
    cod_falla = cod;
    uart_texto("F;");
    uart_num(cod);
    uart_fin();
#if USAR_REGISTRO
    log_agregar(cod);
#endif
}

// ---------------- Causa del reinicio ----------------
// Se lee antes de cualquier CLRWDT (que pone /TO en 1).
static void causa_reinicio(void)
{
    if (!PCONbits.nPOR)                          rst_causa = RST_POR;
    else if (!PCONbits.nBOR)                     rst_causa = RST_BOR;
    else if (PCONbits.STKOVF || PCONbits.STKUNF) rst_causa = RST_PILA;
    else if (!STATUSbits.nTO)                    rst_causa = RST_WDT;
    else if (!PCONbits.nRI)                      rst_causa = RST_INSTR;
    else if (!PCONbits.nRMCLR)                   rst_causa = RST_MCLR;
    else                                         rst_causa = RST_OTRO;
    PCONbits.nPOR = 1;                           // se rearman para el siguiente reinicio
    PCONbits.nBOR = 1;
    PCONbits.nRI = 1;
    PCONbits.nRMCLR = 1;
    PCONbits.STKOVF = 0;
    PCONbits.STKUNF = 0;
}

// ---------------- Voltaje de +5V con la referencia interna ----------------
// El ADC mide la FVR de 1.024 V usando VDD como referencia:
//   VDD = 1.024 V * 1023 / lectura
// La FVR tiene una tolerancia de algunos %, así que es una medida aproximada.
#if USAR_VDD
static void vdd_medir(void)
{
    uint16_t a;
    uint8_t i;
    FVRCON = 0x81;                       // FVR encendida, 1.024 V al ADC
    ADCON1 = 0x90;                       // justificado a la derecha, Fosc/8, ref = VDD
    ADCON0 = 0x7D;                       // canal FVR (11111), ADC encendido
    for (i = 0; i < 100 && !FVRCONbits.FVRRDY; i++) __delay_us(10);
    __delay_us(20);                      // tiempo de adquisición
    ADCON0bits.GO_nDONE = 1;
    for (i = 0; i < 100 && ADCON0bits.GO_nDONE; i++) __delay_us(10);
    a = (uint16_t)(((uint16_t)ADRESH << 8) | ADRESL);
    ADCON0 = 0x00;
    vdd_mv = a ? (uint16_t)(1047552UL / a) : 0;

    if (vdd_mv < VDD_MIN_MV || vdd_mv > VDD_MAX_MV) {
        if (!vdd_aviso) {
            vdd_aviso = 1;
            uart_texto("AVISO;VDD;");
            uart_num(vdd_mv);
            uart_fin();
        }
    } else if (vdd_mv > VDD_MIN_MV + 50 && vdd_mv < VDD_MAX_MV - 50) {
        vdd_aviso = 0;
    }
}
#endif

// ---------------- MAX6675 (SPI por software) ----------------
#if !SIMULAR_SENSORES
static void cs_poner(uint8_t ch, uint8_t nivel)
{
    switch (ch) {
        case 0: CS1 = nivel; break;
        case 1: CS2 = nivel; break;
        case 2: CS3 = nivel; break;
        case 3: CS4 = nivel; break;
        default: CS5 = nivel; break;
    }
}

static int16_t max6675_leer(uint8_t ch)
{
    uint16_t v = 0;

    SCK = 0;
    cs_poner(ch, 0);
    __delay_us(10);
    for (uint8_t i = 0; i < 16; i++) {
        v <<= 1;
        if (SO) v |= 1;
        SCK = 1;
        __delay_us(10);
        SCK = 0;
        __delay_us(10);
    }
    cs_poner(ch, 1);

    if (v & 0x0004) return SENSOR_FALLA; // termopar desconectado
    if (v & 0x8002) return SENSOR_FALLA; // módulo ausente o ruido
    if (v == 0)     return SENSOR_FALLA; // SO flotando en 0
    return (int16_t)(v >> 3);
}
#endif

#if SIMULAR_SENSORES
// Modelo térmico: el horno es de primer orden (al 100 % llegaría a 325 °C,
// constante de tiempo de 10 min) y el termopar lo sigue con un retraso de
// SIM_TAU_SENSOR_S segundos. Cada sensor tiene un desfase, como en distintas zonas.
static int32_t t_horno = 25L * 4096;
static int32_t t_sensor = 25L * 4096;
static int16_t sim_congelado = 0x7FFF;
static const int8_t DESFASE_C[5] = { -2, -1, 0, 1, 3 };

// Macro (no función): con SIM_FALLA_SEG = 0 el compilador quita todo el código de fallas
#define sim_falla_activa() (SIM_FALLA_SEG && reloj_s >= SIM_FALLA_SEG)

// x += (meta - x) / tau, con división sin signo (ahorra la rutina con signo)
static void acercar(int32_t *x, int32_t meta, uint16_t tau)
{
    if (meta >= *x) *x += (int32_t)((uint32_t)(meta - *x) / tau);
    else            *x -= (int32_t)((uint32_t)(*x - meta) / tau);
}

static void modelo_horno_1s(void)
{
    uint16_t d = duty_actual;
    if (sim_falla_activa() && SIM_FALLA_MODO == 3) d = 0;        // resistencia quemada
    if (sim_falla_activa() && SIM_FALLA_MODO == 4) d = PWM_MAX;  // MOSFET en corto
    acercar(&t_horno, 25L * 4096 + ((int32_t)300 * 4096 / PWM_MAX) * d, 600);
#if SIM_TAU_SENSOR_S
    acercar(&t_sensor, t_horno, SIM_TAU_SENSOR_S);
#else
    t_sensor = t_horno;
#endif
}

static int16_t sensor_simulado(uint8_t ch)
{
    int16_t v = (int16_t)(t_sensor >> 10) + DESFASE_C[ch] * 4;
    if (sim_falla_activa()) {
        if (SIM_FALLA_MODO == 0) return SENSOR_FALLA;
        if (SIM_FALLA_MODO == 1 && ch == 2) {
            if (sim_congelado == 0x7FFF) sim_congelado = v;
            return sim_congelado;
        }
        if (SIM_FALLA_MODO == 2 && ch == 1) return v + 50 * 4;
    }
    return v;
}
#endif

// Lee los 5 sensores, descarta los dudosos y calcula el promedio, el máximo,
// la mediana y la temperatura de control. Devuelve los sensores aceptados.
static uint8_t leer_sensores(void)
{
    int16_t v[5], t, d, suma = 0, max_ac = -32000;
    uint8_t ch, i, j, bit, nv = 0, ok = 0, acept = 0;

    max_q = -32000;
    validos = 0;
    desc_mask = 0;

    // (bit en vez de 1 << ch: en el PIC16 un corrimiento variable es un ciclo)
    for (ch = 0, bit = 1; ch < 5; ch++, bit <<= 1) {
        t = SENSOR_FALLA;
#if SIMULAR_SENSORES
        if (USAR_SENSOR[ch]) t = sensor_simulado(ch);
#else
        if (USAR_SENSOR[ch]) t = max6675_leer(ch);
#endif
        temp_q[ch] = t;
        if (t == SENSOR_FALLA) { cong_s[ch] = 0; continue; }
        validos++;
        if (t > max_q) max_q = t;          // la seguridad usa TODAS las lecturas
        if (t != ult_q[ch]) { ult_q[ch] = t; cong_s[ch] = 0; }
        else if (pot_alta && cong_s[ch] < 255) cong_s[ch]++;
        if (CONGELADO_SEG && cong_s[ch] >= CONGELADO_SEG) {
            desc_mask |= bit;
            continue;
        }
        v[nv++] = t;
        acept |= bit;
    }

    // Mediana (ordenamiento por inserción de hasta 5 valores)
    for (i = 1; i < nv; i++) {
        t = v[i];
        for (j = i; j && v[j - 1] > t; j--) v[j] = v[j - 1];
        v[j] = t;
    }
    if (!nv)          med_q = 0;
    else if (nv & 1)  med_q = v[nv / 2];
    else              med_q = (int16_t)((v[nv / 2 - 1] + v[nv / 2]) / 2);

    // Atípicos: solo se pueden decidir con 3 o más sensores
    for (ch = 0, bit = 1; ch < 5; ch++, bit <<= 1) {
        if (!(acept & bit)) continue;
        t = temp_q[ch];
        d = t - med_q;
        if (d < 0) d = -d;
        if (nv >= 3 && d > SENSOR_DELTA_C * 4) {
            desc_mask |= bit;
            acept &= (uint8_t)~bit;
            continue;
        }
        suma += t;
        ok++;
        if (t > max_ac) max_ac = t;
    }
    prom_q = ok ? (int16_t)(suma / ok) : 0;

    // Temperatura de control
    tctl_q = prom_q;
    if (ctl_modo == 6 && ok)  tctl_q = max_ac;
    else if (ctl_modo == 7)   tctl_q = med_q;
    else if (ctl_modo >= 1 && ctl_modo <= 5) {
        ch = (uint8_t)(ctl_modo - 1);
        if ((acept >> ch) & 1) tctl_q = temp_q[ch];
    }
    return ok;
}

// ---------------- Seguridad (cada segundo, en cualquier estado) ----------------
static void seguridad(uint8_t ok)
{
    // 1. Sobretemperatura en cualquier sensor con lectura
    if (validos && max_q > (int16_t)(T_MAX_SEGURIDAD_C * 4)) falla(F_SOBRETEMP);

    // 2. Sensores insuficientes mientras se calienta
    if ((estado == EST_PERFIL || estado == EST_AUTO) && ok < MIN_SENSORES_OK) falla(F_SENSORES);

    // 3. Embalamiento: mucha potencia, lejos del setpoint, y no sube.
    //    pot_alta también la usa la detección de sensor congelado.
    pot_alta = ok && duty_actual >= RUNAWAY_PWM && (setpoint_q - tctl_q) >= RUNAWAY_ERR_C * 4;
    if (RUNAWAY_SEG && pot_alta) {
        if (!run_s) run_ref_q = tctl_q;
        if (++run_s >= RUNAWAY_SEG) {
            if (tctl_q - run_ref_q < RUNAWAY_DELTA_C * 4) falla(F_EMBALAMIENTO);
            run_s = 0;
        }
    } else {
        run_s = 0;
    }

    // 5. Calor sin PWM: la temperatura sube con la resistencia apagada
    if (CALOR_SIN_PWM_C && ok && duty_actual == 0) {
        if (tctl_q < tmin0_q) tmin0_q = tctl_q;
        if (tctl_q - tmin0_q > CALOR_SIN_PWM_C * 4) falla(F_CALOR);
    } else {
        tmin0_q = 0x7FFF;
    }
}

// ---------------- Diagnóstico con IA (solo PIC16F1847) ----------------
// Cada 10 s calcula 10 características de los últimos 60-120 s y las pasa por un
// árbol de decisión entrenado con datos del propio firmware (ia/entrenar.py).
// Si la misma clase sale IA_PERSIST veces seguidas, manda "IA;n" por serial:
//   0 normal, 1 resistencia abierta, 2 MOSFET en corto, 4 termopar congelado,
//   5 termopar con deriva.
// Es solo un AVISO: no apaga nada. La protección siguen siendo las reglas de seguridad().
// Los cálculos son enteros y deben coincidir con caracteristicas() de ia/entrenar.py.
#if USAR_IA
#include "ia_arbol.h"
#define IA_PERSIST  6                    // 6 evaluaciones = 60 s
#define IA_EF_INI   6                    // evaluaciones de la línea base del horno
#define IA_EF_FIN  18
static int16_t  ia_ult[5];
static uint8_t  ia_quieto[5];
static uint16_t ia_pwm10[12];            // suma de PWM de cada bloque de 10 s (el 0 es el más viejo)
static int16_t  ia_ctl[7], ia_dis[7], ia_sp[7];
static uint16_t ia_acc;
static uint8_t  ia_seg, ia_n10, ia_neval;
static int32_t  ia_base_dT, ia_base_pwm;
static int16_t  ia_base_dis;
static uint8_t  ia_cand, ia_cuenta, ia_clase;
static uint8_t  ia_pred = 0xFF;          // última predicción sin filtrar (para el banco de pruebas)

static void ia_reiniciar(void)
{
    uint8_t i;
    for (i = 0; i < 5; i++) { ia_ult[i] = 0; ia_quieto[i] = 0; }
    for (i = 0; i < 12; i++) ia_pwm10[i] = 0;
    for (i = 0; i < 7; i++) { ia_ctl[i] = 0; ia_dis[i] = 0; ia_sp[i] = 0; }
    ia_acc = 0;
    ia_seg = ia_n10 = ia_neval = 0;
    ia_base_dT = ia_base_pwm = 0;
    ia_base_dis = 0;
    ia_cand = ia_cuenta = ia_clase = 0;
    ia_pred = 0xFF;
}

static void ia_paso_1s(void)
{
    int16_t f[10], t, d, dis = 0, q = 0;
    uint16_t s0 = 0, s1 = 0;
    int32_t ef;
    uint8_t ch, i;

    for (ch = 0; ch < 5; ch++) {
        t = temp_q[ch];
        if (t == SENSOR_FALLA) { ia_quieto[ch] = 0; continue; }
        if (t == ia_ult[ch]) { if (ia_quieto[ch] < 255) ia_quieto[ch]++; }
        else { ia_ult[ch] = t; ia_quieto[ch] = 0; }
        d = t - med_q;
        if (d < 0) d = -d;
        if (d > dis) dis = d;
        if (ia_quieto[ch] > q) q = ia_quieto[ch];
    }
    ia_acc += duty_actual;
    if (++ia_seg < 10) return;
    ia_seg = 0;

    for (i = 0; i < 11; i++) ia_pwm10[i] = ia_pwm10[i + 1];
    ia_pwm10[11] = ia_acc;
    ia_acc = 0;
    for (i = 0; i < 6; i++) { ia_ctl[i] = ia_ctl[i + 1]; ia_dis[i] = ia_dis[i + 1]; ia_sp[i] = ia_sp[i + 1]; }
    ia_ctl[6] = tctl_q;
    ia_dis[6] = dis;
    ia_sp[6] = setpoint_q;
    if (ia_n10 < 12 && ++ia_n10 < 12) return;        // aún no hay 120 s de historia

    for (i = 0; i < 6; i++) { s0 += ia_pwm10[i]; s1 += ia_pwm10[i + 6]; }
    f[0] = (int16_t)(s1 / 60);                       // PWM promedio de los últimos 60 s
    f[1] = tctl_q - ia_ctl[0];                       // cambio de temperatura en 60 s
    f[2] = setpoint_q - tctl_q;                      // error
    f[3] = tctl_q;                                   // temperatura
    f[5] = dis - ia_dis[0];                          // cambio de la dispersión en 60 s
    f[6] = q;                                        // lectura sin cambio más larga (s)
    if (ia_neval < 255) ia_neval++;
    if (ia_neval >= IA_EF_INI && ia_neval <= IA_EF_FIN) {   // línea base: el horno al arrancar
        ia_base_dT += f[1];
        ia_base_pwm += f[0];
        ia_base_dis += dis;
    }
    f[4] = ia_neval > IA_EF_FIN ? (int16_t)(dis - ia_base_dis / (IA_EF_FIN - IA_EF_INI + 1)) : 0;
    ef = 100;                                        // eficiencia de calentamiento vs. la línea base (%)
    if (ia_neval > IA_EF_FIN && f[0] >= 100 && ia_base_dT > 0 && ia_base_pwm > 0) {
        ef = ((int32_t)f[1] * ia_base_pwm * 100) / ((int32_t)f[0] * ia_base_dT);
        if (ef > 300) ef = 300;
        if (ef < -300) ef = -300;
    }
    f[7] = (int16_t)ef;
    f[8] = setpoint_q - ia_sp[0];                    // cambio del setpoint en 60 s
    f[9] = f[0] - (int16_t)(s0 / 60);                // cambio del PWM contra los 60 s anteriores

    if (estado == EST_FALLA) return;                 // ya está apagado: no hay nada que avisar
    ia_pred = ia_arbol(f);
    if (ia_pred == ia_cand) { if (ia_cuenta < 255) ia_cuenta++; }
    else { ia_cand = ia_pred; ia_cuenta = 1; }
    if (ia_cuenta >= IA_PERSIST && ia_cand != ia_clase) {
        ia_clase = ia_cand;
        uart_texto("IA;");
        uart_num(ia_clase);
        uart_fin();
    }
}
#endif

// ---------------- Perfil térmico ----------------
static void perfil_iniciar(void)
{
    Segmento s;
    uint16_t total = 0;
    for (uint8_t i = 0; i < n_seg; i++) {
        ee_leer_seg(ranura, i, &s);
        total += s.rampa_min + s.meseta_min;
    }
    proc_lim = total + total / 4 + MARGEN_MIN;
    proc_min = 0;
    proc_s = 0;

    segmento = 0;
    en_rampa = 1;
    seg_fase = 0;
    integral = 0;
    dfilt = 0;
    t_prev_q = tctl_q;
    run_s = 0;
    sp_inicio_q = tctl_q;
    setpoint_q = tctl_q;
    ee_leer_seg(ranura, 0, &seg_act);
    estado = EST_PERFIL;
#if USAR_IA
    ia_reiniciar();                      // la línea base se mide al arrancar cada perfil
#endif
}

static void perfil_paso_1s(void)
{
    int16_t  objetivo_q = seg_act.temp_c * 4;
    uint16_t dur_s;

    // 4. Tiempo máximo de proceso
    if (++proc_s >= 60) {
        proc_s = 0;
        if (++proc_min > proc_lim) { falla(F_TIEMPO); return; }
    }

    if (en_rampa) {
        dur_s = seg_act.rampa_min * 60u;
        if (seg_fase >= dur_s || dur_s == 0) {
            setpoint_q = objetivo_q;
            en_rampa = 0;
            seg_fase = 0;
        } else {
            // inicio + (objetivo - inicio) * fase / duración, con división sin signo
            int16_t dif = objetivo_q - sp_inicio_q;
            uint8_t baja = dif < 0;
            if (baja) dif = -dif;
            dif = (int16_t)(((uint32_t)(uint16_t)dif * seg_fase) / dur_s);
            setpoint_q = baja ? sp_inicio_q - dif : sp_inicio_q + dif;
            seg_fase++;
        }
    } else {
        setpoint_q = objetivo_q;
        dur_s = seg_act.meseta_min * 60u;
        if (seg_fase >= dur_s) {
            segmento++;
            if (segmento >= n_seg) {
                estado = EST_FIN;
                return;
            }
            ee_leer_seg(ranura, segmento, &seg_act);
            sp_inicio_q = objetivo_q;
            en_rampa = 1;
            seg_fase = 0;
            return;
        }
        seg_fase++;
    }
}

// ---------------- Control PID (punto fijo) ----------------
// P sobre el error, I con integración condicional (antiwindup),
// D sobre la medición con filtro de primer orden.
static uint16_t control_pid(void)
{
    int16_t e = setpoint_q - tctl_q;
    int16_t dm;
    int32_t p, u;

    if (e > 400)  e = 400;
    if (e < -400) e = -400;

    dm = t_prev_q - tctl_q;              // -dT/dt en 0.25 °C/s
    t_prev_q = tctl_q;
    dfilt += (int16_t)((dm * 16 - dfilt) / D_FILTRO);

    // P = e·KP/4 y D = KD·dfilt/64, con >> en vez de / (corrimiento aritmético)
    p = ((int32_t)e * kp + (((int32_t)kd * dfilt) >> 4)) >> 2;
    u = p + (int32_t)((uint32_t)integral >> 8);

    // Solo integra si la salida no está saturada en la misma dirección del error
    if (!((u >= PWM_MAX && e > 0) || (u <= 0 && e < 0))) {
        integral += (int32_t)e * ki;
        if (integral < 0) integral = 0;
        if (integral > (int32_t)PWM_MAX * 256) integral = (int32_t)PWM_MAX * 256;
        u = p + (int32_t)((uint32_t)integral >> 8);
    }

    if (u < 0) u = 0;
    if (u > PWM_MAX) u = PWM_MAX;
    return (uint16_t)u;
}

// ---------------- Autoajuste por relé ----------------
#if USAR_AUTOAJUSTE
static int16_t  at_sp_q, at_max, at_min;
static uint8_t  at_on, at_ciclo, at_pid;
static uint16_t at_t, at_t_on, at_tu, at_pp;

static void at_iniciar(int16_t t_c, uint8_t pid)
{
    at_sp_q = t_c * 4;
    at_pid = pid;
    at_on = (tctl_q < at_sp_q);
    at_ciclo = 0;
    at_t = 0;
    at_t_on = 0;
    at_tu = 0;
    at_pp = 0;
    at_max = at_min = tctl_q;
    setpoint_q = at_sp_q;
    run_s = 0;
    integral = 0;
    estado = EST_AUTO;
}

static int16_t at_limitar(uint32_t v, uint16_t hi)
{
    return (int16_t)(v > hi ? hi : v);
}

// Ku = 4·d/(π·a) con d = 1023/2 y a = amplitud. Reglas de Tyreus-Luyben:
//   PID: Kp = Ku/2.2, Ti = 2.2·Tu, Td = Tu/6.3      PI: Kp = Ku/3.2, Ti = 2.2·Tu
// Con pp = suma de los pico a pico (0.25 °C) y tu = suma de los periodos (s)
// de AT_CICLOS ciclos, en las unidades de KP, KI y KD:
//   PID: KP = 2368·N/pp   KI = 68887·N²/(pp·tu)   KD = 376·tu/pp
//   PI:  KP = 1628·N/pp   KI = 47360·N²/(pp·tu)
static void at_fin(uint8_t exito)
{
    uint32_t a, b, c;
    estado = EST_ESPERA;
    pwm_set(0);
    integral = 0;
    if (exito && at_pp > AT_CICLOS * 2 * AT_HIST_Q && at_tu >= AT_CICLOS * 4) {
        if (at_pid) {
            a = 2368UL * AT_CICLOS / at_pp;
            b = 68887UL * AT_CICLOS * AT_CICLOS / ((uint32_t)at_pp * at_tu);
            c = 376UL * at_tu / at_pp;
        } else {
            a = 1628UL * AT_CICLOS / at_pp;
            b = 47360UL * AT_CICLOS * AT_CICLOS / ((uint32_t)at_pp * at_tu);
            c = 0;
        }
        kp = at_limitar(a, 1000);
        if (!kp) kp = 1;
        ki = at_limitar(b, 1000);
        kd = at_limitar(c, 30000);
        cfg_guardar();
        uart_texto("A;OK;");
        uart_num(kp);                 uart_sep();
        uart_num(ki);                 uart_sep();
        uart_num(kd);                 uart_sep();
        uart_num(at_tu / AT_CICLOS);  uart_sep();     // periodo Tu (s)
        uart_num(at_pp / AT_CICLOS);                  // pico a pico (0.25 °C)
        uart_fin();
        return;
    }
    uart_texto("A;ERR\r\n");
}

static uint16_t autoajuste_1s(void)
{
    int16_t y = tctl_q;
    at_t++;
    if (y > at_max) at_max = y;
    if (y < at_min) at_min = y;
    if (y > at_sp_q + AT_MARGEN_C * 4) { falla(F_AUTOAJUSTE); return 0; }
    if (at_t > AT_MAX_SEG) { at_fin(0); return 0; }

    if (at_on) {
        if (y > at_sp_q + AT_HIST_Q) at_on = 0;
    } else if (y < at_sp_q - AT_HIST_Q) {
        at_on = 1;                       // empieza un ciclo nuevo
        if (at_ciclo >= 2) {             // el primer ciclo completo se descarta
            at_tu += at_t - at_t_on;
            at_pp += (uint16_t)(at_max - at_min);
        }
        at_t_on = at_t;
        at_max = at_min = y;
        if (++at_ciclo > AT_CICLOS + 1) { at_fin(1); return 0; }
    }
    return at_on ? PWM_MAX : 0;
}
#endif

// ---------------- Comandos por serial ----------------
static const char *cp;                   // posición de lectura en el comando

// Lee un número del comando y avanza. Devuelve 0 si no hay número.
static uint8_t leer_num(int16_t *v)
{
    int16_t n = 0;
    uint8_t hay = 0, neg = 0;
    char c;
    while ((c = *cp) == ' ' || c == ',') cp++;
    if (c == '-') { neg = 1; cp++; }
    while ((c = *cp) >= '0' && c <= '9') {
        // demasiado grande: queda en 32767 y se rechaza después
        n = n > 3275 ? 32767 : (int16_t)(n * 10 + (c - '0'));
        cp++;
        hay = 1;
    }
    *v = neg ? (int16_t)-n : n;
    return hay;
}

// 1 si v está fuera de [lo, hi]
static uint8_t fuera(int16_t v, int16_t lo, int16_t hi)
{
    return v < lo || v > hi;
}

static uint8_t hex_val(char c)
{
    if (c >= '0' && c <= '9') return (uint8_t)(c - '0');
    if (c >= 'A' && c <= 'F') return (uint8_t)(c - 'A' + 10);
    if (c >= 'a' && c <= 'f') return (uint8_t)(c - 'a' + 10);
    return 0xFF;
}

static void uart_campo(int16_t n)        // ";n"
{
    uart_sep();
    uart_num(n);
}

static void uart_temp(int16_t q)         // ";°C" (de 0.25 °C) o ";X" si no hay lectura
{
    uart_sep();
    if (q == SENSOR_FALLA) uart_char('X');
    else uart_num(q / 4);
}

static void mostrar_perfil(uint8_t r)
{
    Segmento s;
    uint8_t n = ran_n(r);
    uart_texto("N=");
    uart_num(n);
    uart_fin();
    for (uint8_t i = 0; i < n; i++) {
        ee_leer_seg(r, i, &s);
        uart_num(i + 1);
        uart_campo(s.temp_c);
        uart_campo((int16_t)s.rampa_min);
        uart_campo((int16_t)s.meseta_min);
        uart_fin();
    }
}

static void mostrar_ganancias(void)
{
    uart_char('K');
    uart_campo(kp);
    uart_campo(ki);
    uart_campo(kd);
    uart_fin();
}

#if USAR_ESTADO
static void mostrar_estado(void)
{
    uart_texto("EST=");    uart_char((char)estado);
    uart_texto(";COD=");   uart_num(cod_falla);
    uart_texto(";R=");     uart_num(ranura + 1);
    uart_texto(";N=");     uart_num(n_seg);
    uart_texto(";TR=");    uart_num(segmento + 1);
    uart_texto(";FASE=");  uart_char(en_rampa ? 'R' : 'M');
    uart_texto(";KP=");    uart_num(kp);
    uart_texto(";KI=");    uart_num(ki);
    uart_texto(";KD=");    uart_num(kd);
    uart_texto(";CTL=");   uart_num(ctl_modo);
    uart_texto(";TEL=");   uart_num(tele_s);
    uart_texto(";VDD=");   uart_num(vdd_mv);
#if USAR_IA
    uart_texto(";IA=");    uart_num(ia_clase);
#endif
    uart_texto(";RST=");   uart_texto(RST_TXT[rst_causa]);
    uart_fin();
}
#endif

#define R_ERR   0                        // respuesta: ERR
#define R_OK    1                        // respuesta: OK
#define R_NADA  2                        // el comando ya contestó (listas)

static void procesar_comando(void)
{
    char *buf = (char *)rx_buf;
    char c, *q;
    int16_t n[4];
    uint8_t k = 0, r, x = 0, h1, h2, ocupado, res = R_ERR, guardar = 0;
    Segmento s;

    if (rx_desborde) goto fin;

    // Suma de verificación opcional: ...*hh
    for (q = buf; *q && *q != '*'; q++) x ^= (uint8_t)*q;
    if (*q == '*') {
        h1 = hex_val(q[1]);
        h2 = h1 == 0xFF ? 0xFF : hex_val(q[2]);
        if (h2 == 0xFF || (uint8_t)((h1 << 4) | h2) != x) goto fin;
        *q = 0;
    }

    c = buf[0];
    if (c >= 'a' && c <= 'z') c = (char)(c - 32);   // acepta minúsculas
    cp = buf + 1;
    while (k < 4 && leer_num(&n[k])) k++;

    ocupado = (estado == EST_PERFIL || estado == EST_AUTO);
    r = ranura;                          // ranura del comando (L n, B n, D n, P n ...)
    if ((k && (c == 'L' || c == 'B' || c == 'D')) || (c == 'P' && k == 4)) {
        if (fuera(n[0], 1, N_RANURAS)) goto fin;
        r = (uint8_t)(n[0] - 1);
    }

    switch (c) {
        case 'I':
            if (estado == EST_FALLA || estado == EST_AUTO || n_seg == 0) break;
            perfil_iniciar();
            res = R_OK;
            break;
        case 'S':
            if (estado != EST_FALLA) estado = EST_ESPERA;
            pwm_set(0);
            integral = 0;
            res = R_OK;
            break;
        case 'L':
            mostrar_perfil(r);
            res = R_NADA;
            break;
        case 'B':
        case 'D':
            if (ocupado) break;                     // primero detén con S
            if (c == 'B') ran_borrar(r); else ran_defecto(r);
            res = R_OK;
            break;
        case 'P':
            if (ocupado || k < 3) break;
            if (k == 4) { n[0] = n[1]; n[1] = n[2]; n[2] = n[3]; }
            if (fuera(n[0], 0, T_MAX_SEGURIDAD_C - 10) || fuera(n[1], 0, 1000) ||
                fuera(n[2], 0, 1000)) break;
            s.temp_c = n[0];
            s.rampa_min = (uint16_t)n[1];
            s.meseta_min = (uint16_t)n[2];
            res = ran_agregar(r, &s);               // 0 si la ranura está llena
            break;
        case 'R':
            if (ocupado || k != 1 || fuera(n[0], 1, N_RANURAS)) break;
            ranura = (uint8_t)(n[0] - 1);
            n_seg = ran_n(ranura);
            guardar = 1;
            break;
        case 'K':
            if (k == 0) { mostrar_ganancias(); res = R_NADA; break; }
            if (k != 3 || fuera(n[0], 0, 1000) || fuera(n[1], 0, 1000) ||
                fuera(n[2], 0, 30000)) break;
            kp = n[0]; ki = n[1]; kd = n[2];
            guardar = 1;
            break;
        case 'C':
            if (k != 1 || fuera(n[0], 0, 7)) break;
            ctl_modo = (uint8_t)n[0];
            guardar = 1;
            break;
        case 'T':
            if (k != 1 || fuera(n[0], 0, 255)) break;
            tele_s = (uint8_t)n[0];
            tele_cnt = 0;
            guardar = 1;
            break;
#if USAR_AUTOAJUSTE
        case 'A':
            if (ocupado || estado == EST_FALLA || k < 1 ||
                fuera(n[0], 1, T_MAX_SEGURIDAD_C - 10)) break;
            at_iniciar(n[0], (uint8_t)(k < 2 || n[1] != 0));
            res = R_OK;
            break;
#endif
#if USAR_ESTADO
        case '?':
            mostrar_estado();
            res = R_NADA;
            break;
        case 'H':
            uart_texto("I S L[n] B[n] D[n] P[n] t,r,m R n K[kp,ki,kd] C m A t[,0] T s ? F[0]\r\n");
            res = R_NADA;
            break;
#endif
#if USAR_REGISTRO
        case 'F':
            if (k == 0) { log_mostrar(); res = R_NADA; break; }
            if (n[0] != 0) break;
            log_borrar();
            res = R_OK;
            break;
#endif
        default:                         // también A, ?, H y F si no están en este chip
            break;
    }
    if (guardar) {
        cfg_guardar();
        res = R_OK;
    }
fin:
    if (res == R_OK) uart_ok();
    else if (res == R_ERR) uart_err();
    rx_n = 0;
    rx_desborde = 0;
    rx_listo = 0;
}

static void telemetria(void)
{
    uart_num((int32_t)reloj_s);
    uart_temp(setpoint_q);
    uart_temp(prom_q);
    uart_campo((int16_t)duty_actual);
    for (uint8_t ch = 0; ch < 5; ch++) uart_temp(temp_q[ch]);
    uart_sep();
    uart_char((char)estado);
    // Columnas nuevas de la v2 (al final, para no romper programas de la v1)
    uart_temp(tctl_q);
    uart_campo(desc_mask);
    uart_campo(cod_falla);
    uart_campo((int16_t)vdd_mv);
    uart_fin();
}

// ---------------- Inicialización ----------------
static void init(void)
{
    ANSELA = 0x00;               // IMPORTANTE: los pines arrancan como analógicos;
    ANSELB = 0x00;               // aquí se vuelven digitales
    CM1CON0 = 0x00;              // comparadores apagados
    CM2CON0 = 0x00;
    APFCON0 = 0x00;              // RX en RB1, CCP1 en RB3 (posición por defecto)
    APFCON1 = 0x00;              // TX en RB2
    OPTION_REG = 0x80;           // pull-ups internos apagados
    WDTCON = 0x16;               // watchdog de ~2 s

    LATA = 0x0A;                 // CS1 y CS2 en alto, SCK en bajo
    LATB = 0x35;                 // CS3, CS4, CS5 y TX en alto; RB3 (potencia) en bajo
    TRISA = 0xE4;                // RA2 entrada (SO)
    TRISB = 0x02;                // RB1 (RX) entrada; RB2 (TX) salida, como lo pide
                                 // el modelo de Proteus; el resto salidas

#if MODO_SALIDA == SALIDA_VENTANA
    CCP1CON = 0x00;              // RB3 como salida digital normal
    PR2 = 249;                   // Timer2: 1 us x 4 x 250 x 10 = 10 ms
    T2CON = 0x4D;                // postescala 1:10, encendido, preescala 1:4
    PIR1bits.TMR2IF = 0;
    PIE1bits.TMR2IE = 1;
#else
    CCPTMRS = 0x00;              // CCP1 usa el Timer2
    PR2 = 0xFF;                  // PWM 244 Hz
    CCPR1L = 0;
    CCP1CON = 0x0C;
    T2CON = 0x06;
#endif

    BAUDCON = 0x00;              // generador de 8 bits
    SPBRGH = 0;
    SPBRGL = 25;                 // 9600 baudios con 4 MHz y BRGH = 1
    TXSTA = 0x24;                // transmisor activo, alta velocidad
    RCSTA = 0x90;                // puerto serial y receptor activos

    T1CON = 0x31;                // Timer1: preescala 1:8 -> 0.5 s
    TMR1H = 0x0B;
    TMR1L = 0xDC;
    PIR1bits.TMR1IF = 0;
    PIE1bits.TMR1IE = 1;
    PIE1bits.RCIE = 1;
    INTCONbits.PEIE = 1;
    INTCONbits.GIE = 1;
}

// ---------------- Programa principal ----------------
void main(void)
{
    uint8_t ok;

    causa_reinicio();            // antes de cualquier CLRWDT
    init();
    pwm_set(0);
    uart_texto("\r\nHORNO PWM rev D - " NOMBRE_CHIP "\r\n");
    uart_texto("FW " VERSION_FW ";RST=");
    uart_texto(RST_TXT[rst_causa]);
    uart_fin();

#if MODO_PRUEBA
    pwm_set(PWM_PRUEBA);
    uart_texto("MODO PRUEBA\r\n");
    while (1) { CLRWDT(); }
#endif

    ee_revisar();
    mostrar_perfil(ranura);
    uart_texto("seg;sp;prom;pwm;T1;T2;T3;T4;T5;est;ctl;desc;cod;vdd\r\n");

    for (uint8_t i = 0; i < 3; i++) {    // primera conversión de los MAX6675
        __delay_ms(100);
        CLRWDT();
    }
    leer_sensores();
    setpoint_q = tctl_q;
#if AUTOARRANQUE
    // Si el reinicio fue por el watchdog o por la pila, algo falló en el
    // programa: no se vuelve a calentar solo, se espera el comando I.
    if (n_seg && rst_causa != RST_WDT && rst_causa != RST_PILA) perfil_iniciar();
#endif

    while (1) {
#if SIMULAR_SENSORES
        __delay_ms(10);                  // cada vuelta = 1 s del horno
        CLRWDT();
        if (rx_listo) procesar_comando();
        modelo_horno_1s();
#else
        while (medios_segundos < 2) {    // espera 1 s atendiendo el serial
            CLRWDT();
            if (rx_listo) procesar_comando();
        }
        INTCONbits.GIE = 0;
        medios_segundos -= 2;
        INTCONbits.GIE = 1;
#endif
        reloj_s++;

        ok = leer_sensores();
#if USAR_VDD
        vdd_medir();
#endif

        // Seguridad: activa siempre, aunque el perfil no esté corriendo
        seguridad(ok);

        if (estado == EST_PERFIL) perfil_paso_1s();
        if (estado == EST_PERFIL) {
            pwm_set(control_pid());
#if USAR_AUTOAJUSTE
        } else if (estado == EST_AUTO) {
            pwm_set(autoajuste_1s());
#endif
        } else {
            pwm_set(0);
            integral = 0;
        }
#if USAR_IA
        ia_paso_1s();                    // diagnóstico con IA (solo avisa)
#endif

        if (tele_s && ++tele_cnt >= tele_s) {
            tele_cnt = 0;
            telemetria();
        }
    }
}
