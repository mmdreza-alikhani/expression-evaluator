#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Run the benchmark for all three implementations and record real numbers.

Every implementation runs its own 10 timed iterations (tokenize + convert +
evaluate) and prints avg/min/max. This script measures the peak working set of
each process from the outside and writes bench/results.json + bench/results.md.
"""

from __future__ import annotations

import json
import re
import shutil
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
DATA = HERE / "data"

PROCESS_QUERY_INFORMATION = 0x0400
PROCESS_VM_READ = 0x0010


# --------------------------------------------------------------- peak memory
def _windows_memory_api():
    import ctypes
    from ctypes import wintypes

    class PROCESS_MEMORY_COUNTERS(ctypes.Structure):
        _fields_ = [
            ("cb", wintypes.DWORD),
            ("PageFaultCount", wintypes.DWORD),
            ("PeakWorkingSetSize", ctypes.c_size_t),
            ("WorkingSetSize", ctypes.c_size_t),
            ("QuotaPeakPagedPoolUsage", ctypes.c_size_t),
            ("QuotaPagedPoolUsage", ctypes.c_size_t),
            ("QuotaPeakNonPagedPoolUsage", ctypes.c_size_t),
            ("QuotaNonPagedPoolUsage", ctypes.c_size_t),
            ("PagefileUsage", ctypes.c_size_t),
            ("PeakPagefileUsage", ctypes.c_size_t),
        ]

    psapi = ctypes.WinDLL("psapi", use_last_error=True)
    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    psapi.GetProcessMemoryInfo.argtypes = [
        wintypes.HANDLE,
        ctypes.POINTER(PROCESS_MEMORY_COUNTERS),
        wintypes.DWORD,
    ]
    kernel32.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
    kernel32.OpenProcess.restype = wintypes.HANDLE
    kernel32.CloseHandle.argtypes = [wintypes.HANDLE]

    return ctypes, psapi, kernel32, PROCESS_MEMORY_COUNTERS


IS_WINDOWS = sys.platform == "win32"
if IS_WINDOWS:
    ctypes, psapi, kernel32, PMC = _windows_memory_api()
else:
    USR_BIN_TIME = shutil.which("/usr/bin/time") or (
        "/usr/bin/time" if Path("/usr/bin/time").exists() else None
    )


def run_process(cmd):
    """Run cmd, return (stdout, peak_bytes, elapsed_wall_ms)."""
    if IS_WINDOWS:
        proc = subprocess.Popen(
            cmd, cwd=str(ROOT), stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True
        )
        handle = kernel32.OpenProcess(
            PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, False, proc.pid
        )
        peak = 0
        while proc.poll() is None:
            if handle:
                counters = PMC()
                counters.cb = ctypes.sizeof(counters)
                if psapi.GetProcessMemoryInfo(handle, ctypes.byref(counters), counters.cb):
                    peak = max(peak, counters.PeakWorkingSetSize)
            time.sleep(0.002)
        out, err = proc.communicate()
        if handle:
            counters = PMC()
            counters.cb = ctypes.sizeof(counters)
            if psapi.GetProcessMemoryInfo(handle, ctypes.byref(counters), counters.cb):
                peak = max(peak, counters.PeakWorkingSetSize)
            kernel32.CloseHandle(handle)
        if proc.returncode != 0:
            raise RuntimeError("%s failed (%d): %s" % (cmd, proc.returncode, err.strip()))
        if not peak:
            peak = 0
        return out, peak, 0.0

    # POSIX: /usr/bin/time -v reports "Maximum resident set size (kbytes)"
    if USR_BIN_TIME is None:
        proc = subprocess.run(cmd, cwd=str(ROOT), capture_output=True, text=True)
        if proc.returncode != 0:
            raise RuntimeError("%s failed: %s" % (cmd, proc.stderr))
        return proc.stdout, 0, 0.0
    proc = subprocess.run(
        [USR_BIN_TIME, "-v"] + list(cmd), cwd=str(ROOT), capture_output=True, text=True
    )
    if proc.returncode != 0:
        raise RuntimeError("%s failed: %s" % (cmd, proc.stderr))
    m = re.search(r"Maximum resident set size \(kbytes\):\s*(\d+)", proc.stderr)
    peak = int(m.group(1)) * 1024 if m else 0
    return proc.stdout, peak, 0.0


# --------------------------------------------------------------- benchmarks
IMPLS = {
    "Python": lambda f: [sys.executable, str(ROOT / "python" / "evaluator.py"), "--bench", str(f)],
    "Go": lambda f: [str(ROOT / "bin" / ("evaluator-go.exe" if IS_WINDOWS else "evaluator-go")), "-bench", str(f)],
    "C": lambda f: [str(ROOT / "bin" / ("evaluator.exe" if IS_WINDOWS else "evaluator")), "--bench", str(f)],
}

FIELD = re.compile(
    r"bench lang=(\w+) tokens=(\d+) avg_ms=([\d.]+) min_ms=([\d.]+) max_ms=([\d.]+) result=(\S+)"
)


def main() -> int:
    if not (DATA / "expr_100k.txt").exists() or not (DATA / "expr_1m.txt").exists():
        subprocess.run([sys.executable, str(HERE / "gen_cases.py")], check=True)

    sizes = ["expr_100k.txt", "expr_1m.txt"]
    rows = []
    for name in sizes:
        path = DATA / name
        for lang, build_cmd in IMPLS.items():
            cmd = build_cmd(path)
            print("running %-7s %-14s ..." % (lang, name), flush=True)
            out, peak, _ = run_process(cmd)
            m = FIELD.search(out)
            if not m:
                raise RuntimeError("unexpected output from %s:\n%s" % (lang, out))
            rows.append(
                {
                    "lang": m.group(1),
                    "input": name,
                    "tokens": int(m.group(2)),
                    "avg_ms": float(m.group(3)),
                    "min_ms": float(m.group(4)),
                    "max_ms": float(m.group(5)),
                    "result": m.group(6),
                    "peak_mb": round(peak / (1024.0 * 1024.0), 1) if peak else None,
                }
            )

    (HERE / "results.json").write_text(json.dumps(rows, indent=2), encoding="utf-8")

    lines = [
        "| Language | Input | Tokens | Avg (ms) | Min (ms) | Max (ms) | Peak RSS (MB) |",
        "|---|---|---:|---:|---:|---:|---:|",
    ]
    for r in rows:
        peak = "-" if r["peak_mb"] is None else "%.1f" % r["peak_mb"]
        lines.append(
            "| %s | %s | %s | %.3f | %.3f | %.3f | %s |"
            % (
                r["lang"],
                r["input"].replace("expr_", "").replace(".txt", ""),
                f"{r['tokens']:,}",
                r["avg_ms"],
                r["min_ms"],
                r["max_ms"],
                peak,
            )
        )
    table = "\n".join(lines)
    (HERE / "results.md").write_text(table + "\n", encoding="utf-8")
    print()
    print(table)

    # cross-check that all three agree on the value
    for name in sizes:
        values = [r["result"] for r in rows if r["input"] == name]
        if len(set(values)) != 1:
            print("WARNING: results differ for %s: %s" % (name, values))
        else:
            print("%s: all implementations agree on result %s" % (name, values[0]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
