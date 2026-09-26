# -*- coding: utf-8 -*-
"""RealForge DLSS 5 - serveur local (Python 3.9+, bibliotheque standard uniquement).

Interface : http://127.0.0.1:8765
Pilote engine/realforge_engine.exe : chaque image (ou frame video) passe dans NVIDIA DLSS
comme une frame de jeu, et l'add-on RenoDX DLSS5 y applique le Neural Rendering DLSS 5.
"""
import configparser
import json
import mimetypes
import os
import re
import shutil
import subprocess
import sys
import threading
import time
import traceback
import uuid
import webbrowser
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import parse_qs, unquote, urlparse

APP = "RealForge DLSS 5"
import hashlib as _hashlib
# version = empreinte du code : une instance plus ancienne encore ouverte est remplacee automatiquement
try:
    VERSION = _hashlib.sha1(open(__file__, "rb").read()).hexdigest()[:12]
except Exception:
    VERSION = "dev"
ROOT = Path(__file__).resolve().parent
ENGINE = ROOT / "engine"
WEB = ROOT / "web"
WORK = ROOT / "work"
EXE = ENGINE / "realforge_engine.exe"
CONFIG_FILE = ROOT / "config.json"
PORT = int(os.environ.get("REALFORGE_PORT", "8765"))
NO_WINDOW = 0x08000000 if os.name == "nt" else 0

IMAGE_EXT = {".png", ".jpg", ".jpeg", ".bmp", ".tif", ".tiff", ".webp", ".jxr", ".heic"}
VIDEO_EXT = {".mp4", ".mkv", ".mov", ".avi", ".webm", ".m4v", ".wmv", ".flv", ".ts", ".mts", ".gif"}

REQUIRED = {
    "dxgi.dll": "ReShade (renomme en dxgi.dll)",
    "renodx-dlss5.addon64": "Add-on RenoDX DLSS 5",
    "nvngx_dlssnr.dll": "DLSS 5 Neural Renderer",
    "nvngx_dlss.dll": "DLSS Super Resolution",
}

# ------------------------------------------------------------------ styles de rendu
# Cles de [RenoDX.DLSS5] dans ReShade.ini (lues dans l'add-on RenoDX DLSS5 v7) :
#   NRStyle 0 Defaut / 1 Natural / 2 Cinematic, NRIntensity, NRLocalTone, NRLocalStructure,
#   NRSkinStructure (<0 lisse, >0 renforce, -1 = auto), NRAutoMask (detection personnages),
#   NRPasses (passes empilees : >1 surcharge l'image), NRPreUpscale.
PRESETS = {
    "natural":   {"style": 1, "intensity": 1.0, "tone": 1.0, "structure": 1.0, "skin": 0.0, "mask": 1},
    "cinematic": {"style": 2, "intensity": 1.0, "tone": 1.0, "structure": 1.0, "skin": 0.0, "mask": 1},
    "realism":   {"style": 1, "intensity": 2.0, "tone": 1.5, "structure": 2.0, "skin": 1.5, "mask": 1},
    "ultra":     {"style": 1, "intensity": 2.0, "tone": 2.0, "structure": 2.0, "skin": 2.0, "mask": 1},
}
DEFAULTS = {
    "preset": "custom", "nr": True, "color": "linear", "scale": 1.0, "passes": 1, "mix": 1.0,
    "max_pixels": 33177600, "warmup": 12, "preview_window": True,
    "start_seconds": 0, "max_seconds": 0, "codec": "h264", "crf": 16, "reset_each": False,
    "frame_passes": 1, "keep_frames": False,
}


# ------------------------------------------------------------------ configuration
def load_config():
    cfg = {"dll_dir": str(ROOT.parent)}
    try:
        cfg.update(json.loads(CONFIG_FILE.read_text(encoding="utf-8")))
    except Exception:
        pass
    return cfg


CONFIG = load_config()


def save_config():
    CONFIG_FILE.write_text(json.dumps(CONFIG, indent=2, ensure_ascii=False), encoding="utf-8")


def find_tool(name):
    for cand in (ROOT / "ffmpeg" / "bin" / f"{name}.exe", ROOT / "ffmpeg" / f"{name}.exe"):
        if cand.exists():
            return str(cand)
    return shutil.which(name)


def find_source_dll(name):
    """Fichier de l'utilisateur : racine du dossier des DLL d'abord, puis sous-dossiers."""
    base = Path(CONFIG["dll_dir"])
    if not base.is_dir():
        return None
    for pattern in ("", "*/", "*/*/", "*/*/*/"):
        for p in sorted(base.glob(pattern + name)):
            if ROOT in p.parents or ENGINE in p.parents:
                continue
            if p.is_file():
                return p
    return None


