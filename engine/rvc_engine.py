#!/usr/bin/env python3
"""AI Voice Converter VST - motor de conversion de voz (RVC).

Subcomandos:
  probe        Informa que hay instalado (rvc-python, torch, CUDA, ffmpeg).
  convert      Convierte un audio de voz con un modelo RVC (.pth [+ .index]),
               usando CUDA si esta disponible y, si no, CPU.
  fetch-model  Descarga los modelos base de RVC (hubert_base, rmvpe) que
               necesita la libreria; los usa el instalador.
  setup        Crea un entorno virtual propio (Python 3.10) con torch 2.1.1
               (CUDA 11.8 si hay GPU NVIDIA), rvc-python y ffmpeg.

Protocolo de salida (una linea por mensaje, stdout, UTF-8):
  PROBE  {json}
  STATUS texto
  PROGRESS 0-100
  RESULT {json}
  ERROR  texto
  LOG    texto

La conversion usa la libreria rvc-python (MIT) de daswer123 sobre el proyecto
RVC (Retrieval-based Voice Conversion). Los modelos de voz los aporta el usuario.
"""

from __future__ import annotations

import argparse
import contextlib
import json
import os
import shutil
import subprocess
import sys
import traceback

try:  # salida en UTF-8 y sin buffer, para que el plugin lea en vivo
    sys.stdout.reconfigure(encoding="utf-8", line_buffering=True)
    sys.stderr.reconfigure(encoding="utf-8", line_buffering=True)
except Exception:
    pass

REAL_STDOUT = sys.stdout   # emit() escribe aqui aunque stdout se redirija a LogWriter

TORCH_VERSION = "2.1.1"          # fairseq/RVC cargan checkpoints con torch.load clasico
CUDA_INDEX = "cu118"             # funciona en RTX 20/30/40 con drivers recientes
# Dependencias de torch, instaladas desde PyPI: el indice de PyTorch no las tiene completas
# (con --index-url pip no encuentra, p. ej., flit_core para compilar typing-extensions).
TORCH_DEPS = ["filelock", "typing-extensions", "sympy", "networkx", "jinja2", "fsspec"]
F0_METHODS = ["rmvpe", "harvest", "crepe", "pm"]


def emit(tag: str, payload: str = "") -> None:
    print(f"{tag} {payload}".rstrip(), file=REAL_STDOUT, flush=True)


def data_root() -> str:
    base = os.environ.get("LOCALAPPDATA") or os.path.expanduser("~")
    return os.path.join(base, "AIVoiceConverter")


def add_venv_scripts_to_path() -> None:
    """ffmpeg.exe que instala 'setup' vive junto a python.exe del venv."""
    scripts = os.path.dirname(os.path.abspath(sys.executable))
    os.environ["PATH"] = scripts + os.pathsep + os.environ.get("PATH", "")


def find_ffmpeg():
    add_venv_scripts_to_path()
    return shutil.which("ffmpeg")


class LogWriter:
    """Convierte lo que imprime la libreria (stdout/stderr) en lineas LOG."""

    def __init__(self):
        self.buf = ""

    def write(self, s):
        self.buf += s
        parts = self.buf.replace("\r", "\n").split("\n")
        self.buf = parts.pop()
        for p in parts:
            if p.strip():
                emit("LOG", p.strip()[:300])
        return len(s)

    def flush(self):
        pass

    def isatty(self):
        return False


def patch_torch_load(torch) -> None:
    """torch >= 2.6 usa weights_only=True por defecto y rompe los checkpoints de fairseq/RVC."""
    orig = torch.load

    def load(*args, **kwargs):
        kwargs.setdefault("weights_only", False)
        return orig(*args, **kwargs)

    torch.load = load


# --------------------------------------------------------------------------
# probe
# --------------------------------------------------------------------------
def cmd_probe(_args) -> int:
    info = {
        "python": ".".join(map(str, sys.version_info[:3])),
        "python_ok": sys.version_info[:2] == (3, 10),
        "rvc": False,
        "torch": None,
        "cuda": False,
        "gpu": None,
        "nvidia_gpu_present": shutil.which("nvidia-smi") is not None,
        "ffmpeg": find_ffmpeg() is not None,
        "soundfile": False,
    }
    try:
        import importlib.metadata as md
        md.version("rvc-python")
        info["rvc"] = True
    except Exception:
        pass
    try:
        import torch
        info["torch"] = torch.__version__
        info["cuda"] = bool(torch.cuda.is_available())
        if info["cuda"]:
            info["gpu"] = torch.cuda.get_device_name(0)
    except Exception:
        pass
    try:
        import soundfile  # noqa: F401
        info["soundfile"] = True
    except Exception:
        pass
    emit("PROBE", json.dumps(info))
    return 0


