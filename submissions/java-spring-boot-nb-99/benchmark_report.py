"""Render benchmark results without a browser, JavaScript, or external packages."""

import html
import json
from pathlib import Path


def write_text(path, text):
    """Readers see either the previous complete report or the new one."""
    path = Path(path)
    temporary = path.with_suffix(path.suffix + ".tmp")
    temporary.write_text(text, encoding="utf-8")
    temporary.replace(path)


def format_number(value, digits=2):
    return "n/a" if value is None else f"{value:,.{digits}f}"


def chart(runs, title, series, threshold=None):
    points = [run for run in runs if run.get("metrics") and run["phase"] != "warmup"]
    escape = html.escape
    width, height = 760, 280
    left, right, top, bottom = 75, 25, 30, 60
    values = [run["metrics"][key] for run in points for key, _, _ in series]
    ymax = max(values + ([threshold] if threshold is not None else []) + [1]) * 1.15
    xmax = max([run["vus"] for run in points] + [1])

    def x(vus):
        return left + vus / xmax * (width - left - right)

    def y(value):
        return height - bottom - value / ymax * (height - top - bottom)

    parts = [
        f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {width} {height}" role="img" aria-label="{escape(title)}">',
        f"<title>{escape(title)}</title>",
        '<rect width="100%" height="100%" fill="#ffffff"/>',
        '<g font-family="sans-serif" font-size="12" fill="#334155">',
    ]
    for i in range(5):
        value = ymax * i / 4
        parts += [
            f'<path d="M {left} {y(value):.1f} H {width - right}" stroke="#dbe3ec"/>',
            f'<text x="{left - 10}" y="{y(value) + 4:.1f}" text-anchor="end">{value:.2f}</text>',
            f'<text x="{x(xmax * i / 4):.1f}" y="{height - bottom + 22}" text-anchor="middle">{xmax * i / 4:.0f}</text>',
        ]
    if threshold is not None:
        parts += [
            f'<path d="M {left} {y(threshold):.1f} H {width - right}" stroke="#b42318" stroke-dasharray="6 4"/>',
            f'<text x="{width - right}" y="{y(threshold) - 6:.1f}" text-anchor="end">Limit {threshold:g}</text>',
        ]
    for index, (key, label, color) in enumerate(series):
        ordered = sorted(points, key=lambda run: (run["vus"], run["id"]))
        for run in ordered:
            description = f"Run {run['id']}, {run['phase']}, {run['vus']} users: {label} {run['metrics'][key]:.2f}, {run['status']}"
            fill = "white" if run["phase"] == "confirm" else color
            stroke = color if run["status"] == "passed" else "#b42318"
            parts.append(
                f'<circle cx="{x(run["vus"]):.1f}" cy="{y(run["metrics"][key]):.1f}" r="4" fill="{fill}" stroke="{stroke}" stroke-width="2"><title>{escape(description)}</title></circle>'
            )
        parts.append(
            f'<text x="{left + index * 220}" y="{height - 9}" fill="{color}">{escape(label)}</text>'
        )
    parts.append(
        f'<text x="{left + 250}" y="{height - 9}">Filled: probe. Outlined: confirmation.</text>'
        f'<text x="{width - right}" y="{height - 30}" text-anchor="end">Virtual users</text></g></svg>'
    )
    return "\n".join(parts)