def prepare_engine():
    """Lie (lien physique, 0 octet) ou copie les DLL de l'utilisateur a cote du moteur."""
    ENGINE.mkdir(exist_ok=True)
    report = []
    for name, desc in REQUIRED.items():
        src, dst = find_source_dll(name), ENGINE / name
        if not src:
            report.append({"file": name, "desc": desc, "ok": dst.exists(), "src": None,
                           "note": "deja dans engine" if dst.exists() else "introuvable"})
            continue
        try:
            try:
                same = dst.exists() and os.path.samefile(dst, src)
            except OSError:
                same = False
            if not same:
                if dst.exists():
                    dst.unlink()
                try:
                    os.link(src, dst)
                except OSError:
                    shutil.copy2(src, dst)
            report.append({"file": name, "desc": desc, "ok": True, "src": str(src), "note": "pret"})
        except Exception as e:
            report.append({"file": name, "desc": desc, "ok": False, "src": str(src), "note": f"erreur : {e}"})
    for stale in ("renodx-dlss.addon64",):  # l'ancien add-on ne doit jamais etre charge avec celui-ci
        if (ENGINE / stale).exists():
            (ENGINE / stale).unlink()
    return report


def resolve_look(s):
    """Reglages NR effectifs a partir du style choisi (ou des curseurs en mode perso)."""
    preset = s.get("preset", "custom")
    look = dict(PRESETS.get(preset, PRESETS["natural"]))
    if preset == "custom":
        # base = qualite Natural (rendu de reference) ; l'utilisateur ajuste a partir de la
        look = dict(PRESETS["natural"])
        for k in ("style", "intensity", "tone", "structure", "skin", "mask"):
            if s.get(k) is not None:
                look[k] = s[k]
        # une valeur negative de Local Skin lisse la peau et efface la texture : jamais sous 0
        look["skin"] = max(0.0, float(look["skin"]))
    return look


def write_reshade_ini(s):
    """Ecrit TOUTES les cles NR a chaque rendu : aucun reste d'un ancien reglage."""
    ini_path = ENGINE / "ReShade.ini"
    cp = configparser.RawConfigParser(strict=False, interpolation=None)
    cp.optionxform = str
    try:
        cp.read(ini_path, encoding="utf-8-sig")
    except Exception:
        cp = configparser.RawConfigParser(strict=False, interpolation=None)
        cp.optionxform = str
    for sec in ("GENERAL", "ADDON", "OVERLAY", "RenoDX.DLSS5"):
        if not cp.has_section(sec):
            cp.add_section(sec)
    cp.set("ADDON", "AddonPath", str(ENGINE))
    cp.set("OVERLAY", "TutorialProgress", "4")
    look = resolve_look(s)
    fmt = lambda v: (f"{float(v):.3f}".rstrip("0").rstrip(".")) if isinstance(v, float) else str(int(v))
    sec = "RenoDX.DLSS5"
    values = {
        "EnableHooks": 2 if s.get("nr", True) else 0,
        "NRStyle": int(look["style"]),
        "NRIntensity": float(look["intensity"]),
        "NRLocalTone": float(look["tone"]),
        "NRLocalStructure": float(look["structure"]),
        "NRSkinStructure": float(look["skin"]),
        "NRAutoMask": int(look["mask"]),
        "NRPasses": int(s.get("_round_passes", min(4, int(s.get("passes", 1))))),
        "NRPreUpscale": 0,
        "NRChainedHistory": 1,
        "NRResolutionScale": 1,
        "NRFollowInputRes": 0,
    }
    for k, v in values.items():
        cp.set(sec, k, fmt(v))
    with open(ini_path, "w", encoding="utf-8") as f:
        cp.write(f, space_around_delimiters=False)
    return values


def reshade_log_lines():
    log = ENGINE / "ReShade.log"
    try:
        return log.read_text(encoding="utf-8", errors="replace").splitlines()
    except Exception:
        return []


