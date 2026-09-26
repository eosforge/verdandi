"""离线读取已退出进程的 Astra 探针; 只用标准库, 不启动服务或构建."""

from __future__ import annotations

import argparse
from array import array
import hashlib
import json
import math
import mmap
import os
from pathlib import Path
import re
import struct

ROOT = Path(__file__).resolve().parents[1]
HEADER = struct.Struct("<16Q")
ENTRY = struct.Struct("<9QII")
MAGIC = 0x4153545241505246
MACRO = re.compile(r'ASTRA_PROFILE_(?:SCOPE|BEGIN|COUNT|CONSUME)\([^"\n]*"([^"\n]+)"')


def identify(name):
    """与 C++ consteval 相同的 FNV-1a, 只处理固定站点名."""
    value = 14695981039346656037
    for byte in name.encode("utf-8"):
        value = ((value ^ byte) * 1099511628211) & ((1 << 64) - 1)
    return value


def inventory(root):
    """静态扫描手写插桩位置并检查名称散列碰撞, 不执行任何 C++ 或测试."""
    result = {}
    for directory in ("common", "star", "pulsar", "comet/cpp"):
        for path in sorted((root / "astra" / directory).rglob("*")):
            if path.suffix not in {".hpp", ".cpp"} or "generated" in path.parts:
                continue
            data = path.read_bytes()
            digest = hashlib.sha256(data).hexdigest()
            for number, line in enumerate(data.decode("utf-8").splitlines(), 1):
                for match in MACRO.finditer(line):
                    name = match[1]
                    site = identify(name)
                    row = result.setdefault(site, {"name": name, "locations": []})
                    if row["name"] != name:
                        raise ValueError("Probe label hash collision")
                    row["locations"].append({"path": path.relative_to(root).as_posix(), "line": number, "sha256": digest})
    return result


def distribution(values):
    """报告采样观测的精确顺序统计, 不将低样本 P99.9 当总体尾延迟."""
    ordered = sorted(values)
    if not ordered:
        return None
    return {
        "count": len(ordered),
        "sum": sum(ordered),
        "mean": sum(ordered) / len(ordered),
        **{name: ordered[max(0, math.ceil(fraction * len(ordered)) - 1)] for name, fraction in (("p50", 0.5), ("p95", 0.95), ("p99", 0.99), ("p999", 0.999))},
        "max": ordered[-1],
        "tail_samples_sufficient": len(ordered) >= 10000,
    }


