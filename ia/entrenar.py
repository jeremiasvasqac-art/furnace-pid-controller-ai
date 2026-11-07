#!/usr/bin/env python3
"""
entrenar.py - Entrena la IA de diagnóstico del horno y la compara con las reglas.

1. Lee las corridas de ia/datos (generadas por generar_datos.py con el firmware real).
2. Calcula cada 10 s las MISMAS 7 características que calcula el firmware
   (función ia_paso_1s en horno_pwm_16f18x7.c), con aritmética entera.
3. Entrena un árbol de decisión (CART, índice de Gini, pesos por clase) escrito
   aquí mismo, sin librerías externas.
4. Mide el modelo en corridas de prueba que no vio al entrenar y lo compara con
   las reglas fijas del firmware (fallas F;n y sensores descartados).
5. Exporta el árbol a C (ia_arbol.h) para el PIC16F1847 y escribe
   ia/resultados.md con todas las métricas.

Uso:  python ia/entrenar.py
"""
import csv
import glob
import json
import os
import random
import statistics
import time

RAIZ = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DATOS = os.path.join(RAIZ, "ia", "datos")
SALIDA_H = os.path.join(RAIZ, "ia_arbol.h")
SALIDA_MD = os.path.join(RAIZ, "ia", "resultados.md")
SALIDA_JSON = os.path.join(RAIZ, "ia", "modelo.json")

CLASES = ["normal", "resistencia abierta", "MOSFET en corto", "resistencia degradada",
          "termopar congelado", "termopar con deriva"]
CLASES_EN = ["Normal", "Open heater", "Shorted MOSFET/SSR", "Degraded heater",
             "Stuck thermocouple", "Drifting thermocouple"]
CARAC = ["pwm_prom", "dT60", "error", "temp", "dispersion", "d_dispersion", "quieto", "eficiencia", "dSP60", "dPWM"]
CARAC_EN = ["mean PWM (60 s)", "temperature change (60 s)", "setpoint error",
            "control temperature", "sensor spread vs median, minus own baseline", "spread change (60 s)",
            "longest unchanged reading", "heating efficiency vs. own baseline (%)", "setpoint change (60 s)", "PWM change vs. previous 60 s"]
K = len(CLASES)
# Resistencia degradada (3): sin una línea base guardada de cada horno se confunde con
# un horno sano con poca potencia (muchas falsas alarmas). El modelo no la aprende;
# la siguen cubriendo las reglas del firmware (falla 3). Queda como trabajo futuro.
EXCLUIR = {3}
PERSIST = 6           # evaluaciones iguales seguidas para dar un aviso (30 s)
SEMILLA = 7


# ---------------------------------------------------------------- características
def div_c(a, b):
    """División entera como en C (trunca hacia cero)."""
    q = abs(a) // abs(b)
    return q if (a >= 0) == (b > 0) else -q


EF_INI, EF_FIN = 6, 18     # evaluaciones 6 a 18 (segundos ~120 a ~240): línea base de eficiencia