def nr_verdict(since):
    """(ok, message) d'apres ReShade.log (reecrit a chaque lancement du moteur)."""
    log = ENGINE / "ReShade.log"
    if not log.exists() or log.stat().st_mtime < since - 2:
        return False, "ReShade ne s'est pas charge (dxgi.dll absent ou bloque) : le rendu DLSS 5 n'a pas eu lieu."
    text = "\n".join(reshade_log_lines())
    if "renodx-dlss5" not in text.lower() and "DLSS 5 Neural Rendering" not in text:
        return False, "L'add-on RenoDX DLSS 5 n'a pas ete charge par ReShade."
    engaged = [int(e) for st, e in re.findall(r"NR-VERDICT v\d+ state=(\w+).*?evals=(\d+)", text) if st == "ENGAGED"]
    if engaged and max(engaged) > 0:
        return True, f"Neural Rendering applique ({max(engaged)} evaluations)"
    m = re.search(r"feature 18 create failed with (0x[0-9a-fA-F]+)", text)
    if m:
        return False, f"le runtime DLSS 5 a refuse de demarrer ({m.group(1)}). Essayez une taille de travail plus petite."
    m = re.search(r"NR-VERDICT v\d+ state=\w+ stage=(\w+) reason=([\w-]+)", text)
    why = f" (etape {m.group(1)}, raison {m.group(2)})" if m else ""
    return False, "le Neural Rendering ne s'est pas engage" + why + "."


def dlss_indicator_on():
    if os.name != "nt":
        return False
    try:
        import winreg
        with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, r"SOFTWARE\NVIDIA Corporation\Global\NGXCore") as k:
            return int(winreg.QueryValueEx(k, "ShowDlssIndicator")[0]) != 0
    except Exception:
        return False


def disable_dlss_indicator():
    args = 'add "HKLM\\SOFTWARE\\NVIDIA Corporation\\Global\\NGXCore" /v ShowDlssIndicator /t REG_DWORD /d 0 /f'
    subprocess.run(["powershell", "-NoProfile", "-Command",
                    f"Start-Process reg.exe -Verb RunAs -Wait -WindowStyle Hidden -ArgumentList '{args}'"],
                   creationflags=NO_WINDOW, timeout=180)
    return not dlss_indicator_on()


# ------------------------------------------------------------------ jobs
JOBS, ORDER = {}, []
LOCK = threading.Lock()
WAKE = threading.Event()


class Cancelled(Exception):
    pass


def save_job(job):
    try:
        (WORK / job["id"] / "job.json").write_text(json.dumps(public(job), ensure_ascii=False), encoding="utf-8")
    except Exception:
        pass


def public(job):
    return {k: v for k, v in job.items() if k != "proc"}


def log(job, msg):
    job["log"].append(time.strftime("%H:%M:%S  ") + msg)
    del job["log"][:-500]


def is_progress(line):
    return bool(re.match(r"[a-z_0-9]+=", line))


def run_cmd(job, cmd, on_line=None, cwd=None):
    log(job, "> " + " ".join(f'"{c}"' if " " in str(c) else str(c) for c in cmd))
    proc = subprocess.Popen([str(c) for c in cmd], cwd=cwd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            creationflags=NO_WINDOW, text=True, encoding="utf-8", errors="replace", bufsize=1)
    job["proc"] = proc
    last_error = None
    for line in proc.stdout:
        line = line.rstrip()
        if not line:
            continue
        if job["cancel"]:
            proc.kill()
            break
        if line.startswith("ERROR"):
            last_error = line[6:]
        if on_line is None or not on_line(line):
            log(job, line)
    proc.wait()
    job["proc"] = None
    if job["cancel"]:
        raise Cancelled()
    if proc.returncode != 0:
        raise RuntimeError(last_error or f"{Path(cmd[0]).name} a quitte avec le code {proc.returncode}")


def engine_args(s, sequence):
    a = [EXE, "--color", "sdr" if s.get("color") == "sdr" else "linear", "--scale", s.get("_round_scale", s.get("scale", 1.0)),
         "--warmup", s.get("warmup", 12), "--max-pixels", int(s.get("max_pixels", 33177600)),
         "--mix", float(s.get("_round_mix", s.get("mix", 1.0)))]
    if s.get("_orig"):
        a += ["--orig", s["_orig"]]
    if sequence:
        a += ["--mode", "sequence", "--frame-passes", s.get("frame_passes", 1)]
        if s.get("reset_each"):
            a.append("--reset-each")
    if not s.get("preview_window", True):
        a.append("--hidden")
    return a


def engine_cb(job, base, span):
    def cb(line):
        m = re.match(r"PROGRESS (\d+) (\d+)", line)
        if m:
            i, n = int(m.group(1)), int(m.group(2))
            job["progress"] = base + span * i / max(1, n)
            pre = job.get("_round_label", "Rendu DLSS 5")
            job["stage"] = f"{pre} : image {i}/{n}" if n > 1 else pre
            return True
        m = re.match(r"DIFF ([\d.]+)", line)
        if m:
            d = float(m.group(1))
            job.setdefault("_diffs", []).append(d)
            log(job, f"Difference moyenne rendu / original : {d:.2f} (sur 255)")
            return True
        if line.startswith("INFO image") and "trop grande" in line:
            job["notes"].append("Image reduite pour le Neural Rendering : " + line.split(":", 1)[1].strip())
        return line == "DONE"
    return cb