# --------------------------------------------------------------------------
# helpers de RVC
# --------------------------------------------------------------------------
def make_rvc(device: str):
    """Crea RVCInference (descarga hubert/rmvpe la primera vez)."""
    from rvc_python.infer import RVCInference
    models_dir = os.path.join(data_root(), "rvc_models")  # vacio: cargamos por ruta
    os.makedirs(models_dir, exist_ok=True)
    return RVCInference(models_dir=models_dir, device=device)


def cmd_fetch_model(_args) -> int:
    add_venv_scripts_to_path()
    emit("STATUS", "Descargando modelos base de RVC (hubert, rmvpe)...")
    log = LogWriter()
    try:
        with contextlib.redirect_stdout(log), contextlib.redirect_stderr(log):
            import torch
            patch_torch_load(torch)
            make_rvc("cpu:0")
    except Exception as exc:
        emit("ERROR", f"No se pudieron descargar los modelos base: {exc}")
        return 16
    emit("PROGRESS", "100")
    return 0


# --------------------------------------------------------------------------
# convert
# --------------------------------------------------------------------------
def rerun_on_cpu(args) -> int:
    cmd = [sys.executable, "-u", os.path.abspath(__file__), "convert", args.input,
           "--out", args.out, "--model", args.model, "--index", args.index,
           "--pitch", str(args.pitch), "--f0", args.f0,
           "--index-rate", str(args.index_rate), "--protect", str(args.protect),
           "--filter-radius", str(args.filter_radius), "--rms-mix", str(args.rms_mix),
           "--version", args.version, "--device", "cpu", "--no-retry"]
    p = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                         encoding="utf-8", errors="replace")
    for line in p.stdout:
        print(line.rstrip("\n"), flush=True)
    return p.wait()


def cmd_convert(args) -> int:
    if args.device == "cpu":
        os.environ["CUDA_VISIBLE_DEVICES"] = "-1"   # antes de importar torch
    if not os.path.isfile(args.input):
        emit("ERROR", f"No existe el archivo: {args.input}")
        return 15
    if not os.path.isfile(args.model):
        emit("ERROR", f"No existe el modelo: {args.model}")
        return 14
    index = args.index if args.index and os.path.isfile(args.index) else ""

    if not find_ffmpeg():
        emit("ERROR", "No se encontro ffmpeg. Usa Motor > Instalar motor para que se instale.")
        return 13

    log = LogWriter()
    try:
        import torch
        import soundfile as sf
        import numpy as np
        from rvc_python.infer import RVCInference  # noqa: F401
    except Exception as exc:
        emit("ERROR", f"Faltan dependencias (rvc-python/torch/soundfile): {exc}")
        return 12
    patch_torch_load(torch)

    cuda = bool(torch.cuda.is_available()) and args.device != "cpu"
    gpu_name = torch.cuda.get_device_name(0) if cuda else None
    device = "cuda" if cuda else "cpu"
    if cuda:
        emit("STATUS", f"Convirtiendo con CUDA ({gpu_name})...")
    elif args.device == "cuda":
        emit("STATUS", "No hay CUDA disponible: usando CPU (mucho mas lento)...")
    else:
        emit("STATUS", "Convirtiendo con CPU (mas lento que con GPU NVIDIA)...")

    out_dir = os.path.join(args.out, "stems")
    shutil.rmtree(out_dir, ignore_errors=True)
    os.makedirs(out_dir, exist_ok=True)
    tmp_wav = os.path.join(args.out, "converted_raw.wav")

    try:
        emit("PROGRESS", "5")
        emit("STATUS", "Cargando modelo de voz...")
        with contextlib.redirect_stdout(log), contextlib.redirect_stderr(log):
            rvc = make_rvc("cuda:0" if cuda else "cpu:0")
            rvc.load_model(args.model, version=args.version, index_path=index)
            rvc.set_params(f0method=args.f0, f0up_key=int(args.pitch),
                           index_rate=float(args.index_rate), filter_radius=int(args.filter_radius),
                           rms_mix_rate=float(args.rms_mix), protect=float(args.protect),
                           resample_sr=0)
            emit("PROGRESS", "30")
            emit("STATUS", f"Convirtiendo voz ({'CUDA' if cuda else 'CPU'})...")
            rvc.infer_file(args.input, tmp_wav)
    except Exception as exc:
        if cuda and not args.no_retry:
            emit("STATUS", f"CUDA fallo ({str(exc)[:80]}). Reintentando en CPU...")
            return rerun_on_cpu(args)
        emit("ERROR", f"Fallo la conversion: {exc}")
        traceback.print_exc(file=sys.stdout)
        return 18

    if not os.path.isfile(tmp_wav):
        emit("ERROR", "El motor no genero audio. Revisa que el modelo sea RVC v1/v2 valido.")
        return 19

    emit("STATUS", "Guardando resultado...")
    data, sr = sf.read(tmp_wav, always_2d=True, dtype="float32")
    peak = float(np.abs(data).max()) if data.size else 0.0
    if peak > 1.0:
        data = data / (peak * 1.01)
    sf.write(os.path.join(out_dir, "converted.wav"), data, int(sr), subtype="PCM_24")
    with contextlib.suppress(Exception):
        os.remove(tmp_wav)

    result = {
        "stems_dir": out_dir,
        "stems": ["converted"],
        "samplerate": int(sr),
        "device": device,
        "gpu": gpu_name,
        "bpm": 0,
        "key": "",
        "hz": 0,
        "model": os.path.splitext(os.path.basename(args.model))[0],
    }
    with open(os.path.join(args.out, "result.json"), "w", encoding="utf-8") as f:
        json.dump(result, f)
    emit("PROGRESS", "100")
    emit("RESULT", json.dumps(result))
    return 0


