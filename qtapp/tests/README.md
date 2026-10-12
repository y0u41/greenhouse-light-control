# qtapp/tests —— 单元测试基座

## 怎么跑

```powershell
# 工具链必须先加进 PATH，否则 cc1.exe 会以 0xC0000135 静默退出，gcc 一条错误都不打
$env:PATH = "E:\Qt\6.12.0\mingw_64\bin;E:\Qt\Tools\mingw1310_64\bin;E:\Qt\Tools\CMake_64\bin;$env:PATH"

cmake -S D:\glc\qtapp -B D:\glc\qtapp\build
cmake --build D:\glc\qtapp\build
ctest --test-dir D:\glc\qtapp\build --output-on-failure
```

只跑 Qt 侧：`D:\glc\qtapp\build\test_commandengine.exe`

一致性脚本用的 Python 解释器由 CMake 变量 `GREENHOUSE_PYTHON` 决定
（默认本机 DSH 运行时自带的 python.exe）；Qt 测试内可用环境变量 `QTPY` 覆盖。

## 内容

| 目标 / 用例 | 说明 |
| --- | --- |
| target `test_commandengine` | 表驱动测 `CommandEngine::validate` / `buildCmd` |
| ctest `commandengine` | 上述可执行文件 |
| ctest `protocol_parity_python` | Python 侧 standalone 报告 |

被测翻译单元是**真实实现**（`src/commandengine.cpp` 等），测试里没有第二份校验逻辑。

## 用例表的单一事实源

`../../tools/validate_cases.json` 是这张表的唯一出处，C++ 侧与 Python 侧都读它：

- C++：`test_commandengine.cpp::loadCases()`
- Python：`tools/check_validate_parity.py`

改了用例表或 `tools/protocol_schema.py` 之后，必须重生成快照，否则
`paritySnapshotIsCurrent` 会红灯：

```powershell
python tools/check_validate_parity.py --write
```

## ★ 一致性守卫：口径与现状

`parityWithPythonSchema` 拿同一张用例表分别喂给 `CommandEngine::validate` 与
`protocol_schema.validate_cmd`，**比对判定**。口径（已拍板）：

- **判定不同 → FAIL（硬门槛）。** `commandengine.h` 承诺的是"**校验规则**完全一致"，
  规则指**判定**。两侧对同一条指令给出不同结论就是真分叉，不允许放松。
- **`why` 文案不同 → informational。** 只打印（QINFO / Python 侧 stdout）不判失败。
  `why` 是给人看的 **UI 文案**，不是协议语义。
- 判定一致 ≠ 看不见文案差异：差异仍逐条打印，并登记在本节台账里。

`ctest` 现状：**2/2 passed**，Qt 测试 `Totals: 47 passed, 0 failed`（33 条用例 × 判定 +
9 条 buildCmd + 5 个框架用例）。

守卫**不会退化成永远绿灯**（这是本口径最需要证明的一点）：把任一条用例的 `expect`
改错、或改 `commandengine.cpp` 的任一边界，`commandengine` 与 `protocol_parity_python`
都会立刻变红。实测方式：

```powershell
# 1) 故意把 tools/validate_cases.json 里 set_th_hys150_value_hys_plus_1 的 expect 改成 false
#    -> ctest 变红（【判定不一致 · FAIL】）
# 2) 改回 true -> ctest 变绿
```

### 已修复的判定分叉（原 P1，1 条）

用例 `set_th_value_float_noninteger_truncated`（原名 `..._noninteger_rejected`，
`{"action":"set_th","value":2000.9}`，hys=150）：

| 侧 | 修前 | 修后 |
| --- | --- | --- |
| C++（旧 `v.toInt()`） | `false`（2000.9 被当成 0 → `0 <= 150`） | `true`（向零截断得 2000） |
| Python `int(2000.9)` | `true` | `true` |
| 固件 `protocol.c` 的 `(int32_t)` | `true` | `true` |

Python 是声明的单一事实源，固件也同意向零截断，离群的是 Qt。`commandengine.cpp`
已改为**逐字镜像 Python 的运算顺序**（先截断 → 再查量程 → 再比回差），用例期望同步改为
`true`、用例名去掉 `_rejected`。

## 已知文案差异（非规则分叉，不影响判定）

> **这份台账是有价值的知识，不是因为不再判失败就可以删的东西。** 它记录了"同一判定、
> 两种措辞"的完整清单，供将来统一 UI 文案时参考；文案差异仍然在每次 `ctest` 里逐条打印。

### A. 失败路径措辞差异（2 条，**仍在**）

| 用例 | C++ why | Python why | 性质 |
| --- | --- | --- | --- |
| `action_empty_string` | 缺少 action 字段 | 未知动作：（空） | 分支归属差异：空串算"缺失"还是"未知" |
| `action_non_string_number` | 缺少 action 字段 | 未知动作：1 | `QJsonValue::toString()` 得空串 vs Python `str(1)` 非空 |

两条判定同为 `false`，只是措辞与分支归属不同。

### B. 失败路径措辞差异（4 条，**已随截断修复收敛**）

历史台账原有 **6** 条 why 差异，其中这 4 条是旧 `v.toInt()` 的连带后果；C++ 改成
向零截断（并去掉文案里多余的 "ADC" 字样）后已与 Python 逐字一致。保留在此以免知识丢失：

| 用例 | 历史 C++ why | Python why | 现状 |
| --- | --- | --- | --- |
| `set_th_hys150_value_negative` | 阈值 -1 超出 **ADC 量程** 0-4095 | 阈值 -1 超出**量程** 0-4095 | 已一致 |
| `set_th_hys150_value_adc_max_plus_1` | 阈值 4096 超出 **ADC 量程** 0-4095 | 阈值 4096 超出**量程** 0-4095 | 已一致 |
| `set_th_hys150_value_huge` | 阈值 99999 超出 **ADC 量程** 0-4095 | 阈值 99999 超出**量程** 0-4095 | 已一致 |
| `set_th_value_float_at_hys_rejected` | 阈值 0 必须大于回差 150 | 阈值 150 必须大于回差 150 | 已一致（C++ 现亦截断为 150） |

即：6 条历史差异 = 2 条仍在（上表 A）+ 4 条已收敛（本表 B）。

### C. 成功路径返回值约定差异（14 条，非规则分叉）

C++ 约定"成功不写 `why`"（保持空串），Python 约定"成功返回 `ok`"。33 条用例中 14 条
成功用例全属此类，判定 100% 一致。测试把它单列一档，避免淹没真正的判定差异。

## 边界语义（已在用例里钉住，与 `commandengine.cpp` 现行行为一致）

- `action` 缺失/空/null → false，why="缺少 action 字段"
- 6 个合法动作：`relay_on` `relay_off` `auto` `set_th` `get_state` `alarm_test`；`query` 必须被拒
- `set_th`：必须有 `value`、必须 `isDouble()`、**先向零截断取整**、`0 <= n <= 4095`、**`n > hys`（严格大于）**
- 非整数 `value`（如 `2000.9`）→ 截断为 `2000`，**放行**（与 Python `int()`、固件 `(int32_t)` 一致）
- `-0.5` → 截断为 `0`（**向零，不是 floor**），故 hys=-10 时放行；这也钉死了"必须先截断再查量程"
- `hys=0` 时 `value=0` → false，`value=1` → true
- 非 `set_th` 动作即使带越界 `value` 也不校验
