"""共享 env 动作空间常量。

env_m.py 必须与 Botzone 官方版本保持一致（不修改 env_m.py 文件本身），
但训练代码需要明确的"HU 动作索引"等常量来实现 force-HU 等通用工具。
本模块作为 single source of truth：

- HU_ACTION_INDEX: 国标麻将动作空间中 HU（胡牌）动作的索引。env_m.py
  内部 step / action_mask 编码也是 39（约定），变更时必须同步两处。

把常量集中在这里而不是 env_m.py，可以让 env_m.py 文件无需修改即可与
Botzone 官网游戏环境对齐；训练侧 shared_action_force / scheduler 等
模块统一从此 import。
"""
from __future__ import annotations

# 国标麻将动作空间约定（与 env_m.py 内部硬编码一致）
HU_ACTION_INDEX: int = 39
