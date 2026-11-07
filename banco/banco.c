/*
 * banco.c - Banco de pruebas del firmware del horno en la PC (sin el PIC).
 *
 * Incluye el firmware tal cual (horno_pwm_16f18x7.c) con la cabecera falsa
 * xc_pc.h y lo hace correr contra un modelo del horno:
 *   - El horno es de primer orden (al 100 % llegaría a 325 °C, tau = 600 s) y
 *     el termopar lo sigue con 20 s de retraso; cada sensor tiene un desfase.
 *   - Los MAX6675 se modelan bit a bit en la pata SO (RA2), así que se prueba
 *     también la lectura por SPI de software y la detección de fallas.
 *   - La potencia se toma de los registros reales: CCPR1L/CCP1CON en modo PWM,
 *     o el pin RB3 (LATB3) en modo ventana.
 *   - Cada CLRWDT() del lazo de espera es medio segundo (una interrupción del
 *     Timer1). En modo SIMULAR_SENSORES el tiempo lo marca __delay_ms(10).
 *
 * Uso:   banco <escenario> [-v] [-o salida.txt] [-ee_in a.bin] [-ee_out b.bin]
 *        banco lista
 * Sale con 0 si todo pasó, 1 si algo falló y 2 si el escenario no aplica.
 */
#define main pic_main
#include "../horno_pwm_16f18x7.c"
#undef main

#include <math.h>
#include <stdarg.h>
#ifdef _MSC_VER
#define strdup _strdup
#endif

/* Respuesta esperada de los comandos extra: si el chip no los tiene, ERR */
#if USAR_ESTADO
#define SI_EST(x) x
#else
#define SI_EST(x) "ERR"
#endif
#if USAR_REGISTRO
#define SI_REG(x) x
#else
#define SI_REG(x) "ERR"
#endif

#if MODO_PRUEBA
#define BUILD "prueba"
#elif SIMULAR_SENSORES && SIM_FALLA_SEG
#define BUILD "simfalla"
#elif SIMULAR_SENSORES
#define BUILD "sim"
#elif MODO_SALIDA == SALIDA_VENTANA
#define BUILD "ventana"
#elif defined(_16F1827)
#define BUILD "r1827"      /* sin extras: autoajuste, registro, VDD, ? y H */
#else
#define BUILD "real"       /* 16F1847: todo */
#endif

/* ======================= Resultados ======================= */
static int n_ok = 0, n_mal = 0, verbose = 0;
static FILE *fsal = NULL;

static void chequeo(int cond, const char *fmt, ...)
{
    va_list ap;
    printf(cond ? "  [OK]    " : "  [FALLA] ");
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
    if (cond) n_ok++; else n_mal++;
}

static void nota(const char *fmt, ...)
{
    va_list ap;
    printf("  [nota]  ");
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
}

/* ======================= Tiempo ======================= */
static long t_banco = 0;      /* segundos del banco */
static long t_fin = 100;
static int  medio = 0;

/* ======================= Renglones del serial ======================= */
typedef struct { long t; char *s; } Lin;
static Lin *lins = NULL;
static int nlins = 0, caplins = 0;

typedef struct { long seg; int sp, prom, pwm, T[5]; char est; int ctl, desc, cod, vdd; long t; int duty_reg; } Tel;
static Tel *tels = NULL;
static int ntels = 0, captels = 0;
static int tel_mal_formadas = 0;

static uint16_t duty_registro(void)
{
    if ((CCP1CON & 0x0F) == 0x0C) return (uint16_t)((CCPR1L << 2) | ((CCP1CON >> 4) & 3));
    return 0;
}

static int es_telemetria(const char *s, Tel *t)
{
    char tmp[256], *f[20], *p;
    int n = 0;
    if (!(s[0] >= '0' && s[0] <= '9')) return 0;
    if (!strchr(s, ';')) return 0;
    strncpy(tmp, s, 255); tmp[255] = 0;
    p = tmp;
    f[n++] = p;
    while ((p = strchr(p, ';')) != NULL && n < 20) { *p++ = 0; f[n++] = p; }
    if (n < 10) return 0;      /* listas del perfil o del registro: "1;100;10;15" */
    if (n != 14) { tel_mal_formadas++; return 0; }
    t->seg = atol(f[0]); t->sp = atoi(f[1]); t->prom = atoi(f[2]); t->pwm = atoi(f[3]);
    for (int i = 0; i < 5; i++) t->T[i] = (f[4 + i][0] == 'X') ? -999 : atoi(f[4 + i]);
    t->est = f[9][0]; t->ctl = atoi(f[10]); t->desc = atoi(f[11]); t->cod = atoi(f[12]); t->vdd = atoi(f[13]);
    t->t = t_banco;
    t->duty_reg = duty_registro();
    return 1;
}

static void nueva_linea(const char *s)
{
    Tel t;
    if (nlins == caplins) { caplins = caplins ? caplins * 2 : 1024; lins = realloc(lins, caplins * sizeof(Lin)); }
    lins[nlins].t = t_banco;
    lins[nlins].s = strdup(s);
    nlins++;
    if (es_telemetria(s, &t)) {
        if (ntels == captels) { captels = captels ? captels * 2 : 1024; tels = realloc(tels, captels * sizeof(Tel)); }
        tels[ntels++] = t;
        if (verbose) printf("  %6ld | %s\n", t_banco, s);
    } else {
        printf("  %6ld < %s\n", t_banco, s);
    }
}

static char tx_slot;
static int tx_pend = 0;
static char linea[256];
static int nlin = 0;

static void tx_byte(char c)
{
    if (fsal) fputc(c, fsal);
    if (c == '\r') return;
    if (c == '\n') { linea[nlin] = 0; if (nlin) nueva_linea(linea); nlin = 0; }
    else if (nlin < 255) linea[nlin++] = c;
}

char *banco_txreg(void)
{
    if (tx_pend) tx_byte(tx_slot);
    tx_pend = 1;
    return &tx_slot;
}

static void tx_flush(void)
{
    if (tx_pend) { tx_pend = 0; tx_byte(tx_slot); }
}

static void enviar(const char *s)
{
    printf("  %6ld > %s\n", t_banco, s);
    for (; *s; s++) { RCREG = (uint8_t)*s; PIR1bits.RCIF = 1; isr(); PIR1bits.RCIF = 0; }
    RCREG = '\r'; PIR1bits.RCIF = 1; isr(); PIR1bits.RCIF = 0;
}

