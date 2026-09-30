#!/usr/bin/env python3
"""CastMirror real-device soak log parser and run-record emitter.

WHAT THIS DOES
--------------
Reads a raw `castmirror` console log (ANSI colour codes, carriage-return
separated `[LIVE]` progress lines, and timestamped core Logger lines) plus an
optional operator metadata sidecar and an optional polled-samples TSV, and
writes ONE machine-readable run record:

    schema = "castmirror.device_soak.run.v1"

WHAT THIS DOES NOT DO
---------------------
It does NOT measure anything itself and it NEVER invents a value. Every field
it cannot derive from the log, the samples TSV or the operator metadata is
written as JSON `null` and listed in the record's `"not_measured"` array.
In particular glass-to-glass latency is NOT measured by any CastMirror
software -- it must be supplied by the operator from an external camera
observation (`--g2g-ms` / metadata `glass_to_glass_ms`), together with the
method and its uncertainty.

USAGE
-----
  soak_parse.py parse --log RUN.log [--samples samples.tsv] [--meta meta.json] \
      --out run.json [--summary run.txt] [--run-id ID] [--synthetic] \
      [--model M] [--md MD] [--firmware F] [--band 5GHz] [--distance 3] \
      [--preset Balanced] [--bitrate 8000] [--delay 200] \
      [--capture monitor|window] [--duration 300] \
      [--g2g-ms 143] [--g2g-method "..."] [--g2g-uncertainty-ms 8] \
      [--app-id 0F5096E8] [--notes "..."]

  samples.tsv columns (tab separated, one row per poll, header optional):
      elapsed_ms  fps  bitrate_kbps  rtt_ms  loss_percent  target_delay_ms
"""

from __future__ import annotations

import argparse
import datetime as _dt
import json
import os
import platform
import re
import sys

SCHEMA = "castmirror.device_soak.run.v1"

# Documented preset defaults from core/include/castcore/types.h
# (QualityPresetDefaultBitrateKbps) and the Game/Cinema target-delay overrides
# applied by the GTK GUI. These are CONFIGURATION constants, not measurements,
# and are only used to fill target_bitrate_kbps / target_delay_ms when the
# operator did not supply them. The record labels the source of each value.
PRESET_DEFAULT_BITRATE_KBPS = {
    "Auto": 8000, "High": 12000, "Balanced": 8000,
    "Smooth": 5000, "Game": 8000, "Cinema": 16000,
}
PRESET_DEFAULT_DELAY_MS = {
    "Auto": 200, "High": 200, "Balanced": 200,
    "Smooth": 200, "Game": 150, "Cinema": 400,
}

ANSI_RE = re.compile(r"\x1b\[[0-9;]*[A-Za-z]")
TS_RE = re.compile(r"^(\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}\.\d{3}) ")
LIVE_RE = re.compile(
    r"\[LIVE\]\s+FPS:\s*([\d.]+)\s*\|\s*Bitrate:\s*([\d.]+)\s*Mbps"
    r"\s*\|\s*RTT:\s*([\d.]+)\s*ms\s*\|\s*Loss:\s*([\d.]+)%"
    r"\s*\|\s*Target Delay:\s*(\d+)ms"
)
# tools/poc-join emits a differently-shaped live line; the local synthetic
# harness feeds it through the SAME parser and schema so the output format
# stays coherent across both surfaces.
LIVE_STATS_RE = re.compile(
    r"\[LIVE STATS\]\s*FPS:\s*([\d.]+)\s*,\s*Bitrate:\s*([\d.]+)\s*kbps"
    r"\s*,\s*RTT:\s*([\d.]+)\s*ms\s*,\s*Loss:\s*([\d.]+)%"
)
STATS_RE = re.compile(
    r"Session stats:\s*frames=(\d+)\s+nack=(\d+)\s+pli=(\d+)\s+loss=([\d.]+)%"
    r"\s+bitrate=(\d+)kbps\s+fps=([\d.]+)"
)
RUNG_INIT_RE = re.compile(
    r"Initialized Adaptive Controller at Ladder Rung (\d+)"
    r"\s*\((\d+)x(\d+)@([\d.]+)fps,\s*(\d+)\s*kbps,\s*target_delay=(\d+)ms\)"
)
RUNG_DOWN_RE = re.compile(r"Adaptive bitrate downshift -> (\d+) kbps \(rung (\d+)\)(.*)")
RUNG_EMERG_RE = re.compile(r"Emergency bitrate downshift -> (\d+) kbps \(rung (\d+)\)(.*)")
RUNG_UP_RE = re.compile(r"Adaptive bitrate upshift -> (\d+) kbps \(rung (\d+)\)")
RUNG_RAMP_RE = re.compile(r"Aggressive bitrate ramp-up -> (\d+) kbps \(from (\d+), target (\d+)\)")
RECOVERY_START_RE = re.compile(r"Starting Session Recovery \(Max Timeout: (\d+)s\)\. Reason: (.*)")
RECOVERY_OK_RE = re.compile(r"Session Recovery Succeeded \(resolved after (\d+)s\)")
APP_ID_RE = re.compile(r"\b([0-9A-Fa-f]{8})\b")
DEVICE_LINE_RE = re.compile(r"^\s*\[\d+\]\s+(.+?)\s+\((.+?)\)\s+@\s+(\S+)\s+\[(.+?)\]")

