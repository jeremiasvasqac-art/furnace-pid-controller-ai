# PIC16F1847 simulation evidence

This folder is the evidence behind the "final chip" and "62 percent" claims in the AI-SED pitch. It was produced on November 22, 2026, from repository commit `0133109`. The firmware in the repository root was not changed.

## Tools

- MPLAB XC8 v4.00, optimization `-Os`, device pack PIC12-16F1xxx_DFP 1.9.258
- MPLAB X v6.35 instruction-set simulator (`mdb`), PIC16F1847 at 4 MHz
- Visual Studio C++ for the PC test bench

## Memory and timing

| Item | Result |
|---|---|
| Program memory, full AI build | 6,621 of 8,192 words (80.8%) |
| Data memory, full AI build | 375 of 1,024 bytes (36.6%) |
| AI evaluation time | 2.3 to 5.3 ms in 24 sampled evaluations, once every 10 s |

Memory comes from the XC8 summary of the production build. Timing comes from the simulator stopwatch between source lines 1005 and 1610 of `horno_pwm_16f18x7.c`, on the clean simulation build. The stopwatch was calibrated against the firmware's 10 ms delay, which measured exactly 10,000 instruction cycles (1 µs each).

## Fault tests on the unmodified firmware

Each fault was injected at second 600 with the firmware's own `SIM_FALLA_*` options and built-in furnace model (`SIMULAR_SENSORES=1`). Telemetry prints every 10 s, so each time is accurate to within 10 s.

| Fault | AI warning | Fixed-rule fault | Log |
|---|---|---|---|
| Frozen sensor (TC3) | `IA;4` by 710 s | None | `uart_frozen_sensor.txt` |
| Open heater | `IA;1` by 740 s | `F;3` by 880 s | `uart_open_heater.txt` |
| Shorted switch | `IA;2` by 690 s | `F;5` by 720 s | `uart_shorted_switch.txt` |

## False-warning check

| Clean run | Sensor noise | AI warnings | Log |
|---|---|---|---|
| Unmodified firmware | None | **False frozen-sensor warnings near 2,910 s and 3,390 s** | `uart_clean_no_noise.txt` |
| Test copy, 3 seeds | ±1 LSB, probability 0.25 | None | `uart_clean_noise_1.txt` to `_3.txt` |

The unmodified simulation model produces readings that never change during a long soak, which the tree associates with a frozen sensor. A very stable real sensor could trigger the same advisory warning. It would not stop the oven.

`horno_pwm_noise_test.c` is the test copy. Its only change adds sensor noise to the simulation stub, using the PC test bench's default. The three faults were also named with noise (`uart_noise_*.txt`).

## Why the AI flagged 62% of shorted switches

`shorted_switch_analysis.py` reproduces the 45 held-out shorted-switch runs with the repository's generator, split, features, and saved tree. Its output is in `shorted_switch_analysis.txt`.

- The reproduction matches `ia/resultados.md`: AI 28 of 45 (62%), median 98 s; rules 45 of 45, median 130 s.
- In **all 17 runs the AI did not flag, the fixed rules latched first** (median 72 s after the fault). The firmware stops AI evaluation after a latch.
- In 15 of those 17 runs, the AI was already partway through the six matching checks it needs before warning.
- In a test build with the heat-without-PWM rule disabled (`CALOR_SIN_PWM_C=0`), the AI flagged 40 of 45 (89%). This is a diagnostic test only. The rule should stay enabled.

## How to reproduce a fault run

Work in an empty folder outside the repository, such as `C:\build`, and point `$repo` at your repository copy:

```powershell
$repo = "C:\path\to\furnace-pid-controller-ai"
```

Set `SIM_FALLA_MODO` to 1 (frozen sensor), 3 (open heater), or 4 (shorted switch):

```powershell
& "C:\Program Files\Microchip\xc8\v4.00\bin\xc8-cc.exe" -mcpu=16F1847 "-mdfp=C:\Program Files\Microchip\MPLABX\v6.35\packs\Microchip\PIC12-16F1xxx_DFP\1.9.258\xc8" -Os -gdwarf-3 -DSIMULAR_SENSORES=1 -DSIM_FALLA_SEG=600 -DSIM_FALLA_MODO=1 -o fault.elf "$repo\horno_pwm_16f18x7.c"
```

Save this as `run.mdb`, replacing `C:/build` with your folder and keeping the forward slashes:

```text
Device PIC16F1847
Hwtool SIM
Set oscillator.frequency 4
Set oscillator.frequencyunit Mega
Set uart1io.uartioenabled true
Set uart1io.output file
Set uart1io.outputfile C:/build/uart.txt
Program C:/build/fault.elf
Run
Wait 55000
Halt
Quit
```

```powershell
& "C:\Program Files\Microchip\MPLABX\v6.35\mplab_platform\bin\mdb.bat" "C:\build\run.mdb"
```

For the noise runs, compile `$repo\evidence\pic16f1847-simulation\horno_pwm_noise_test.c` instead. Clean runs omit the `SIM_FALLA_*` options and use `-DSIM_SEED=0xACE1`, `0x1234`, or `0xBEEF`.

## How to reproduce the shorted-switch analysis

Open a Visual Studio x64 Developer PowerShell at the root of a repository copy, then build both test benches:

```powershell
New-Item -ItemType Directory -Force banco\salida
```

```powershell
cl /nologo /utf-8 /W1 /wd4068 /wd4996 /D_CRT_SECURE_NO_WARNINGS /DBANCO_PC /D_16F1847 /Ibanco banco\banco.c /Febanco\salida\banco_real.exe /Fobanco\salida\
```

```powershell
cl /nologo /utf-8 /W1 /wd4068 /wd4996 /D_CRT_SECURE_NO_WARNINGS /DBANCO_PC /D_16F1847 /DCALOR_SIN_PWM_C=0 /Ibanco banco\banco.c /Febanco\salida\banco_no5.exe /Fobanco\salida\
```

Then run the analysis:

```powershell
python -I evidence\pic16f1847-simulation\shorted_switch_analysis.py . "$env:TEMP\shorted-check"
```

## Limits

- This is simulation only. Nothing here ran on a physical PIC, board, heater, or furnace.
- Each final-chip case is one run on one built-in furnace model. The 270-run statistics come from the PC test bench.
- A drifting sensor was not run on the final-chip model, because the firmware's simulation model has no drift fault.
- `AVISO;VDD;0` in the logs is a simulator limitation: it does not emulate the ADC channel used to measure VDD.
