## Summary

<!-- Briefly describe what this change does and why. -->

## Type of change

- [ ] Search / Evaluation (strength change)
- [ ] Performance (non-functional speed-up)
- [ ] Protocol / CLI / Assets
- [ ] Bug fix / Reliability
- [ ] Build / CI / Packaging
- [ ] Documentation / Refactoring

## Verification

- [ ] Built cleanly with `cmake --build` (Release)
- [ ] CTest suite passes (`ctest --output-on-failure`), including `koi_architecture_boundaries`
- [ ] If documentation changed: `python tools/docs/prepare_pages.py && mkdocs build --strict` passes with zero warnings
- [ ] If non-functional search/eval change: `koi-engine bench` node count is bit-identical
- [ ] If functional search/eval change: SPRT / paired match result recorded below

### Bench / SPRT Results (if applicable)

```text
Nodes searched  :
Nodes/second    :
SPRT / Match    :
```