# Substrings that mark a disconnect / reconnect / session-failure event.
DISCONNECT_MARKERS = (
    "Starting Session Recovery",
    "Cast Channel socket disconnected",
    "Receiver closed connection",
    "Cast channel disconnected",
    "Connection lost",
    "Video stalled",
    "Capture source was lost",
    "Shared window was closed",
    "Reconnect timed out",
    "Device authentication failed",
    "Session failed",
)


def strip_ansi(text: str) -> str:
    return ANSI_RE.sub("", text)


def split_log_lines(text: str):
    """Split on LF and on the bare CR the CLI uses for its [LIVE] progress line."""
    for raw in re.split(r"[\r\n]", text):
        yield raw.rstrip()


def ts_to_ms(ts: str):
    """'2026-09-23 10:30:01.250' -> milliseconds since local midnight."""
    try:
        hh, mm, rest = ts[11:13], ts[14:16], ts[17:]
        ss, mmm = rest.split(".")
        return ((int(hh) * 60 + int(mm)) * 60 + int(ss)) * 1000 + int(mmm)
    except Exception:
        return None


def _num(v):
    if v is None:
        return None
    try:
        f = float(v)
    except (TypeError, ValueError):
        return None
    return f


def load_samples_tsv(path: str):
    rows = []
    if not path or not os.path.isfile(path):
        return rows
    with open(path, "r", encoding="utf-8", errors="replace") as fh:
        for line in fh:
            line = line.strip()
            if not line or line.lower().startswith("elapsed"):
                continue
            parts = line.split("\t")
            if len(parts) < 6:
                parts = line.split(",")
            if len(parts) < 6:
                continue
            rows.append({
                "t_ms": int(float(parts[0])),
                "fps": _num(parts[1]),
                "bitrate_kbps": _num(parts[2]),
                "rtt_ms": _num(parts[3]),
                "loss_fraction": (lambda p: None if p is None else p / 100.0)(_num(parts[4])),
                "target_delay_ms": _num(parts[5]),
            })
    return rows