/* Primer renglón exactamente igual a txt entre t0 y t1. -1 si no hay. */
static long buscar_exacto(const char *txt, long t0, long t1)
{
    for (int i = 0; i < nlins; i++)
        if (lins[i].t >= t0 && lins[i].t <= t1 && !strcmp(lins[i].s, txt)) return lins[i].t;
    return -1;
}

/* Primer renglón que contiene txt entre t0 y t1 (incluidos). -1 si no hay. */
static long buscar(const char *txt, long t0, long t1)
{
    for (int i = 0; i < nlins; i++)
        if (lins[i].t >= t0 && lins[i].t <= t1 && strstr(lins[i].s, txt)) return lins[i].t;
    return -1;
}

/* ======================= EEPROM ======================= */
uint8_t eemem[256];
static uint8_t eedat;
static uint8_t eecon2_val;
static int eecon2_pend = 0, ee_paso = 0;
static long ee_escrituras = 0;
static int ee_errores = 0;

static void ee_eecon2_procesar(void)
{
    if (!eecon2_pend) return;
    eecon2_pend = 0;
    if (eecon2_val == 0x55) ee_paso = 1;
    else if (eecon2_val == 0xAA && ee_paso == 1) ee_paso = 2;
    else ee_paso = 0;
}

static void ee_procesar(void)
{
    ee_eecon2_procesar();
    if (r_eecon1.s.RD) { eedat = eemem[EEADRL]; r_eecon1.s.RD = 0; }
    if (r_eecon1.s.WR) {
        if (ee_paso == 2 && r_eecon1.s.WREN && !r_eecon1.s.CFGS && !r_eecon1.s.EEPGD) {
            eemem[EEADRL] = eedat;
            ee_escrituras++;
        } else {
            ee_errores++;
            printf("  !!! escritura de EEPROM sin la secuencia correcta (dir %u)\n", EEADRL);
        }
        r_eecon1.s.WR = 0;
        ee_paso = 0;
    }
}

void *banco_eecon1(void) { ee_procesar(); return &r_eecon1; }
uint8_t *banco_eedat(void) { ee_procesar(); return &eedat; }
uint8_t *banco_eecon2(void)
{
    ee_eecon2_procesar();
    if (r_intcon.s.GIE) { ee_errores++; printf("  !!! EECON2 escrito con GIE = 1\n"); }
    eecon2_pend = 1;
    return &eecon2_val;
}

/* ======================= ADC / FVR ======================= */
static double vdd_sim = 5.00;

void *banco_fvrcon(void)
{
    r_fvrcon.s.FVRRDY = r_fvrcon.s.FVREN;
    return &r_fvrcon;
}

void *banco_adcon0(void)
{
    if (r_adcon0.s.GO_nDONE) {
        int a = 0;
        if (r_adcon0.s.ADON && r_adcon0.s.CHS == 31 && r_fvrcon.s.FVREN && r_fvrcon.s.ADFVR == 1)
            a = (int)(1.024 * 1023.0 / vdd_sim);
        else
            printf("  !!! ADC mal configurado para leer la FVR\n");
        if (a > 1023) a = 1023;
        ADRESH = (uint8_t)(a >> 8);
        ADRESL = (uint8_t)a;
        r_adcon0.s.GO_nDONE = 0;
    }
    return &r_adcon0;
}

/* ======================= Modelo del horno ======================= */
enum { S_NORMAL, S_ABIERTO, S_AUSENTE, S_FIJO, S_CONGELADO, S_DESFASE, S_DERIVA };
enum { C_NORMAL, C_QUEMADA, C_CORTO, C_DEGRADADA };
/* Parámetros del horno (los de fábrica son los del handoff; el modo dataset los cambia) */
static double m_k = 300.0, m_tau = 600.0, m_taus = 20.0, m_amb = 25.0, m_degr = 1.0;
static double h_horno = 25.0, h_sens = 25.0;
static int    cal_modo = C_NORMAL;
static int    s_modo[5];
static double s_val[5];
static int    s_cong_listo[5];
static double s_off[5] = { -2, -1, 0, 1, 3 };   /* desfase de cada zona (°C); -zonas lo escala */
static double p_ruido = 0.25;                     /* probabilidad de ±1 LSB por lectura; -ruido */
static double vent_acum = 0;
static int    vent_n = 0;
static unsigned semilla = 12345;
static int    ruido = 1;
static double t_max_horno = 0;

static int ruido_lsb(void)
{
    double r;
    semilla = semilla * 1103515245u + 12345u;
    r = ((semilla >> 8) & 0xFFFF) / 65536.0;
    return r < p_ruido / 2 ? -1 : (r < p_ruido ? 1 : 0);
}

static void modelo_1s(void)
{
    double u;
#if MODO_SALIDA == SALIDA_VENTANA
    u = vent_n ? vent_acum / vent_n : 0.0;
    vent_acum = 0; vent_n = 0;
#else
    u = duty_registro() / 1023.0;
#endif
    if (cal_modo == C_QUEMADA) u = 0;
    if (cal_modo == C_CORTO) u = 1;
    if (cal_modo == C_DEGRADADA) u *= m_degr;
    h_horno += (m_amb + m_k * u - h_horno) / m_tau;
    h_sens  += (h_horno - h_sens) / m_taus;
    if (h_horno > t_max_horno) t_max_horno = h_horno;
    for (int ch = 0; ch < 5; ch++)
        if (s_modo[ch] == S_DERIVA) s_val[ch] += m_degr;      /* deriva: °C por segundo */
}

static uint16_t palabra_max6675(int ch)
{
    double t = h_sens + s_off[ch];
    int q;
    switch (s_modo[ch]) {
        case S_AUSENTE: return 0x0000;
        case S_FIJO: t = s_val[ch]; break;
        case S_CONGELADO:
            if (!s_cong_listo[ch]) { s_val[ch] = t; s_cong_listo[ch] = 1; }
            t = s_val[ch];
            break;
        case S_DESFASE:
        case S_DERIVA: t += s_val[ch]; break;
        default: break;
    }
    q = (int)floor(t * 4 + 0.5);
    if (ruido && s_modo[ch] != S_FIJO && s_modo[ch] != S_CONGELADO) q += ruido_lsb();
    if (q < 0) q = 0;
    if (q > 4095) q = 4095;
    return (uint16_t)((q << 3) | (s_modo[ch] == S_ABIERTO ? 4 : 0));
}

