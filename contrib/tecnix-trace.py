#!/usr/bin/env nix-shell
#!nix-shell -i python3 -p python3 --pure

# Convert a Tecnix evaluation trace (`--option tecnix-trace <dir>`, see
# plans/tecnix-tracing/usage.md) into a visualisation format. Reads the SQLite
# file with the standard library only; nothing here is part of Nix.
#
#   ./contrib/tecnix-trace.py speedscope trace.sqlite -o trace.speedscope.json
#       One "evented" profile per evaluation thread (or per root with
#       --split root): open/close events straight from the records. Load at
#       https://www.speedscope.app (client side) or `nix-shell -p speedscope
#       --run "speedscope trace.speedscope.json"`. Time order, left-heavy and
#       sandwich views; the sandwich view is "who forced this position".
#
#   ./contrib/tecnix-trace.py perfetto trace.sqlite -o trace.json
#       Chrome trace-event JSON: a track per evaluation thread, roots and
#       waits as slices, garbage-collection pauses on their own track. Load
#       at https://ui.perfetto.dev or chrome://tracing. Best for parallel runs.
#
#   ./contrib/tecnix-trace.py folded trace.sqlite [--weight self_dur|self_gc_alloc] [--shared] > trace.folded
#       Collapsed stacks for `flamegraph.pl` / `inferno-flamegraph` /
#       speedscope's importer, weighted by self time (ns) or self GC bytes.
#       With --shared, every subtree rooted at a record needed by more than
#       one root is moved under a top-level `[shared]` frame, so widths are
#       causal between targets instead of first-payer (README §4.4).
#
#   ./contrib/tecnix-trace.py dot trace.sqlite [--top N] [--max-consumers K] [--weight ...] | dot -Tsvg > trace.svg
#       The attribution graph, at target level only: one node per target
#       (its own weight, its amortised share of shared work, its fetch and
#       store I/O -- amortised the same way), an edge between two targets weighted by the shared work
#       they both need, and one "infrastructure" node for work needed by more
#       than K targets (default 50), with each target's share of it on the
#       edge. --top N keeps the N targets with the largest own weight (and
#       their partners above --min-edge); --root picks targets by name.
#       Everything is computed in SQL from the file's views; see below for
#       large traces.
#
# Traces of 10^8 records are beyond sqlite3's recursive views; load the tables
# into DuckDB (`ATTACH 'trace.sqlite' (TYPE sqlite)`, copy `records`, `reuses`,
# `roots`, `exprs`) and run the views' SQL there (printed by `--print-sql`).
#
# Common options: --root NAME (restrict to records of one root; repeatable),
# --mark-shared (prefix shared records' names with `[shared] `).
#
# Times are nanoseconds from the start of the run with GC pauses already
# excluded, so a child's end can extend past its parent's by the pauses that
# fell in the parent alone; the tree is used for nesting and ends are clamped
# so every viewer sees well-formed stacks. Traces of 10^7 records and more are
# for the SQL views, not for a browser.

import argparse
import json
import sqlite3
import sys
from collections import defaultdict

_db = None  # the open trace, for commands that use its views


def load(path, roots_filter, need_records=True):
    db = sqlite3.connect(f"file:{path}?mode=ro", uri=True)
    db.row_factory = sqlite3.Row
    global _db
    _db = db

    # Views in a file nix wrote, or tables in a sidecar exported from DuckDB
    # (the same definitions, materialised) for traces too big for sqlite3.
    have = {r[0] for r in db.execute("select name from sqlite_master where type in ('view', 'table')")}
    missing = {"chunk_weight", "chunk_consumer", "chunk_consumers", "target_costs"} - have
    if missing:
        sys.exit(f"{path} lacks {', '.join(sorted(missing))}: it was written by an older nix; re-run the evaluation")

    roots = {r["id"]: r["name"] for r in db.execute("select id, name from roots")}
    exprs = {}
    for e in db.execute("select id, file, line, col from exprs"):
        name = e["file"] or "?"
        if e["line"] is not None:
            name = f"{name}:{e['line']}:{e['col']}"
        exprs[e["id"]] = name

    where = ""
    params = ()
    if roots_filter:
        ids = [i for i, n in roots.items() if n in roots_filter]
        if not ids:
            sys.exit(f"no root named {', '.join(roots_filter)}; roots are: {', '.join(roots.values())}")
        where = f"where root in ({','.join('?' * len(ids))})"
        params = tuple(ids)

    records = {}
    shared = set()
    if need_records:
        for r in db.execute(
            f"select id, parent, root, expr, kind, start, dur, gc_alloc, self_dur, self_gc_alloc from records {where}",
            params,
        ):
            records[r["id"]] = dict(r)
        shared = {r[0] for r in db.execute("select id from shared")}
    gc = [tuple(r) for r in db.execute("select start, end from gc order by start")]
    return roots, exprs, records, shared, gc


