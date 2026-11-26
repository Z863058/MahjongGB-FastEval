# -*- coding: utf-8 -*-
"""模型筛选漏斗结果汇总（配 _sel_screen.sh）。

解析 logs/_sel_<cand>_<opp>_*.log 中 pimc_sweep 的 `DONE config=` 行，按对手分布权重
（强 S=0.2 / 中 M=0.3 / 弱 W=0.5，可 env SEL_W_{S,M,W} 覆盖）聚合每个 (候选, 模式) 的
WeightedScore = ΣW_opp * avgScore_opp（avgScore=New 每局净得分，复式 duplicate=True）。

输出：① 明细 ② 加权排序 ③ deal_in 净差决策 ④ 终选建议。
判据：WeightedScore 主排序；ND ΔWR±SE 作显著性辅证；差异 <2σ 不下硬结论。

用法：py -3.12 -X utf8 _sel_summarize.py [--logdir logs] [--glob "_sel_*.log"]
"""
import argparse
import glob
import os
import re
import sys

try:
    from shared_stdio_utf8 import configure_stdio_utf8
    configure_stdio_utf8()
except Exception:
    pass

# 对手分布权重，env 可覆盖。
# 【2026-06-26 纯 actor + 双锚点】部署已为纯 actor、且现无中档异质 bot（V2 不可用），
# field 锚点收为 强 S=RL2025 + 弱 W=PG。实赛弱者占多数 → 弱权重更高：S=0.3 / W=0.7、
# M=0（缺中档不计为缺档，见 weighted()）。若日后补回中档：export SEL_W_M=0.3 等。
W = {
    "S": float(os.environ.get("SEL_W_S", "0.3")),
    "M": float(os.environ.get("SEL_W_M", "0.0")),
    "W": float(os.environ.get("SEL_W_W", "0.7")),
}
OPP_ORDER = ["S", "M", "W"]
OPP_LABEL = {"S": "强RL2025", "M": "中V2", "W": "弱PG"}
CFG2MODE = {"new_actor_only": "actor", "deal_in_only": "dealin"}

# pimc_sweep DONE 行（[pimc_sweep.py] _run_one L251-255）。值可能为 None / n/a。
_NUM = r"(-?[\d.]+|None|n/a)"
DONE_RE = re.compile(
    r"DONE config=(?P<cfg>\S+)\s+"
    r"ND=" + _NUM + r"\s+LB95=" + _NUM + r"\s+n_nd=" + _NUM + r"\s+"
    r"\u0394WR=" + _NUM + r"\u00b1" + _NUM + r"\s+"
    r"abs=" + _NUM + r"\s+absLB95=" + _NUM + r"\s+"
    r"avgScore=" + _NUM + r"\s+score=" + _NUM + r"\s+n=" + _NUM
)


def _f(s):
    """字符串转 float；None/n/a → None。"""
    if s is None or s in ("None", "n/a"):
        return None
    try:
        return float(s)
    except (TypeError, ValueError):
        return None


def _parse_name(path):
    """从 _sel_<cand>_<opp>_<cfgs>.log 取 (cand, opp)。"""
    base = os.path.basename(path)
    if not base.startswith("_sel_") or not base.endswith(".log"):
        return None, None
    rest = base[len("_sel_"):-len(".log")]
    parts = rest.split("_", 2)
    if len(parts) < 2:
        return None, None
    cand, opp = parts[0], parts[1]
    if opp not in W:
        return cand, None
    return cand, opp


def collect(logdir, pattern):
    # data[cand][mode][opp] = dict(avg, nd, lb95, n, dwr, se)
    data = {}
    for path in sorted(glob.glob(os.path.join(logdir, pattern))):
        cand, opp = _parse_name(path)
        if cand is None or opp is None:
            continue
        try:
            with open(path, "r", encoding="utf-8", errors="replace") as fh:
                text = fh.read()
        except OSError:
            continue
        for m in DONE_RE.finditer(text):
            mode = CFG2MODE.get(m.group("cfg"))
            if mode is None:
                continue
            avg = _f(m.group(9))   # avgScore（第 9 个捕获组）
            rec = {
                "avg": avg,
                "nd": _f(m.group(2)),
                "lb95": _f(m.group(3)),
                "n": _f(m.group(11)),
                "dwr": _f(m.group(6)),
                "se": _f(m.group(7)),
            }
            data.setdefault(cand, {}).setdefault(mode, {})[opp] = rec
    return data