static int max_ch = -1, max_n = 16;
static uint16_t max_w;
static int spi_errores = 0;

void *banco_porta(void)
{
    int lows = 0, ch = -1;
    if (!r_lata.s.LATA1) { lows++; ch = 0; }
    if (!r_lata.s.LATA3) { lows++; ch = 1; }
    if (!r_latb.s.LATB0) { lows++; ch = 2; }
    if (!r_latb.s.LATB4) { lows++; ch = 3; }
    if (!r_latb.s.LATB5) { lows++; ch = 4; }
    if (lows > 1) { spi_errores++; ch = -1; }
    if (ch < 0) { r_porta.s.RA2 = 0; max_ch = -1; return &r_porta; }
    if (r_lata.s.LATA0) spi_errores++;          /* SCK debe estar en bajo al leer */
    if (ch != max_ch || max_n >= 16) { max_ch = ch; max_n = 0; max_w = palabra_max6675(ch); }
    r_porta.s.RA2 = (max_w >> (15 - max_n)) & 1;
    max_n++;
    return &r_porta;
}

/* ======================= Guion de cada escenario ======================= */
enum { P_CMD, P_SENSOR, P_TODOS, P_CAL, P_VDD };
typedef struct { long t; int tipo; int a, b; double v; const char *cmd; const char *espera; } Paso;
#define CMD(t, c, e)          { t, P_CMD, 0, 0, 0, c, e }
#define SENSOR(t, ch, m, v)   { t, P_SENSOR, ch, m, v, NULL, NULL }
#define TODOS(t, m)           { t, P_TODOS, 0, m, 0, NULL, NULL }
#define CAL(t, m)             { t, P_CAL, m, 0, 0, NULL, NULL }
#define VDD(t, v)             { t, P_VDD, 0, 0, v, NULL, NULL }
#define FIN_GUION             { -1, 0, 0, 0, 0, NULL, NULL }

static const Paso *guion = NULL;
static int espera_desde = -1;
static const char *espera_txt = NULL, *espera_cmd = NULL;
static long ee_escr_marca = -1;

static void verificar_espera(void)
{
    int vio = 0, neg;
    const char *e;
    if (!espera_txt) return;
    neg = espera_txt[0] == '!';
    e = neg ? espera_txt + 1 : espera_txt;
    for (int i = espera_desde; i < nlins; i++) if (strstr(lins[i].s, e)) vio = 1;
    if (neg) chequeo(!vio, "'%s' no responde \"%s\"", espera_cmd, e);
    else     chequeo(vio, "'%s' responde \"%s\"", espera_cmd, e);
    espera_txt = NULL;
}

static void eventos(void)
{
    if (!guion) return;
    for (const Paso *p = guion; p->t >= 0; p++) {
        if (p->t != t_banco) continue;
        switch (p->tipo) {
            case P_CMD:
                tx_flush();
                verificar_espera();
                espera_desde = nlins;
                espera_txt = p->espera;
                espera_cmd = p->cmd;
                enviar(p->cmd);
                break;
            case P_SENSOR:
                s_modo[p->a] = p->b; s_val[p->a] = p->v; s_cong_listo[p->a] = 0;
                printf("  %6ld * TC%d -> modo %d (%.0f)\n", t_banco, p->a + 1, p->b, p->v);
                break;
            case P_TODOS:
                for (int i = 0; i < 5; i++) { s_modo[i] = p->b; s_cong_listo[i] = 0; }
                printf("  %6ld * todos los sensores -> modo %d\n", t_banco, p->b);
                break;
            case P_CAL:
                cal_modo = p->a;
                printf("  %6ld * resistencia -> %s\n", t_banco,
                       p->a == C_QUEMADA ? "quemada" : p->a == C_CORTO ? "MOSFET en corto" : "normal");
                break;
            case P_VDD:
                vdd_sim = p->v;
                printf("  %6ld * VDD -> %.2f V\n", t_banco, p->v);
                break;
        }
    }
}

/* ======================= Invariantes (cada segundo) ======================= */
static int inv_pwm_fuera = 0, inv_reportado = 0;

static void invariantes(void)
{
#if !MODO_PRUEBA
    int encendido = duty_actual != 0;
#if MODO_SALIDA == SALIDA_VENTANA
    encendido |= r_latb.s.LATB3;
#else
    encendido |= duty_registro() != 0;
#endif
    if (encendido && estado != EST_PERFIL && estado != EST_AUTO) {
        inv_pwm_fuera++;
        if (!inv_reportado) { inv_reportado = 1; printf("  !!! potencia encendida en el estado %c (t=%ld)\n", estado, t_banco); }
    }
#endif
}

/* ======================= Reloj del banco ======================= */
static void terminar(void);
static long ee_escr_inicio = 0;

/* ---- Modo dataset (datos para entrenar la IA) ----
 * Clases: 0 normal, 1 resistencia abierta, 2 MOSFET en corto, 3 resistencia
 * degradada (entrega solo una fracción de la potencia), 4 termopar congelado,
 * 5 termopar con deriva. Cada segundo se guarda lo que el firmware ve. */
static FILE  *fds = NULL;
static int    ds_clase = 0, ds_canal = 0, ds_activa = 0;
static long   ds_t_falla = -1, ds_t_fin_f = -1;
static double ds_sev = 0;

static void ds_activar(void)
{
    ds_activa = 1;
    switch (ds_clase) {
        case 1: cal_modo = C_QUEMADA; break;
        case 2: cal_modo = C_CORTO; break;
        case 3: cal_modo = C_DEGRADADA; m_degr = ds_sev; break;
        case 4: s_modo[ds_canal] = S_CONGELADO; s_cong_listo[ds_canal] = 0; break;
        case 5: s_modo[ds_canal] = S_DERIVA; s_val[ds_canal] = 0; m_degr = ds_sev; break;
        default: break;
    }
}