def report(directory, state):
    directory = Path(directory)
    write_text(directory / "results.json", json.dumps(state, indent=2) + "\n")
    runs = state["runs"]
    charts = [
        (
            "p95.svg",
            "p95 latency, milliseconds",
            [("p95_ms", "p95, ms", "#175cd3")],
            500,
        ),
        (
            "p99.svg",
            "p99 latency, milliseconds",
            [("p99_ms", "p99, ms", "#6941c6")],
            1000,
        ),
        (
            "throughput.svg",
            "Average requests per second",
            [("rps", "Requests/s", "#027a48")],
            None,
        ),
        (
            "errors.svg",
            "Failed requests, percent",
            [("error_percent", "Failed requests, %", "#b42318")],
            1,
        ),
    ]
    svgs = {
        filename: chart(runs, title, series, threshold)
        for filename, title, series, threshold in charts
    }
    for filename, svg in svgs.items():
        write_text(directory / filename, svg)
    confirmed = state.get("confirmed_vus")
    short_confirmation = (
        state["environment"].get("settings", {}).get("confirm_seconds", 300) < 300
    )
    custom_ramp = state["environment"].get("settings", {}).get("ramp_seconds", 60) != 60
    label = (
        "Smoke-test baseline"
        if short_confirmation
        else ("Custom-ramp baseline" if custom_ramp else "Confirmed local baseline")
    )
    result = (
        f"{label}: {confirmed:,} virtual users{' or more, a lower bound' if state['status'] == 'capped' else ''}."
        if confirmed
        else "No confirmed result."
    )
    notes = [
        "Local Compose benchmark, not an official challenge score. k6 and Nginx share the host with the app.",
        "Latency and throughput include ramp-up, hold, ramp-down, and graceful completion. Charts exclude warm-up.",
        "Chart points are not joined: probes and confirmations have different hold durations. Outlined points are confirmations.",
        "One fresh seeded database per search. Warm-up and later runs share that instance, so writes accumulate.",
        "Benchmark status uses the challenge thresholds. Endpoint check failures are reported separately; unexercised checks are not passes.",
        "Memory peak is cumulative for the app instance. CPU is the app's average over each complete run, not a sampled peak.",
    ]
    headers = [
        "Run",
        "Phase",
        "Users",
        "Hold",
        "Result",
        "Requests",
        "Req/s",
        "p95 ms",
        "p99 ms",
        "Errors %",
        "Checks",
    ]
    rows = []
    for run in runs:
        metrics = run.get("metrics", {})
        checks = run.get("checks", {})
        check_result = (
            "not available"
            if not checks
            else (
                "failed"
                if any(c["failed"] for c in checks.values())
                else (
                    "not fully exercised"
                    if any(c["passed"] == 0 for c in checks.values())
                    else "passed"
                )
            )
        )
        rows.append(
            [
                str(run["id"]),
                run["phase"],
                str(run["vus"]),
                f"{run['hold_seconds']}s",
                run["status"],
                str(metrics.get("requests", "n/a")),
                format_number(metrics.get("rps")),
                format_number(metrics.get("p95_ms")),
                format_number(metrics.get("p99_ms")),
                format_number(metrics.get("error_percent"), 3),
                check_result,
            ]
        )
    md = [
        "# Java / SQLite benchmark",
        "",
        result,
        "",
        f"Search status: {state['status']}.",
        "",
        state.get("message", ""),
        "",
        "## Run overview",
        "",
        "| " + " | ".join(headers) + " |",
        "| " + " | ".join(["---"] * len(headers)) + " |",
    ]
    md.extend("| " + " | ".join(row) + " |" for row in rows)
    escape = html.escape
    html_rows = "".join(
        "<tr>" + "".join(f"<td>{escape(cell)}</td>" for cell in row) + "</tr>"
        for row in rows
    )
    details = []
    for run in runs:
        heading = f"Run {run['id']}: {run['phase']}, {run['vus']:,} users"
        outcome = f"Outcome: {run['status']}. Exit code: {run.get('exit_code', 'n/a')}."
        timing = f"Started: {run['started_at']}. Wall time: {format_number(run.get('wall_seconds'))} seconds."
        links = [
            (label, run[key])
            for label, key in (
                ("Console log", "log"),
                ("Raw k6 summary", "summary"),
                ("Structured k6 events", "events"),
            )
            if run.get(key)
        ]
        resource = run.get("resources", {})
        usage = (
            (
                f"App average CPU: {format_number(resource.get('cpu_percent'))}% of one core. "
                f"Instance cgroup memory peak, including page cache: {format_number(resource.get('peak_memory_mib'))} MiB. "
                f"Instance peak swap: {format_number(resource.get('swap_peak_mib'))} MiB. "
                f"OOM kills: {resource.get('oom_kills', 'n/a')}. Restart count: {resource.get('restart_count', 'n/a')}."
            )
            if resource
            else ""
        )
        metrics = run.get("metrics", {})
        latency = (
            f"Average latency: {format_number(metrics.get('avg_ms'))} ms. Maximum latency: {format_number(metrics.get('max_ms'))} ms."
            if metrics
            else ""
        )
        md += [
            "",
            f"## {heading}",
            "",
            outcome,
            "",
            timing,
            "",
        ]
        table = ["| Condition | Result |", "| --- | --- |"]
        condition_rows = []
        for label, passed in run.get("thresholds", {}).items():
            condition_rows.append((label, "passed" if passed else "failed"))
        for label, counts in run.get("checks", {}).items():
            status = (
                "failed"
                if counts["failed"]
                else ("passed" if counts["passed"] else "not exercised")
            )
            condition_rows.append(
                (
                    label,
                    f"{status}: {counts['passed']:,} passed, {counts['failed']:,} failed",
                )
            )
        table.extend(f"| {label} | {value} |" for label, value in condition_rows)
        md += table + [""] + [f"[{label}]({href})" for label, href in links]
        error = "Error: " + run["error"].replace("\n", " ") if run.get("error") else ""
        for paragraph in (error, latency, usage):
            if paragraph:
                md += ["", paragraph]
        details.append(
            f"<section><h3>{escape(heading)}</h3><p>{escape(outcome)}</p><p>{escape(timing)}</p><ul>"
            + "".join(
                f"<li>{escape(label)}: {escape(value)}</li>"
                for label, value in condition_rows
            )
            + "</ul><p>"
            + " / ".join(
                f'<a href="{escape(href, quote=True)}">{escape(label)}</a>'
                for label, href in links
            )
            + "</p>"
            + "".join(
                f"<p>{escape(paragraph)}</p>"
                for paragraph in (error, latency, usage)
                if paragraph
            )
            + "</section>"
        )
    confirmations = [
        r
        for r in runs
        if r["phase"] == "confirm" and r["vus"] == confirmed and r["status"] == "passed"
    ]
    spread = ""
    if confirmations:
        values = [r["metrics"]["p95_ms"] for r in confirmations]
        spread = f"Successful same-instance, sequential confirmations: {len(values)}. p95 range: {min(values):.2f} to {max(values):.2f} ms. This is not fresh-instance variability."
        if len(values) == 1:
            spread += " A single confirmation does not measure run-to-run variability."
        md += ["", "## Confirmation spread", "", spread]
    md += ["", "## Charts", ""]
    md.extend(f"![{title}]({filename})\n" for filename, title, _, _ in charts)
    md += [
        "## Environment and settings",
        "",
        "```json",
        json.dumps(state["environment"], indent=2),
        "```",
        "",
        "## Interpretation",
        "",
    ]
    md.extend("- " + note for note in notes)
    write_text(directory / "report.md", "\n".join(md) + "\n")
    plots = "".join(
        f"<figure><figcaption>{escape(title)}</figcaption>{svgs[filename]}</figure>"
        for filename, title, _, _ in charts
    )
    page = f"""<!doctype html>
<html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width, initial-scale=1"><title>Java / SQLite benchmark</title>
<style>
:root {{ color-scheme: light; color: #24344b; background: #edf2f8; font-family: "Segoe UI", "Helvetica Neue", sans-serif; }}
body {{ margin: 0; }} main {{ max-width: 1200px; margin: auto; padding: 32px 24px 64px; }}
h1 {{ font-size: clamp(1.7rem, 4vw, 2.7rem); font-weight: 650; margin-bottom: 12px; }}
h2 {{ margin-top: 36px; }} p, li {{ max-width: 78ch; line-height: 1.6; }}
header {{ border-left: 6px solid #175cd3; padding-left: 20px; }}
a {{ color: #175cd3; }} a:focus-visible {{ outline: 3px solid #6941c6; outline-offset: 4px; }}
.table-scroll {{ overflow-x: auto; }} table {{ width: 100%; border-collapse: collapse; background: white; font-variant-numeric: tabular-nums; }}
th, td {{ text-align: left; padding: 12px 10px; border-bottom: 1px solid #dbe3ec; white-space: nowrap; }}
th {{ background: #dbe7f5; }} .plots {{ display: grid; grid-template-columns: repeat(2, minmax(0, 1fr)); gap: 20px; }}
figure {{ margin: 0; background: white; padding: 16px; }} figcaption {{ font-weight: 600; margin-bottom: 10px; }} svg {{ display: block; width: 100%; }}
section {{ border-bottom: 1px solid #cbd5e1; padding: 12px 0; }} pre {{ overflow: auto; background: white; padding: 20px; }}
@media(max-width: 720px) {{ main {{ padding: 20px 12px; }} .plots {{ grid-template-columns: 1fr; }} }}
</style></head><body><main><header><h1>Java / SQLite benchmark</h1><p>{escape(result)}</p><p>Search status: {escape(state["status"])}. {escape(state.get("message", ""))}</p></header>
<h2>Run overview</h2><div class="table-scroll"><table><thead><tr>{"".join(f'<th scope="col">{escape(h)}</th>' for h in headers)}</tr></thead><tbody>{html_rows}</tbody></table></div>
<h2>Load and response time</h2><div class="plots">{plots}</div><p>{escape(spread)}</p>
<h2>Thresholds and endpoint checks</h2>{"".join(details)}
<h2>Environment and settings</h2><pre>{escape(json.dumps(state["environment"], indent=2))}</pre>
<h2>Interpretation</h2><ul>{"".join(f"<li>{escape(note)}</li>" for note in notes)}</ul>
</main></body></html>"""
    write_text(directory / "report.html", page)