def frame_name(rec, roots, exprs, shared, mark_shared):
    if rec["kind"] == "root":
        name = f"root:{roots.get(rec['root'], rec['root'])}"
    else:
        name = exprs.get(rec["expr"], "?")
        if rec["kind"] == "wait":
            name = f"[wait] {name}"
    if mark_shared and rec["id"] in shared:
        name = f"[shared] {name}"
    return name


def build_tree(records):
    """children[parent id] -> child ids sorted by start; top-level records have
    a parent outside the (possibly filtered) record set."""
    children = defaultdict(list)
    tops = []
    for rec in records.values():
        if rec["parent"] in records:
            children[rec["parent"]].append(rec["id"])
        else:
            tops.append(rec["id"])
    for ids in children.values():
        ids.sort(key=lambda i: records[i]["start"])
    tops.sort(key=lambda i: records[i]["start"])
    return children, tops


def compute_ends(records, children, tops):
    """end[id]: start + dur, widened to cover the children and clamped to the
    parent, so that intervals nest."""
    end = {}

    def widen(i):
        e = records[i]["start"] + records[i]["dur"]
        for c in children.get(i, ()):
            e = max(e, widen(c))
        end[i] = e
        return e

    def clamp(i, limit):
        if limit is not None:
            end[i] = min(end[i], limit)
            records[i]["start"] = min(records[i]["start"], end[i])
        for c in children.get(i, ()):
            clamp(c, end[i])

    sys.setrecursionlimit(max(sys.getrecursionlimit(), 1_000_000))
    for t in tops:
        widen(t)
        clamp(t, None)
    return end


def thread_of(rec):
    return rec["id"] >> 32


def cmd_speedscope(args, roots, exprs, records, shared, gc):
    children, tops = build_tree(records)
    end = compute_ends(records, children, tops)

    frames = []
    frame_index = {}

    def frame(rec):
        name = frame_name(rec, roots, exprs, shared, args.mark_shared)
        if name not in frame_index:
            frame_index[name] = len(frames)
            frames.append({"name": name})
        return frame_index[name]

    groups = defaultdict(list)
    for t in tops:
        key = thread_of(records[t]) if args.split == "thread" else records[t]["root"]
        groups[key].append(t)

    profiles = []
    for key in sorted(groups):
        events = []

        def emit(i):
            rec = records[i]
            events.append({"type": "O", "frame": frame(rec), "at": rec["start"]})
            for c in children.get(i, ()):
                emit(c)
            events.append({"type": "C", "frame": frame(rec), "at": end[i]})

        for t in groups[key]:
            emit(t)
        name = f"thread {key}" if args.split == "thread" else roots.get(key, str(key))
        profiles.append(
            {
                "type": "evented",
                "name": name,
                "unit": "nanoseconds",
                "startValue": events[0]["at"],
                "endValue": events[-1]["at"],
                "events": events,
            }
        )

    out = {
        "$schema": "https://www.speedscope.app/file-format-schema.json",
        "shared": {"frames": frames},
        "profiles": profiles,
        "name": args.trace,
        "activeProfileIndex": 0,
        "exporter": "tecnix-trace.py",
    }
    json.dump(out, open_output(args), separators=(",", ":"))


def cmd_perfetto(args, roots, exprs, records, shared, gc):
    children, tops = build_tree(records)
    end = compute_ends(records, children, tops)
    pid = 1
    events = [{"ph": "M", "name": "process_name", "pid": pid, "args": {"name": "nix evaluation"}}]
    threads = sorted({thread_of(r) for r in records.values()})
    for t in threads:
        events.append({"ph": "M", "name": "thread_name", "pid": pid, "tid": t, "args": {"name": f"eval thread {t}"}})
    gc_tid = (max(threads) if threads else 0) + 1
    events.append({"ph": "M", "name": "thread_name", "pid": pid, "tid": gc_tid, "args": {"name": "garbage collection"}})

    for rec in records.values():
        events.append(
            {
                "ph": "X",
                "name": frame_name(rec, roots, exprs, shared, args.mark_shared),
                "cat": rec["kind"],
                "ts": rec["start"] / 1000,
                "dur": (end[rec["id"]] - rec["start"]) / 1000,
                "pid": pid,
                "tid": thread_of(rec),
                "args": {
                    "id": rec["id"],
                    "root": roots.get(rec["root"], rec["root"]),
                    "self_dur_ns": rec["self_dur"],
                    "gc_alloc": rec["gc_alloc"],
                    "self_gc_alloc": rec["self_gc_alloc"],
                    "shared": rec["id"] in shared,
                },
            }
        )
    for start, stop in gc:
        events.append(
            {"ph": "X", "name": "GC pause", "cat": "gc", "ts": start / 1000, "dur": (stop - start) / 1000, "pid": pid, "tid": gc_tid}
        )

    json.dump({"traceEvents": events, "displayTimeUnit": "ns"}, open_output(args), separators=(",", ":"))