def parse_log(path: str):
    """Return (events, samples_from_log, meta_from_log)."""
    events = {
        "rungs": [], "disconnects": [], "stats": [], "device_lines": [], "app_ids": [],
    }
    samples = []
    first_ts = None
    last_ts = None
    current_ts = None
    live_index = 0

    if not path or not os.path.isfile(path):
        return events, samples, {"first_ts": None, "last_ts": None}

    with open(path, "r", encoding="utf-8", errors="replace") as fh:
        text = fh.read()
    text = strip_ansi(text)

    for line in split_log_lines(text):
        if not line:
            continue
        m = TS_RE.match(line)
        if m:
            current_ts = m.group(1)
            if first_ts is None:
                first_ts = current_ts
            last_ts = current_ts
            body = line[m.end():]
            base_ms = ts_to_ms(current_ts)
        else:
            body = line
            base_ms = ts_to_ms(current_ts) if current_ts else None

        rel_ms = None
        if base_ms is not None and first_ts is not None:
            rel_ms = base_ms - ts_to_ms(first_ts)

        m = LIVE_RE.search(body)
        if m:
            live_index += 1
            t = rel_ms if rel_ms is not None else live_index * 1000
            samples.append({
                "t_ms": t,
                "fps": _num(m.group(1)),
                "bitrate_kbps": _num(m.group(2)) * 1000.0,
                "rtt_ms": _num(m.group(3)),
                "loss_fraction": _num(m.group(4)) / 100.0,
                "target_delay_ms": _num(m.group(5)),
            })
            continue

        m = LIVE_STATS_RE.search(body)
        if m:
            live_index += 1
            t = rel_ms if rel_ms is not None else live_index * 1000
            samples.append({
                "t_ms": t,
                "fps": _num(m.group(1)),
                "bitrate_kbps": _num(m.group(2)),
                "rtt_ms": _num(m.group(3)),
                "loss_fraction": _num(m.group(4)) / 100.0,
                "target_delay_ms": None,
            })
            continue

        m = STATS_RE.search(body)
        if m:
            events["stats"].append({
                "t_ms": rel_ms,
                "frames": int(m.group(1)),
                "nack": int(m.group(2)),
                "pli": int(m.group(3)),
                "loss_fraction": _num(m.group(4)) / 100.0,
                "bitrate_kbps": float(m.group(5)),
                "fps": _num(m.group(6)),
            })
            continue

        m = RUNG_INIT_RE.search(body)
        if m:
            events["rungs"].append({
                "t_ms": rel_ms, "kind": "init", "direction": None,
                "to_rung": int(m.group(1)), "from_rung": None,
                "bitrate_kbps": float(m.group(5)),
                "resolution": "%sx%s" % (m.group(2), m.group(3)),
                "fps": _num(m.group(4)),
                "target_delay_ms": float(m.group(6)),
                "detail": "",
            })
            continue

        for regex, kind, direction in (
            (RUNG_DOWN_RE, "downshift", "down"),
            (RUNG_EMERG_RE, "emergency-downshift", "down"),
        ):
            m = regex.search(body)
            if m:
                events["rungs"].append({
                    "t_ms": rel_ms, "kind": kind, "direction": direction,
                    "to_rung": int(m.group(2)), "from_rung": None,
                    "bitrate_kbps": float(m.group(1)),
                    "detail": m.group(3).strip().lstrip(":").strip(),
                })
                break

        m = RUNG_UP_RE.search(body)
        if m:
            events["rungs"].append({
                "t_ms": rel_ms, "kind": "upshift", "direction": "up",
                "to_rung": int(m.group(2)), "from_rung": None,
                "bitrate_kbps": float(m.group(1)), "detail": "",
            })
            continue

        m = RUNG_RAMP_RE.search(body)
        if m:
            events["rungs"].append({
                "t_ms": rel_ms, "kind": "bitrate-ramp", "direction": "up",
                "to_rung": None, "from_rung": None,
                "bitrate_kbps": float(m.group(1)),
                "detail": "from %s kbps, target %s kbps" % (m.group(2), m.group(3)),
            })
            continue

        m = RECOVERY_START_RE.search(body)
        if m:
            events["disconnects"].append({
                "t_ms": rel_ms, "kind": "reconnect-start",
                "reason": m.group(2).strip(),
                "recovery_timeout_s": int(m.group(1)),
                "recovered": None,
            })
            continue

        if RECOVERY_OK_RE.search(body):
            for entry in reversed(events["disconnects"]):
                if entry.get("recovered") is None:
                    entry["recovered"] = True
                    entry["recovered_t_ms"] = rel_ms
                    break
            continue

        for marker in DISCONNECT_MARKERS:
            if marker in body:
                events["disconnects"].append({
                    "t_ms": rel_ms, "kind": "disconnect", "reason": body.strip(),
                    "recovered": None,
                })
                break

        m = DEVICE_LINE_RE.match(body)
        if m:
            events["device_lines"].append({
                "name": m.group(1).strip(), "model": m.group(2).strip(),
                "ip": m.group(3).strip(), "status": m.group(4).strip(),
            })

        for m in APP_ID_RE.finditer(body):
            events["app_ids"].append(m.group(1))

    return events, samples, {"first_ts": first_ts, "last_ts": last_ts}


