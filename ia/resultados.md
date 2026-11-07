# Resultados de la IA de diagnóstico (generado por ia/entrenar.py)

- Corridas del firmware: 900 (630 entrenamiento, 270 prueba, separadas por corrida)
- Ventanas de entrenamiento: 292260; ventanas de prueba: 142337
- Modelo: árbol de decisión CART, profundidad 7, 18 hojas, 10 características enteras
- Profundidad elegida por validación: 7 (4: F1=0.684, 5: F1=0.760, 6: F1=0.760, 7: F1=0.784, 8: F1=0.782)

## Por ventana de 10 s (corridas de prueba)

Exactitud: **94.5 %**, F1 macro: **0.724**

| Clase | Precisión | Recall | F1 |
|---|---|---|---|
| Normal | 0.94 | 1.00 | 0.97 |
| Open heater | 0.75 | 0.57 | 0.65 |
| Shorted MOSFET/SSR | 0.93 | 0.67 | 0.78 |
| Degraded heater | 0.00 | 0.00 | 0.00 |
| Stuck thermocouple | 0.99 | 0.98 | 0.98 |
| Drifting thermocouple | 0.99 | 0.93 | 0.96 |

Matriz de confusión (filas = real, columnas = predicho):

| | 0 | 1 | 2 | 3 | 4 | 5 |
|---|---|---|---|---|---|---|
| 0 Normal | 111104 | 1 | 34 | 0 | 62 | 48 |
| 1 Open heater | 679 | 920 | 0 | 0 | 0 | 1 |
| 2 Shorted MOSFET/SSR | 209 | 0 | 431 | 0 | 1 | 0 |
| 3 Degraded heater | 5584 | 302 | 0 | 0 | 0 | 0 |
| 4 Stuck thermocouple | 329 | 0 | 0 | 0 | 15109 | 38 |
| 5 Drifting thermocouple | 442 | 0 | 0 | 0 | 46 | 6997 |

## Por corrida: IA (aviso tras 6 evaluaciones iguales = 60 s) vs reglas del firmware

| Falla | Corridas | IA: detectada y bien clasificada | IA: latencia mediana | Reglas: detectada | Reglas: latencia mediana | Reglas: tipo correcto |
|---|---|---|---|---|---|---|
| Open heater | 45 | 43 (96 %) | 137 s | 43 (96 %) | 297 s | 43 |
| Shorted MOSFET/SSR | 45 | 28 (62 %) | 98 s | 45 (100 %) | 130 s | 45 |
| Degraded heater | 45 | no modelada | - | 30 (67 %) | 301 s | 30 |
| Stuck thermocouple | 45 | 45 (100 %) | 141 s | 43 (96 %) | 789 s | no distingue (solo "sensor descartado") |
| Drifting thermocouple | 45 | 45 (100 %) | 129 s | 45 (100 %) | 372 s | no distingue (solo "sensor descartado") |

Falsas alarmas (antes de la falla o en corridas normales) en 270 corridas de prueba: **IA 1**, reglas 0. Corridas normales: 45.

En hornos bien dimensionados (necesitan <= 55 % de PWM para la temperatura máxima de su perfil): IA 1 de 186 corridas. Todas son avisos: la IA no apaga el horno, las reglas siguen siendo la protección.
Falsas alarmas de la IA por clase avisada: Open heater 0, Shorted MOSFET/SSR 0, Degraded heater 0, Stuck thermocouple 1, Drifting thermocouple 0

La coincidencia exacta entre el árbol del firmware y este modelo se comprueba con ia/verificar.py.
