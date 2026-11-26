"""统一 HU 强制工具：合法胡牌时强制选 HU 动作。

国标麻将合法胡牌收益为正，拒胡会把 rollout/eval 拖成全流局，导致
nd_games==0 与 checkpoint 长期 _pending。本模块统一替换 PPO/DMC 各自的
4 处独立实现，避免后续修改时漏改。

hu_idx 从 shared_env_constants 取（env_m.py 必须保持与 Botzone 官网一致，
因此不在 env_m.py 中暴露常量；shared_env_constants 是 single source of truth）。
缺常量即结构性问题，fail-fast 暴露。
"""
from __future__ import annotations

from typing import Any, Tuple

import numpy as np

# 不允许 try/except 兜底默认值；常量缺失即结构性问题。
from shared_env_constants import HU_ACTION_INDEX as DEFAULT_HU_IDX

try:  # torch 是可选依赖，避免无 torch 的子进程导入失败
    import torch as _torch
except Exception:  # pragma: no cover - 极少数纯 numpy 环境
    _torch = None


def _to_numpy(arr: Any) -> np.ndarray:
    if _torch is not None and isinstance(arr, _torch.Tensor):
        return arr.detach().cpu().numpy()
    return np.asarray(arr)


def force_hu_if_legal(
    actions: Any,
    action_mask: Any,
    hu_idx: int = DEFAULT_HU_IDX,
) -> Tuple[np.ndarray, int]:
    """合法胡牌时把对应 batch 行的动作强制为 hu_idx。

    actions: numpy 或 torch tensor，shape [B] / [B, ...]；返回 numpy 副本。
    action_mask: numpy 或 torch tensor，shape [B, A]；mask[:, hu_idx] > 0.5
        视为可胡。
    返回 (forced_actions_np, forced_count)；非法 mask shape 时 forced_count=0
    并原样返回。
    """
    if action_mask is None:
        return _to_numpy(actions).copy(), 0

    mask_np = _to_numpy(action_mask)
    if mask_np.ndim < 2 or mask_np.shape[-1] <= hu_idx:
        # mask shape 不符；返回原 actions，不强制
        return _to_numpy(actions).copy(), 0

    actions_np = _to_numpy(actions).copy()
    try:
        hu_rows = mask_np[..., hu_idx] > 0.5
    except Exception:
        return actions_np, 0

    n = int(hu_rows.sum())
    if n > 0:
        actions_np[hu_rows] = hu_idx
    return actions_np, n