def cmd_folded(args, roots, exprs, records, shared, gc):
    weight = args.weight
    # The stack of a record: from the highest shared ancestor (inclusive) under
    # `[shared]` when --shared, otherwise from its root.
    parent = {i: r["parent"] for i, r in records.items()}
    names = {i: frame_name(r, roots, exprs, shared, args.mark_shared) for i, r in records.items()}
    counts = defaultdict(int)
    for i, rec in records.items():
        w = rec[weight]
        if w <= 0 or rec["kind"] == "wait":
            continue
        chain = []
        j = i
        while j in records:
            chain.append(j)
            j = parent[j]
        chain.reverse()  # root ... record
        anchor = next((k for k, x in enumerate(chain) if x in shared), None) if args.shared else None
        if anchor is not None:
            # Needed by several roots: attributed to none of them.
            stack = ["[shared]"] + [names[x] for x in chain[anchor:]]
        else:
            stack = [names[x] for x in chain]
            if records[chain[0]]["kind"] != "root":  # the root record was filtered out
                stack.insert(0, f"root:{roots.get(rec['root'], rec['root'])}")
        counts[";".join(stack)] += w
    out = open_output(args)
    for stack, w in sorted(counts.items(), key=lambda kv: -kv[1]):
        out.write(f"{stack} {w}\n")


def fmt_weight(w, weight):
    if weight == "self_dur":
        if w >= 1e10:
            return f"{w / 1e9:.1f} s"
        return f"{w / 1e6:.1f} ms" if w >= 1e5 else f"{w / 1e3:.0f} µs"
    for unit in ("B", "KB", "MB", "GB"):
        if w < 1024 or unit == "GB":
            return f"{w:.0f} {unit}" if unit == "B" else f"{w:.1f} {unit}"
        w /= 1024


def dot_id(s):
    """A quoted DOT string; labels may contain \\n line breaks, so only quotes are escaped."""
    return '"' + s.replace('"', '\\"') + '"'


DOT_SQL = {
    # own weight, amortised share, and I/O per target
    "targets": """
        select root, name, own_{w} as own, shared_{w}_amortized as share,
               fetch_dur_amortized as fetch_dur, store_dur_amortized as store_dur
        from target_costs where name like '//%'
    """,
    # shared work between pairs of targets: chunks needed by at most K targets
    "edges": """
        with narrow as (select chunk from chunk_consumers where consumers <= {k})
        select a.root as a, b.root as b, sum(w.self_{w}) as weight
        from chunk_consumer a join chunk_consumer b on a.chunk = b.chunk and a.root < b.root
        join narrow n on n.chunk = a.chunk
        join chunk_weight w on w.chunk = a.chunk
        group by a.root, b.root
    """,
    # each target's share of work needed by more than K targets
    "infra": """
        select c.root, sum(w.self_{w} * 1.0 / cc.consumers) as share, sum(w.self_{w}) as needed
        from chunk_consumer c join chunk_consumers cc on cc.chunk = c.chunk and cc.consumers > {k}
        join chunk_weight w on w.chunk = c.chunk
        group by c.root
    """,
}


