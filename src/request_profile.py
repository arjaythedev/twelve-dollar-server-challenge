"""Classify Python 3.15 collapsed stack samples from the unchanged request path."""

import re
from collections import Counter
from pathlib import Path
from typing import TypedDict

STAGES = {
    "database": "SQL + DB wrapper",
    "row_mapping": "Row materialization",
    "response_json": "Response JSON",
    "auth": "Auth (incl. JWT JSON)",
    "request_json": "Request-body JSON",
    "gc": "Garbage collection",
    "request_other": "Routing / response / other",
}


class HotFrame(TypedDict):
    frame: str
    samples: int


class ModeProfile(TypedDict):
    mode: str
    total_thread_samples: int
    request_samples: int
    outside_request_samples: int
    stages: dict[str, int]
    database_methods: dict[str, int]
    top_frames: list[HotFrame]
    outside_top_frames: list[HotFrame]
    sampling_attempts: int
    sampling_rate_hz: float
    read_errors_percent: float


def stage_for(frames: list[str]) -> str | None:
    if not any(frame.startswith("app.py:Application.__call__:") for frame in frames):
        return None
    if "<GC>" in frames:
        return "gc"
    if any(frame.startswith("auth.py:authenticate:") for frame in frames):
        return "auth"
    if any(frame.startswith("codec.py:encode_json:") for frame in frames):
        return "response_json"
    if any(frame.startswith("codec.py:decode_json:") for frame in frames):
        return "request_json"
    if any(frame.startswith("db.py:_post:") for frame in frames):
        return "row_mapping"
    if any(frame.startswith("db.py:Database.") for frame in frames):
        return "database"
    return "request_other"


def analyze(output: Path, mode: str) -> ModeProfile:
    stages: Counter[str] = Counter({stage: 0 for stage in STAGES})
    methods: Counter[str] = Counter()
    leaves: Counter[str] = Counter()
    outside_leaves: Counter[str] = Counter()
    total = 0
    outside = 0
    with (output / f"{mode}.collapsed").open(encoding="utf-8") as source:
        for line in source:
            stack, count_text = line.rstrip().rsplit(" ", 1)
            count = int(count_text)
            if count <= 0:
                raise ValueError("sample counts must be positive")
            frames = stack.split(";")
            leaf = next(
                (
                    frame
                    for frame in reversed(frames)
                    if frame != "<native>" and not frame.startswith("tid:")
                ),
                "<native>",
            )
            total += count
            stage = stage_for(frames)
            if stage is None:
                outside += count
                outside_leaves[leaf] += count
                continue
            stages[stage] += count
            leaves[leaf] += count
            database = next(
                (
                    frame.rsplit(":", 1)[0]
                    for frame in frames
                    if frame.startswith("db.py:Database.")
                ),
                None,
            )
            if database is not None:
                methods[database] += count
    log = (output / f"{mode}-sampler.log").read_text(encoding="utf-8")

    def statistic(pattern: str) -> float:
        match = re.search(pattern, log)
        if match is None:
            raise ValueError(f"missing sampler statistic: {pattern}")
        return float(match[1])

    return ModeProfile(
        mode=mode,
        total_thread_samples=total,
        request_samples=total - outside,
        outside_request_samples=outside,
        stages=dict(stages),
        database_methods=dict(methods),
        top_frames=[
            HotFrame(frame=frame, samples=count) for frame, count in leaves.most_common(15)
        ],
        outside_top_frames=[
            HotFrame(frame=frame, samples=count) for frame, count in outside_leaves.most_common(10)
        ],
        sampling_attempts=int(statistic(r"Captured (\d+) samples")),
        sampling_rate_hz=statistic(r"Sample rate: ([\d.]+)"),
        read_errors_percent=statistic(r"Error rate: ([\d.]+)"),
    )


def display(profiles: list[ModeProfile]) -> None:
    for profile in profiles:
        print(
            f"{profile['mode']}: {profile['request_samples']} request samples / "
            f"{profile['total_thread_samples']} valid thread samples; "
            f"{profile['read_errors_percent']:.2f}% sampler read errors"
        )
    print("Stage                          CPU %   Wall %  (sampled Python request stacks)")
    for stage, label in STAGES.items():
        values = [
            100 * profile["stages"][stage] / profile["request_samples"]
            if profile["request_samples"]
            else 0.0
            for profile in profiles
        ]
        print(f"{label:28} " + "  ".join(f"{value:6.2f}" for value in values))