def probe_video(ffprobe, path):
    out = subprocess.run([ffprobe, "-v", "error", "-select_streams", "v:0", "-show_entries",
                          "stream=avg_frame_rate,r_frame_rate,width,height:format=duration", "-of", "json", str(path)],
                         capture_output=True, text=True, creationflags=NO_WINDOW)
    info = json.loads(out.stdout or "{}")
    st = (info.get("streams") or [{}])[0]
    fps = st.get("avg_frame_rate") or ""
    if fps in ("", "0/0"):
        fps = st.get("r_frame_rate") or "30/1"
    return fps, float((info.get("format") or {}).get("duration") or 0), st.get("width"), st.get("height")


MAX_ADDON_PASSES = 4   # l'add-on RenoDX empile au plus 4 passes NR par evaluation
MAX_PASSES = 4


def render_rounds(job, jdir, src, dst, sequence, base, span):
    """Passes NR > 4 : on renvoie le resultat dans DLSS 5 par tours de 4 passes.
    Le dosage (mix) et l'agrandissement ne s'appliquent qu'une fois, par rapport a l'original."""
    s = job["settings"]
    total = max(1, min(MAX_PASSES, int(s.get("passes", 1))))
    rounds = -(-total // MAX_ADDON_PASSES)
    cur = Path(src)
    for r in range(rounds):
        last = r == rounds - 1
        rs = dict(s)
        rs["_round_passes"] = min(MAX_ADDON_PASSES, total - r * MAX_ADDON_PASSES)
        rs["_round_scale"] = s.get("scale", 1.0) if r == 0 else 1.0
        rs["_round_mix"] = 1.0  # toujours 100 % du rendu DLSS 5
        if last and rounds > 1:
            rs["_orig"] = str(src)
        out = Path(dst) if last else (jdir / (f"round{r + 1}" if sequence else f"round{r + 1}.png"))
        if sequence:
            shutil.rmtree(out, ignore_errors=True)
            out.mkdir(parents=True)
        vals = write_reshade_ini(rs)
        done_passes = r * MAX_ADDON_PASSES + rs["_round_passes"]
        job["_round_label"] = "Rendu DLSS 5" if rounds == 1 else f"Rendu DLSS 5 - tour {r + 1}/{rounds} (passes {done_passes}/{total})"
        log(job, f"Tour {r + 1}/{rounds} - reglages NR : " + ", ".join(f"{k}={v}" for k, v in vals.items()))
        t_round = time.time()
        run_cmd(job, engine_args(rs, sequence) + ["--in", cur, "--out", out],
                on_line=engine_cb(job, base + span * r / rounds, span / rounds), cwd=ENGINE)
        if s.get("nr", True):
            ok, msg = nr_verdict(t_round)
            if not ok:
                raise RuntimeError("DLSS 5 non applique : " + msg)
            log(job, f"OK tour {r + 1}/{rounds} : {msg}")
            eff = re.findall(r"NR effective settings: .*?passes=(\d+)", "\n".join(reshade_log_lines()))
            if eff:
                got = int(eff[-1])
                log(job, f"Passes appliquees par l'add-on au tour {r + 1} : {got} (demandees : {rs['_round_passes']})")
                if got != rs["_round_passes"]:
                    job["notes"].append(f"Tour {r + 1} : l'add-on a applique {got} passe(s) au lieu de {rs['_round_passes']}.")
        if r > 0 and cur != Path(src):  # nettoie le resultat intermediaire precedent
            if cur.is_dir():
                shutil.rmtree(cur, ignore_errors=True)
            else:
                cur.unlink(missing_ok=True)
        cur = out
    job.pop("_round_label", None)
    diffs = job.pop("_diffs", [])
    if s.get("nr", True) and diffs and max(diffs) < 1.0:
        job["notes"].append("Le rendu est presque identique a l'original (difference moyenne "
                            f"{max(diffs):.2f}/255). Augmentez NR Intensity / Local Structure / Local Skin.")


def process_image(job, jdir):
    out = jdir / "result.png"
    render_rounds(job, jdir, job["input"], out, False, 0.0, 1.0)
    job["output_url"] = f"/media/{job['id']}/result.png"


def process_video(job, jdir):
    s = job["settings"]
    ffmpeg, ffprobe = find_tool("ffmpeg"), find_tool("ffprobe")
    if not ffmpeg or not ffprobe:
        raise RuntimeError("ffmpeg introuvable : relancez start.bat, il le telecharge automatiquement.")
    try:
        ver = subprocess.run([ffmpeg, "-version"], capture_output=True, text=True, creationflags=NO_WINDOW).stdout.splitlines()[0]
        log(job, f"{ver}  ({ffmpeg})")
    except Exception:
        pass
    fps, dur, w, h = probe_video(ffprobe, job["input"])
    start, maxs = float(s.get("start_seconds") or 0), float(s.get("max_seconds") or 0)
    total = max(0.0, (min(dur - start, maxs) if maxs > 0 else dur - start)) if dur else 0
    log(job, f"Video {w}x{h}, {fps} i/s, {total:.1f} s a traiter")
    fin, fout = jdir / "frames_in", jdir / "frames_out"
    for d in (fin, fout):
        shutil.rmtree(d, ignore_errors=True)
        d.mkdir(parents=True)
    ss = ["-ss", f"{start}"] if start > 0 else []
    tt = ["-t", f"{maxs}"] if maxs > 0 else []

    job["stage"] = "Extraction des images"

    def ext_cb(line):
        m = re.match(r"out_time_us=(\d+)", line)
        if m and total:
            job["progress"] = 0.08 * min(1.0, int(m.group(1)) / 1e6 / total)
        return is_progress(line)
    run_cmd(job, [ffmpeg, "-hide_banner", "-loglevel", "error", "-y", *ss, "-i", job["input"], *tt, "-map", "0:v:0",
                  "-vf", f"fps={fps}", "-pix_fmt", "rgb24", "-progress", "pipe:1", "-nostats",
                  str(fin / "%06d.png")], on_line=ext_cb)
    n = len(list(fin.glob("*.png")))
    if not n:
        raise RuntimeError("aucune image n'a pu etre extraite de la video")
    log(job, f"{n} images extraites")

    render_rounds(job, jdir, fin, fout, True, 0.08, 0.84)

    job["stage"] = "Encodage de la video"
    codec, crf = s.get("codec", "h264"), str(s.get("crf", 16))
    venc = {"h265": ["-c:v", "libx265", "-crf", crf, "-preset", "slow", "-tag:v", "hvc1", "-pix_fmt", "yuv420p"],
            "nvenc": ["-c:v", "h264_nvenc", "-preset", "p7", "-cq", crf, "-b:v", "0", "-pix_fmt", "yuv420p"],
            "prores": ["-c:v", "prores_ks", "-profile:v", "5", "-vendor", "apl0", "-pix_fmt", "yuv444p12le"]}.get(
        codec, ["-c:v", "libx264", "-crf", crf, "-preset", "slow", "-pix_fmt", "yuv420p"])
    out = jdir / ("result.mov" if codec == "prores" else "result.mp4")

    def enc_cb(line):
        m = re.match(r"frame=(\d+)", line)
        if m:
            job["progress"] = 0.92 + 0.08 * min(1.0, int(m.group(1)) / n)
        return is_progress(line)
    run_cmd(job, [ffmpeg, "-hide_banner", "-loglevel", "error", "-y", "-framerate", fps, "-i", str(fout / "%06d.png"),
                  *ss, "-i", job["input"], "-map", "0:v:0", "-map", "1:a?", *venc,
                  "-vf", "scale=trunc(iw/2)*2:trunc(ih/2)*2",
                  *(["-c:a", "pcm_s24le"] if codec == "prores" else ["-c:a", "aac", "-b:a", "256k"]), "-shortest",
                  "-progress", "pipe:1", "-nostats", str(out)], on_line=enc_cb)
    job["output_url"] = f"/media/{job['id']}/{out.name}"
    if start > 0 or maxs > 0:
        cut = jdir / "original_cut.mp4"
        try:
            run_cmd(job, [ffmpeg, "-hide_banner", "-loglevel", "error", "-y", *ss, "-i", job["input"], *tt,
                          "-c:v", "libx264", "-crf", "18", "-preset", "veryfast", "-pix_fmt", "yuv420p", "-c:a", "aac",
                          "-progress", "pipe:1", "-nostats", str(cut)], on_line=is_progress)
            job["compare_url"] = f"/media/{job['id']}/{cut.name}"
        except Exception:
            pass
    if not s.get("keep_frames"):
        shutil.rmtree(fin, ignore_errors=True)
        shutil.rmtree(fout, ignore_errors=True)


def worker():
    while True:
        WAKE.wait()
        with LOCK:
            job = next((JOBS[j] for j in ORDER if JOBS[j]["status"] == "queued"), None)
            if job is None:
                WAKE.clear()
                continue
            job["status"] = "running"
        t0 = time.time()
        jdir = WORK / job["id"]
        try:
            if not EXE.exists():
                raise RuntimeError("le moteur n'est pas compile : relancez start.bat")
            missing = [r["file"] for r in prepare_engine() if not r["ok"]]
            if missing:
                raise RuntimeError("fichiers DLSS 5 manquants : " + ", ".join(missing))
            job["stage"] = "Demarrage du moteur DLSS 5"
            (process_video if job["kind"] == "video" else process_image)(job, jdir)
            ok, msg = (True, "Neural Rendering applique") if job["settings"].get("nr", True) else (True, "Neural Rendering desactive (DLSS seul)")
            log(job, ("OK : " if ok else "ECHEC : ") + msg)
            if ok:
                job["status"], job["stage"], job["progress"] = "done", "Termine", 1.0
            else:
                job["status"], job["stage"] = "error", "DLSS 5 non applique : " + msg
                job["output_url"] = None  # ne jamais presenter une image non traitee comme un rendu
        except Cancelled:
            job["status"], job["stage"] = "cancelled", "Annule"
        except Exception as e:
            job["status"], job["stage"] = "error", f"Erreur : {e}"
            log(job, traceback.format_exc(limit=2))
        for l in reshade_log_lines():
            if re.search(r"NR-VERDICT|NR effective|create failed|evaluate failed|ERROR", l):
                log(job, "[ReShade] " + l[l.find("|", 20) + 1:].strip()[:400] if "|" in l else l[:400])
        job["elapsed"] = round(time.time() - t0, 1)
        save_job(job)


# ------------------------------------------------------------------ HTTP
def status():
    files = []
    for name, desc in REQUIRED.items():
        src = find_source_dll(name)
        files.append({"file": name, "desc": desc, "found": str(src) if src else None})
    return {"app": APP, "version": VERSION, "engine_built": EXE.exists(), "dll_dir": CONFIG["dll_dir"], "files": files,
            "ffmpeg": bool(find_tool("ffmpeg")), "dlss_indicator": dlss_indicator_on(),
            "busy": any(j["status"] == "running" for j in JOBS.values()), "presets": PRESETS, "defaults": DEFAULTS}


def diagnose():
    out = {"files": prepare_engine(), "engine": [], "reshade": []}
    if not EXE.exists():
        out["engine"] = ["Moteur absent : relancez start.bat"]
        return out
    write_reshade_ini(dict(DEFAULTS))
    try:
        r = subprocess.run([str(EXE), "--diagnose"], cwd=ENGINE, capture_output=True, text=True, timeout=120,
                           creationflags=NO_WINDOW, encoding="utf-8", errors="replace")
        out["engine"] = (r.stdout + r.stderr).splitlines()
    except Exception as e:
        out["engine"] = [f"echec : {e}"]
    out["reshade"] = [l for l in reshade_log_lines() if re.search(r"DLSS|NR|RenoDX|NGX|ERROR|WARN", l)][-80:]
    return out


def open_folder(path):
    path.mkdir(parents=True, exist_ok=True)
    if os.name == "nt":
        os.startfile(str(path))


class Handler(BaseHTTPRequestHandler):
    server_version = "RealForge/2.0"

    def log_message(self, *a):
        pass

    def send_json(self, obj, code=200):
        body = json.dumps(obj, ensure_ascii=False).encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def read_json(self):
        n = int(self.headers.get("Content-Length") or 0)
        return json.loads(self.rfile.read(n) or b"{}")

    def send_file(self, path, download_name=None):
        if not path.is_file():
            return self.send_error(404)
        size = path.stat().st_size
        start, end = 0, size - 1
        rng = self.headers.get("Range")
        m = re.match(r"bytes=(\d*)-(\d*)", rng or "")
        if m and (m.group(1) or m.group(2)):
            if m.group(1):
                start = int(m.group(1))
                end = min(int(m.group(2)), size - 1) if m.group(2) else end
            else:
                start = max(0, size - int(m.group(2)))
            self.send_response(206)
            self.send_header("Content-Range", f"bytes {start}-{end}/{size}")
        else:
            self.send_response(200)
        self.send_header("Content-Type", mimetypes.guess_type(path.name)[0] or "application/octet-stream")
        self.send_header("Accept-Ranges", "bytes")
        self.send_header("Content-Length", str(end - start + 1))
        self.send_header("Cache-Control", "no-store")
        if download_name:
            self.send_header("Content-Disposition", f"attachment; filename=\"{download_name}\"")
        self.end_headers()
        try:
            with open(path, "rb") as f:
                f.seek(start)
                left = end - start + 1
                while left > 0:
                    chunk = f.read(min(1 << 20, left))
                    if not chunk:
                        break
                    self.wfile.write(chunk)
                    left -= len(chunk)
        except (ConnectionResetError, BrokenPipeError, ConnectionAbortedError):
            pass

    def do_GET(self):
        u = urlparse(self.path)
        p = unquote(u.path)
        if p in ("/", "/index.html"):
            return self.send_file(WEB / "index.html")
        if p == "/api/status":
            return self.send_json(status())
        if p == "/api/jobs":
            with LOCK:
                return self.send_json([public(JOBS[j]) for j in reversed(ORDER)])
        m = re.fullmatch(r"/(media|download)/([0-9a-f]{12})/([\w.\-]+)", p)
        if m:
            f = WORK / m.group(2) / m.group(3)
            name = None
            if m.group(1) == "download" and m.group(2) in JOBS:
                name = Path(JOBS[m.group(2)]["name"]).stem + "_RealForge_DLSS5" + f.suffix
                name = re.sub(r'[^\w.\- ]', "_", name)
            return self.send_file(f, name)
        self.send_error(404)

    def do_POST(self):
        p = urlparse(self.path).path
        try:
            if p == "/api/upload":
                return self.upload(parse_qs(urlparse(self.path).query))
            if p == "/api/jobs":
                data = self.read_json()
                settings = dict(DEFAULTS)
                settings.update(data.get("settings") or {})
                out = []
                for uid in data.get("ids", []):
                    if not re.fullmatch(r"[0-9a-f]{12}", uid):
                        continue
                    meta = json.loads((WORK / uid / "upload.json").read_text(encoding="utf-8"))
                    job = {"id": uid, "name": meta["name"], "kind": meta["kind"], "status": "queued",
                           "stage": "En attente", "progress": 0.0, "log": [], "notes": [], "settings": settings,
                           "input": str(WORK / uid / meta["file"]), "input_url": f"/media/{uid}/{meta['file']}",
                           "output_url": None, "compare_url": None, "created": time.time(), "elapsed": None,
                           "proc": None, "cancel": False}
                    with LOCK:
                        JOBS[uid] = job
                        if uid in ORDER:
                            ORDER.remove(uid)
                        ORDER.append(uid)
                    out.append(public(job))
                WAKE.set()
                return self.send_json(out)
            m = re.fullmatch(r"/api/jobs/([0-9a-f]{12})/(cancel|delete|open)", p)
            if m:
                return self.job_action(m.group(1), m.group(2))
            if p == "/api/diagnose":
                return self.send_json(diagnose())
            if p == "/api/indicator-off":
                disable_dlss_indicator()
                return self.send_json(status())
            if p == "/api/config":
                data = self.read_json()
                if data.get("dll_dir"):
                    CONFIG["dll_dir"] = data["dll_dir"]
                    save_config()
                return self.send_json(status())
            if p == "/api/shutdown":
                if self.client_address[0] != "127.0.0.1":
                    return self.send_json({"error": "refuse"}, 403)
                self.send_json({"ok": True})
                say("Arret demande par une version plus recente de RealForge.")
                threading.Timer(0.3, lambda: os._exit(0)).start()
                return
            if p == "/api/open-work":
                open_folder(WORK)
                return self.send_json({"ok": True})
        except Exception as e:
            return self.send_json({"error": str(e)}, 500)
        self.send_error(404)

    def upload(self, q):
        name = Path(q.get("name", ["fichier"])[0]).name
        ext = Path(name).suffix.lower()
        kind = "image" if ext in IMAGE_EXT else "video" if ext in VIDEO_EXT else None
        if not kind:
            return self.send_json({"error": f"format non pris en charge ({ext})"}, 400)
        uid = uuid.uuid4().hex[:12]
        jdir = WORK / uid
        jdir.mkdir(parents=True)
        left = int(self.headers.get("Content-Length") or 0)
        with open(jdir / ("input" + ext), "wb") as f:
            while left > 0:
                chunk = self.rfile.read(min(1 << 20, left))
                if not chunk:
                    break
                f.write(chunk)
                left -= len(chunk)
        (jdir / "upload.json").write_text(json.dumps({"name": name, "kind": kind, "file": "input" + ext}), encoding="utf-8")
        return self.send_json({"id": uid, "name": name, "kind": kind, "url": f"/media/{uid}/input{ext}"})

    def job_action(self, jid, action):
        job = JOBS.get(jid)
        if action == "open":
            open_folder(WORK / jid)
        elif action == "cancel" and job:
            job["cancel"] = True
            if job["status"] == "queued":
                job["status"], job["stage"] = "cancelled", "Annule"
            if job.get("proc"):
                try:
                    job["proc"].kill()
                except Exception:
                    pass
        elif action == "delete":
            if job and job["status"] == "running":
                return self.send_json({"error": "annulez d'abord ce rendu"}, 409)
            with LOCK:
                JOBS.pop(jid, None)
                if jid in ORDER:
                    ORDER.remove(jid)
            shutil.rmtree(WORK / jid, ignore_errors=True)
        return self.send_json({"ok": True})


def restore_jobs():
    WORK.mkdir(exist_ok=True)
    for d in sorted((p for p in WORK.iterdir() if p.is_dir()), key=lambda p: p.stat().st_mtime):
        try:
            job = json.loads((d / "job.json").read_text(encoding="utf-8"))
        except Exception:
            continue
        if job.get("status") in ("queued", "running"):
            job["status"], job["stage"] = "cancelled", "Interrompu"
        job.update(proc=None, cancel=False)
        JOBS[d.name] = job
        ORDER.append(d.name)


LOG_FILE = ROOT / "realforge.log"


def say(msg):
    """Affiche dans la console ET ecrit dans realforge.log (pour le diagnostic)."""
    line = time.strftime("[%Y-%m-%d %H:%M:%S] ") + msg
    try:
        print(line, flush=True)
    except Exception:
        pass
    try:
        with open(LOG_FILE, "a", encoding="utf-8") as f:
            f.write(line + "\n")
    except Exception:
        pass


def running_version(port):
    """Version de RealForge qui tourne sur ce port ('' si version tres ancienne, None si autre programme)."""
    import urllib.request
    try:
        with urllib.request.urlopen(f"http://127.0.0.1:{port}/api/status", timeout=2) as r:
            d = json.loads(r.read().decode("utf-8"))
            return d.get("version", "") if d.get("app") == APP else None
    except Exception:
        return None


def stop_instance(port):
    import urllib.request
    try:
        req = urllib.request.Request(f"http://127.0.0.1:{port}/api/shutdown", data=b"{}", method="POST",
                                     headers={"Content-Type": "application/json"})
        urllib.request.urlopen(req, timeout=3).read()
    except Exception:
        pass


def open_browser(url):
    """Ouvre l'interface dans le navigateur par defaut (plusieurs methodes, Windows d'abord)."""
    if "--no-browser" in sys.argv:
        return
    try:
        if os.name == "nt":
            os.startfile(url)
            return
    except Exception as e:
        say(f"os.startfile a echoue ({e}), essai avec webbrowser")
    try:
        if not webbrowser.open(url):
            raise RuntimeError("aucun navigateur")
    except Exception:
        try:
            subprocess.Popen(["cmd", "/c", "start", "", url], creationflags=NO_WINDOW)
        except Exception as e:
            say(f"Impossible d'ouvrir le navigateur ({e}). Ouvrez vous-meme : {url}")


def main():
    say(f"Demarrage de {APP} (Python {sys.version.split()[0]}) depuis {ROOT}")
    restore_jobs()
    srv = None
    for port in range(PORT, PORT + 20):
        try:
            srv = ThreadingHTTPServer(("127.0.0.1", port), Handler)
            break
        except OSError:
            ver = running_version(port)
            if ver == VERSION:
                url = f"http://127.0.0.1:{port}/"
                say(f"{APP} est deja ouvert sur {url} : ouverture du navigateur.")
                open_browser(url)
                return
            if ver is not None:
                say(f"Une ancienne version de {APP} tourne sur le port {port} : remplacement par la nouvelle.")
                stop_instance(port)
                for _ in range(20):
                    time.sleep(0.25)
                    try:
                        srv = ThreadingHTTPServer(("127.0.0.1", port), Handler)
                        break
                    except OSError:
                        pass
                if srv:
                    break
                say(f"L'ancienne version ne s'est pas arretee (fermez sa fenetre) : utilisation d'un autre port.")
                continue
            say(f"Port {port} deja utilise par un autre programme, essai du suivant.")
    if srv is None:
        raise RuntimeError(f"aucun port libre entre {PORT} et {PORT + 19}")
    url = f"http://127.0.0.1:{srv.server_address[1]}/"
    threading.Thread(target=worker, daemon=True).start()
    say(f"Interface prete : {url}   (gardez cette fenetre ouverte ; Ctrl+C pour arreter)")
    say(f"Dossier des fichiers DLSS 5 : {CONFIG['dll_dir']}")
    threading.Timer(0.6, open_browser, args=(url,)).start()
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    try:
        main()
    except Exception:
        say("ERREUR au demarrage :\n" + traceback.format_exc())
        sys.exit(1)