static void ds_registrar(void)
{
    fprintf(fds, "%ld,%d,%d,%d,%u", t_banco, setpoint_q, tctl_q, med_q, duty_actual);
    for (int ch = 0; ch < 5; ch++) fprintf(fds, ",%d", temp_q[ch]);
#if USAR_IA
    fprintf(fds, ",%c,%d,%d,%d,%d,%d\n", estado, cod_falla, desc_mask, ds_activa ? ds_clase : 0, ia_pred, ia_clase);
    ia_pred = 0xFF;                      /* marca: sin predicción nueva en este segundo */
#else
    fprintf(fds, ",%c,%d,%d,%d,255,0\n", estado, cod_falla, desc_mask, ds_activa ? ds_clase : 0);
#endif
    if (estado == EST_FALLA && ds_t_fin_f < 0) { ds_t_fin_f = t_banco + 30; t_fin = ds_t_fin_f; }
}

static void segundo(void)
{
    t_banco++;
    if (fds && t_banco == ds_t_falla) ds_activar();
#if !SIMULAR_SENSORES
    modelo_1s();
#endif
    if (fds) ds_registrar();
    invariantes();
    eventos();
    if (t_banco == 3) ee_escr_inicio = ee_escrituras;
    if (t_banco >= t_fin) terminar();
}

static void medio_segundo(void)
{
#if MODO_SALIDA == SALIDA_VENTANA
    for (int i = 0; i < 50; i++) {       /* 50 interrupciones de 10 ms */
        r_pir1.s.TMR2IF = 1;
        if (r_intcon.s.GIE && r_pie1.s.TMR2IE) isr();
        vent_acum += r_latb.s.LATB3;
        vent_n++;
    }
#endif
    r_pir1.s.TMR1IF = 1;
    if (r_intcon.s.GIE && r_pie1.s.TMR1IE) isr();
    if (++medio == 2) { medio = 0; segundo(); }
}

void banco_clrwdt(void)
{
    tx_flush();
#if !SIMULAR_SENSORES
    medio_segundo();
#endif
}

void banco_delay_ms(unsigned ms)
{
    tx_flush();
#if SIMULAR_SENSORES
    if (ms == 10) segundo();
#else
    (void)ms;
#endif
}

/* ======================= Ayudas para revisar la telemetría ======================= */
typedef struct { double err_max, sobrepaso; int n; } Metrica;

/* Error máximo |ctl - ref| entre t0 y t1 (en segundos del firmware) */
static Metrica medir(long t0, long t1, int ref)
{
    Metrica m = { 0, -999, 0 };
    for (int i = 0; i < ntels; i++) {
        if (tels[i].seg < t0 || tels[i].seg > t1) continue;
        double e = fabs((double)tels[i].ctl - ref);
        if (e > m.err_max) m.err_max = e;
        if (tels[i].ctl - ref > m.sobrepaso) m.sobrepaso = tels[i].ctl - ref;
        m.n++;
    }
    return m;
}

static const Tel *ultima_tel(void) { return ntels ? &tels[ntels - 1] : NULL; }

static void revisar_telemetria_basica(void)
{
    chequeo(tel_mal_formadas == 0, "telemetría con 14 columnas (%d renglones mal formados)", tel_mal_formadas);
#if MODO_SALIDA == SALIDA_PWM && !MODO_PRUEBA
    int mal_pwm = 0;
    for (int i = 0; i < ntels; i++) if (tels[i].pwm != tels[i].duty_reg) mal_pwm++;
    chequeo(mal_pwm == 0, "el PWM de la telemetría coincide con CCPR1L/CCP1CON (%d diferencias)", mal_pwm);
#endif
}

/* Revisa el perfil de fábrica (100 °C / 180 °C / 60 °C) que empezó en t0 */
static void revisar_perfil_defecto(long t0, double tol)
{
    Metrica m1 = medir(t0 + 900, t0 + 1500, 100);
    Metrica m2 = medir(t0 + 2700, t0 + 4200, 180);
    Metrica s1 = medir(t0 + 600, t0 + 1500, 100);
    Metrica s2 = medir(t0 + 2400, t0 + 4200, 180);
    Metrica sub = { 0, 0, 0 };
    for (int i = 0; i < ntels; i++)      /* seguimiento de la rampa de subida a 180 */
        if (tels[i].seg >= t0 + 1600 && tels[i].seg <= t0 + 2400) {
            double e = fabs((double)tels[i].ctl - tels[i].sp);
            if (e > sub.err_max) sub.err_max = e;
        }
    nota("meseta 100 °C: error máx %.0f °C, sobrepaso %.0f °C", m1.err_max, s1.sobrepaso);
    nota("meseta 180 °C: error máx %.0f °C, sobrepaso %.0f °C", m2.err_max, s2.sobrepaso);
    nota("rampa a 180 °C: error máx de seguimiento %.0f °C", sub.err_max);
    chequeo(m1.n > 0 && m1.err_max <= tol, "meseta de 100 °C dentro de ±%.0f °C", tol);
    chequeo(m2.n > 0 && m2.err_max <= tol, "meseta de 180 °C dentro de ±%.0f °C", tol);
    chequeo(s1.sobrepaso <= 5 && s2.sobrepaso <= 5, "sobrepaso de 5 °C o menos");
}

static void revisar_sin_fallas(void)
{
    int n = 0;
    for (int i = 0; i < nlins; i++) if (!strncmp(lins[i].s, "F;", 2) && strlen(lins[i].s) == 3) n++;
    chequeo(n == 0, "sin fallas de seguridad");
}

/* ======================= Escenarios ======================= */
static const Paso G_PERFIL[] = { FIN_GUION };

static void fin_perfil(void)
{
    const Tel *u = ultima_tel();
    chequeo(buscar("HORNO PWM rev D - PIC16F18", 0, 2) >= 0, "mensaje de arranque");
    chequeo(buscar("RST=POR", 0, 2) >= 0, "causa de reinicio = POR");
    chequeo(buscar("seg;sp;prom;pwm;T1;T2;T3;T4;T5;est;ctl;desc;cod;vdd", 0, 2) >= 0, "encabezado de telemetría");
    revisar_sin_fallas();
    chequeo(u && u->est == 'T', "el perfil termina en el estado T");
    revisar_telemetria_basica();
#if SIMULAR_SENSORES
    revisar_perfil_defecto(0, 3);
#else
    revisar_perfil_defecto(0, 2);
#endif
    chequeo(ee_escrituras - ee_escr_inicio == 0, "no se escribe la EEPROM mientras corre el perfil (%ld escrituras)",
            ee_escrituras - ee_escr_inicio);
    if (u) nota("VDD medido: %d mV", u->vdd);
}

