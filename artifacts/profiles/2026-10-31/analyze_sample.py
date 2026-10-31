#!/usr/bin/env python3
"""Parse macOS `sample` call-tree output for the Koi CPU profile (2026-10-31).

Usage: python analyze_sample.py <sample.txt> [--json out.json] [--top N]

Percentages in the table are relative to the samples of the search worker
thread(s) only; the process main thread is idle (join/read) and is excluded.
Depth model for the sample call-graph: prefix is 4 leading spaces followed by
2 characters per frame ("+ ", "! ", ": ", "| ").
"""
from __future__ import annotations

import json
import re
import sys
from collections import Counter

WAIT_PATTERNS = (
    "__ulock_wait", "__psynch_cvwait", "_pthread_cond_wait", "semaphore_wait",
    "mach_msg", "__semwait", "std::__1::condition_variable::wait",
)

LEAF_PATTERNS = [
    "on_make_move", "prepare_child_slot", "NnueEvaluatorWorker::evaluate",
    "EvaluationFeatureExtractor::extract", "refresh_slot_perspective",
    "accumulate_hidden_sparse_scalar", "encode_sparse_v4",
    "halfka_king_bucket", "vector<", "malloc", "operator new",
    "_platform_memmove", "_platform_memset", "bzero",
    "SearchMovePicker::next", "prepare_candidates", "ensure_stage_sorted",
    "static_exchange", "for_each_exchange_attacker",
    "candidate_is_legal", "native_move_gives_check", "generate_pseudo",
    "legal_moves_with_metadata", "legal_tactical_moves_with_metadata",
    "apply_unchecked", "apply_generated_move",
    "TranspositionTable::probe", "TranspositionTable::store",
    "quiet_history_score", "priority(", "update_correction",
    "square_attacked", "rook_attacks", "bishop_attacks", "queen_attacks",
    "native_position_features", "refresh_king_masks",
    "repetition_count", "draw_status",
]
PARENT_PATTERNS = ["on_make_move", "prepare_child_slot", "NnueEvaluatorWorker::evaluate",
                   "SearchContext::negamax", "SearchContext::quiescence"]


def parse(path: str):
    lines = open(path, encoding="utf-8", errors="replace").read().splitlines()
    start = lines.index("Call graph:")
    end = next(i for i in range(start, len(lines))
               if lines[i].startswith("Total number in stack"))
    threads, nodes, cur, stack = [], [], None, []
    bad = 0
    for ln in lines[start + 1:end]:
        tm = re.match(r"^(\s*)(\d+) (Thread_.*)$", ln)
        if tm and not ln.lstrip().startswith("+"):
            cur = {"name": tm.group(3).strip(), "count": int(tm.group(2)),
                   "roots": []}
            threads.append(cur)
            stack = []
            continue
        m = re.match(r"^([ +!:|]+?)(\d+) (.*)$", ln)
        if not m:
            bad += 1
            continue
        pref, cnt, rest = m.group(1), int(m.group(2)), m.group(3)
        level = (len(pref) - 4) // 2
        sym = re.split(r"\s{2,}\(in ", rest)[0].strip()
        n = {"sym": sym, "count": cnt, "level": level, "children": [], "self": cnt}
        while stack and stack[-1]["level"] >= level:
            stack.pop()
        if stack:
            stack[-1]["children"].append(n)
        elif cur is not None:
            cur["roots"].append(n)
        stack.append(n)
        nodes.append(n)
    for t in threads:
        for r in t["roots"]:
            _walk(r)
    return threads, nodes, bad


def _walk(n):
    n["self"] = n["count"] - sum(c["count"] for c in n["children"])
    for c in n["children"]:
        _walk(c)


def subtree(n):
    out = []

    def rec(x):
        out.append(x)
        for c in x["children"]:
            rec(c)
    rec(n)
    return out


def has(node, needle):
    return needle in node["sym"]


