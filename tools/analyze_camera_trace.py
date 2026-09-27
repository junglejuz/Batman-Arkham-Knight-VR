"""Summarize F1's camera/mode traces; measurements, not a headset acceptance test."""
import argparse
import collections
import csv
import json
import math
from pathlib import Path
import statistics


def percentile(values, fraction):
    values = sorted(values)
    return values[round((len(values) - 1) * fraction)] if values else None


def summary(values):
    values = [v for v in values if math.isfinite(v)]
    if not values:
        return {"samples": 0}
    return {
        "samples": len(values),
        "min": min(values), "median": statistics.median(values),
        "p95": percentile(values, 0.95), "max": max(values),
        "stddev": statistics.pstdev(values),
    }


def angular_step(a, b):
    return (b - a + 180.0) % 360.0 - 180.0


def read_tail(path, seconds):
    with path.open(newline="", encoding="utf-8-sig") as source:
        rows = list(csv.DictReader(source))
    if not rows:
        raise ValueError(f"No recorded rows: {path}")
    cutoff = float(rows[-1]["t_sec"]) - seconds
    return [r for r in rows if float(r["t_sec"]) >= cutoff]


def analyse_camera(path, seconds):
    rows = read_tail(path, seconds)
    if len(rows) < 2:
        raise ValueError("At least two camera samples are required")
    intervals = [(float(b["t_sec"]) - float(a["t_sec"])) * 1000
                 for a, b in zip(rows, rows[1:])]
    steps = collections.Counter(int(b["frame"]) - int(a["frame"])
                                for a, b in zip(rows, rows[1:]))
    result = {
        "file": str(path), "rows": len(rows),
        "span_seconds": float(rows[-1]["t_sec"]) - float(rows[0]["t_sec"]),
        "sample_interval_ms": summary(intervals),
        "camera_finalize_count_steps": dict(steps),
        "native_eye_samples": dict(collections.Counter(r["eye"] for r in rows)),
        "angular_step_degrees": {}, "lean_range_mm": {},
        "limits": "Present-side samples; do not prove GPU frame ownership or compositor behavior. Test stillness is supplied by the user.",
    }
    # Circular differences avoid a false 360-degree jump across +/-180.
    for field in ("raw_head_yaw", "raw_head_pitch", "raw_head_roll",
                  "base_yaw_deg", "base_pitch_deg", "base_roll_deg",
                  "final_yaw_deg", "final_pitch_deg", "final_roll_deg",
                  "live_yaw", "live_pitch", "live_roll", "display_roll"):
        values = [float(r[field]) for r in rows]
        deltas = [abs(angular_step(a, b)) for a, b in zip(values, values[1:])]
        result["angular_step_degrees"][field] = summary(deltas)
    for field in ("lean_x_m", "lean_y_m", "lean_z_m"):
        values = [float(r[field]) for r in rows]
        result["lean_range_mm"][field] = (max(values) - min(values)) * 1000
    return result


def analyse_mode(path, seconds):
    rows = read_tail(path, seconds)
    return {
        "file": str(path), "rows": len(rows),
        "gameplay_fraction": sum(int(r["verdict"]) for r in rows) / len(rows),
        "mode_transitions": sum(a["verdict"] != b["verdict"] for a, b in zip(rows, rows[1:])),
        "camera_fov_degrees": summary([float(r["cam_fov"]) for r in rows]),
        "projection_builds_per_present": dict(collections.Counter(r["proj_hits"] for r in rows)),
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("camera_trace", type=Path)
    parser.add_argument("--mode-trace", type=Path)
    parser.add_argument("--seconds", type=float, default=10.0)
    args = parser.parse_args()
    if args.seconds <= 0:
        parser.error("--seconds must be positive")
    result = {"camera": analyse_camera(args.camera_trace, args.seconds)}
    if args.mode_trace:
        result["mode"] = analyse_mode(args.mode_trace, args.seconds)
    print(json.dumps(result, indent=2, allow_nan=False))


if __name__ == "__main__":
    main()