static const Paso G_COMANDOS[] = {
    CMD(5, "L", "N=3"),
    CMD(6, "B", "ERR"),                 /* no se borra con el perfil corriendo */
    CMD(7, "S", "OK"),
    CMD(8, "B", "OK"),
    CMD(9, "P 120,5,10", "OK"),
    CMD(10, "p 50,10,0", "OK"),         /* minúsculas */
    CMD(11, "P 999,1,1", "ERR"),        /* pasa de la temperatura de seguridad */
    CMD(12, "P 80", "ERR"),             /* incompleto */
    CMD(13, "L", "2;50;10;0"),
    CMD(14, "P 2 150,1,1", "OK"),       /* tramo en la ranura 2 */
    CMD(15, "L 2", "1;150;1;1"),
    CMD(16, "L 5", "ERR"),
    CMD(17, "R 2", "OK"),
    CMD(18, "?", SI_EST("R=2;N=1")),
    CMD(19, "R 1", "OK"),
    CMD(20, "K", "K;32;16;0"),
    CMD(21, "K 40,20,5", "OK"),
    CMD(22, "K", "K;40;20;5"),
    CMD(23, "K 2000,1,1", "ERR"),
    CMD(24, "C 7", "OK"),
    CMD(25, "C 9", "ERR"),
    CMD(26, "T 5", "OK"),
    CMD(27, "H", SI_EST("I S L")),
    CMD(28, "L*4C", "N=2"),             /* suma de verificación correcta */
    CMD(29, "L*00", "ERR"),             /* suma incorrecta */
    CMD(30, "P 100,10,15 este renglon es demasiado largo", "ERR"),
    CMD(31, "Z", "ERR"),
    CMD(32, "I", "OK"),
    CMD(33, "R 2", "ERR"),              /* no se cambia de ranura corriendo */
    CMD(34, "A 100", "ERR"),
    CMD(35, "P 3 100,1,1", "ERR"),
    CMD(36, "K*4B", "K;40;20;5"),       /* con suma, como los manda horno_pc.py */
    CMD(37, "L 2*5E", "1;150;1;1"),
    CMD(38, "L 2*5e", "1;150;1;1"),     /* hexadecimal en minúsculas */
    CMD(100, "S", "OK"),
    CMD(101, "C 0", "OK"),
    CMD(102, "T 10", "OK"),
    CMD(103, "K 32,16,0", "OK"),
    CMD(110, "I", "OK"),                /* 120 °C en 5 min, 10 min, 50 °C en 10 min */
    CMD(2000, "D", "OK"),
    CMD(2001, "L", "3;60;30;0"),
    CMD(2002, "F", SI_REG("REG")),
    CMD(2004, "K 40,20,5", "OK"),
    CMD(2005, "T 30", "OK"),
    CMD(2006, "R 2", "OK"),
    FIN_GUION
};

static void intervalo_tel(long t0, long t1, int esperado)
{
    int mal = 0, n = 0;
    for (int i = 1; i < ntels; i++)
        if (tels[i - 1].t >= t0 && tels[i].t <= t1) { n++; if (tels[i].seg - tels[i - 1].seg != esperado) mal++; }
    chequeo(n > 0 && mal == 0, "telemetría cada %d s entre %ld y %ld", esperado, t0, t1);
}

static void fin_comandos(void)
{
    Metrica m = medir(110 + 300 + 300, 110 + 900, 120);   /* ultimos 5 min de la meseta */
    Metrica s = medir(110 + 300, 110 + 900, 120);
    const Tel *antes = NULL;
    for (int i = 0; i < ntels; i++) if (tels[i].t < 2000) antes = &tels[i];
    intervalo_tel(30, 99, 5);
    intervalo_tel(105, 1999, 10);
    intervalo_tel(2010, 2100, 30);
    chequeo(antes && antes->est == 'T', "el perfil de la ranura 1 terminó (T) antes de t=2000");
    nota("meseta 120 °C: error máx %.0f °C, sobrepaso %.0f °C", m.err_max, s.sobrepaso);
    chequeo(m.n > 0 && m.err_max <= 2, "la meseta de 120 °C se asienta en ±2 °C (últimos 5 min)");
    revisar_sin_fallas();
    revisar_telemetria_basica();
}

static const Paso G_PERSISTENCIA[] = {
    CMD(5, "S", "OK"),
    CMD(6, "K", "K;40;20;5"),
    CMD(7, "?", SI_EST("R=2;N=1")),
    CMD(8, "L 1", "3;60;30;0"),
    CMD(9, "L", "1;150;1;1"),
    CMD(10, "?", SI_EST("TEL=30")),
    FIN_GUION
};

static void fin_persistencia(void)
{
    chequeo(buscar("1;150;1;1", 0, 2) >= 0, "al encender muestra el perfil de la ranura activa (2)");
}

static const Paso G_SOBRETEMP[] = {
    SENSOR(200, 3, S_FIJO, 260),
    CMD(210, "I", "ERR"),
    CMD(211, "S", "OK"),
    CMD(212, "?", SI_EST("EST=F;COD=1")),
    CMD(213, "F", SI_REG(";255")),
    CMD(214, "F 0", SI_REG("OK")),
    CMD(215, "F", SI_REG("REG")),
    FIN_GUION
};

static void fin_sobretemp(void)
{
    long t = buscar_exacto("F;1", 0, t_fin);
    const Tel *u = ultima_tel();
    chequeo(t >= 200 && t <= 202, "falla 1 (sobretemperatura) en t=%ld", t);
    chequeo(u && u->est == 'F' && u->pwm == 0 && u->cod == 1, "se queda en F con PWM 0 y código 1");
#if USAR_REGISTRO
    chequeo(eemem[EE_LOG + 1] == 0, "el registro de fallas se borró con F 0");
#endif
}

static const Paso G_SIN_SENSORES[] = {
    SENSOR(300, 0, S_ABIERTO, 0), SENSOR(300, 1, S_AUSENTE, 0), SENSOR(300, 2, S_ABIERTO, 0), SENSOR(300, 3, S_AUSENTE, 0),
    SENSOR(600, 4, S_ABIERTO, 0),
    CMD(650, "F", SI_REG("REG")),
    FIN_GUION
};

