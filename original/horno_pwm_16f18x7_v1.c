/*
 * ============================================================================
 *  Controlador Horno PWM  -  rev D (con puerto serial)  -  Semibuhos
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
 *   - Sigue un perfil térmico (rampas y mesetas) guardado en la EEPROM.
 *     El perfil se carga desde la computadora por el puerto serial.
 *   - Controla la resistencia con PWM en RB3 (CCP1) y un control PI.
 *   - Manda la temperatura por serial cada 10 s.
 *   - Apaga la resistencia si pasa la temperatura máxima, si no hay
 *     sensores válidos o si el programa se traba (watchdog).
 *
 *  Conexiones (rev D):
 *   RA0 (pin 17) -> SCK  (común a los 5 MAX6675)
 *   RA2 (pin 1)  <- SO   (común a los 5 MAX6675)
 *   RA1 (pin 18) -> CS1  (TC1, J9)
 *   RA3 (pin 2)  -> CS2  (TC2, J10)
 *   RB0 (pin 6)  -> CS3  (TC3, J11)
 *   RB4 (pin 10) -> CS4  (TC4, J12)   <- antes en RB1
 *   RB5 (pin 11) -> CS5  (TC5, J13)   <- antes en RB2
 *   RB1 (pin 7)  <- RX   (J14 pin 3, viene del TX del adaptador USB)
 *   RB2 (pin 8)  -> TX   (J14 pin 2, va al RX del adaptador USB)
 *   RB3 (pin 9)  -> PWM_PIC (R1 -> LED del PC817)
 *   RB6, RB7 (pines 12 y 13): libres para el conector de programación J15
 *
 *  Puerto serial: 9600 baudios, 8 bits, sin paridad, 1 bit de parada.
 *  Escribe un comando y presiona Enter:
 *
 *    I              Iniciar el perfil
 *    S              Detener (resistencia apagada)
 *    L              Mostrar el perfil guardado
 *    B              Borrar el perfil (para cargar uno nuevo)
 *    P t,r,m        Agregar un tramo: llegar a t °C en r minutos y
 *                   mantener m minutos. Ejemplo:  P 100,10,15
 *    D              Restaurar el perfil de fábrica (el de PERFIL_DEFECTO)
 *
 *  Telemetría, un renglón cada 10 s:
 *    seg;setpoint;promedio;pwm;T1;T2;T3;T4;T5;estado
 *    (temperaturas en °C, pwm de 0 a 1023, X = sensor sin lectura)
 *    estado: E = en espera, C = calentando/siguiendo perfil,
 *            F = falla de seguridad, T = perfil terminado
 * ============================================================================
 */

// ---------------- Bits de configuración (PIC16F1827 / PIC16F1847) ----------------
// CONFIG1
#pragma config FOSC     = XT     // Cristal de 4 MHz (igual que antes)
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
#pragma config PLLEN    = OFF    // Sin PLL: el PIC corre a 4 MHz como el 16F628A
#pragma config STVREN   = ON
#pragma config BORV     = HI     // Reinicio si VDD baja de ~2.5 V
#pragma config LVP      = OFF    // Libera RB4 (lo usamos para CS4)

#include <xc.h>
#include <stdint.h>

#define _XTAL_FREQ 4000000UL

#if defined(_16F1827)
#define NOMBRE_CHIP "PIC16F1827"
#else
#define NOMBRE_CHIP "PIC16F1847"
#endif

// ============================================================================
//  CONFIGURACIÓN DEL USUARIO
// ============================================================================

// 1 = al encender, arranca el perfil solo (sin computadora conectada).
// 0 = espera el comando I por el serial.
#define AUTOARRANQUE       1

// MODO DE PRUEBA: 1 = PWM fijo sin leer sensores (para el osciloscopio).
// ¡Regrésalo a 0 antes de conectar la resistencia del horno!
#define MODO_PRUEBA        0
#define PWM_PRUEBA       512    // 0-1023 (512 = 50 %)