def weighted(per_opp, cand, mode):
    """ΣW_opp*avg；RL2025 vs 强(自打)缺档按净分 0 计。返回 (score, 覆盖权重, 缺档列表)。
    权重为 0 的对手档（如双锚点下的中档 M）直接跳过：不累计、不计入覆盖、不算缺档。"""
    total, cover, missing = 0.0, 0.0, []
    for opp in OPP_ORDER:
        if W.get(opp, 0.0) == 0.0:
            continue  # 该档权重 0（双锚点下 M=0）→ 忽略，不视为缺档
        rec = per_opp.get(opp)
        avg = rec["avg"] if rec else None
        if avg is None and cand == "rl2025" and opp == "S":
            avg = 0.0  # RL2025 自打 = 平局净分 0
        if avg is None:
            missing.append(opp)
            continue
        total += W[opp] * avg
        cover += W[opp]
    return total, cover, missing


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument("--logdir", default="logs")
    ap.add_argument("--glob", dest="pattern", default="_sel_*.log")
    args = ap.parse_args(argv)

    data = collect(args.logdir, args.pattern)
    if not data:
        print("[sel] 未在 %s/%s 解析到任何 DONE 行；先跑 _sel_screen.sh。"
              % (args.logdir, args.pattern))
        return 1

    print("权重 强S=%.2f 中M=%.2f 弱W=%.2f（env SEL_W_{S,M,W} 可改）\n"
          % (W["S"], W["M"], W["W"]))

    print("== 明细 avgScore（每局净得分；括号 ND%% / LB95%%）==")
    hdr = "%-8s %-7s | %-22s %-22s %-22s" % (
        "候选", "模式", OPP_LABEL["S"], OPP_LABEL["M"], OPP_LABEL["W"])
    print(hdr); print("-" * len(hdr))
    for cand in sorted(data):
        for mode in ("actor", "dealin"):
            if mode not in data[cand]:
                continue
            cells = []
            for opp in OPP_ORDER:
                rec = data[cand][mode].get(opp)
                if rec and rec["avg"] is not None:
                    cells.append("%+.3f (%.1f/%.1f)" % (
                        rec["avg"], rec["nd"] or 0, rec["lb95"] or 0))
                elif cand == "rl2025" and opp == "S":
                    cells.append("0.000 (自打)")
                else:
                    cells.append("--")
            print("%-8s %-7s | %-22s %-22s %-22s"
                  % (cand, mode, cells[0], cells[1], cells[2]))

    _formula = " + ".join("%.2f*%s" % (W[o], o) for o in OPP_ORDER if W.get(o, 0.0) != 0.0)
    print("\n== 加权排序 WeightedScore = %s ==" % _formula)
    rows = []
    for cand in data:
        for mode in ("actor", "dealin"):
            if mode not in data[cand]:
                continue
            sc, cover, miss = weighted(data[cand][mode], cand, mode)
            rows.append((sc, cover, miss, cand, mode))
    rows.sort(key=lambda r: (-(r[1] >= 0.999), -r[0]))  # 先全覆盖、再按分降序
    print("%-8s %-7s | %-12s %-8s %s" % ("候选", "模式", "Weighted", "覆盖", "缺档"))
    print("-" * 56)
    for sc, cover, miss, cand, mode in rows:
        tag = "" if cover >= 0.999 else " (部分)"
        miss_s = ("缺" + ",".join(OPP_LABEL[o] for o in miss)) if miss else "-"
        print("%-8s %-7s | %+11.4f%s %-8s %s"
              % (cand, mode, sc, tag, "%.0f%%" % (cover * 100), miss_s))

    print("\n== deal_in 净差（同模型 dealin − actor，>0 建议开 deal_in）==")
    for cand in sorted(data):
        d = data[cand]
        if "actor" in d and "dealin" in d:
            a, ca, _ = weighted(d["actor"], cand, "actor")
            b, cb, _ = weighted(d["dealin"], cand, "dealin")
            if ca >= 0.999 and cb >= 0.999:
                verdict = "开 deal_in" if b - a > 0 else "关 deal_in(纯 actor)"
                print("  %-6s : dealin−actor = %+.4f  -> %s" % (cand, b - a, verdict))
            else:
                print("  %-6s : 覆盖不全，补齐三对手后再判" % cand)

    if rows:
        best = rows[0]
        print("\n== 终选建议 ==")
        print("  WeightedScore 第一：%s / %s = %+.4f%s"
              % (best[3], best[4], best[0], "" if best[1] >= 0.999 else " (部分，慎判)"))
        # 【2026-06-26 最弱分项否决门】纯 actor 须在每个 field 锚点都不显著翻负——任一
        # 有权重的对手档 ΔWR < -1.0pp（或 avgScore<0）即对该档「输」，弱场失分会被 256
        # 轮复式放大、直接拖排名（iter_830 第 30 名教训）。命中则 WARN，要求复核/换候选。
        bd = data.get(best[3], {}).get(best[4], {})
        _regress = []
        for opp in OPP_ORDER:
            if W.get(opp, 0.0) == 0.0:
                continue
            rec = bd.get(opp)
            if not rec:
                continue
            _dwr = rec.get("dwr")
            _avg = rec.get("avg")
            if (_dwr is not None and _dwr < -1.0) or (_avg is not None and _avg < 0.0):
                _regress.append("%s(ΔWR=%s avg=%s)" % (
                    OPP_LABEL[opp],
                    "n/a" if _dwr is None else "%+.2fpp" % _dwr,
                    "n/a" if _avg is None else "%+.3f" % _avg))
        if _regress:
            print("  [WARN] 最弱分项否决门命中：" + "; ".join(_regress)
                  + " → 该候选在上述档翻负 >1pp/净负，慎选，优先复核或换候选。")
        else:
            print("  [OK] 各有权重 field 锚点均未显著翻负（ΔWR≥-1pp 且 avgScore≥0）。")
        print("  门：与次优差 <2σ（看 ND ΔWR±SE）则取低 TLE 风险者；纯 actor 无 deal_in/PIMC，")
        print("      提交前过 _p4_p95_stress.sh <model> 单核 P95<3800ms 即可。")
    return 0


if __name__ == "__main__":
    sys.exit(main())