static void fin_sin_sensores(void)
{
    long t = buscar_exacto("F;2", 0, t_fin);
    int x = 0;
    for (int i = 0; i < ntels; i++) if (tels[i].seg > 310 && tels[i].seg < 600 && tels[i].T[0] == -999 && tels[i].T[4] != -999) x++;
    chequeo(x > 0, "con 1 sensor de 5 sigue el control (MIN_SENSORES_OK = 1)");
    chequeo(t >= 600 && t <= 602, "falla 2 (sin sensores) en t=%ld", t);
#if USAR_REGISTRO
    chequeo(buscar_exacto("1;2;600;0", 650, 650) >= 0, "la falla 2 queda en el registro de la EEPROM");
#endif
}

static const Paso G_EMBALAMIENTO[] = { CAL(300, C_QUEMADA), FIN_GUION };

static void fin_embalamiento(void)
{
    long t = buscar_exacto("F;3", 0, t_fin);
    chequeo(t > 300 && t < 300 + RUNAWAY_SEG * 4, "falla 3 (embalamiento) en t=%ld", t);
}

static const Paso G_CORTO[] = { CMD(200, "S", "OK"), CAL(300, C_CORTO), FIN_GUION };

static void fin_corto(void)
{
    long t = buscar_exacto("F;5", 0, t_fin);
    chequeo(t > 300 && t < 600, "falla 5 (calor con PWM en 0) en t=%ld", t);
    chequeo(buscar_exacto("F;1", 0, t_fin) < 0, "se conserva la primera causa (no aparece F;1)");
}

static const Paso G_ATIPICO[] = { SENSOR(500, 1, S_DESFASE, 60), FIN_GUION };

static void fin_atipico(void)
{
    int desc = 0, prom_mal = 0;
    for (int i = 0; i < ntels; i++) {
        if (tels[i].seg <= 510) continue;
        if (tels[i].desc & 2) desc++;
        if (abs(tels[i].prom - tels[i].T[2]) > 4) prom_mal++;
    }
    chequeo(desc > 0, "TC2 (+60 °C) aparece descartado (desc bit 1)");
    chequeo(prom_mal == 0, "el promedio no se va con el sensor descartado");
    revisar_sin_fallas();
}

static const Paso G_CONGELADO1[] = {
    CMD(5, "S", "OK"), CMD(6, "C 3", "OK"), CMD(7, "I", "OK"),
    SENSOR(100, 2, S_CONGELADO, 0),
    FIN_GUION
};

static void fin_congelado1(void)
{
    int desc = 0;
    const Tel *u = ultima_tel();
    for (int i = 0; i < ntels; i++) if (tels[i].desc & 4) desc++;
    chequeo(desc > 0, "TC3 congelado aparece descartado (desc bit 2)");
    revisar_sin_fallas();
    chequeo(u && u->est == 'C' && abs(u->ctl - u->sp) <= 3, "el control sigue con los demás sensores");
}

static const Paso G_CONGELADO5[] = { TODOS(100, S_CONGELADO), FIN_GUION };

static void fin_congelado5(void)
{
    long t2 = buscar_exacto("F;2", 0, t_fin), t3 = buscar_exacto("F;3", 0, t_fin);
    nota("F;2 en t=%ld, F;3 en t=%ld", t2, t3);
    chequeo(t2 > 100 || t3 > 100, "con todos los sensores congelados entra en falla");
    chequeo(t_max_horno < T_MAX_SEGURIDAD_C, "el horno real no pasó de %d °C (máx %.0f °C)", T_MAX_SEGURIDAD_C, t_max_horno);
}

static const Paso G_AUTOAJUSTE[] = {
    CMD(5, "S", "OK"),
    CMD(6, "A 150", "OK"),
    CMD(2500, "K", "K;"),
    CMD(2501, "I", "OK"),
    FIN_GUION
};

static void fin_autoajuste(void)
{
    long t = buscar("A;OK", 0, t_fin);
    int k1 = -1, k2 = -1, k3 = -1, tu = -1, pp = -1;
    for (int i = 0; i < nlins; i++)
        if (strncmp(lins[i].s, "A;OK;", 5) == 0) sscanf(lins[i].s + 5, "%d;%d;%d;%d;%d", &k1, &k2, &k3, &tu, &pp);
    chequeo(t > 0, "el autoajuste termina (t=%ld)", t);
    nota("ganancias: KP=%d KI=%d KD=%d (Tu=%d s, pico a pico=%.2f °C)", k1, k2, k3, tu, pp / 4.0);
    chequeo(k1 >= 1 && k2 >= 0 && k3 >= 0, "ganancias válidas");
    chequeo(buscar("A;ERR", 0, t_fin) < 0, "sin A;ERR");
    revisar_sin_fallas();
    revisar_perfil_defecto(2501, 2);
    chequeo(ultima_tel() && ultima_tel()->est == 'T', "el perfil con las ganancias nuevas termina en T");
}

static const Paso G_WDT[] = { FIN_GUION };
static void ini_wdt(void) { PCON = 0x0F; STATUSbits.nTO = 0; }
static void fin_wdt(void)
{
    const Tel *u = ultima_tel();
    chequeo(buscar("RST=WDT", 0, 2) >= 0, "informa RST=WDT");
    chequeo(u && u->est == 'E', "después del watchdog no arranca solo (estado E)");
}

static void ini_bor(void) { PCON = 0x0E; }
static void fin_bor(void)
{
    const Tel *u = ultima_tel();
    chequeo(buscar("RST=BOR", 0, 2) >= 0, "informa RST=BOR");
    chequeo(u && u->est == 'C', "después de bajo voltaje sí arranca el perfil");
}

static void ini_mclr(void) { PCON = 0x07; }
static void fin_mclr(void) { chequeo(buscar("RST=MCLR", 0, 2) >= 0, "informa RST=MCLR"); }

static void ini_pila(void) { PCON = 0x8F; }
static void fin_pila(void)
{
    const Tel *u = ultima_tel();
    chequeo(buscar("RST=PILA", 0, 2) >= 0, "informa RST=PILA");
    chequeo(u && u->est == 'E', "después de un error de pila no arranca solo");
}

