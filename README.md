# MahjongGB-FastEval

C++11 Chinese-Standard-Mahjong environment plus a 4-game duplicate mirror helper.

## Build

```bash
pip install pybind11
python _setup_env_m.py build_ext --inplace
```

## Duplicate block

`fasteval_wrapper.mirror_assignments(seed)` returns four jobs. Each job uses the same wall seed and a different learner seat. Score the learner against the other seats on that wall. The shared deal cancels in the contrast.

## Selection statistic

`shared_eval_metrics.wilson_lb95` is the lower 95% Wilson bound used to reject one-batch spikes.

## License

MIT. See `LICENSE`.

## Citation

See `CITATION.cff`. The public ladder result and the 516,000-game table belong to the accompanying paper, not to this repository's README.

Repository: https://github.com/Z863058/MahjongGB-FastEval
