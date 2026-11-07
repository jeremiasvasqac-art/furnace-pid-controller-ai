#!/usr/bin/env python3
"""
horno_pc.py - Programa de PC para el controlador del horno PWM (rev D, firmware v2).

Lee la telemetría por el puerto serial, la guarda en CSV, la grafica en vivo
y carga perfiles desde un archivo. También sirve para revisar una captura ya
guardada (por ejemplo la salida del banco de pruebas).

Necesita:  pip install pyserial matplotlib    (matplotlib solo para --graf)

Ejemplos:
  python horno_pc.py monitor COM5 --csv corrida.csv --graf
  python horno_pc.py cargar COM5 perfil.txt --ranura 2
  python horno_pc.py cmd COM5 "K"
  python horno_pc.py cmd COM5 "A 150"
  python horno_pc.py archivo captura.txt --csv corrida.csv --graf

Formato del archivo de perfil (un tramo por renglón, # = comentario):
  # temp_C, rampa_min, meseta_min
  100, 10, 15
  180, 15, 30
  60, 30, 0
"""
import argparse
import csv
import sys
import time

BAUDIOS = 9600
COLUMNAS_V1 = ["seg", "sp", "prom", "pwm", "T1", "T2", "T3", "T4", "T5", "est"]
COLUMNAS_V2 = COLUMNAS_V1 + ["ctl", "desc", "cod", "vdd"]
FALLAS = {
    1: "sobretemperatura",
    2: "sensores insuficientes",
    3: "embalamiento (PWM alto y no sube)",
    4: "tiempo máximo de proceso",
    5: "calor con el PWM en 0 (MOSFET en corto)",
    6: "autoajuste fuera de margen",
}


# ---------------------------------------------------------------- protocolo
def suma(texto: str) -> str:
    """Suma de verificación estilo NMEA: XOR de todos los caracteres."""
    x = 0
    for c in texto.encode("ascii"):
        x ^= c
    return f"{x:02X}"


def con_suma(comando: str) -> str:
    return f"{comando}*{suma(comando)}"


def leer_telemetria(renglon: str):
    """Devuelve un dict si el renglón es telemetría (v1 o v2); si no, None."""
    partes = renglon.strip().split(";")
    if len(partes) not in (len(COLUMNAS_V1), len(COLUMNAS_V2)) or not partes[0].isdigit():
        return None
    cols = COLUMNAS_V2 if len(partes) == len(COLUMNAS_V2) else COLUMNAS_V1
    d = {}
    for nombre, valor in zip(cols, partes):
        if nombre == "est":
            d[nombre] = valor
        elif valor == "X":
            d[nombre] = None
        else:
            try:
                d[nombre] = int(valor)
            except ValueError:
                return None
    return d


def leer_perfil(ruta: str):
    tramos = []
    with open(ruta, encoding="utf-8") as f:
        for n, renglon in enumerate(f, 1):
            renglon = renglon.split("#", 1)[0].strip()
            if not renglon:
                continue
            try:
                t, r, m = (int(v) for v in renglon.replace(";", ",").split(","))
            except ValueError:
                sys.exit(f"{ruta}:{n}: se esperaba 'temp, rampa, meseta' y dice '{renglon}'")
            tramos.append((t, r, m))
    if not tramos:
        sys.exit(f"{ruta}: no tiene tramos")
    if len(tramos) > 9:
        sys.exit(f"{ruta}: tiene {len(tramos)} tramos y cada ranura admite 9")
    return tramos


# ---------------------------------------------------------------- serial
def abrir(puerto: str):
    try:
        import serial
    except ImportError:
        sys.exit("Falta pyserial:  pip install pyserial")
    s = serial.Serial(puerto, BAUDIOS, timeout=0.2)
    time.sleep(0.2)
    s.reset_input_buffer()
    return s


def renglones(fuente):
    """Genera renglones de texto desde un puerto serial."""
    pendiente = b""
    while True:
        pendiente += fuente.read(256)
        while b"\n" in pendiente:
            r, pendiente = pendiente.split(b"\n", 1)
            yield r.decode("ascii", "replace").strip("\r")
        yield None  # sin datos por ahora