def analyze(path, sites, trace=None):
    """单文件 mmap 分析, 父记录按槽号直查, 无百万项 Python 父链字典. 进程必须已经退出."""
    groups = {}
    with path.open("rb") as stream:
        if path.stat().st_size < HEADER.size:
            raise ValueError(f"{path.name}: truncated header")
        with mmap.mmap(stream.fileno(), 0, access=mmap.ACCESS_READ) as data:
            magic, version, width, capacity, count, missed, errors, sample, cpu, *reserved = HEADER.unpack_from(data)
            if magic != MAGIC or version != 1 or width != ENTRY.size or any(reserved):
                raise ValueError(f"{path.name}: unsupported profile format")
            if (
                not capacity
                or capacity % 64
                or count % 64
                or capacity > (len(data) - HEADER.size) // ENTRY.size
                or not 1 <= sample <= 65536
                or cpu not in (0, 1)
            ):
                raise ValueError(f"{path.name}: invalid header bounds")
            spans = counters = intervals = holes = 0
            for slot in range(min(count, capacity)):
                entry = ENTRY.unpack_from(data, HEADER.size + slot * ENTRY.size)
                if not any(entry):
                    holes += 1
                    continue  # 线程最后一块未用槽, 也可能是异常终止时的未完成跨度; 不能据此宣称正常退出.
                start, elapsed, own, used, exclusive, number, parent, site, value, thread, flags = entry
                if site not in sites or not thread or flags not in (0, 1, 2, 4) or own > elapsed or exclusive > used:
                    raise ValueError(f"{path.name}: invalid record at slot {slot}")
                kind = "counter" if flags == 2 else "interval" if flags == 4 else "span"
                if kind == "span":
                    if number != slot + 1 or flags != cpu or value or (not cpu and (used or exclusive)):
                        raise ValueError(f"{path.name}: invalid span at slot {slot}")
                    spans += 1
                else:
                    if (
                        number
                        or used
                        or exclusive
                        or (kind == "counter" and (elapsed or own or not parent))
                        or (kind == "interval" and (parent or value or own != elapsed))
                    ):
                        raise ValueError(f"{path.name}: invalid event at slot {slot}")
                    counters += kind == "counter"
                    intervals += kind == "interval"
                parent_name = None
                if parent:
                    if parent > slot:
                        raise ValueError(f"{path.name}: invalid parent position")
                    ancestor = ENTRY.unpack_from(data, HEADER.size + (parent - 1) * ENTRY.size)
                    if (
                        ancestor[5] != parent
                        or ancestor[9] != thread
                        or ancestor[10] not in (0, 1)
                        or ancestor[0] > start
                        or start + elapsed > ancestor[0] + ancestor[1]
                        or ancestor[7] not in sites
                    ):
                        raise ValueError(f"{path.name}: missing or invalid parent for slot {slot}")
                    parent_name = sites[ancestor[7]]["name"]
                key = (site, parent_name, kind)
                group = groups.get(key)
                if group is None:
                    group = {"wall_ns": array("Q"), "own_ns": array("Q"), "cpu_ns": array("Q"), "cpu_own_ns": array("Q"), "values": array("Q")}
                    groups[key] = group
                if kind == "counter":
                    group["values"].append(value)
                else:
                    group["wall_ns"].append(elapsed)
                    group["own_ns"].append(own)
                    if kind == "span" and cpu:
                        group["cpu_ns"].append(used)
                        group["cpu_own_ns"].append(exclusive)
                if trace:
                    trace(
                        {
                            "name": sites[site]["name"],
                            "cat": kind,
                            "ph": "i" if kind == "counter" else "X",
                            "pid": path.stem,
                            "tid": thread,
                            "ts": start / 1000,
                            "dur": elapsed / 1000,
                            "s": "t",
                            "args": {"id": number, "parent": parent, "value": value},
                        }
                    )
    rows = []
    for (site, parent, kind), values in groups.items():
        rows.append(
            {"name": sites[site]["name"], "parent": parent, "kind": kind, **{metric: distribution(samples) for metric, samples in values.items() if samples}}
        )
    return {
        "file": path.name,
        "sample": sample,
        "cpu": bool(cpu),
        "reserved": count,
        "capacity": capacity,
        "unused_slots": holes,
        "missed": missed,
        "errors": errors,
        "spans": spans,
        "counters": counters,
        "intervals": intervals,
        "complete": not (missed or errors or count > capacity),
        "groups": sorted(rows, key=lambda row: row.get("own_ns", {}).get("sum", 0), reverse=True),
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path, nargs="?")
    parser.add_argument("--source", type=Path, default=ROOT)
    parser.add_argument("--inventory", type=Path, help="只输出静态站点目录, 不运行测试")
    parser.add_argument("--output", type=Path)
    parser.add_argument("--trace", type=Path, help="可选 Chrome Trace JSON, 可能大于原二进制")
    options = parser.parse_args()
    sites = inventory(options.source.resolve())
    if options.inventory:
        options.inventory.write_text(json.dumps(sites, ensure_ascii=False, indent=2), encoding="utf-8")
    if options.directory is None:
        if not options.inventory:
            parser.error("directory or --inventory is required")
        return 0
    if not options.output:
        parser.error("--output is required with directory")
    paths = sorted(options.directory.glob("*.profile"))
    if not paths:
        parser.error("No profile files; initialization or collection was not successful")
    # 输出流逐项写, 不在内存构建另一份完整时间线. 离线导出不影响被测服务.
    reports = []
    with options.trace.open("w", encoding="utf-8") if options.trace else open(os.devnull, "w", encoding="utf-8") as output:
        output.write('{"traceEvents":[')
        first = True

        def emit(record):
            nonlocal first
            output.write(("" if first else ",") + json.dumps(record, ensure_ascii=False))
            first = False

        for path in paths:
            reports.append(analyze(path, sites, emit if options.trace else None))
        output.write("]}")
    report = {
        "scope": "sampled synchronous spans and separate asynchronous intervals; not request end-to-end latency",
        "requires_successful_process_exit": True,
        "sites": sites,
        "processes": reports,
    }
    options.output.write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")
    print(f"Analyzed {len(reports)} process files; complete={all(item['complete'] for item in reports)}")
    return 0 if all(item["complete"] for item in reports) else 1


if __name__ == "__main__":
    raise SystemExit(main())