// SIMULACIÓN EN PROTEUS: 1 = no lee los MAX6675; usa un modelo térmico del
// horno y corre unas 50 veces más rápido. ¡Ponlo en 0 para el PIC real!
#define SIMULAR_SENSORES   0
#define SIM_FALLA_SEG      0    // 0 = sin falla; N = desconecta los sensores en el segundo N

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

#define T_MAX_SEGURIDAD_C  250   // Si CUALQUIER sensor pasa de aquí, se apaga todo
#define MIN_SENSORES_OK      1   // Mínimo de sensores válidos para seguir calentando

// Ganancias del control PI (ajústalas probando con tu horno)
#define KP   8
#define KI   1

// ============================================================================
//  Fin de la configuración. No necesitas cambiar lo de abajo.
// ============================================================================

#define N_DEFECTO    (sizeof(PERFIL_DEFECTO) / sizeof(PERFIL_DEFECTO[0]))
#define MAX_SEG      40           // tramos que caben en la EEPROM de 256 bytes (6 bytes c/u)
#define PWM_MAX      1023
#define SENSOR_FALLA (-1)

// EEPROM: [0] = 0xA5 (perfil válido), [1] = número de tramos,
//         [2] = suma de verificación, [3...] = tramos de 6 bytes
#define EE_MAGIA     0xA5
#define EE_N         1
#define EE_SUMA      2
#define EE_DATOS     3

// Pines
// Las salidas usan los registros LAT (el 16F1847 los tiene; evita errores
// de lectura-modificación-escritura). La entrada SO se lee del puerto.
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

static volatile uint8_t medios_segundos = 0;
static volatile char    rx_buf[20];
static volatile uint8_t rx_n = 0;
static volatile uint8_t rx_listo = 0;

static int16_t  temp_q[5];        // temperaturas en 0.25 °C (o SENSOR_FALLA)
static int16_t  prom_q, max_q;
static uint8_t  estado = EST_ESPERA;
static uint8_t  n_seg = 0;
static uint8_t  segmento = 0;
static uint8_t  en_rampa = 1;
static uint16_t seg_fase = 0;
static uint16_t reloj_s = 0;
static int16_t  sp_inicio_q;
static int16_t  setpoint_q = 0;
static int32_t  integral = 0;
static uint16_t duty_actual = 0;
static Segmento seg_act;          // tramo en curso (leído de la EEPROM)

// ---------------- Interrupciones ----------------
void __interrupt() isr(void)
{
    if (PIR1bits.TMR1IF) {
        TMR1H = 0x0B;            // 0.5 s
        TMR1L = 0xDC;
        PIR1bits.TMR1IF = 0;
        medios_segundos++;
    }
    if (PIR1bits.RCIF) {
        char c = RCREG;
        if (RCSTAbits.OERR) { RCSTAbits.CREN = 0; RCSTAbits.CREN = 1; }
        if (!rx_listo) {
            if (c == '\r' || c == '\n') {
                if (rx_n) { rx_buf[rx_n] = 0; rx_listo = 1; }
            } else if (rx_n < sizeof(rx_buf) - 1) {
                rx_buf[rx_n++] = c;
            }
        }
    }
}

