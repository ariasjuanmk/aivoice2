# AI Voice Converter (RVC) (prototipo 0.2, solo Windows)

Plugin VST3 de **efecto** que convierte una pista de voz con tus modelos **RVC** (`.pth` + `.index`)
y reproduce el resultado sincronizado con el DAW (REAPER, Cubase, Ableton...).
Usa **CUDA** (pensado para GPU NVIDIA, p. ej. RTX 3060) y, si no hay, **CPU** (mucho más lento).

> **Estado:** el código está escrito pero **todavía no fue compilado ni probado en un DAW**. Solo se probó
> el motor Python con módulos simulados. Es de esperar algún error de compilación en el primer build.
> La instalación de RVC en Windows (fairseq, pyworld, faiss) es la parte más frágil: mirá `install.log` si falla.

## Modo ARA (0.2): usa los clips de la pista directamente
Si tu DAW soporta **ARA2** (REAPER, Cubase 12+, Studio One, Cakewalk...), el plugin se enlaza por ARA y **lee el audio de los clips
de la pista donde está puesto**, sin grabar. Al enlazarse, la ventana cambia sola a "modo ARA":
1. Insertá el plugin en la pista de voz (o aplicalo como extensión ARA del clip, según tu DAW) y abrí su ventana.
2. Esperá unos segundos: el plugin lee el audio de cada clip ("N leídos").
3. Elegí el modelo y tocá **Convertir clips de la pista**. Cada clip se convierte una vez; clips que comparten el mismo audio se convierten juntos.
4. Dale play: el plugin reproduce **en lugar del clip original** la voz convertida, respetando cortes, recortes y posición de cada clip.
   Los clips sin convertir suenan normales. **Voz convertida / Voz original / Ajuste de posición** funcionan igual que en el modo normal.
Si el DAW no soporta ARA (Ableton, FL Studio...), el plugin funciona en el modo normal de abajo (grabar desde la pista o cargar un archivo).

Limitaciones de esta versión: no guarda la conversión dentro del proyecto del DAW (al reabrirlo tocá "Convertir" otra vez: usa la caché y es
instantáneo); el audio de cada clip se carga entero en memoria (clips muy largos usan mucha RAM); no soporta clips con time-stretch; sin fundidos
en los bordes de cada clip. **No se pudo probar contra ningún DAW**: cómo ofrece cada host las extensiones ARA puede variar.

## Modo normal (sin ARA)
1. Poné el plugin como **efecto** (insert) en la pista con la voz.
2. **Modelo de voz:** "Agregar modelo..." y elegí el `.pth` (y su `.index` si lo tenés; mejora el parecido).
   Se copian a `%LOCALAPPDATA%\AIVoiceConverter\models\<nombre>\`. También podés copiarlos a mano (botón "Carpeta").
3. Dos formas de darle el audio:
   - **Grabar desde la pista:** "Grabar desde la pista", dale play en el DAW sobre la parte de voz y tocá
     "Detener y convertir". Queda alineada con el proyecto automáticamente (hasta 10 minutos).
   - **Archivo:** soltá, elegí o pegá la ruta de un WAV/MP3/FLAC y tocá "Convertir" (usá "Ajuste de posición" para alinearla).
4. Mientras no haya voz convertida, la pista pasa sin cambios. Con resultado: **Voz convertida** y **Voz original**
   son dos controles de nivel (por defecto solo se oye la convertida). Se puede arrastrar el WAV convertido a la línea de tiempo.
5. Si repetís el mismo audio con el mismo modelo y parámetros, usa la caché y no reprocesa.

## Parámetros
- **Tono (semitonos):** +12 = una octava arriba. Voz masculina → femenina suele ir +12; al revés, -12.
- **Parecido (index rate):** cuánto se usa el `.index`. Más alto = más parecido, pero puede sonar más artificial.
- **Proteger consonantes:** 0.5 = máxima protección de consonantes y respiraciones.
- **Filtro de pitch:** suaviza el tono (3 o más reduce la voz entrecortada).
- **Volumen del modelo:** 1 = volumen del modelo, 0 = copia el de la voz original.
- **Detector de tono:** rmvpe (recomendado), harvest, crepe, pm.

## Motor e instalación
- RVC en Windows necesita **Python 3.10 exacto** (fairseq/numpy). El instalador descarga Python 3.10.11 (solo para tu usuario) si no lo encuentra.
- Entorno propio en `%LOCALAPPDATA%\AIVoiceConverter\venv` con **PyTorch 2.1.1 + CUDA 11.8** (funciona en RTX 20/30/40) o CPU,
  `rvc-python` y ffmpeg. Se fija `pip<24.1` porque una dependencia de fairseq no es compatible con pip más nuevo.
- Los modelos base de RVC (hubert, rmvpe) se descargan de Hugging Face (`Daswer123/RVC_Base`) durante la instalación.
- Si CUDA falla en ejecución, reintenta en CPU.
- Si `rvc-python` no compila (error de "Microsoft Visual C++ 14.0"), instalá *Build Tools for Visual Studio* con la carga de trabajo C++ y reintentá desde **Motor > Instalar motor**.
- La salida de RVC (40/48 kHz) se remuestrea con interpolación lineal al sample rate del proyecto (suficiente para prototipo).

## Compilar
**GitHub Actions** (el build descarga además el SDK de ARA 2.2.0 de Celemony, Apache 2.0): subí esta carpeta a un repo, creá `.github/workflows/build-windows.yml` (viene en el zip) y corré el workflow.
Artefactos: `AIVoiceConverter-Setup` (instalador) y `AI-Voice-Converter-VST3-Windows` (solo el `.vst3`).

**Local:** Visual Studio 2022 (C++), CMake 3.22+, Inno Setup 6.
```
cmake -B build -A x64
cmake --build build --config Release --target AIVoiceConverter_VST3
```

## Uso responsable y licencias
- Usá **solo modelos de voz propios o con permiso explícito** de la persona cuya voz representan. Clonar la voz de alguien
  sin su consentimiento puede ser ilegal y dañino; no presentes audio convertido como si lo hubiera dicho otra persona.
- **JUCE** se usa bajo GPLv3 (o licencia comercial): si distribuís el plugin, debe cumplir la GPL.
- Motor: `rvc-python` (MIT, daswer123) sobre RVC (Retrieval-based-Voice-Conversion-WebUI, MIT). Revisá también la licencia de cada modelo de voz.