# --------------------------------------------------------------------------
# setup: entorno virtual propio (Python 3.10) con torch 2.1.1 + rvc-python + ffmpeg
# --------------------------------------------------------------------------
def venv_python(venv: str) -> str:
    if os.name == "nt":
        return os.path.join(venv, "Scripts", "python.exe")
    return os.path.join(venv, "bin", "python")


def run_logged(cmd) -> int:
    emit("LOG", " ".join(cmd))
    p = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                         text=True, encoding="utf-8", errors="replace")
    for line in p.stdout:
        line = line.strip()
        if line:
            emit("LOG", line[:300])
    return p.wait()


def torch_has_cuda(py: str) -> bool:
    return subprocess.run([py, "-c", "import torch,sys; sys.exit(0 if torch.cuda.is_available() else 1)"],
                          capture_output=True).returncode == 0


def install_torch(py: str, suffix: str, index: str) -> int:
    """torch/torchaudio sin dependencias desde el indice de PyTorch + sus dependencias desde PyPI."""
    rc = run_logged([py, "-m", "pip", "install", "--no-deps", "--force-reinstall",
                     f"torch=={TORCH_VERSION}{suffix}", f"torchaudio=={TORCH_VERSION}{suffix}",
                     "--index-url", index])
    if rc != 0:
        return rc
    return run_logged([py, "-m", "pip", "install", *TORCH_DEPS])


def install_ffmpeg(py: str, venv: str) -> bool:
    """Si no hay ffmpeg en el sistema, usa el binario de imageio-ffmpeg y lo deja como ffmpeg.exe en el venv."""
    if shutil.which("ffmpeg"):
        return True
    if run_logged([py, "-m", "pip", "install", "imageio-ffmpeg"]) != 0:
        return False
    out = subprocess.run([py, "-c", "import imageio_ffmpeg; print(imageio_ffmpeg.get_ffmpeg_exe())"],
                         capture_output=True, text=True)
    exe = out.stdout.strip()
    if out.returncode != 0 or not os.path.isfile(exe):
        return False
    dest = os.path.join(os.path.dirname(py), "ffmpeg.exe" if os.name == "nt" else "ffmpeg")
    shutil.copyfile(exe, dest)
    return True