def main():
    path = sys.argv[1]
    out_json = None
    top_n = 25
    for i, a in enumerate(sys.argv):
        if a == "--json":
            out_json = sys.argv[i + 1]
        if a == "--top":
            top_n = int(sys.argv[i + 1])

    threads, nodes, bad = parse(path)
    report = {"file": path, "parse_bad_lines": bad, "threads": []}
    base = 0
    for t in threads:
        is_search = any(has(n, "SearchContext::negamax") for n in subtree_roots(t))
        waits = sum(n["self"] for n in subtree_roots(t)
                    if any(p in n["sym"] for p in WAIT_PATTERNS))
        totals = sum(n["count"] for n in t["roots"])
        report["threads"].append({"name": t["name"], "samples": t["count"],
                                  "root_total": totals, "search": is_search,
                                  "wait_self": waits})
        if is_search:
            base += t["count"]
    search_threads = [t for t in threads
                      if any(has(n, "SearchContext::negamax") for n in subtree_roots(t))]
    all_nodes = [n for t in search_threads for n in subtree_roots(t)]
    print(f"== {path}")
    print(f"bad_lines={bad} threads={len(threads)} search_threads={len(search_threads)} "
          f"search_samples={base}")
    for th in report["threads"]:
        print(f"  {th['name'][:44]:44s} samples={th['samples']:6d} search={th['search']} "
              f"wait_self={th['wait_self']:5d} ({100.0*th['wait_self']/max(th['samples'],1):.1f}%)")

    self_c = Counter()
    incl_c = Counter()
    for n in all_nodes:
        self_c[n["sym"]] += n["self"]
        if not _has_ancestor(n, n["sym"]):
            incl_c[n["sym"]] += n["count"]
    report["search_samples"] = base
    report["self_top"] = [{"sym": s, "self": c, "self_pct": 100.0 * c / base}
                          for s, c in self_c.most_common(60)]
    report["incl_top"] = [{"sym": s, "incl": c, "incl_pct": 100.0 * c / base}
                          for s, c in incl_c.most_common(60)]
    print(f"\n-- SELF (top {top_n}) --")
    print(f"{'self%':>7} {'self':>8} {'incl%':>7} {'incl':>8}  symbol")
    for sym, c in self_c.most_common(top_n):
        print(f"{100.0*c/base:7.2f} {c:8d} {100.0*incl_c[sym]/base:7.2f} "
              f"{incl_c[sym]:8d}  {sym[:105]}")
    print(f"\n-- INCLUSIVE-ONCE (top {top_n}) --")
    for sym, c in incl_c.most_common(top_n):
        print(f"{100.0*c/base:7.2f} {c:8d}  self={100.0*self_c[sym]/base:6.2f}%  {sym[:105]}")

    # ---- qsearch subtree ----
    q_nodes = []
    for n in all_nodes:
        if has(n, "SearchContext::quiescence") and not _has_ancestor(n, "SearchContext::quiescence"):
            q_nodes.append(n)
    q_total = sum(n["count"] for n in q_nodes)
    q_all = [x for n in q_nodes for x in subtree(n)]
    q_self = Counter()
    for n in q_all:
        q_self[n["sym"]] += n["self"]
    report["qsearch"] = {"samples": q_total, "pct": 100.0 * q_total / base,
                         "self_top": [{"sym": s, "self": c, "pct_search": 100.0 * c / base,
                                       "pct_q": 100.0 * c / max(q_total, 1)}
                                      for s, c in q_self.most_common(40)]}
    print(f"\n-- QSEARCH SUBTREE: {q_total} samples = {100.0*q_total/base:.2f}% of search samples --")
    print(f"{'%search':>8} {'%ofQ':>7} {'self':>8}  symbol")
    for sym, c in q_self.most_common(22):
        print(f"{100.0*c/base:8.2f} {100.0*c/max(q_total,1):7.2f} {c:8d}  {sym[:105]}")

    # negamax (non-qsearch) work = everything else
    nq_self = Counter(self_c)
    for s, c in q_self.items():
        nq_self[s] -= c
    report["nonq_self"] = [{"sym": s, "self": c, "pct": 100.0 * c / base}
                           for s, c in nq_self.most_common(30) if c > 0]

    # ---- ancestor-attributed inclusive probes ----
    probes = Counter()
    for n in all_nodes:
        leaf = next((p for p in LEAF_PATTERNS if p in n["sym"]), None)
        if leaf is None:
            continue
        anc = n
        parent = None
        while True:
            anc = _parent_index.get(id(anc))
            if anc is None:
                break
            match = next((p for p in PARENT_PATTERNS if p in anc["sym"]), None)
            if match:
                parent = match
                break
        if parent:
            probes[(leaf, parent)] += n["count"]
    report["probes"] = [{"leaf": l, "parent": p, "samples": c, "pct": 100.0 * c / base}
                        for (l, p), c in sorted(probes.items(), key=lambda kv: -kv[1])[:40]]
    print("\n-- ANCESTOR-ATTRIBUTED INCLUSIVE PROBES --")
    print(f"{'%search':>8} {'samples':>8}  leaf <- nearest interesting ancestor")
    for (leaf, parent), c in sorted(probes.items(), key=lambda kv: -kv[1])[:28]:
        print(f"{100.0*c/base:8.2f} {c:8d}  {leaf[:42]:42s} <- {parent[:44]}")

    # ---- per-thread top self (T4 helper states) ----
    print("\n-- PER-THREAD SELF TOP 6 --")
    report["per_thread"] = []
    for t in search_threads:
        ts = Counter()
        tn = [x for n in t["roots"] for x in subtree(n)]
        for n in tn:
            ts[n["sym"]] += n["self"]
        rows = [(s, c) for s, c in ts.most_common(6)]
        report["per_thread"].append({"name": t["name"], "samples": t["count"],
                                     "top": [{"sym": s, "self": c, "pct": 100.0 * c / t["count"]}
                                             for s, c in rows]})
        print(f"  {t['name'][:40]:40s} n={t['count']}")
        for s, c in rows:
            print(f"      {100.0*c/t['count']:6.2f}% {c:7d}  {s[:95]}")

    if out_json:
        json.dump(report, open(out_json, "w", encoding="utf-8"), indent=1)


# parent index built in parse via a global map
_parent_index = {}


def subtree_roots(t):
    return [x for r in t["roots"] for x in subtree(r)]


def _has_ancestor(n, needle):
    a = _parent_index.get(id(n))
    while a is not None:
        if needle in a["sym"]:
            return True
        a = _parent_index.get(id(a))
    return False


_orig_parse = parse


def parse(path):  # noqa: F811 - extend the original parser with parent links
    threads, nodes, bad = _orig_parse(path)
    global _parent_index
    _parent_index = {}
    for n in nodes:
        for c in n["children"]:
            _parent_index[id(c)] = n
    return threads, nodes, bad


if __name__ == "__main__":
    main()