/* EEPROM con el formato de la versión 1 (perfil de 3 tramos en la dirección 0) */
static void ee_v1(const int seg[][3], int n, int suma_mal)
{
    uint8_t s = 0;
    memset(eemem, 0xFF, 256);
    eemem[0] = 0xA5;
    eemem[1] = (uint8_t)n;
    for (int i = 0; i < n; i++)
        for (int j = 0; j < 3; j++) {
            eemem[3 + i * 6 + j * 2] = (uint8_t)seg[i][j];
            eemem[4 + i * 6 + j * 2] = (uint8_t)(seg[i][j] >> 8);
        }
    for (int a = 3; a < 3 + n * 6; a++) s += eemem[a];
    eemem[2] = (uint8_t)(s + suma_mal);
}
static const int V1[3][3] = { { 90, 5, 5 }, { 150, 10, 20 }, { 40, 10, 0 } };
static void ini_migracion(void) { ee_v1(V1, 3, 0); }
static void ini_corrupto(void) { ee_v1(V1, 3, 1); }
static const Paso G_MIGRACION[] = { CMD(5, "S", "OK"), CMD(6, "K", "K;32;16;0"), CMD(7, "L 2", "N=0"), FIN_GUION };
static void fin_migracion(void) { chequeo(buscar("2;150;10;20", 0, 2) >= 0, "conserva el perfil de la versión 1"); }
static void fin_corrupto(void) { chequeo(buscar("1;100;10;15", 0, 2) >= 0, "suma mala: carga el perfil de fábrica"); }

static const Paso G_VDD[] = { VDD(50, 4.50), VDD(100, 5.0), VDD(150, 5.40), FIN_GUION };
static void fin_vdd(void)
{
    chequeo(buscar("AVISO;VDD;", 0, 40) < 0, "sin aviso con 5.00 V");
    chequeo(buscar("AVISO;VDD;4", 50, 52) >= 0, "aviso con 4.50 V");
    chequeo(buscar("AVISO;VDD;5", 150, 152) >= 0, "aviso con 5.40 V");
}

static const Paso G_PRUEBA[] = { FIN_GUION };
static void fin_prueba(void)
{
    chequeo(buscar("MODO PRUEBA", 0, 2) >= 0, "mensaje MODO PRUEBA");
    chequeo(duty_registro() == PWM_PRUEBA, "PWM fijo de %d en CCP1 (leído %u)", PWM_PRUEBA, duty_registro());
}

static const Paso G_SIMFALLA[] = { FIN_GUION };
static void fin_simfalla(void)
{
    long t = buscar_exacto("F;2", 0, t_fin);
    chequeo(t >= SIM_FALLA_SEG && t <= SIM_FALLA_SEG + 2, "falla 2 simulada en t=%ld", t);
}

static void ini_nada(void) {}

/* IA: resistencia abierta en t=2000 (meseta de 180 °C del perfil de fábrica) */
static const Paso G_IA[] = { CAL(2000, C_QUEMADA), FIN_GUION };
static void fin_ia(void)
{
#if USAR_IA
    long t_ia = buscar_exacto("IA;1", 0, t_fin), t_f = buscar_exacto("F;3", 0, t_fin);
    nota("IA;1 en t=%ld, regla F;3 en t=%ld", t_ia, t_f);
    chequeo(buscar("IA;", 0, 1999) < 0, "sin avisos de la IA antes de la falla");
    chequeo(t_ia > 2000 && (t_f < 0 || t_ia < t_f), "la IA avisa resistencia abierta antes que la regla");
#endif
}

static void fin_dataset(void)
{
    if (fds) fclose(fds);
    fds = NULL;
    nota("dataset: clase %d desde t=%ld", ds_clase, ds_t_falla);
}

typedef struct {
    const char *nombre, *builds, *desc;
    const Paso *guion;
    long t_fin;
    void (*ini)(void);
    void (*fin)(void);
} Escenario;

static const Escenario ESC[] = {
    { "perfil",       "real,r1827,ventana,sim", "perfil de fábrica completo",            G_PERFIL,       6200, ini_nada,      fin_perfil },
    { "comandos",     "real,r1827",  "todos los comandos y la suma de verificación",    G_COMANDOS,     2100, ini_nada,      fin_comandos },
    { "persistencia", "real,r1827",  "lo guardado por 'comandos' sobrevive al reinicio", G_PERSISTENCIA,   15, ini_nada,      fin_persistencia },
    { "sobretemp",    "real,r1827",  "un sensor pasa de 250 °C",                         G_SOBRETEMP,     300, ini_nada,      fin_sobretemp },
    { "sin_sensores", "real,r1827",  "se desconectan los sensores",                      G_SIN_SENSORES,  700, ini_nada,      fin_sin_sensores },
    { "embalamiento", "real,r1827",  "la resistencia se quema",                          G_EMBALAMIENTO, 1200, ini_nada,      fin_embalamiento },
    { "corto",        "real,r1827",  "el MOSFET queda en corto",                         G_CORTO,         900, ini_nada,      fin_corto },
    { "atipico",      "real,r1827",  "TC2 marca 60 °C de más",                           G_ATIPICO,       800, ini_nada,      fin_atipico },
    { "congelado1",   "real,r1827",  "TC3 (sensor de control) se congela",               G_CONGELADO1,   1500, ini_nada,      fin_congelado1 },
    { "congelado5",   "real,r1827",  "todos los sensores se congelan",                   G_CONGELADO5,    900, ini_nada,      fin_congelado5 },
    { "autoajuste",   "real",  "autoajuste a 150 °C y perfil con esas ganancias",  G_AUTOAJUSTE,   8700, ini_nada,      fin_autoajuste },
    { "wdt",          "real,r1827",  "reinicio por watchdog",                            G_WDT,            30, ini_wdt,       fin_wdt },
    { "bor",          "real,r1827",  "reinicio por bajo voltaje",                        G_WDT,            30, ini_bor,       fin_bor },
    { "mclr",         "real",  "reinicio por MCLR",                                G_WDT,            10, ini_mclr,      fin_mclr },
    { "pila",         "real",  "reinicio por desborde de la pila",                 G_WDT,            30, ini_pila,      fin_pila },
    { "migracion",    "real,r1827",  "EEPROM con el formato de la versión 1",            G_MIGRACION,      10, ini_migracion, fin_migracion },
    { "corrupto",     "real,r1827",  "EEPROM con la suma de verificación mala",          G_WDT,            10, ini_corrupto,  fin_corrupto },
    { "vdd",          "real",  "aviso de +5V fuera de rango",                      G_VDD,           200, ini_nada,      fin_vdd },
    { "prueba",       "prueba", "MODO_PRUEBA = 1",                                 G_PRUEBA,         10, ini_nada,      fin_prueba },
    { "simfalla",     "simfalla", "SIMULAR_SENSORES con SIM_FALLA_SEG",            G_SIMFALLA,      700, ini_nada,      fin_simfalla },
    { "ia",           "real",  "aviso de la IA con la resistencia abierta",        G_IA,           2600, ini_nada,      fin_ia },
    { "dataset",      "real,r1827", "genera datos para la IA (usar con -p y -ds)", G_PERFIL,       6200, ini_nada,      fin_dataset },
};
static const Escenario *esc = NULL;
static const char *ee_out = NULL;

