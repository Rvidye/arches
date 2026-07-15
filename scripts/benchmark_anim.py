# Animation benchmark for Arches -- companion to benchmark.py, but sweeps a keyframe clip
# (scene x strategy x frame) instead of a single static render.
#
# Two targets (set MODE):
#   "native" -> src/trax-kernel/trax-kernel.exe  (host software renderer, Part 1 of the anim guide).
#               Fast; metrics come from stdout: Node steps / Prim steps / Memory Traffic / Runtime.
#               Positional args:  <scene> <w> <h>  + the --anim-* flags.
#   "sim"    -> build/.../arches-v2.exe  (cycle simulator, needs anim guide Parts 2-3 ported).
#               Metrics come from the single "ANIM_CSV,..." line the sim prints. All --key=value args.
#
# Output per run: a .log (full stdout) + the frame PNG, both named <scene>-<strategy>-fNNN.
# Output per (scene,strategy): a CSV of per-frame metrics for plotting refit-vs-rebuild.

import os
import re
import csv
import shutil
import subprocess

# --- what to sweep -----------------------------------------------------------
MODE     = "native"                 # "native" (works now) or "sim" (after guide Parts 2-3)
SCENES   = ["wooddoll", "hand"]     # keyframe scenes under datasets/<scene>/<scene>_NNN.obj
STRATS   = ["rebuild", "refit"]     # BVH update strategy per frame
FRAMES   = 57                       # number of interpolated frames to render (0 .. FRAMES-1)
RES      = 256                      # framebuffer w=h (sim needs >=160; native is unrestricted)
BVH_PRESET = 0                      # sim only; native uses its own default preset

# --- paths (derived from this script's location: scripts/ is under the repo root) -------------
REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
NATIVE_EXE = os.path.join(REPO_ROOT, "build", "src", "trax-kernel", "trax-kernel.exe")
SIM_EXE    = os.path.join(REPO_ROOT, "build", "src", "arches-v2", "Release", "arches-v2.exe")
DATASET_DIR = os.path.join(REPO_ROOT, "datasets")
LOG_DIR   = os.path.join(REPO_ROOT, "scripts", "anim-logs")
IMG_DIR   = os.path.join(REPO_ROOT, "scripts", "anim-images")
CSV_DIR   = os.path.join(REPO_ROOT, "scripts", "anim-results")


def get_anim_configs():
    """Every (scene, strategy, frame) triple to run, as a flat list of dicts."""
    configs = []
    for scene in SCENES:
        for strategy in STRATS:
            for frame in range(FRAMES):
                configs.append({
                    "scene": scene, "strategy": strategy, "frame": frame,
                    "anim_frames": FRAMES, "res": RES,
                })
    return configs


def build_command(config):
    """Turn a config into (argv list, cwd, produced_png_path)."""
    s, w, h = config["scene"], config["res"], config["res"]
    fr, nfr, strat = config["frame"], config["anim_frames"], config["strategy"]
    if MODE == "native":
        # native takes positional <scene> <w> <h>, writes frame_<NNN>.png to cwd, needs relative datasets/
        argv = [NATIVE_EXE, s, str(w), str(h),
                f"--anim-frames={nfr}", f"--anim-frame={fr}", f"--anim-strategy={strat}"]
        produced = os.path.join(REPO_ROOT, f"frame_{fr:03d}.png")
        return argv, REPO_ROOT, produced
    else:  # "sim"
        argv = [SIM_EXE, "--arch-name=TRaX", f"--scene-name={s}", f"--dataset-dir={DATASET_DIR}",
                f"--framebuffer-width={w}", f"--framebuffer-height={h}", f"--bvh-preset={BVH_PRESET}",
                f"--anim-frames={nfr}", f"--anim-frame={fr}", f"--anim-strategy={strat}"]
        # sim writes out.png in its own dir; run there so it lands next to the exe
        return argv, os.path.dirname(SIM_EXE), os.path.join(os.path.dirname(SIM_EXE), "out.png")


def tag(config):
    return f"{config['scene']}-{config['strategy']}-f{config['frame']:03d}"


def parse_metrics(config, stdout):
    """Extract per-frame numbers from a run's stdout into a dict (columns depend on MODE)."""
    row = {"scene": config["scene"], "strategy": config["strategy"], "frame": config["frame"]}
    if MODE == "native":
        def grab(pat):
            m = re.search(pat, stdout)
            return float(m.group(1)) if m else ""
        row["node_steps"]   = grab(r"Node steps:\s*([\d.]+)")
        row["prim_steps"]   = grab(r"Prim steps:\s*([\d.]+)")
        row["mem_B_per_ray"] = grab(r"Memory Traffic:\s*([\d.]+)")
        row["runtime_ms"]   = grab(r"Runtime:\s*(\d+)ms")
    else:  # "sim": harvest the single ANIM_CSV line the simulator prints
        line = next((l for l in stdout.splitlines() if l.startswith("ANIM_CSV")), "")
        # ANIM_CSV,scene,mode,frame,t,mag,strategy,cycles,dram_B,l2_B,l1_B,rays,nodes,tris,sah,node_B,leaf_B,build_ms
        cols = ["_", "scene", "mode", "frame", "t", "mag", "strategy", "cycles", "dram_B", "l2_B",
                "l1_B", "rays", "nodes", "tris", "sah", "node_B", "leaf_B", "build_ms"]
        vals = line.split(",")
        if len(vals) == len(cols):
            row.update({c: v for c, v in zip(cols, vals) if c != "_"})
    return row


def run():
    exe = NATIVE_EXE if MODE == "native" else SIM_EXE
    if not os.path.exists(exe):
        raise FileNotFoundError(f"{MODE} executable not found at: {exe}")
    for d in (LOG_DIR, IMG_DIR, CSV_DIR):
        os.makedirs(d, exist_ok=True)

    configs = get_anim_configs()
    rows_by_group = {}          # (scene,strategy) -> list of metric rows, for per-group CSVs

    for idx, config in enumerate(configs):
        argv, cwd, produced_png = build_command(config)
        t = tag(config)
        log_path = os.path.join(LOG_DIR, t + ".log")
        print(f"[{idx + 1}/{len(configs)}] {t}: {' '.join(argv)}")

        try:
            proc = subprocess.run(argv, cwd=cwd, capture_output=True, text=True, check=True)
            stdout = proc.stdout + proc.stderr
        except subprocess.CalledProcessError as e:
            stdout = (e.stdout or "") + (e.stderr or "") + f"\n[exit {e.returncode}]\n"
            print(f"    !! run failed (exit {e.returncode}) -- see {log_path}")
        except Exception as ex:
            stdout = f"[python error] {ex}\n"
            print(f"    !! {ex}")

        with open(log_path, "w") as fh:      # keep the noisy 'Invalid line' etc. out of the console
            fh.write(stdout)

        # name + move the frame image
        if os.path.exists(produced_png):
            shutil.move(produced_png, os.path.join(IMG_DIR, t + ".png"))

        rows_by_group.setdefault((config["scene"], config["strategy"]), []).append(
            parse_metrics(config, stdout))

    # one CSV per (scene, strategy): frames in order, ready to plot
    for (scene, strat), rows in rows_by_group.items():
        if not rows:
            continue
        out_csv = os.path.join(CSV_DIR, f"{scene}_{strat}.csv")
        with open(out_csv, "w", newline="") as fh:
            w = csv.DictWriter(fh, fieldnames=list(rows[0].keys()))
            w.writeheader()
            w.writerows(rows)
        print(f"wrote {out_csv}  ({len(rows)} frames)")


if __name__ == "__main__":
    run()
