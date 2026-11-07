#!/usr/bin/env python3
"""
verificar.py - Comprueba que el árbol que corre en el firmware (ia_arbol.h dentro
del PIC, simulado en el banco) da EXACTAMENTE la misma predicción que el modelo en
Python, en cada evaluación de cada corrida del dataset.

Requiere un dataset generado con el firmware que ya incluye la IA (columna iap).
Uso:  python ia/verificar.py
"""
import glob
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import entrenar as E  # noqa: E402

arbol = json.load(open(E.SALIDA_JSON, encoding="utf-8"))["arbol"]
corridas = evals = difs = 0
for ruta in sorted(glob.glob(os.path.join(E.DATOS, "c[0-9]*.csv"))):
    import csv
    with open(ruta, newline="") as f:
        iap = [int(r["iap"]) for r in csv.DictReader(f)]
    filas = E.leer_corrida(ruta)
    py = {i: E.predecir(arbol, f) for i, f in E.caracteristicas(filas) if filas[i]["est"] != "F"}
    fw = {i: p for i, p in enumerate(iap) if p != 255}
    corridas += 1
    evals += len(py)
    if py != fw:
        difs += 1
        malas = [i for i in set(py) | set(fw) if py.get(i) != fw.get(i)][:3]
        print(f"{os.path.basename(ruta)}: diferencias en filas {malas} "
              f"(python {[py.get(i) for i in malas]}, firmware {[fw.get(i) for i in malas]})")
print(f"{corridas} corridas, {evals} evaluaciones: "
      + ("firmware y Python coinciden en todas." if difs == 0 else f"{difs} corridas con diferencias."))
sys.exit(1 if difs else 0)