static void terminar(void)
{
    tx_flush();
    if (nlin) { linea[nlin] = 0; nueva_linea(linea); nlin = 0; }
    verificar_espera();
    printf("  -- fin en t=%ld s --\n", t_banco);
    esc->fin();
    chequeo(ee_errores == 0, "escrituras de EEPROM con la secuencia correcta");
    chequeo(spi_errores == 0, "bus SPI: un solo CS a la vez y SCK en bajo al leer");
    chequeo(inv_pwm_fuera == 0, "la potencia solo se enciende en los estados C y A");
    printf("  Resultado [%s/%s]: %d bien, %d mal\n", BUILD, esc->nombre, n_ok, n_mal);
    if (ee_out) { FILE *f = fopen(ee_out, "wb"); if (f) { fwrite(eemem, 1, 256, f); fclose(f); } }
    if (fsal) fclose(fsal);
    exit(n_mal ? 1 : 0);
}

/* Escribe un perfil "t,r,m;t,r,m;..." en la ranura 1 de la EEPROM (formato del firmware) */
static void ee_perfil(const char *txt)
{
    int n = 0, t, r, m;
    uint8_t suma = 0;
    while (n < SEG_POR_RANURA && sscanf(txt, "%d,%d,%d", &t, &r, &m) == 3) {
        int v[3] = { t, r, m };
        for (int j = 0; j < 3; j++) {
            eemem[3 + n * 6 + j * 2] = (uint8_t)v[j];
            eemem[4 + n * 6 + j * 2] = (uint8_t)(v[j] >> 8);
        }
        n++;
        txt = strchr(txt, ';');
        if (!txt) break;
        txt++;
    }
    for (int a = 3; a < 3 + n * 6; a++) suma += eemem[a];
    eemem[0] = EE_MAGIA;
    eemem[1] = (uint8_t)n;
    eemem[2] = suma;
}

int main(int argc, char **argv)
{
    const char *nombre = argc > 1 ? argv[1] : "lista", *ee_in = NULL, *perfil_arg = NULL;
    long t_fin_arg = 0;
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "-v")) verbose = 1;
        else if (!strcmp(argv[i], "-o") && i + 1 < argc) fsal = fopen(argv[++i], "wb");
        else if (!strcmp(argv[i], "-ee_in") && i + 1 < argc) ee_in = argv[++i];
        else if (!strcmp(argv[i], "-ee_out") && i + 1 < argc) ee_out = argv[++i];
        else if (!strcmp(argv[i], "-sin_ruido")) ruido = 0;
        else if (!strcmp(argv[i], "-t_fin") && i + 1 < argc) t_fin_arg = atol(argv[++i]);
        else if (!strcmp(argv[i], "-ruido") && i + 1 < argc) p_ruido = atof(argv[++i]);
        else if (!strcmp(argv[i], "-zonas") && i + 1 < argc) {
            double g = atof(argv[++i]);
            for (int ch = 0; ch < 5; ch++) s_off[ch] *= g;
        }
        else if (!strcmp(argv[i], "-perfil") && i + 1 < argc) perfil_arg = argv[++i];
        else if (!strcmp(argv[i], "-ds") && i + 1 < argc) {
            fds = fopen(argv[++i], "w");
            if (fds) fprintf(fds, "t,sp,ctl,med,pwm,T1,T2,T3,T4,T5,est,cod,desc,clase,iap,iac\n");
        }
        else if (!strcmp(argv[i], "-p") && i + 9 < argc) {
            /* -p k tau tau_sensor ambiente clase t_falla severidad canal semilla */
            m_k = atof(argv[i + 1]); m_tau = atof(argv[i + 2]); m_taus = atof(argv[i + 3]);
            m_amb = atof(argv[i + 4]); ds_clase = atoi(argv[i + 5]); ds_t_falla = atol(argv[i + 6]);
            ds_sev = atof(argv[i + 7]); ds_canal = atoi(argv[i + 8]); semilla = (unsigned)atol(argv[i + 9]);
            h_horno = h_sens = m_amb;
            i += 9;
        }
    }
    for (size_t i = 0; i < sizeof(ESC) / sizeof(ESC[0]); i++) {
        if (!strcmp(nombre, "lista")) printf("%-13s [%s] %s\n", ESC[i].nombre, ESC[i].builds, ESC[i].desc);
        if (!strcmp(nombre, ESC[i].nombre)) esc = &ESC[i];
    }
    if (!esc) return strcmp(nombre, "lista") ? 2 : 0;
    if (!strstr(esc->builds, BUILD)) { printf("El escenario '%s' no aplica a la compilación '%s'\n", esc->nombre, BUILD); return 2; }

    printf("== [%s] %s: %s ==\n", BUILD, esc->nombre, esc->desc);
    memset(eemem, 0xFF, sizeof(eemem));
    if (ee_in) { FILE *f = fopen(ee_in, "rb"); if (f) { fread(eemem, 1, 256, f); fclose(f); } else { printf("no se pudo abrir %s\n", ee_in); return 1; } }
    PCON = 0x0C;               /* encendido: /POR = 0 */
    STATUSbits.nTO = 1;
    STATUSbits.nPD = 1;
    if (perfil_arg) ee_perfil(perfil_arg);
    guion = esc->guion;
    t_fin = t_fin_arg ? t_fin_arg : esc->t_fin;
    esc->ini();
    pic_main();
    return 1;                  /* pic_main nunca regresa */
}