def cmd_setup(args) -> int:
    if sys.version_info[:2] != (3, 10):
        emit("ERROR", "Se necesita Python 3.10 (rvc-python/fairseq no funcionan en otras versiones). "
                      "Instala Python 3.10.11 desde python.org.")
        return 11
    venv = args.venv
    py = venv_python(venv)

    emit("STATUS", "Creando entorno virtual...")
    emit("PROGRESS", "3")
    if not os.path.isfile(py):
        if run_logged([sys.executable, "-m", "venv", venv]) != 0:
            emit("ERROR", "No se pudo crear el entorno virtual.")
            return 20

    emit("STATUS", "Preparando pip...")
    # omegaconf 2.0.6 (dependencia de fairseq) tiene metadatos que pip >= 24.1 rechaza
    if run_logged([py, "-m", "pip", "install", "pip<24.1", "setuptools<70", "wheel"]) != 0:
        emit("ERROR", "No se pudo preparar pip.")
        return 21
    emit("PROGRESS", "8")

    have_nvidia = shutil.which("nvidia-smi") is not None and not args.cpu_only
    gpu_ok = False
    if have_nvidia:
        emit("STATUS", f"Instalando PyTorch {TORCH_VERSION} con CUDA ({CUDA_INDEX})... (~2.5 GB)")
        rc = install_torch(py, f"+{CUDA_INDEX}", f"https://download.pytorch.org/whl/{CUDA_INDEX}")
        gpu_ok = rc == 0 and torch_has_cuda(py)
        if not gpu_ok:
            emit("LOG", "PyTorch CUDA no quedo operativo; se instalara la version CPU")
    if not gpu_ok:
        emit("STATUS", f"Instalando PyTorch {TORCH_VERSION} (CPU)...")
        if install_torch(py, "+cpu", "https://download.pytorch.org/whl/cpu") != 0:
            emit("ERROR", "No se pudo instalar PyTorch.")
            return 22
    emit("PROGRESS", "55")

    emit("STATUS", "Instalando rvc-python (fairseq, faiss, pyworld...)")
    if run_logged([py, "-m", "pip", "install", "rvc-python", "soundfile"]) != 0:
        emit("ERROR", "No se pudo instalar rvc-python. Si el registro menciona 'Microsoft Visual C++ 14.0', "
                      "instala 'Build Tools for Visual Studio' (carga de trabajo C++) y reintenta.")
        return 23
    emit("PROGRESS", "85")

    emit("STATUS", "Instalando ffmpeg...")
    if not install_ffmpeg(py, venv):
        emit("STATUS", "Aviso: no se pudo instalar ffmpeg. Instalalo con 'winget install ffmpeg'.")
    if gpu_ok and not torch_has_cuda(py):
        emit("STATUS", "Aviso: PyTorch perdio CUDA al instalar rvc-python; se usara la CPU.")
    emit("PROGRESS", "100")
    emit("RESULT", json.dumps({"python": py}))
    return 0


# --------------------------------------------------------------------------
def main() -> int:
    # redes lentas / cortes: pip espera mas y reintenta mas veces (evita ReadTimeoutError)
    os.environ.setdefault("PIP_DEFAULT_TIMEOUT", "180")
    os.environ.setdefault("PIP_RETRIES", "10")
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)

    sub.add_parser("probe")

    sp = sub.add_parser("convert")
    sp.add_argument("input")
    sp.add_argument("--out", required=True, help="carpeta de salida (cache)")
    sp.add_argument("--model", required=True, help="ruta del .pth")
    sp.add_argument("--index", default="", help="ruta del .index (opcional)")
    sp.add_argument("--pitch", type=int, default=0, help="semitonos")
    sp.add_argument("--f0", choices=F0_METHODS, default="rmvpe")
    sp.add_argument("--index-rate", type=float, default=0.5)
    sp.add_argument("--protect", type=float, default=0.33)
    sp.add_argument("--filter-radius", type=int, default=3)
    sp.add_argument("--rms-mix", type=float, default=1.0)
    sp.add_argument("--version", choices=["v1", "v2"], default="v2")
    sp.add_argument("--device", choices=["auto", "cuda", "cpu"], default="auto")
    sp.add_argument("--no-retry", action="store_true", help=argparse.SUPPRESS)

    sub.add_parser("fetch-model")

    st = sub.add_parser("setup")
    st.add_argument("--venv", required=True)
    st.add_argument("--cpu-only", action="store_true")

    args = ap.parse_args()
    try:
        return {"probe": cmd_probe, "convert": cmd_convert, "setup": cmd_setup,
                "fetch-model": cmd_fetch_model}[args.cmd](args)
    except SystemExit:
        raise
    except Exception as exc:
        emit("ERROR", f"Error inesperado: {exc}")
        traceback.print_exc(file=sys.stdout)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
