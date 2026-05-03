# CAB-TR v1.4

CAB-TR es un cargador/convolucionador de respuestas a impulso con tres slots, pensado para simulacion de pantallas, mezcla paralela de IRs, rutas serie/hibridas y trabajo Mid/Side.

## Concepto

Cada slot A/B/C puede cargar una IR y procesarla con su propia cadena: ganancia de entrada/salida, trim START/END, SIZE, filtros HP/LP, TILT, RESO, DIST, DELAY, PAN, FRED/Angle, MIX, EXP, CHAOS D/F, inversion, normalizacion y routing Mid/Side.

El sistema de rutas permite:
- `A->B->C`
- `A|B|C`
- `A->B|C`
- `A|B->C`
- `(A|B)->C`
- `A->(B|C)`

El sistema `SUM BUS` permite enviar cada loader a Stereo, Mid o Side en los puntos de suma paralela.

## Interfaz

- **Barras**: arrastre horizontal. Click derecho para entrada numerica.
- **ENABLE A/B/C**: activa o desactiva cada loader.
- **Browse `...`**: abre el explorador integrado de archivos.
- **Seccion I/O desplegable**: muestra controles avanzados por loader.
- **Filtro**: abre el prompt HP/LP con frecuencia, pendiente y on/off.
- **EXP**: click izquierdo activa/desactiva; click derecho abre el prompt.
- **CHSD / CHSF**: chaos de delay/ganancia y chaos de filtros.
- **ALIGN**: alinea tiempo/fase entre loaders activos.
- **Export**: renderiza la cadena estatica combinada a una IR.

## Controles globales

### INPUT / OUTPUT (-INF a +24 dB)

Ganancia global de entrada y salida. El suelo interno es -144 dB y se muestra como `-INF`; 0 dB queda centrado en el control.

### MIX

Mezcla global dry/wet. En modo `INSERT` actua como crossfade. En modo `SEND`, `DRY LEVEL` y `WET LEVEL` son independientes.

### ROUTE

Selecciona la topologia de los tres loaders: serie, paralelo o hibrida.

### ALIGN

Analiza las IRs activas, calcula compensacion temporal y aplica delay/polaridad donde proceda para reducir comb filtering.

### MATCH

Tilt EQ global adaptativo basado en el analisis espectral de las IRs cargadas.

### LIMITER

Limitador dual con modos `NONE`, `WET` y `GLOBAL`.

## Controles por loader

### IN / OUT (-INF a +24 dB)

Ganancia de entrada y salida por loader. Mismo rango y curva que INPUT/OUTPUT global.

### HP / LP

Filtros high-pass y low-pass con pendientes de 6, 12 o 24 dB/oct.

### START / END

Recorte temporal de la IR antes de convolucion.

### SIZE

Reescalado de la IR entre 0.25x y 4.0x.

### DELAY

Delay post-loader de 0 a 1000 ms con precision de 0.001 ms. Tambien lo usa `ALIGN`.

### FRED / Angle

Simulacion de microfono off-axis tipo Fredman mediante retardo corto y mezcla.

### MIX

Mezcla dry/wet independiente por loader.

### EXP

Expander/gate por loader con orden `PRE` o `POST`, threshold, ratio, knee, attack y release.

### CHAOS

- **CHSD**: modulacion de micro-delay y ganancia.
- **CHSF**: modulacion de filtros HP/LP.

## Export

Exporta una IR combinada con las partes estaticas de la cadena: routing, MODE IN/OUT, SUM BUS, ganancias, filtros, TILT, DIST, PAN, DELAY, FRED, MIX, MATCH, DC block, output e inversion.

Quedan fuera por naturaleza dinamica:
- `CHAOS D`
- `CHAOS F`
- `EXP`
- `LIMITER`

## Detalles tecnicos

- Convolucion por FFTConvolver con cola en background thread.
- Sin latencia anadida por la convolucion principal.
- Crossfade de IR de 50 ms para evitar clicks al reconstruir.
- Recargas de IR rate-limited para evitar picos de CPU.
- Filtros HP/LP con coeficientes actualizados cada 32 samples.
- Delay fraccional suavizado para ALIGN y offset manual.
- Mix global y mix por loader optimizados para evitar copias dry innecesarias cuando el camino esta 100% wet y estable.
- El delay estable evita recalcular `setDelay()` por sample, pero mantiene la linea alimentada para reentrada sin clicks.
- El audio thread evita asignaciones dinamicas en la ruta normal.

## Build

- JUCE, C++17, VST3.
- Visual Studio 2022, Windows x64.
- FFTW opcional; fallback a JUCE FFT cuando no esta disponible.

## Changelog v1.4

- Tres loaders A/B/C con routing serie, paralelo e hibrido.
- Sistema `SUM BUS` para Stereo/Mid/Side.
- Export de IR combinada.
- Limitador dual `WET/GLOBAL`.
- Expander por loader con orden `PRE/POST`.
- Ganancias INPUT/OUTPUT e IN/OUT unificadas en -INF a +24 dB.
- Optimizaciones de mix dry/wet y delay estable sin cambiar la funcionalidad sonora esperada.