def cmd_dot(args, roots, exprs, records, shared, gc):
    weight = args.weight
    w = weight.removeprefix("self_")
    sql = {name: q.format(w=w, k=args.max_consumers) for name, q in DOT_SQL.items()}
    if args.print_sql:
        for name, q in sql.items():
            print(f"-- {name}\n{q.strip()};\n")
        return

    targets = {row["root"]: dict(row) for row in _db.execute(sql["targets"])}
    if args.root:
        selected = {r for r, t in targets.items() if t["name"] in args.root}
    else:
        selected = {r for r, _ in sorted(targets.items(), key=lambda kv: -kv[1]["own"])[: args.top]}

    # Edges lighter than --min-edge (default: 1% of the heaviest selected
    # target's own weight) are noise: a target shares a few microseconds with
    # hundreds of others.
    min_edge = args.min_edge
    if min_edge is None:
        min_edge = 0.01 * max((targets[r]["own"] for r in selected), default=0)
    edges = []
    for a, b, wt in _db.execute(sql["edges"]):
        if (a in selected or b in selected) and wt >= min_edge and wt > 0:
            edges.append((a, b, wt))
    edges.sort(key=lambda e: -e[2])
    shown = set(selected)
    for a, b, _ in edges:
        shown.add(a)
        shown.add(b)
    infra = {row["root"]: (row["share"], row["needed"]) for row in _db.execute(sql["infra"])}

    def target_label(r):
        t = targets[r]
        parts = [t["name"].replace("%aarch64-darwin", ""), f"own {fmt_weight(t['own'], weight)}"]
        if t["share"]:
            parts.append(f"shared share {fmt_weight(t['share'], weight)}")
        if weight == "self_dur" and (t["fetch_dur"] or t["store_dur"]):
            parts.append(f"fetch {fmt_weight(t['fetch_dur'], weight)}, store {fmt_weight(t['store_dur'], weight)}")
        return "\\n".join(parts)

    out = open_output(args)
    out.write("graph tecnix {\n  layout=neato; overlap=false; splines=true;\n")
    out.write("  node [shape=box, fontname=Helvetica, fontsize=10, style=filled, fillcolor=lightyellow];\n")
    out.write("  edge [fontname=Helvetica, fontsize=8, color=gray50];\n")
    for r in shown:
        if r in targets:
            fill = "lightyellow" if r in selected else "white"
            out.write(f"  {dot_id('t' + str(r))} [label={dot_id(target_label(r))}, fillcolor={fill}];\n")
    for a, b, wt in edges:
        if a in targets and b in targets:
            out.write(f"  {dot_id('t' + str(a))} -- {dot_id('t' + str(b))} [label={dot_id(fmt_weight(wt, weight))}];\n")
    infra_shown = [(r, infra[r]) for r in selected if r in infra and infra[r][0] > 0]
    if infra_shown:
        total_needed = max(n for _, (_, n) in infra_shown)
        label = f"infrastructure\\nneeded by more than {args.max_consumers} targets\\n{fmt_weight(total_needed, weight)}"
        out.write(f"  infra [label={dot_id(label)}, shape=ellipse, fillcolor=lightblue];\n")
        for r, (share, _) in infra_shown:
            out.write(f"  {dot_id('t' + str(r))} -- infra [label={dot_id(fmt_weight(share, weight))}, style=dashed];\n")
    out.write("}\n")


def open_output(args):
    if args.output and args.output != "-":
        return open(args.output, "w")
    return sys.stdout


def main():
    p = argparse.ArgumentParser(description=__doc__ or "Convert a Tecnix trace.")
    sub = p.add_subparsers(dest="command", required=True)
    for name, fn in (("speedscope", cmd_speedscope), ("perfetto", cmd_perfetto), ("folded", cmd_folded), ("dot", cmd_dot)):
        sp = sub.add_parser(name)
        sp.set_defaults(fn=fn)
        sp.add_argument("trace", help="tecnix-trace-<pid>.sqlite")
        sp.add_argument("-o", "--output", default="-")
        sp.add_argument("--root", action="append", default=[], help="only this root (repeatable); for dot, only these targets' nodes")
        sp.add_argument("--mark-shared", action="store_true", help="prefix records needed by several roots with [shared]")
        if name == "speedscope":
            sp.add_argument("--split", choices=("thread", "root"), default="thread", help="one profile per thread or per root")
        if name in ("folded", "dot"):
            sp.add_argument("--weight", choices=("self_dur", "self_gc_alloc"), default="self_dur")
        if name == "folded":
            sp.add_argument("--shared", action="store_true", help="hoist shared subtrees under a top-level [shared] frame")
        if name == "dot":
            sp.add_argument("--top", type=int, default=20, help="the N targets with the largest own weight (default 20)")
            sp.add_argument("--max-consumers", type=int, default=50, help="work needed by more targets than this is 'infrastructure' (default 50)")
            sp.add_argument("--print-sql", action="store_true", help="print the queries instead of the graph (for DuckDB)")
            sp.add_argument("--min-edge", type=float, default=None, help="hide edges lighter than this (ns or bytes; default 1%% of the heaviest selected target's own weight)")
    args = p.parse_args()
    # `dot` needs every record (the chunks a target needs were computed by
    # other roots) and applies --root to the target nodes instead.
    roots, exprs, records, shared, gc = load(
        args.trace, set() if args.command == "dot" else set(args.root), need_records=args.command != "dot")
    args.fn(args, roots, exprs, records, shared, gc)


if __name__ == "__main__":
    main()