def enviar(s, comando: str, espera_s: float = 2.0, verificar=True):
    """Manda un comando (con suma) y regresa los renglones que lleguen."""
    linea = con_suma(comando) if verificar else comando
    s.write((linea + "\r\n").encode("ascii"))
    fin = time.time() + espera_s
    salida = []
    for r in renglones(s):
        if r:
            salida.append(r)
            if r in ("OK", "ERR"):
                break
        if time.time() > fin:
            break
    return salida


# ---------------------------------------------------------------- gráfica
class Grafica:
    def __init__(self):
        try:
            import matplotlib.pyplot as plt
        except ImportError:
            sys.exit("Falta matplotlib:  pip install matplotlib")
        self.plt = plt
        plt.ion()
        self.fig, (self.ax_t, self.ax_p) = plt.subplots(2, 1, sharex=True, figsize=(10, 6),
                                                        gridspec_kw={"height_ratios": [3, 1]})
        self.fig.canvas.manager.set_window_title("Horno PWM")
        self.datos = []

    def agregar(self, d):
        self.datos.append(d)

    def dibujar(self, final=False):
        if not self.datos:
            return
        x = [d["seg"] / 60 for d in self.datos]
        self.ax_t.clear()
        self.ax_p.clear()
        self.ax_t.plot(x, [d["sp"] for d in self.datos], "k--", label="setpoint")
        self.ax_t.plot(x, [d["prom"] for d in self.datos], label="promedio", linewidth=2)
        for i in range(1, 6):
            v = [d[f"T{i}"] for d in self.datos]
            if any(t is not None for t in v):
                self.ax_t.plot(x, v, linewidth=0.8, alpha=0.6, label=f"T{i}")
        self.ax_t.set_ylabel("°C")
        self.ax_t.legend(loc="upper left", fontsize=8)
        self.ax_t.grid(True, alpha=0.3)
        u = self.datos[-1]
        titulo = f"estado {u['est']}  ·  {u['prom']} °C  ·  PWM {u['pwm'] * 100 // 1023} %"
        if u.get("cod"):
            titulo += f"  ·  FALLA {u['cod']}: {FALLAS.get(u['cod'], '?')}"
        self.ax_t.set_title(titulo)
        self.ax_p.plot(x, [d["pwm"] * 100 / 1023 for d in self.datos], color="tab:red")
        self.ax_p.set_ylabel("PWM %")
        self.ax_p.set_xlabel("minutos")
        self.ax_p.set_ylim(-5, 105)
        self.ax_p.grid(True, alpha=0.3)
        self.fig.tight_layout()
        if final:
            self.plt.ioff()
            self.plt.show()
        else:
            self.plt.pause(0.01)


# ---------------------------------------------------------------- comandos
class Registro:
    """Guarda la telemetría en CSV y avisa de fallas y descartes."""

    def __init__(self, ruta_csv, grafica):
        self.csv = None
        self.f = None
        if ruta_csv:
            self.f = open(ruta_csv, "w", newline="", encoding="utf-8")
            self.csv = csv.writer(self.f)
            self.csv.writerow(COLUMNAS_V2)
        self.grafica = grafica
        self.n = 0
        self.desc_ant = 0

    def renglon(self, r: str, eco=True):
        d = leer_telemetria(r)
        if d is None:
            if eco:
                print(f"  {r}")
            if r.startswith("F;") and r[2:].isdigit():
                print(f"  *** FALLA {r[2:]}: {FALLAS.get(int(r[2:]), '?')}")
            return
        self.n += 1
        if self.csv:
            self.csv.writerow([d.get(c, "") if d.get(c) is not None else "X" for c in COLUMNAS_V2])
            self.f.flush()
        if d.get("desc", 0) != self.desc_ant:
            descartados = [f"T{i + 1}" for i in range(5) if d["desc"] >> i & 1]
            print(f"  aviso: sensores descartados: {', '.join(descartados) or 'ninguno'}")
            self.desc_ant = d.get("desc", 0)
        if eco:
            t = " ".join("--" if d[f"T{i}"] is None else f"{d[f'T{i}']:3d}" for i in range(1, 6))
            print(f"  {d['seg']:6d} s  sp {d['sp']:4d}  prom {d['prom']:4d}  pwm {d['pwm']:4d}  [{t}]  {d['est']}")
        if self.grafica:
            self.grafica.agregar(d)

    def cerrar(self):
        if self.f:
            self.f.close()


