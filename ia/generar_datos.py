#!/usr/bin/env python3
"""
generar_datos.py - Genera el dataset para la IA de diagnóstico del horno.

Corre el FIRMWARE REAL (compilado para la PC en banco/salida/banco_real.exe)
contra cientos de hornos simulados distintos. En cada corrida se elige al azar:
  - el horno: potencia (subida máxima K), constante de tiempo, retraso del
    termopar y temperatura ambiente;
  - una falla y el segundo en que aparece (o ninguna).
  - un perfil de 2 a 4 tramos que ese horno pueda alcanzar;
  - el ruido de los termopares y la diferencia de temperatura entre zonas.
El banco guarda, segundo a segundo, lo que el firmware ve y decide.

Clases:
  0 normal                      3 resistencia degradada (entrega 25-60 % de su potencia)
  1 resistencia abierta         4 termopar congelado (un canal)
  2 MOSFET / relé en corto      5 termopar con deriva (un canal, 0.02-0.15 °C/s)

Uso:  python ia/generar_datos.py [n_corridas] [semilla]
Solo usa la librería estándar de Python.
"""
import csv
import os
import random
import subprocess
import sys
import time

RAIZ = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXE = os.path.join(RAIZ, "banco", "salida", "banco_real.exe" if os.name == "nt" else "banco_real")
DATOS = os.path.join(RAIZ, "ia", "datos")


def perfil(rng, k, amb):
    """Perfil aleatorio de 2 a 4 tramos que este horno pueda alcanzar."""
    tope = int(min(240, amb + 0.85 * k - 10))
    n = rng.randint(2, 4)
    tramos = []
    for j in range(n):
        if j == 0:
            temp = rng.randint(min(80, tope), tope)            # siempre empieza calentando
        elif j == n - 1 and rng.random() < 0.6:
            temp = rng.randint(40, 80)                          # enfriamiento final
        else:
            temp = rng.randint(60, tope)
        meseta = 0 if (j == n - 1 and temp <= 80) else rng.randint(5, 40)
        tramos.append((temp, rng.randint(5, 40), meseta))
    return tramos


def parametros(rng, clase):
    k = rng.uniform(230, 450)          # °C que subiría al 100 % de PWM
    tau = rng.uniform(300, 1200)       # constante de tiempo del horno (s)
    taus = rng.uniform(5, 40)          # retraso del termopar (s)
    amb = rng.uniform(15, 35)          # ambiente (°C)
    tramos = perfil(rng, k, amb)
    dur = sum(r + m for _, r, m in tramos) * 60
    t_falla = rng.randint(300, max(400, dur - 300)) if clase else 999999
    sev = 0.0
    if clase == 3:
        sev = rng.uniform(0.25, 0.60)  # fracción de potencia que queda
    elif clase == 5:
        sev = rng.uniform(0.02, 0.15)  # deriva en °C/s
    canal = rng.randint(0, 4)
    return (k, tau, taus, amb, clase, t_falla, sev, canal, rng.randint(1, 10**6)), tramos, dur


def main():
    n = int(sys.argv[1]) if len(sys.argv) > 1 else 600
    semilla = int(sys.argv[2]) if len(sys.argv) > 2 else 2026
    if not os.path.exists(EXE):
        sys.exit(f"No existe {EXE}. Compila primero el banco (banco/correr_banco.ps1).")
    os.makedirs(DATOS, exist_ok=True)
    rng = random.Random(semilla)
    t0 = time.time()
    with open(os.path.join(DATOS, "corridas.csv"), "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["corrida", "k", "tau", "tau_s", "amb", "clase", "t_falla", "sev", "canal", "semilla",
                    "t_max", "duracion", "perfil", "ruido", "zonas"])
        for i in range(n):
            clase = i % 6                       # clases balanceadas por corrida
            p, tramos, dur = parametros(rng, clase)
            txt = ";".join(f"{a},{b},{c}" for a, b, c in tramos)
            ruido = rng.uniform(0.03, 0.40)     # probabilidad de ±0.25 °C de ruido por lectura
            zonas = rng.uniform(0.5, 4.0)       # escala de la diferencia entre zonas (hasta -8..+12 °C)
            archivo = os.path.join(DATOS, f"c{i:04d}.csv")
            args = ([EXE, "dataset", "-p"] + [f"{v:.4f}" if isinstance(v, float) else str(v) for v in p]
                    + ["-perfil", txt, "-t_fin", str(dur + 200), "-ruido", f"{ruido:.3f}",
                       "-zonas", f"{zonas:.3f}", "-ds", archivo])
            subprocess.run(args, stdout=subprocess.DEVNULL, check=False)
            w.writerow([i] + [round(v, 4) if isinstance(v, float) else v for v in p]
                       + [max(a for a, _, _ in tramos), dur, txt, round(ruido, 3), round(zonas, 3)])
            if (i + 1) % 50 == 0:
                print(f"  {i + 1}/{n} corridas ({time.time() - t0:.0f} s)")
    print(f"Listo: {n} corridas en {DATOS}")


if __name__ == "__main__":
    main()