def caracteristicas(filas):
    """Réplica exacta (enteros) de ia_paso_1s() del firmware.
    filas: lista de dicts por segundo. Devuelve lista de (i_fila, [10 enteros])."""
    ult = [0] * 5
    quieto = [0] * 5
    pwm10 = []          # sumas de PWM de los últimos 12 bloques de 10 s (120 s)
    snap_ctl = []       # temperatura de control cada 10 s (últimos 7)
    snap_dis = []       # dispersión cada 10 s (últimos 7)
    snap_sp = []        # setpoint cada 10 s (últimos 7)
    acc = 0
    seg = 0
    neval = 0
    base_dT = base_pwm = base_dis = 0
    salida = []
    # El firmware reinicia la IA al arrancar el perfil (perfil_iniciar): se empieza a
    # contar desde la primera fila con el perfil corriendo.
    inicio = next((i for i, r in enumerate(filas) if r["est"] in ("C", "A")), len(filas))
    for i, r in enumerate(filas):
        if i < inicio:
            continue
        med = r["med"]
        dis = 0
        for ch in range(5):
            t = r["T"][ch]
            if t == -1:
                quieto[ch] = 0
                continue
            if t == ult[ch]:
                quieto[ch] = min(255, quieto[ch] + 1)
            else:
                ult[ch] = t
                quieto[ch] = 0
            d = abs(t - med)
            if d > dis:
                dis = d
        acc += r["pwm"]
        seg += 1
        if seg < 10:
            continue
        seg = 0
        pwm10.append(acc)
        acc = 0
        snap_ctl.append(r["ctl"])
        snap_dis.append(dis)
        snap_sp.append(r["sp"])
        if len(pwm10) > 12:
            pwm10.pop(0)
        if len(snap_ctl) > 7:
            snap_ctl.pop(0)
            snap_dis.pop(0)
            snap_sp.pop(0)
        if len(pwm10) < 12:
            continue
        q = max([quieto[ch] for ch in range(5) if r["T"][ch] != -1], default=0)
        f = [sum(pwm10[6:]) // 60, r["ctl"] - snap_ctl[0], r["sp"] - r["ctl"], r["ctl"],
             dis, dis - snap_dis[0], q]
        neval += 1
        if EF_INI <= neval <= EF_FIN:          # línea base: el horno al empezar (sano)
            base_dT += f[1]
            base_pwm += f[0]
            base_dis += dis
        # dispersión relativa a la del propio horno al arrancar (quita las diferencias
        # normales entre zonas); 0 mientras se mide la línea base
        f[4] = dis - base_dis // (EF_FIN - EF_INI + 1) if neval > EF_FIN else 0
        ef = 100
        if neval > EF_FIN and f[0] >= 100 and base_dT > 0 and base_pwm > 0:
            ef = max(-300, min(300, div_c(f[1] * base_pwm * 100, f[0] * base_dT)))
        f.append(ef)
        f.append(r["sp"] - snap_sp[0])
        f.append(f[0] - sum(pwm10[:6]) // 60)
        salida.append((i, f))
    return salida


def leer_corrida(ruta):
    filas = []
    with open(ruta, newline="") as f:
        for r in csv.DictReader(f):
            filas.append({
                "t": int(r["t"]), "sp": int(r["sp"]), "ctl": int(r["ctl"]), "med": int(r["med"]),
                "pwm": int(r["pwm"]), "T": [int(r[f"T{c}"]) for c in range(1, 6)],
                "est": r["est"], "cod": int(r["cod"]), "desc": int(r["desc"]), "clase": int(r["clase"]),
            })
    return filas


# ---------------------------------------------------------------- árbol CART
def gini(cuentas, total):
    if total <= 0:
        return 0.0
    return 1.0 - sum((c / total) ** 2 for c in cuentas)


def entrenar_arbol(X, y, w, prof_max, min_hoja):
    """Árbol CART con pesos. Nodos: {"f", "u", "si", "no"} u hoja {"c"}."""
    def hoja(idx):
        cu = [0.0] * K
        for i in idx:
            cu[y[i]] += w[i]
        return {"c": max(range(K), key=lambda c: cu[c]), "n": len(idx),
                "dist": [round(v, 2) for v in cu]}

    def construir(idx, prof):
        cu = [0.0] * K
        for i in idx:
            cu[y[i]] += w[i]
        tot = sum(cu)
        if prof >= prof_max or len(idx) < 2 * min_hoja or max(cu) >= tot * 0.999:
            return hoja(idx)
        mejor = None
        g_padre = gini(cu, tot)
        for f in range(len(X[0])):
            orden = sorted(idx, key=lambda i: X[i][f])
            izq = [0.0] * K
            ti = 0.0
            for j in range(len(orden) - 1):
                i = orden[j]
                izq[y[i]] += w[i]
                ti += w[i]
                a, b = X[i][f], X[orden[j + 1]][f]
                if a == b or j + 1 < min_hoja or len(orden) - j - 1 < min_hoja:
                    continue
                der = [cu[c] - izq[c] for c in range(K)]
                td = tot - ti
                g = (ti * gini(izq, ti) + td * gini(der, td)) / tot
                if mejor is None or g < mejor[0]:
                    mejor = (g, f, (a + b) // 2)
        if mejor is None or mejor[0] >= g_padre - 1e-9:
            return hoja(idx)
        _, f, u = mejor
        si = [i for i in idx if X[i][f] <= u]
        no = [i for i in idx if X[i][f] > u]
        return {"f": f, "u": u, "si": construir(si, prof + 1), "no": construir(no, prof + 1)}

    return construir(list(range(len(X))), 0)


def predecir(nodo, x):
    while "c" not in nodo:
        nodo = nodo["si"] if x[nodo["f"]] <= nodo["u"] else nodo["no"]
    return nodo["c"]


def podar(nodo):
    """Une ramas cuyas dos hojas dan la misma clase (no cambia las predicciones)."""
    if "c" in nodo:
        return nodo
    nodo["si"] = podar(nodo["si"])
    nodo["no"] = podar(nodo["no"])
    if "c" in nodo["si"] and "c" in nodo["no"] and nodo["si"]["c"] == nodo["no"]["c"]:
        return {"c": nodo["si"]["c"], "n": nodo["si"]["n"] + nodo["no"]["n"]}
    return nodo


def contar(nodo):
    if "c" in nodo:
        return 1, 0
    a, b = contar(nodo["si"])
    c, d = contar(nodo["no"])
    return a + c, 1 + max(b, d)


def a_c(nodo, sangria=1):
    s = "    " * sangria
    if "c" in nodo:
        return f"{s}return {nodo['c']};   /* {CLASES[nodo['c']]} */\n"
    return (f"{s}if (f[{nodo['f']}] <= {nodo['u']}) {{   /* {CARAC[nodo['f']]} */\n"
            + a_c(nodo["si"], sangria + 1) + f"{s}}}\n" + a_c(nodo["no"], sangria))


# ---------------------------------------------------------------- métricas
def matriz(y_real, y_pred):
    m = [[0] * K for _ in range(K)]
    for a, b in zip(y_real, y_pred):
        m[a][b] += 1
    return m


def prf(m):
    res = []
    for c in range(K):
        tp = m[c][c]
        fp = sum(m[r][c] for r in range(K)) - tp
        fn = sum(m[c]) - tp
        p = tp / (tp + fp) if tp + fp else 0.0
        r = tp / (tp + fn) if tp + fn else 0.0
        f1 = 2 * p * r / (p + r) if p + r else 0.0
        res.append((p, r, f1))
    return res


def avisos(preds):
    """Filtro del firmware: cambia de clase solo tras PERSIST evaluaciones iguales."""
    actual, cand, n, out = 0, 0, 0, []
    for p in preds:
        if p == cand:
            n += 1
        else:
            cand, n = p, 1
        if n >= PERSIST:
            actual = cand
        out.append(actual)
    return out


def principal():
    t0 = time.time()
    with open(os.path.join(DATOS, "corridas.csv"), newline="") as f:
        meta = {int(r["corrida"]): r for r in csv.DictReader(f)}
    corridas = []
    for ruta in sorted(glob.glob(os.path.join(DATOS, "c[0-9]*.csv"))):
        n = int(os.path.basename(ruta)[1:5])
        filas = leer_corrida(ruta)
        corridas.append({"n": n, "filas": filas, "f": caracteristicas(filas),
                         "clase": int(meta[n]["clase"]), "t_falla": int(meta[n]["t_falla"]),
                         "meta": meta[n]})
    print(f"{len(corridas)} corridas leídas ({time.time() - t0:.0f} s)")

    # División por corrida (no por ventana), estratificada por clase
    rng = random.Random(SEMILLA)
    entren, prueba = [], []
    for c in range(K):
        grupo = [r for r in corridas if r["clase"] == c]
        rng.shuffle(grupo)
        corte = int(len(grupo) * 0.7)
        entren += grupo[:corte]
        prueba += grupo[corte:]

    def muestras(lista, para_entrenar):
        X, y = [], []
        for r in lista:
            for i, f in r["f"]:
                fila = r["filas"][i]
                if fila["est"] == "F":
                    continue                      # el firmware ya apagó todo
                et = fila["clase"]
                if para_entrenar and et in EXCLUIR:
                    continue                      # clase que el modelo no aprende (ver EXCLUIR)
                if para_entrenar and et:
                    if fila["t"] - r["t_falla"] < 60:
                        continue                  # transición: la falla aún no se nota
                    if et in (1, 3) and (f[0] < 50 or f[8] < 0):
                        continue                  # sin potencia o en bajada no se ve una falla de la resistencia
                X.append(f)
                y.append(et)
        return X, y

    Xe, ye = muestras(entren, True)
    cuenta = [ye.count(c) for c in range(K)]
    pesos_c = [(len(ye) / (K * c)) ** 0.5 if c else 0 for c in cuenta]   # balance suave
    we = [pesos_c[c] for c in ye]
    print(f"Ventanas de entrenamiento: {len(Xe)}  por clase: {cuenta}")

    # Elegir profundidad con validación dentro del entrenamiento
    rng2 = random.Random(SEMILLA + 1)
    val_runs = set(r["n"] for r in rng2.sample(entren, len(entren) // 4))
    sub = [r for r in entren if r["n"] not in val_runs]
    val = [r for r in entren if r["n"] in val_runs]
    Xs, ys = muestras(sub, True)
    ws = [pesos_c[c] for c in ys]
    Xv, yv = muestras(val, True)
    eleccion = []
    for prof in (4, 5, 6, 7, 8):
        a = podar(entrenar_arbol(Xs, ys, ws, prof, 40))
        f1 = statistics.mean(x[2] for x in prf(matriz(yv, [predecir(a, x) for x in Xv])))
        hojas, _ = contar(a)
        eleccion.append((prof, f1, hojas))
        print(f"  profundidad {prof}: F1 macro de validación {f1:.3f}, {hojas} hojas")
    mejor_f1 = max(e[1] for e in eleccion)
    prof = min(e[0] for e in eleccion if e[1] >= mejor_f1 - 0.01)
    arbol = podar(entrenar_arbol(Xe, ye, we, prof, 40))
    hojas, profundidad = contar(arbol)
    print(f"Árbol final: profundidad {profundidad}, {hojas} hojas ({time.time() - t0:.0f} s)")

    # ---------------- Evaluación por ventana (corridas de prueba)
    Xp, yp = muestras(prueba, False)
    pp = [predecir(arbol, x) for x in Xp]
    m = matriz(yp, pp)
    met = prf(m)
    exact = sum(m[c][c] for c in range(K)) / len(yp)

    # ---------------- Evaluación por corrida: IA (con filtro) vs reglas del firmware
    det = {c: {"ia": [], "ia_ok": 0, "reg": [], "reg_tipo": 0, "n": 0} for c in range(1, K)}
    falsas_ia = falsas_reg = 0
    falsas_por = [0] * K
    bien_dim = falsas_bien_dim = 0      # hornos bien dimensionados: <= 55 % de PWM a la temp. máxima
    normales = 0
    for r in prueba:
        evals = [(r["filas"][i], f) for i, f in r["f"] if r["filas"][i]["est"] != "F"]
        av = avisos([predecir(arbol, f) for _, f in evals])
        tf = r["t_falla"]
        # falsas alarmas: antes de la falla (o en toda la corrida normal)
        fa = [a for (fila, _), a in zip(evals, av) if fila["t"] < tf and a != 0]
        nec = (float(r["meta"]["t_max"]) - float(r["meta"]["amb"])) / float(r["meta"]["k"])
        bien_dim += nec <= 0.55
        if fa:
            falsas_ia += 1
            falsas_por[fa[0]] += 1
            falsas_bien_dim += nec <= 0.55
        reg_antes = any((fila["cod"] or fila["desc"]) for fila in r["filas"] if fila["t"] < tf)
        falsas_reg += reg_antes
        if r["clase"] == 0:
            normales += 1
            continue
        d = det[r["clase"]]
        d["n"] += 1
        t_ia = next((fila["t"] for (fila, _), a in zip(evals, av) if fila["t"] >= tf and a == r["clase"]), None)
        if t_ia is not None:
            d["ia"].append(t_ia - tf)
            d["ia_ok"] += 1
        t_reg = next((fila for fila in r["filas"] if fila["t"] >= tf and (fila["cod"] or fila["desc"])), None)
        if t_reg is not None:
            d["reg"].append(t_reg["t"] - tf)
            tipo = {1: t_reg["cod"] == 3, 2: t_reg["cod"] in (1, 5), 3: t_reg["cod"] == 3,
                    4: bool(t_reg["desc"]) or t_reg["cod"] == 2, 5: bool(t_reg["desc"])}[r["clase"]]
            d["reg_tipo"] += tipo

    # ---------------- Exportar a C y JSON
    with open(SALIDA_H, "w", encoding="utf-8") as f:
        f.write("/*\n * ia_arbol.h - Árbol de decisión para el diagnóstico del horno.\n"
                " * GENERADO por ia/entrenar.py: no lo edites a mano.\n"
                f" * Profundidad {profundidad}, {hojas} hojas. Entradas f[0..9] (enteros, 0.25 °C):\n")
        for j, c in enumerate(CARAC):
            f.write(f" *   f[{j}] {c}: {CARAC_EN[j]}\n")
        f.write(" * Salida: " + ", ".join(f"{i} {c}" for i, c in enumerate(CLASES)) + "\n */\n")
        f.write("static uint8_t ia_arbol(const int16_t *f)\n{\n" + a_c(arbol) + "}\n")
    with open(SALIDA_JSON, "w", encoding="utf-8") as f:
        json.dump({"caracteristicas": CARAC, "clases": CLASES, "arbol": arbol}, f)

    # ---------------- Reporte
    L = []
    L.append("# Resultados de la IA de diagnóstico (generado por ia/entrenar.py)\n")
    L.append(f"- Corridas del firmware: {len(corridas)} ({len(entren)} entrenamiento, {len(prueba)} prueba, separadas por corrida)")
    L.append(f"- Ventanas de entrenamiento: {len(Xe)}; ventanas de prueba: {len(yp)}")
    L.append(f"- Modelo: árbol de decisión CART, profundidad {profundidad}, {hojas} hojas, {len(CARAC)} características enteras")
    L.append(f"- Profundidad elegida por validación: {prof} (" + ", ".join(f"{p}: F1={v:.3f}" for p, v, _ in eleccion) + ")\n")
    L.append("## Por ventana de 10 s (corridas de prueba)\n")
    L.append(f"Exactitud: **{exact * 100:.1f} %**, F1 macro: **{statistics.mean(x[2] for x in met):.3f}**\n")
    L.append("| Clase | Precisión | Recall | F1 |\n|---|---|---|---|")
    for c in range(K):
        L.append(f"| {CLASES_EN[c]} | {met[c][0]:.2f} | {met[c][1]:.2f} | {met[c][2]:.2f} |")
    L.append("\nMatriz de confusión (filas = real, columnas = predicho):\n")
    L.append("| | " + " | ".join(str(c) for c in range(K)) + " |\n|---|" + "---|" * K)
    for c in range(K):
        L.append(f"| {c} {CLASES_EN[c]} | " + " | ".join(str(v) for v in m[c]) + " |")
    L.append(f"\n## Por corrida: IA (aviso tras {PERSIST} evaluaciones iguales = {PERSIST * 10} s) vs reglas del firmware\n")
    L.append("| Falla | Corridas | IA: detectada y bien clasificada | IA: latencia mediana | Reglas: detectada | Reglas: latencia mediana | Reglas: tipo correcto |")
    L.append("|---|---|---|---|---|---|---|")
    for c in range(1, K):
        if c in EXCLUIR:
            d = det[c]
            mr = f"{statistics.median(d['reg']):.0f} s" if d["reg"] else "-"
            L.append(f"| {CLASES_EN[c]} | {d['n']} | no modelada | - | {len(d['reg'])} ({100 * len(d['reg']) / d['n']:.0f} %) | {mr} | {d['reg_tipo']} |")
            continue
        d = det[c]
        mi = f"{statistics.median(d['ia']):.0f} s" if d["ia"] else "-"
        mr = f"{statistics.median(d['reg']):.0f} s" if d["reg"] else "-"
        L.append(f"| {CLASES_EN[c]} | {d['n']} | {d['ia_ok']} ({100 * d['ia_ok'] / d['n']:.0f} %) | {mi} | "
                 f"{len(d['reg'])} ({100 * len(d['reg']) / d['n']:.0f} %) | {mr} | "
                 + ("no distingue (solo \"sensor descartado\")" if c in (4, 5) else str(d["reg_tipo"])) + " |")
    L.append(f"\nFalsas alarmas (antes de la falla o en corridas normales) en {len(prueba)} corridas de prueba: "
             f"**IA {falsas_ia}**, reglas {falsas_reg}. Corridas normales: {normales}.")
    L.append(f"\nEn hornos bien dimensionados (necesitan <= 55 % de PWM para la temperatura máxima de su perfil): "
             f"IA {falsas_bien_dim} de {bien_dim} corridas. "
             f"Todas son avisos: la IA no apaga el horno, las reglas siguen siendo la protección.")
    L.append("Falsas alarmas de la IA por clase avisada: " + ", ".join(f"{CLASES_EN[c]} {falsas_por[c]}" for c in range(1, K)))
    L.append("\nLa coincidencia exacta entre el árbol del firmware y este modelo se comprueba con ia/verificar.py.")
    with open(SALIDA_MD, "w", encoding="utf-8") as f:
        f.write("\n".join(L) + "\n")
    print("\n".join(L))
    print(f"\nTotal: {time.time() - t0:.0f} s. Escritos {SALIDA_H}, {SALIDA_MD}")


if __name__ == "__main__":
    principal()
