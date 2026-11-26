# -*- coding: utf-8 -*-
"""最小复式评测入口。同一 seed 发 4 局，学习者轮换座位。"""
from __future__ import annotations

from shared_eval_metrics import wilson_lb95


def mirror_assignments(seed: int, n_seats: int = 4):
    """返回一个块内的 (seed, learner_seat) 列表。牌墙由 seed 决定。"""
    if n_seats != 4:
        raise ValueError("this benchmark uses 4 seats")
    return [{"seed": int(seed), "learner_seat": seat} for seat in range(n_seats)]


def reset(seed: int = 0):
    """构造一局环境。需要本机已安装 MahjongGB。"""
    from env_m import ChineseStandardMahjongEnv
    env = ChineseStandardMahjongEnv()
    return env, {"srand": int(seed)}


def step(env, action):
    return env.step(action)


def run_mirror_block(seed: int):
    """只生成块内座位安排。具体对局由调用方用 reset/step 跑完。"""
    return mirror_assignments(seed)
