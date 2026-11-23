"""Reproduces the 45 held-out shorted-switch runs and explains the AI's missed runs.

Uses the repository's own data generator, feature code, split and saved model.
Usage: python -I shorted_switch_analysis.py <repo copy> <output folder>
"""
import json
import os
import random
import statistics
import subprocess
import sys

RUN, OUT = sys.argv[1], sys.argv[2]
IA = os.path.join(RUN, "ia")
sys.path.append(IA)
import generar_datos as gd  # noqa: E402
import entrenar as tr  # noqa: E402

EXE_REAL = os.path.join(RUN, "banco", "salida", "banco_real.exe")
EXE_NO5 = os.path.join(RUN, "banco", "salida", "banco_no5.exe")
os.makedirs(OUT, exist_ok=True)

# Same stratified split as entrenar.py
split = random.Random(tr.SEMILLA)
test = {}
for c in range(6):
    group = [i for i in range(900) if i % 6 == c]
    split.shuffle(group)
    test[c] = set(group[int(len(group) * 0.7):])

with open(os.path.join(IA, "modelo.json"), encoding="utf-8") as fh:
    tree = json.load(fh)["arbol"]

# Same random sequence as generar_datos.py 900 2026
rng = random.Random(2026)
cases = []
for i in range(900):
    clase = i % 6
    p, tramos, dur = gd.parametros(rng, clase)
    txt = ";".join(f"{a},{b},{m}" for a, b, m in tramos)
    ruido = rng.uniform(0.03, 0.40)
    zonas = rng.uniform(0.5, 4.0)
    if i in test[2]:
        cases.append((i, p, txt, dur, ruido, zonas))


def simulate(exe, tag, case):
    i, p, txt, dur, ruido, zonas = case
    path = os.path.join(OUT, f"{tag}{i:04d}.csv")
    args = [exe, "dataset", "-p"] + [f"{v:.4f}" if isinstance(v, float) else str(v) for v in p]
    args += ["-perfil", txt, "-t_fin", str(dur + 200), "-ruido", f"{ruido:.3f}", "-zonas", f"{zonas:.3f}", "-ds", path]
    subprocess.run(args, stdout=subprocess.DEVNULL, check=False)
    return path


def score(path, tf):
    rows = tr.leer_corrida(path)
    features = tr.caracteristicas(rows)
    evals = [(rows[i], f) for i, f in features if rows[i]["est"] != "F"]
    preds = [tr.predecir(tree, f) for _, f in evals]
    warned = tr.avisos(preds)
    t_ai = next((r["t"] for (r, _), a in zip(evals, warned) if r["t"] >= tf and a == 2), None)
    t_rule = next((r for r in rows if r["t"] >= tf and (r["cod"] or r["desc"])), None)
    latch = next((r for r in rows if r["t"] >= tf and r["est"] == "F"), None)
    after = [pr for (r, _), pr in zip(evals, preds) if r["t"] >= tf]
    streak = 0
    for pr in reversed(after):
        if pr != 2:
            break
        streak += 1
    return {
        "ai": None if t_ai is None else t_ai - tf,
        "rule": None if t_rule is None else t_rule["t"] - tf,
        "latch": None if latch is None else latch["t"] - tf,
        "latch_cod": None if latch is None else latch["cod"],
        "checks": len(after),
        "named": sum(1 for pr in after if pr == 2),
        "streak": streak,
    }


real = {}
no5 = {}
for case in cases:
    tf = case[1][5]
    real[case[0]] = score(simulate(EXE_REAL, "r", case), tf)
    no5[case[0]] = score(simulate(EXE_NO5, "n", case), tf)

ai = [s["ai"] for s in real.values() if s["ai"] is not None]
rule = [s["rule"] for s in real.values() if s["rule"] is not None]
print(f"REPRODUCTION: {len(real)} shorted test runs")
print(f"  AI detected {len(ai)} ({100 * len(ai) / len(real):.0f}%), median {statistics.median(ai):.0f} s")
print(f"  Rules detected {len(rule)} ({100 * len(rule) / len(real):.0f}%), median {statistics.median(rule):.0f} s")

missed = {i: s for i, s in real.items() if s["ai"] is None}
print(f"\nMISSED BY THE AI: {len(missed)} runs")
print("  run  rule_s latch_cod checks_before_latch checks_named_short final_streak | AI_with_rule5_off_s")
for i, s in sorted(missed.items()):
    print(f"  {i:4d} {str(s['rule']):>6} {str(s['latch_cod']):>9} {s['checks']:>19} {s['named']:>18} {s['streak']:>12} | {no5[i]['ai']}")

latched = [s for s in missed.values() if s["latch"] is not None]
under6 = [s for s in latched if s["checks"] < tr.PERSIST]
partial = [s for s in latched if s["streak"] > 0]
print(f"\n  latched by the rules before any AI warning: {len(latched)} of {len(missed)}")
print(f"  had fewer than {tr.PERSIST} AI checks before the latch: {len(under6)} of {len(missed)}")
print(f"  were on a live 'shorted' streak when the rules latched: {len(partial)} of {len(missed)}")
print(f"  rule latency in missed runs: median {statistics.median([s['latch'] for s in latched]):.0f} s" if latched else "")

ai_no5 = [s["ai"] for s in no5.values() if s["ai"] is not None]
print(f"\nCOUNTERFACTUAL (rule 5 disabled, other rules active): AI detected {len(ai_no5)} of {len(no5)} ({100 * len(ai_no5) / len(no5):.0f}%)")
if ai_no5:
    print(f"  median {statistics.median(ai_no5):.0f} s")
print(f"  of the {len(missed)} missed runs, AI now detects {sum(1 for i in missed if no5[i]['ai'] is not None)}")
