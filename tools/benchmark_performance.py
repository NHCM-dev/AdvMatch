"""Repeat audited native/Java benchmarks and save raw output plus medians.

Build once before running. No dependency installation or Git mutation is done.
"""
from pathlib import Path
import argparse
import csv
import json
import os
import platform
import re
import statistics
import subprocess
import time

ROOT = Path(__file__).resolve().parents[1]


def runCase(name, command, metric, rounds, baselineCommand=None):
    results = []
    baselineResults = []
    def execute(candidate, label, index):
        process = subprocess.run(candidate, cwd=ROOT, capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=600)
        output = process.stdout + process.stderr
        if process.returncode != 0:
            raise RuntimeError(f"{name} failed ({process.returncode}):\n{output}")
        values = dict(re.findall(r"([A-Za-z][A-Za-z_0-9]*)=([^\s]+)", output))
        if metric not in values or "validation=passed" not in output:
            raise RuntimeError(f"Missing audited performance output for {name}:\n{output}")
        print(f"{label} round={index + 1} {metric}={values[metric]}", flush=True)
        return {"metrics": values, "output": output}
    for index in range(rounds):
        # Alternate which binary/JAR runs first to reduce one-way clock/thermal
        # drift. Every run uses a fresh process and performs its own audit.
        if baselineCommand is not None and index % 2 == 0:
            baselineResults.append(execute(baselineCommand, name + "_baseline", index))
        results.append(execute(command, name, index))
        if baselineCommand is not None and index % 2 != 0:
            baselineResults.append(execute(baselineCommand, name + "_baseline", index))
    def summarize(candidate, runs):
        rates = [float(result["metrics"][metric]) for result in runs]
        return {"command": candidate, "metric": metric, "median": statistics.median(rates), "minimum": min(rates), "maximum": max(rates), "runs": runs}
    result = summarize(command, results)
    if baselineResults:
        result["baseline"] = summarize(baselineCommand, baselineResults)
        result["change_percent"] = (result["median"] / result["baseline"]["median"] - 1) * 100 if result["baseline"]["median"] else None
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", default="build/release")
    parser.add_argument("--rounds", type=int, default=3)
    parser.add_argument("--orders", type=int, default=1_000_000)
    parser.add_argument("--perpetual-orders", type=int, default=100_000)
    parser.add_argument("--producers", type=int, default=4)
    parser.add_argument("--batch-size", type=int, default=1024)
    parser.add_argument("--ring-size", type=int, default=4096)
    parser.add_argument("--busy-spin", action="store_true")
    parser.add_argument("--java", action="store_true")
    parser.add_argument("--baseline-dir", help="Compare with saved native executables, alternating process order")
    parser.add_argument("--baseline-jar", help="Compare Java SDK JARs against the same current native DLL")
    parser.add_argument("--sweep", action="store_true", help="Sweep native spot producer/ring/batch combinations")
    parser.add_argument("--output", default="build/performance-results.json")
    options = parser.parse_args()
    if options.rounds < 1 or options.orders < 1 or options.perpetual_orders < 1:
        parser.error("rounds and order counts must be positive")
    build = (ROOT / options.build_dir).resolve()
    extension = ".exe" if os.name == "nt" else ""
    def binary(kind):
        path = build / ("advmatch_" + kind + "_benchmark" + extension)
        if not path.is_file():
            raise RuntimeError(f"Build the benchmark first: {path}")
        return str(path)
    common = ["--producers", str(options.producers), "--ring-size", str(options.ring_size), "--batch-size", str(options.batch_size)]
    spinning = ["--busy-spin"] if options.busy_spin else []
    report = {"recorded_at": time.strftime("%Y-%m-%dT%H:%M:%S%z"), "platform": platform.platform(), "processor": platform.processor(), "logical_threads": os.cpu_count(), "options": vars(options), "cases": {}}
    cache = build / "CMakeCache.txt"
    if cache.is_file():
        report["build_flags"] = [line for line in cache.read_text(encoding="utf-8").splitlines() if line.startswith(("CMAKE_BUILD_TYPE:", "ADVMATCH_ENABLE_IPO:", "ADVMATCH_NATIVE_ARCH:"))]
    cases = [
        ("cpp_spot", [binary("tps"), "--orders", str(options.orders), *common, *spinning], "tps"),
        ("cpp_perpetual", [binary("perpetual"), "--orders", str(options.perpetual_orders), "--warmup", "1000", *common, *spinning], "trade_tps"),
        ("cpp_liquidation", [binary("liquidation"), "--users", "20000", "--positions-per-user", "4", "--symbols", "8", "--waves", "8"], "total_liquidation_tps"),
        ("cpp_perpetual_under_liquidation", [binary("liquidation"), "--users", "20000", "--positions-per-user", "4", "--symbols", "8", "--waves", "8", "--trades-per-wave", "12500"], "ordinary_trade_tps_under_stress")
    ]
    if options.sweep:
        cases = [(f"cpp_spot_p{producers}_r{ring}_b{batch}", [binary("tps"), "--orders", str(options.orders), "--producers", str(producers), "--ring-size", str(ring), "--batch-size", str(batch), *spinning], "tps") for producers in (1, 2, 4, 8) for ring in (32, 1024, 4096) for batch in (256, 1024)]
    if options.java:
        jar = ROOT / "java/advmatch/build/libs/advmatch-1.1.0.jar"
        if not jar.is_file():
            raise RuntimeError("Build the Java JAR before measurement")
        java = str(Path(os.environ["JAVA_HOME"]) / "bin/java.exe") if os.name == "nt" and "JAVA_HOME" in os.environ else "java"
        javaCommand = [java, "-Xms256m", "-Xmx2g", f"-Dadvmatch.nativeDir={build}", "-cp", str(jar)]
        cases += [("java_spot", [*javaCommand, "nhcm.advmatch.benchmark.TpsBenchmark", "--orders", str(options.orders), *common, "--publish-batch", str(min(options.batch_size, 1024))], "trade_tps"),
                  ("java_perpetual", [*javaCommand, "nhcm.advmatch.benchmark.PerpetualBenchmark", "--orders", str(options.perpetual_orders), "--warmup", "1000", *common, "--publish-batch", str(min(options.batch_size, 1024))], "trade_tps")]
    output = (ROOT / options.output).resolve()
    output.parent.mkdir(parents=True, exist_ok=True)
    for name, command, metric in cases:
        baseline = None
        if options.baseline_dir and name.startswith("cpp_"):
            baselineBinary = (ROOT / options.baseline_dir / Path(command[0]).name).resolve()
            if not baselineBinary.is_file():
                raise RuntimeError(f"Missing baseline executable: {baselineBinary}")
            baseline = [str(baselineBinary), *command[1:]]
        elif options.baseline_jar and name.startswith("java_"):
            baselineJar = (ROOT / options.baseline_jar).resolve()
            if not baselineJar.is_file():
                raise RuntimeError(f"Missing baseline JAR: {baselineJar}")
            baseline = command.copy()
            baseline[baseline.index("-cp") + 1] = str(baselineJar)
        report["cases"][name] = runCase(name, command, metric, options.rounds, baseline)
        output.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    with output.with_suffix(".csv").open("w", newline="", encoding="utf-8") as stream:
        writer = csv.writer(stream)
        writer.writerow(["case", "metric", "median", "minimum", "maximum", "baseline_median", "change_percent"])
        for name, result in report["cases"].items():
            writer.writerow([name, result["metric"], result["median"], result["minimum"], result["maximum"], result.get("baseline", {}).get("median", ""), result.get("change_percent", "")])
    print(f"Saved {output}")


if __name__ == "__main__":
    main()