// ---------------- PWM en RB3 (CCP1), 244 Hz, 10 bits ----------------
static void pwm_set(uint16_t d)
{
    if (d > PWM_MAX) d = PWM_MAX;
    duty_actual = d;
    CCPR1L = (uint8_t)(d >> 2);
    CCP1CON = (uint8_t)(0x0C | ((d & 0x03) << 4));
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

static void uart_num(int16_t n)
{
    char buf[6];
    uint8_t i = 0;
    if (n < 0) { uart_char('-'); n = -n; }
    do { buf[i++] = (char)('0' + n % 10); n /= 10; } while (n && i < 6);
    while (i) uart_char(buf[--i]);
}

static void uart_fin(void)  { uart_texto("\r\n"); }
static void uart_ok(void)   { uart_texto("OK\r\n"); }
static void uart_err(void)  { uart_texto("ERR\r\n"); }

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

static void ee_leer_seg(uint8_t i, Segmento *s)
{
    uint8_t a = (uint8_t)(EE_DATOS + i * 6);
    s->temp_c     = (int16_t)(ee_leer(a)     | (ee_leer(a + 1) << 8));
    s->rampa_min  = (uint16_t)(ee_leer(a + 2) | (ee_leer(a + 3) << 8));
    s->meseta_min = (uint16_t)(ee_leer(a + 4) | (ee_leer(a + 5) << 8));
}

static uint8_t ee_suma_calc(uint8_t n)
{
    uint8_t s = 0;
    uint16_t fin = EE_DATOS + n * 6u;
    for (uint16_t a = EE_DATOS; a < fin; a++) s += ee_leer((uint8_t)a);
    return s;
}

static void ee_borrar(void)
{
    ee_escribir(0, EE_MAGIA);
    ee_escribir(EE_N, 0);
    ee_escribir(EE_SUMA, 0);
    n_seg = 0;
}

static uint8_t ee_agregar(const Segmento *s)
{
    uint8_t a, suma;
    if (n_seg >= MAX_SEG) return 0;
    a = (uint8_t)(EE_DATOS + n_seg * 6);
    ee_escribir(a,     (uint8_t)s->temp_c);
    ee_escribir(a + 1, (uint8_t)((uint16_t)s->temp_c >> 8));
    ee_escribir(a + 2, (uint8_t)s->rampa_min);
    ee_escribir(a + 3, (uint8_t)(s->rampa_min >> 8));
    ee_escribir(a + 4, (uint8_t)s->meseta_min);
    ee_escribir(a + 5, (uint8_t)(s->meseta_min >> 8));
    n_seg++;
    suma = ee_suma_calc(n_seg);
    ee_escribir(EE_N, n_seg);
    ee_escribir(EE_SUMA, suma);
    return 1;
}

static void ee_cargar_defecto(void)
{
    ee_borrar();
    for (uint8_t i = 0; i < N_DEFECTO; i++) ee_agregar(&PERFIL_DEFECTO[i]);
}

// Al encender: revisa que la EEPROM tenga un perfil válido.
static void ee_revisar(void)
{
    uint8_t n = ee_leer(EE_N);
    if (ee_leer(0) != EE_MAGIA || n > MAX_SEG || ee_suma_calc(n) != ee_leer(EE_SUMA)) {
        ee_cargar_defecto();
    } else {
        n_seg = n;
    }
}

// ---------------- MAX6675 (SPI por software) ----------------
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

#if SIMULAR_SENSORES
// Modelo térmico de primer orden: al 100 % llegaría a 325 °C, constante de
// tiempo de 10 min. Cada sensor tiene un desfase, como en distintas zonas.
static int32_t t_horno = 25L * 4096;
static const int8_t DESFASE_C[5] = { -2, -1, 0, 1, 3 };

static void modelo_horno_1s(void)
{
    int32_t final = 25L * 4096 + ((int32_t)300 * 4096 / PWM_MAX) * duty_actual;
    t_horno += (final - t_horno) / 600;
}

static int16_t sensor_simulado(uint8_t ch)
{
    if (SIM_FALLA_SEG && reloj_s >= SIM_FALLA_SEG) return SENSOR_FALLA;
    return (int16_t)(t_horno / 1024) + DESFASE_C[ch] * 4;
}
#endif

static uint8_t leer_sensores(void)
{
    int32_t suma = 0;
    uint8_t ok = 0;
    max_q = -32000;

    for (uint8_t ch = 0; ch < 5; ch++) {
        if (!USAR_SENSOR[ch]) { temp_q[ch] = SENSOR_FALLA; continue; }
#if SIMULAR_SENSORES
        temp_q[ch] = sensor_simulado(ch);
#else
        temp_q[ch] = max6675_leer(ch);
#endif
        if (temp_q[ch] != SENSOR_FALLA) {
            suma += temp_q[ch];
            ok++;
            if (temp_q[ch] > max_q) max_q = temp_q[ch];
        }
    }
    prom_q = ok ? (int16_t)(suma / ok) : 0;
    return ok;
}

// ---------------- Perfil térmico ----------------
static void perfil_iniciar(void)
{
    segmento = 0;
    en_rampa = 1;
    seg_fase = 0;
    integral = 0;
    sp_inicio_q = prom_q;
    setpoint_q = prom_q;
    ee_leer_seg(0, &seg_act);
    estado = EST_PERFIL;
}

static void perfil_paso_1s(void)
{
    int16_t  objetivo_q = seg_act.temp_c * 4;
    uint16_t dur_s;

    if (en_rampa) {
        dur_s = seg_act.rampa_min * 60u;
        if (seg_fase >= dur_s || dur_s == 0) {
            setpoint_q = objetivo_q;
            en_rampa = 0;
            seg_fase = 0;
        } else {
            setpoint_q = (int16_t)(sp_inicio_q +
                ((int32_t)(objetivo_q - sp_inicio_q) * seg_fase) / dur_s);
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
            ee_leer_seg(segmento, &seg_act);
            sp_inicio_q = objetivo_q;
            en_rampa = 1;
            seg_fase = 0;
            return;
        }
        seg_fase++;
    }
}

// ---------------- Control PI ----------------
static uint16_t control_pi(void)
{
    int16_t e = setpoint_q - prom_q;
    int32_t salida;

    if (e > 400)  e = 400;
    if (e < -400) e = -400;

    integral += (int32_t)e * KI;
    if (integral < 0) integral = 0;
    if (integral > (int32_t)PWM_MAX * 16) integral = (int32_t)PWM_MAX * 16;

    salida = (int32_t)e * KP + integral / 16;
    if (salida < 0) salida = 0;
    if (salida > PWM_MAX) salida = PWM_MAX;
    return (uint16_t)salida;
}

// ---------------- Comandos por serial ----------------
// Lee un número del texto; avanza el puntero. Devuelve 0 si no hay número.
static uint8_t leer_num(const char **p, int16_t *v)
{
    int16_t n = 0;
    uint8_t hay = 0, neg = 0;
    while (**p == ' ' || **p == ',') (*p)++;
    if (**p == '-') { neg = 1; (*p)++; }
    while (**p >= '0' && **p <= '9') {
        n = (int16_t)(n * 10 + (**p - '0'));
        (*p)++;
        hay = 1;
    }
    *v = neg ? -n : n;
    return hay;
}

static void mostrar_perfil(void)
{
    Segmento s;
    uart_texto("N=");
    uart_num(n_seg);
    uart_fin();
    for (uint8_t i = 0; i < n_seg; i++) {
        ee_leer_seg(i, &s);
        uart_num(i + 1);          uart_char(';');
        uart_num(s.temp_c);       uart_char(';');
        uart_num((int16_t)s.rampa_min); uart_char(';');
        uart_num((int16_t)s.meseta_min);
        uart_fin();
    }
}

static void procesar_comando(void)
{
    const char *p = (const char *)rx_buf;
    char c = rx_buf[0];
    int16_t t, r, m;
    Segmento s;

    if (c >= 'a' && c <= 'z') c = (char)(c - 32);   // acepta minúsculas

    switch (c) {
        case 'I':
            if (estado == EST_FALLA || n_seg == 0) { uart_err(); break; }
            perfil_iniciar();
            uart_ok();
            break;
        case 'S':
            if (estado != EST_FALLA) estado = EST_ESPERA;
            pwm_set(0);
            uart_ok();
            break;
        case 'L':
            mostrar_perfil();
            break;
        case 'B':
        case 'P':
        case 'D':
            if (estado == EST_PERFIL) { uart_err(); break; }   // primero detén con S
            if (c == 'B') { ee_borrar(); uart_ok(); break; }
            if (c == 'D') { ee_cargar_defecto(); uart_ok(); break; }
            p++;
            if (!leer_num(&p, &t) || !leer_num(&p, &r) || !leer_num(&p, &m) ||
                t < 0 || t > T_MAX_SEGURIDAD_C - 10 || r < 0 || r > 1000 || m < 0 || m > 1000) {
                uart_err();
                break;
            }
            s.temp_c = t;
            s.rampa_min = (uint16_t)r;
            s.meseta_min = (uint16_t)m;
            if (ee_agregar(&s)) uart_ok(); else uart_err();
            break;
        default:
            uart_err();
            break;
    }
    rx_n = 0;
    rx_listo = 0;
}

static void telemetria(void)
{
    uart_num((int16_t)reloj_s);     uart_char(';');
    uart_num(setpoint_q / 4);       uart_char(';');
    uart_num(prom_q / 4);           uart_char(';');
    uart_num((int16_t)duty_actual);
    for (uint8_t ch = 0; ch < 5; ch++) {
        uart_char(';');
        if (temp_q[ch] == SENSOR_FALLA) uart_char('X');
        else uart_num(temp_q[ch] / 4);
    }
    uart_char(';');
    uart_char((char)estado);
    uart_fin();
}

// ---------------- Inicialización ----------------
static void init(void)
{
    ANSELA = 0x00;               // IMPORTANTE: en el 16F1847 los pines arrancan
    ANSELB = 0x00;               // como analógicos; aquí se vuelven digitales
    CM1CON0 = 0x00;              // comparadores apagados
    CM2CON0 = 0x00;
    APFCON0 = 0x00;              // RX en RB1, CCP1 en RB3 (posición por defecto)
    APFCON1 = 0x00;              // TX en RB2
    OPTION_REG = 0x80;           // pull-ups internos apagados
    WDTCON = 0x16;               // watchdog de ~2 s

    LATA = 0x0A;                 // CS1 y CS2 en alto, SCK en bajo
    LATB = 0x31;                 // CS3, CS4, CS5 en alto
    TRISA = 0xE4;                // RA2 entrada (SO)
    TRISB = 0x06;                // RB1 y RB2 para el serial, el resto salidas

    PR2 = 0xFF;                  // PWM 244 Hz
    CCPR1L = 0;
    CCP1CON = 0x0C;
    T2CON = 0x06;

    BAUDCON = 0x00;              // generador de 8 bits, como en el 16F628A
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
    uint8_t validos;

    init();
    pwm_set(0);
    uart_texto("\r\nHORNO PWM rev D - " NOMBRE_CHIP "\r\n");

#if MODO_PRUEBA
    pwm_set(PWM_PRUEBA);
    uart_texto("MODO PRUEBA\r\n");
    while (1) { CLRWDT(); }
#endif

    ee_revisar();
    mostrar_perfil();
    uart_texto("seg;sp;prom;pwm;T1;T2;T3;T4;T5;est\r\n");

    for (uint8_t i = 0; i < 3; i++) {    // primera conversión de los MAX6675
        __delay_ms(100);
        CLRWDT();
    }
    leer_sensores();
    setpoint_q = prom_q;
#if AUTOARRANQUE
    if (n_seg) perfil_iniciar();
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

        validos = leer_sensores();

        // Seguridad: activa siempre, aunque el perfil no esté corriendo
        if (validos < MIN_SENSORES_OK && estado == EST_PERFIL) estado = EST_FALLA;
        if (validos && max_q > (int16_t)(T_MAX_SEGURIDAD_C * 4)) estado = EST_FALLA;

        if (estado == EST_PERFIL) {
            perfil_paso_1s();
        }
        if (estado == EST_PERFIL) {
            pwm_set(control_pi());
        } else {
            pwm_set(0);
            integral = 0;
        }

        if (reloj_s % 10 == 0) telemetria();
    }
}
