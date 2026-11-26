"""PPO/DMC 共用的评估趋势指标工具。

集中放置 LB95（Wilson 95% 下界）、滑动窗口均值、no-draw 胜率等 metric
计算函数，避免 PPO 与 DMC 两侧口径漂移。
"""
from __future__ import annotations

import math
from collections.abc import Iterable, Sequence


def wilson_lb95(p: float, n: int, z: float = 1.96) -> float:
    """Wilson 95% 置信区间下界。
    p: 比例 (0~1)；n: 样本数；返回 [0, 1] 范围的下界。
    n<=0 时返回 0.0；对小样本鲁棒。

    【Round-18 R18-C】NaN p 兜底：旧逻辑 `min(1.0, float(NaN))` 因 NaN 比较恒
    False 返回 1.0，再 `max(0.0, 1.0)=1.0`，**静默把 NaN 当作 100% 胜率**返回
    接近 1.0 的虚假高 LB95，污染 DipGuard / probation 决策。NaN 视同 n<=0 同档
    返回 0.0。
    """
    if n <= 0:
        return 0.0
    try:
        _pf = float(p)
    except (TypeError, ValueError):
        return 0.0
    if not math.isfinite(_pf):
        return 0.0
    p = max(0.0, min(1.0, _pf))
    n = int(n)
    z2 = z * z
    denom = 1.0 + z2 / n
    centre = (p + z2 / (2.0 * n)) / denom
    margin = (z / denom) * math.sqrt(
        (p * (1.0 - p) + z2 / (4.0 * n)) / n
    )
    return max(0.0, centre - margin)


def moving_average(seq: Iterable[float], window: int) -> float:
    """末端 `window` 个元素的均值；序列为空或全为非有限值时返回 nan。

    【Round-18 R18-C】过滤 NaN/inf 元素，避免单个脏值毒化整窗均值。
    """
    if window <= 0:
        return float("nan")
    if isinstance(seq, Sequence):
        tail = seq[-window:]
    else:
        tail = list(seq)[-window:]
    if not tail:
        return float("nan")
    _clean = []
    for _x in tail:
        try:
            _xf = float(_x)
        except (TypeError, ValueError):
            continue
        if math.isfinite(_xf):
            _clean.append(_xf)
    if not _clean:
        return float("nan")
    return sum(_clean) / len(_clean)


def nodraw_wr(win_no_draw: float, no_draw_games: float) -> float:
    """no-draw 胜率：n=0 时返回 0.0。

    【Round-18 R18-C】NaN 分子/分母兜底：与 `wilson_lb95` 对称，避免 NaN 进
    比较链路。
    """
    try:
        n = float(no_draw_games)
        w = float(win_no_draw)
    except (TypeError, ValueError):
        return 0.0
    if not math.isfinite(n) or not math.isfinite(w) or n <= 0.0:
        return 0.0
    return w / n


def nd_with_lb95(win_no_draw: float, no_draw_games: float) -> tuple[float, float]:
    """同时返回 ND 与对应 Wilson LB95。"""
    nd = nodraw_wr(win_no_draw, no_draw_games)
    lb = wilson_lb95(nd, int(no_draw_games))
    return nd, lb
