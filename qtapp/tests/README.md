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

## ★ 当前红灯：已确认的 C++ / Python 规则分叉

`parityWithPythonSchema` 目前**故意保持失败**。它拿同一张用例表分别喂给
`CommandEngine::validate` 与 `protocol_schema.validate_cmd`，比对判定与 why 文本。
这些差异是真实存在的，**没有修改任何一侧去让它变绿**。

### P1 判定不一致（1 条，真 bug）

| 用例 | 指令 | hys | C++ | Python |
| --- | --- | --- | --- | --- |
| `set_th_value_float_noninteger_rejected` | `{"action":"set_th","value":2000.9}` | 150 | `false`（why=阈值 0 必须大于回差 150） | `true`（why=ok） |

根因：`qtapp/src/commandengine.cpp:64` 用 `const int n = v.toInt();`。
Qt 的 `QJsonValue::toInt()` 对**非整数 double 不转换、直接返回 0**
（默认参数 `defaultValue = 0`），所以 `2000.9` 被当成 `0`，`0 <= 150` → 拒绝。
Python 侧 `int(2000.9)` 得 `2000` → 放行。

影响面：Qt 自身的下发路径走 `buildCmd(nodeId, action, int value)`，
写入的 `value` 一定是整数，因此**当前线上路径不会命中**；
只有外部 JSON（未来新增的输入源）把非整数阈值送进 `validate` 时才会暴露。
如果要对齐，应改成 `v.toDouble()` 再按明确规则取整——但那是行为变更，需先确认预期语义。

### P2 失败路径 why 文本不一致（6 条）

| 用例 | C++ why | Python why |
| --- | --- | --- |
| `action_empty_string` | 缺少 action 字段 | 未知动作：（空） |
| `action_non_string_number` | 缺少 action 字段 | 未知动作：1 |
| `set_th_hys150_value_negative` | 阈值 -1 超出 **ADC 量程** 0-4095 | 阈值 -1 超出**量程** 0-4095 |
| `set_th_hys150_value_adc_max_plus_1` | 阈值 4096 超出 **ADC 量程** 0-4095 | 阈值 4096 超出**量程** 0-4095 |
| `set_th_hys150_value_huge` | 阈值 99999 超出 **ADC 量程** 0-4095 | 阈值 99999 超出**量程** 0-4095 |
| `set_th_value_float_at_hys_rejected` | 阈值 0 必须大于回差 150 | 阈值 150 必须大于回差 150 |

判定全部相同，只是文案不同；其中最后一条是 P1 那个 `toInt()` 的连带后果。
前两类是"空/非字符串 action 算缺失还是算未知"的分支归属差异。

### P3 约定差异（13 条，不是规则分叉）

成功路径：C++ 约定"不传 `why` 指针就不写、传了也留空"，Python 约定"成功返回 `ok`"。
13 条成功用例全属此类，判定 100% 一致，测试把它单列一档以免淹没上面的真差异。

## 边界语义（已在用例里钉住，与 `commandengine.cpp` 现行行为一致）

- `action` 缺失/空/null → false，why="缺少 action 字段"
- 6 个合法动作：`relay_on` `relay_off` `auto` `set_th` `get_state` `alarm_test`；`query` 必须被拒
- `set_th`：必须有 `value`、必须 `isDouble()`、`0 <= n <= 4095`、**`n > hys`（严格大于）**
- `hys=0` 时 `value=0` → false，`value=1` → true
- 非 `set_th` 动作即使带越界 `value` 也不校验
