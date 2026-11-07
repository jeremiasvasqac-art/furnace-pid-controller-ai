/*
 * ia_arbol.h - Árbol de decisión para el diagnóstico del horno.
 * GENERADO por ia/entrenar.py: no lo edites a mano.
 * Profundidad 7, 18 hojas. Entradas f[0..9] (enteros, 0.25 °C):
 *   f[0] pwm_prom: mean PWM (60 s)
 *   f[1] dT60: temperature change (60 s)
 *   f[2] error: setpoint error
 *   f[3] temp: control temperature
 *   f[4] dispersion: sensor spread vs median, minus own baseline
 *   f[5] d_dispersion: spread change (60 s)
 *   f[6] quieto: longest unchanged reading
 *   f[7] eficiencia: heating efficiency vs. own baseline (%)
 *   f[8] dSP60: setpoint change (60 s)
 *   f[9] dPWM: PWM change vs. previous 60 s
 * Salida: 0 normal, 1 resistencia abierta, 2 MOSFET en corto, 3 resistencia degradada, 4 termopar congelado, 5 termopar con deriva
 */
static uint8_t ia_arbol(const int16_t *f)
{
    if (f[6] <= 87) {   /* quieto */
        if (f[4] <= 3) {   /* dispersion */
            if (f[2] <= 30) {   /* error */
                if (f[2] <= -36) {   /* error */
                    if (f[1] <= 5) {   /* dT60 */
                        return 0;   /* normal */
                    }
                    if (f[7] <= 82) {   /* eficiencia */
                        if (f[3] <= 860) {   /* temp */
                            return 0;   /* normal */
                        }
                        return 2;   /* MOSFET en corto */
                    }
                    return 2;   /* MOSFET en corto */
                }
                if (f[4] <= -3) {   /* dispersion */
                    if (f[9] <= 1) {   /* dPWM */
                        return 5;   /* termopar con deriva */
                    }
                    return 4;   /* termopar congelado */
                }
                if (f[7] <= 109) {   /* eficiencia */
                    return 0;   /* normal */
                }
                return 2;   /* MOSFET en corto */
            }
            if (f[7] <= 5) {   /* eficiencia */
                return 1;   /* resistencia abierta */
            }
            return 0;   /* normal */
        }
        if (f[6] <= 57) {   /* quieto */
            return 5;   /* termopar con deriva */
        }
        return 4;   /* termopar congelado */
    }
    if (f[6] <= 117) {   /* quieto */
        if (f[7] <= 1) {   /* eficiencia */
            if (f[1] <= -1) {   /* dT60 */
                return 4;   /* termopar congelado */
            }
            if (f[0] <= 534) {   /* pwm_prom */
                if (f[5] <= 0) {   /* d_dispersion */
                    return 0;   /* normal */
                }
                return 4;   /* termopar congelado */
            }
            return 4;   /* termopar congelado */
        }
        return 4;   /* termopar congelado */
    }
    return 4;   /* termopar congelado */
}
