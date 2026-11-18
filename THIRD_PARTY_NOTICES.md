# Avisos de licencias de terceros

El codigo propio de este proyecto esta bajo licencia MIT (ver `LICENSE`, (c) 2026 Oliver Tkach).

## Importante si distribuis el plugin compilado
El plugin usa **JUCE** (https://juce.com), que se ofrece bajo **GPL v3** o bajo licencia comercial.
Si distribuis el `.vst3` o el instalador a otras personas, debes cumplir la GPL v3 para el conjunto
(publicar el codigo fuente completo bajo GPL v3), o adquirir una licencia comercial de JUCE.
Para uso personal no hay obligaciones. (MIT es compatible con GPL v3.)

## Componentes que se descargan al instalar el motor (no se incluyen en este repositorio)
Cada uno tiene su propia licencia; revisalas antes de redistribuir:
- Python (PSF License), PyTorch (BSD-3), NumPy, SciPy, SoundFile, librosa (ISC/BSD), imageio-ffmpeg / ffmpeg (LGPL/GPL segun la compilacion)
- Visual C++ Runtime / Build Tools (licencia de Microsoft, si se incluyen en el instalador)
- Inno Setup (licencia propia, gratuita)
- rvc-python y RVC (MIT), fairseq (MIT), faiss, pyworld; los modelos de voz (.pth) tienen la licencia y el consentimiento de quien los creo