def _mark_not_measured(record):
    nm = []
    for path in (
        ("device", "model"), ("device", "md"), ("device", "firmware"),
        ("device", "app_id"), ("device", "target"),
        ("link", "wifi_band"), ("link", "approx_distance_m"),
        ("session", "target_bitrate_kbps"), ("session", "target_delay_ms"),
        ("session", "capture_source"), ("session", "duration_s"),
        ("session", "time_to_first_frame_ms"),
        ("latency", "glass_to_glass_ms"), ("latency", "method"),
        ("latency", "uncertainty_ms"),
        ("throughput", "peak_bitrate_kbps"), ("throughput", "average_bitrate_kbps"),
        ("quality", "packet_loss_fraction_peak"), ("quality", "packet_loss_fraction_mean"),
        ("feedback", "nack_count"), ("feedback", "pli_count"),
    ):
        node = record
        for key in path[:-1]:
            node = node[key]
        if node.get(path[-1]) is None:
            nm.append(".".join(path))
    record["not_measured"] = nm
    return record


def build_record(args):
    events, log_samples, ts_info = parse_log(args.log)
    tsv_samples = load_samples_tsv(args.samples)
    samples = tsv_samples or log_samples
    for s in samples:
        s["bitrate_kbps"] = None if s.get("bitrate_kbps") is None else float(s["bitrate_kbps"])

    meta = {}
    if args.meta and os.path.isfile(args.meta):
        with open(args.meta, "r", encoding="utf-8", errors="replace") as fh:
            meta = json.load(fh) or {}

    def pick(cli_value, meta_key, default=None):
        if cli_value is not None:
            return cli_value
        if meta_key in meta and meta[meta_key] is not None:
            return meta[meta_key]
        return default

    preset = pick(args.preset, "quality_preset", "Auto")
    target_bitrate = pick(args.bitrate, "target_bitrate_kbps",
                          PRESET_DEFAULT_BITRATE_KBPS.get(preset))
    target_delay = pick(args.delay, "target_delay_ms",
                        PRESET_DEFAULT_DELAY_MS.get(preset))

    bitrates = [s["bitrate_kbps"] for s in samples if s.get("bitrate_kbps") is not None]
    losses = [s["loss_fraction"] for s in samples if s.get("loss_fraction") is not None]
    ttff = None
    for s in samples:
        if (s.get("fps") or 0) > 0:
            ttff = s["t_ms"]
            break

    nack = max([st["nack"] for st in events["stats"]], default=None)
    pli = max([st["pli"] for st in events["stats"]], default=None)

    # Device model: prefer operator metadata, else the first discovered device
    # line the log happens to contain (the --device path does not print one).
    dev_model = pick(args.model, "model")
    dev_md = pick(args.md, "md")
    dev_fw = pick(args.firmware, "firmware")
    dev_ip = pick(None, "target", args.device)
    if events["device_lines"]:
        first_dev = events["device_lines"][0]
        dev_model = dev_model or first_dev["model"]
        dev_ip = dev_ip or first_dev["ip"]

    app_id = pick(args.app_id, "app_id")
    if not app_id and events["app_ids"]:
        # Only trust an 8-hex token that looks like a Cast mirroring app id.
        for cand in events["app_ids"]:
            if cand.upper().startswith("0F5096E8") or cand.upper() == "0F5096E8":
                app_id = cand.upper()
                break

    duration = pick(args.duration, "duration_s")
    if duration is None and samples:
        duration = int(max(s["t_ms"] for s in samples) / 1000)

    record = {
        "schema": SCHEMA,
        "run_id": args.run_id or ("synthetic" if args.synthetic else "run"),
        "synthetic": bool(args.synthetic),
        "recorded_at": _dt.datetime.now(_dt.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
        "recorded_by": "scripts/soak_real_device.sh" if not args.synthetic
                       else "scripts/run_soak_test.sh",
        "host": platform.node(),
        "tool": {"git_rev": args.git_rev, "version": args.tool_version},
        "device": {
            "model": dev_model,
            "md": dev_md,
            "firmware": dev_fw,
            "app_id": app_id,
            "target": dev_ip,
        },
        "link": {
            "wifi_band": pick(args.band, "wifi_band"),
            "approx_distance_m": _num(pick(args.distance, "approx_distance_m")),
        },
        "session": {
            "quality_preset": preset,
            "target_bitrate_kbps": _num(target_bitrate),
            "target_bitrate_kbps_source": ("operator" if (args.bitrate is not None or "target_bitrate_kbps" in meta)
                                           else "preset-default"),
            "target_delay_ms": _num(target_delay),
            "target_delay_ms_source": ("operator" if (args.delay is not None or "target_delay_ms" in meta)
                                       else "preset-default"),
            "capture_source": pick(args.capture, "capture_source"),
            "duration_s": _num(duration),
            "time_to_first_frame_ms": ttff,
        },
        "latency": {
            "glass_to_glass_ms": _num(pick(args.g2g_ms, "glass_to_glass_ms")),
            "method": pick(args.g2g_method, "glass_to_glass_method"),
            "uncertainty_ms": _num(pick(args.g2g_uncertainty_ms, "glass_to_glass_uncertainty_ms")),
        },
        "throughput": {
            "peak_bitrate_kbps": max(bitrates) if bitrates else None,
            "average_bitrate_kbps": (sum(bitrates) / len(bitrates)) if bitrates else None,
        },
        "quality": {
            "packet_loss_fraction_peak": max(losses) if losses else None,
            "packet_loss_fraction_mean": (sum(losses) / len(losses)) if losses else None,
        },
        "feedback": {"nack_count": nack, "pli_count": pli},
        "adaptive_rung_transitions": events["rungs"],
        "disconnects": events["disconnects"],
        "samples": samples,
        "log": {
            "path": args.log,
            "first_timestamp": ts_info.get("first_ts"),
            "last_timestamp": ts_info.get("last_ts"),
            "live_samples": len(log_samples),
            "polled_samples": len(tsv_samples),
        },
        "notes": pick(args.notes, "notes", ""),
    }
    return _mark_not_measured(record)


def render_summary(record):
    out = []
    w = out.append
    w("CastMirror soak run summary")
    w("=" * 60)
    w("run_id        : %s" % record["run_id"])
    w("synthetic     : %s" % ("YES - NOT REAL HARDWARE" if record["synthetic"] else "no"))
    w("recorded_at   : %s" % record["recorded_at"])
    w("host          : %s" % record["host"])
    w("")
    d = record["device"]
    w("device.model  : %s" % (d["model"] or "not measured"))
    w("device.md     : %s" % (d["md"] or "not measured"))
    w("device.fw     : %s" % (d["firmware"] or "not measured"))
    w("device.app_id : %s" % (d["app_id"] or "not measured"))
    w("device.target : %s" % (d["target"] or "not measured"))
    l = record["link"]
    w("link.band     : %s" % (l["wifi_band"] or "not measured"))
    w("link.distance : %s" % (l["approx_distance_m"] if l["approx_distance_m"] is not None else "not measured"))
    w("")
    s = record["session"]
    w("preset        : %s" % s["quality_preset"])
    w("target_bitrate: %s kbps (%s)" % (s["target_bitrate_kbps"], s["target_bitrate_kbps_source"]))
    w("target_delay  : %s ms (%s)" % (s["target_delay_ms"], s["target_delay_ms_source"]))
    w("capture_source: %s" % (s["capture_source"] or "not measured"))
    w("duration      : %s s" % (s["duration_s"] if s["duration_s"] is not None else "not measured"))
    w("ttff          : %s ms" % (s["time_to_first_frame_ms"] if s["time_to_first_frame_ms"] is not None else "not measured"))
    w("")
    lat = record["latency"]
    w("glass-to-glass: %s ms (+/- %s ms) via %s" % (
        lat["glass_to_glass_ms"] if lat["glass_to_glass_ms"] is not None else "not measured",
        lat["uncertainty_ms"] if lat["uncertainty_ms"] is not None else "?",
        lat["method"] or "not measured"))
    t = record["throughput"]
    w("bitrate peak  : %s kbps" % (t["peak_bitrate_kbps"] if t["peak_bitrate_kbps"] is not None else "not measured"))
    w("bitrate avg   : %s kbps" % (t["average_bitrate_kbps"] if t["average_bitrate_kbps"] is not None else "not measured"))
    q = record["quality"]
    w("loss peak/avg : %s / %s" % (q["packet_loss_fraction_peak"], q["packet_loss_fraction_mean"]))
    w("nack / pli    : %s / %s" % (record["feedback"]["nack_count"], record["feedback"]["pli_count"]))
    w("")
    w("adaptive rung transitions (%d):" % len(record["adaptive_rung_transitions"]))
    for r in record["adaptive_rung_transitions"]:
        w("  t=%-8s %-20s -> rung %s @ %s kbps %s" % (
            r["t_ms"], r["kind"], r["to_rung"], r["bitrate_kbps"], r["detail"]))
    w("disconnects/reconnects (%d):" % len(record["disconnects"]))
    for r in record["disconnects"]:
        w("  t=%-8s %-18s recovered=%s  %s" % (
            r["t_ms"], r["kind"], r["recovered"], r["reason"]))
    w("")
    w("NOT MEASURED (%d fields): %s" % (
        len(record["not_measured"]), ", ".join(record["not_measured"]) or "(none)"))
    w("")
    w("Reminder: glass-to-glass latency is NOT measured by CastMirror; it requires")
    w("an external camera. Any null above means 'not measured', never zero.")
    return "\n".join(out) + "\n"


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("parse")
    p.add_argument("--log", default=None)
    p.add_argument("--samples", default=None)
    p.add_argument("--meta", default=None)
    p.add_argument("--out", required=True)
    p.add_argument("--summary", default=None)
    p.add_argument("--run-id", dest="run_id", default=None)
    p.add_argument("--synthetic", action="store_true")
    p.add_argument("--model", default=None)
    p.add_argument("--md", default=None)
    p.add_argument("--firmware", default=None)
    p.add_argument("--band", default=None)
    p.add_argument("--distance", default=None)
    p.add_argument("--preset", default=None)
    p.add_argument("--bitrate", default=None)
    p.add_argument("--delay", default=None)
    p.add_argument("--capture", default=None)
    p.add_argument("--duration", default=None)
    p.add_argument("--device", default=None)
    p.add_argument("--g2g-ms", dest="g2g_ms", default=None)
    p.add_argument("--g2g-method", dest="g2g_method", default=None)
    p.add_argument("--g2g-uncertainty-ms", dest="g2g_uncertainty_ms", default=None)
    p.add_argument("--app-id", dest="app_id", default=None)
    p.add_argument("--notes", default="")
    p.add_argument("--git-rev", dest="git_rev", default=None)
    p.add_argument("--tool-version", dest="tool_version", default=None)

    args = ap.parse_args(argv)
    record = build_record(args)

    with open(args.out, "w", encoding="utf-8") as fh:
        json.dump(record, fh, indent=2, sort_keys=False)
        fh.write("\n")
    if args.summary:
        with open(args.summary, "w", encoding="utf-8") as fh:
            fh.write(render_summary(record))
    sys.stderr.write("[soak_parse] wrote %s (synthetic=%s, not_measured=%d)\n" % (
        args.out, record["synthetic"], len(record["not_measured"])))
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