def cmd_monitor(a):
    s = abrir(a.puerto)
    g = Grafica() if a.graf else None
    reg = Registro(a.csv, g)
    print(f"Escuchando {a.puerto} a {BAUDIOS} baudios. Ctrl+C para salir.")
    ultimo = 0
    try:
        for r in renglones(s):
            if r:
                reg.renglon(r)
            if g and time.time() - ultimo > 1:
                g.dibujar()
                ultimo = time.time()
    except KeyboardInterrupt:
        pass
    finally:
        reg.cerrar()
        print(f"{reg.n} renglones de telemetría" + (f" guardados en {a.csv}" if a.csv else ""))


def cmd_archivo(a):
    g = Grafica() if a.graf else None
    reg = Registro(a.csv, g)
    with open(a.ruta, encoding="utf-8", errors="replace") as f:
        for r in f:
            reg.renglon(r.strip("\r\n"), eco=a.eco)
    reg.cerrar()
    print(f"{reg.n} renglones de telemetría" + (f" guardados en {a.csv}" if a.csv else ""))
    if g:
        g.dibujar(final=True)


def cmd_cargar(a):
    tramos = leer_perfil(a.perfil)
    s = abrir(a.puerto)
    rn = f" {a.ranura}" if a.ranura else ""
    pref = f"{a.ranura} " if a.ranura else ""
    print(f"Cargando {len(tramos)} tramos en la ranura {a.ranura or 'activa'}...")
    r = enviar(s, f"B{rn}")
    if "OK" not in r:
        sys.exit("El horno no aceptó B (¿está corriendo un perfil? manda S primero). Respuesta: " + " | ".join(r))
    for t, rm, m in tramos:
        r = enviar(s, f"P {pref}{t},{rm},{m}")
        estado = "OK" if "OK" in r else "ERR"
        print(f"  P {t},{rm},{m}  ->  {estado}")
        if estado != "OK":
            sys.exit("Tramo rechazado (límites: 0 <= temp <= T_MAX-10, 0 <= rampa, meseta <= 1000).")
    lista = [x for x in enviar(s, f"L{rn}", espera_s=1.5) if ";" in x or x.startswith("N=")]
    leidos = [tuple(int(v) for v in x.split(";")[1:]) for x in lista if x.count(";") == 3]
    if leidos == tramos:
        print("Verificado: el perfil guardado coincide con el archivo.")
    else:
        sys.exit(f"El perfil leído no coincide: {leidos}")


def cmd_cmd(a):
    s = abrir(a.puerto)
    for r in enviar(s, a.comando, espera_s=a.espera):
        print(r)


def main():
    p = argparse.ArgumentParser(description="PC del horno PWM (rev D, firmware v2)")
    sub = p.add_subparsers(dest="accion", required=True)

    m = sub.add_parser("monitor", help="leer la telemetría en vivo")
    m.add_argument("puerto")
    m.add_argument("--csv")
    m.add_argument("--graf", action="store_true")
    m.set_defaults(f=cmd_monitor)

    f = sub.add_parser("archivo", help="revisar una captura guardada")
    f.add_argument("ruta")
    f.add_argument("--csv")
    f.add_argument("--graf", action="store_true")
    f.add_argument("--eco", action="store_true", help="imprimir cada renglón")
    f.set_defaults(f=cmd_archivo)

    c = sub.add_parser("cargar", help="cargar un perfil desde un archivo")
    c.add_argument("puerto")
    c.add_argument("perfil")
    c.add_argument("--ranura", type=int, choices=[1, 2, 3, 4])
    c.set_defaults(f=cmd_cargar)

    k = sub.add_parser("cmd", help="mandar un comando")
    k.add_argument("puerto")
    k.add_argument("comando")
    k.add_argument("--espera", type=float, default=2.0, help="segundos para esperar respuesta")
    k.set_defaults(f=cmd_cmd)

    a = p.parse_args()
    a.f(a)


if __name__ == "__main__":
    main()
