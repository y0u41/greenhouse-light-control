// qtapp/tests/test_commandengine.cpp
//
// CommandEngine::validate / buildCmd 的表驱动单元测试 + 跨语言一致性守卫。
//
// 设计要点：
//  1) 用例表单一事实源是 tools/validate_cases.json —— C++ 侧和
//     tools/check_validate_parity.py（Python 侧）读同一张表，避免测试里凭空
//     抄一份"我以为的规则"。
//  2) 每条 false 用例都断言 why 非空，并核对 why 文案确实来自对应分支
//     （commandengine.cpp 的行号记在用例的 rule 字段里）。
//  3) ★ 一致性守卫：同一张表喂给 Proto/CommandEngine，再与 tools/
//     validate_parity_python.json（由 protocol_schema.validate_cmd 生成）逐条
//     比对**判定**。判定不一致时这里红灯，**不修改任何一侧去粉饰**。
//     why 文案差异按已拍板口径降级为 informational（打印但不算失败），
//     台账见 qtapp/tests/README.md「已知文案差异（非规则分叉，不影响判定）」。
//
// QTEST_APPLESS_MAIN：validate/buildCmd 是纯静态函数，不需要 QObject 事件循环，
// 也不需要 QApplication（无 GUI）。用 QProcess 起 Python 也不需要事件循环（同步 API）。

#include <QtTest/QtTest>

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>
#include <QProcess>
#include <QProcessEnvironment>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QTemporaryFile>
#include <QVector>

#include "commandengine.h"
#include "protocol_actions.h"

namespace {

// ---------- 期望判定 ----------
enum class Expect {
    Ok,                     // 必须返回 true
    OkButWhyMustMismatch,   // 返回 false，且必须给出非空 why
};

struct Case {
    QString     id;
    QJsonObject cmd;
    int         hys    = Proto::HYS_DEFAULT;
    Expect      expect = Expect::Ok;
    QString     rule;   // commandengine.cpp 对应分支，便于失败时定位
    QString     note;
};

// ---------- 定位工程根 ----------
// 源码目录布局（qtapp/tests/xxx.cpp）与构建目录布局（qtapp/build/xxx.exe）都覆盖。
QString findProjectRoot()
{
    QStringList seeds;
    seeds << QDir::currentPath()
          << QCoreApplication::applicationDirPath()
          << QStringLiteral(QT_TEST_SRC_DIR);   // 由 CMake 注入 qtapp/tests 的绝对路径

    for (const QString &seed : seeds) {
        QDir d(seed);
        // 向上最多 6 层找 tools/validate_cases.json
        for (int i = 0; i < 6; ++i) {
            if (QFileInfo::exists(d.filePath(QStringLiteral("tools/validate_cases.json")))) {
                return QDir::cleanPath(d.absolutePath());
            }
            if (!d.cdUp()) {
                break;
            }
        }
    }
    return QString();
}

QString testCaseFile()
{
    const QString root = findProjectRoot();
    if (root.isEmpty()) {
        return QString();
    }
    return QDir(root).filePath(QStringLiteral("tools/validate_cases.json"));
}

QString pythonSnapshotFile()
{
    const QString root = findProjectRoot();
    if (root.isEmpty()) {
        return QString();
    }
    return QDir(root).filePath(QStringLiteral("tools/validate_parity_python.json"));
}

QString parityScriptFile()
{
    const QString root = findProjectRoot();
    if (root.isEmpty()) {
        return QString();
    }
    return QDir(root).filePath(QStringLiteral("tools/check_validate_parity.py"));
}

// 任务给定的 Python 解释器；环境变量 QTPY 可覆盖。
QString pythonExe()
{
    const QString fromEnv = QProcessEnvironment::systemEnvironment()
                                .value(QStringLiteral("QTPY"));
    if (!fromEnv.isEmpty() && QFileInfo::exists(fromEnv)) {
        return fromEnv;
    }
    const QString pinned = QStringLiteral(
        "C:/Users/t3205/.dsh/dsh-runtimes/dsh-primary-runtime/dependencies/python/python.exe");
    if (QFileInfo::exists(pinned)) {
        return pinned;
    }
    return QStringLiteral("python");
}

const char *kPinnedPython =
    "C:/Users/t3205/.dsh/dsh-runtimes/dsh-primary-runtime/dependencies/python/python.exe";

// ---------- 用例表加载 ----------
QVector<Case> loadCases(QString *err)
{
    QVector<Case> out;
    const QString path = testCaseFile();
    if (path.isEmpty()) {
        *err = QStringLiteral("找不到 tools/validate_cases.json：请从工程内运行测试"
                              "（或设置 QT_TEST_SRC_DIR 指向 qtapp/tests）");
        return out;
    }

    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        *err = QStringLiteral("无法打开 %1：%2").arg(path, f.errorString());
        return out;
    }
    const QByteArray raw = f.readAll();
    f.close();

    QJsonParseError pe{};
    const QJsonDocument doc = QJsonDocument::fromJson(raw, &pe);
    if (pe.error != QJsonParseError::NoError || !doc.isObject()) {
        *err = QStringLiteral("%1 不是合法 JSON 对象：%2").arg(path, pe.errorString());
        return out;
    }

    const QJsonArray arr = doc.object().value(QStringLiteral("cases")).toArray();
    if (arr.isEmpty()) {
        *err = QStringLiteral("%1 的 cases 为空").arg(path);
        return out;
    }

    QSet<QString> seen;
    for (const QJsonValue &v : arr) {
        const QJsonObject o = v.toObject();
        Case c;
        c.id     = o.value(QStringLiteral("id")).toString();
        c.cmd    = o.value(QStringLiteral("cmd")).toObject();
        c.hys    = o.value(QStringLiteral("hys")).toInt();
        c.expect = o.value(QStringLiteral("expect")).toBool() ? Expect::Ok
                                                             : Expect::OkButWhyMustMismatch;
        c.rule   = o.value(QStringLiteral("rule")).toString();
        c.note   = o.value(QStringLiteral("note")).toString();
        if (c.id.isEmpty()) {
            *err = QStringLiteral("%1 里有用例缺少 id").arg(path);
            return {};
        }
        if (seen.contains(c.id)) {
            *err = QStringLiteral("%1 里用例 id 重复：%2").arg(path, c.id);
            return {};
        }
        seen.insert(c.id);
        out.append(c);
    }
    return out;
}

// ---------- why 文案归属检查 ----------
// 仅用于"哪条分支给出的原因"的归属断言；两侧 why 文案差异由 parityWithPythonSchema
// 打印为 informational（不判失败，见该用例注释里的比对口径）。
enum class WhyKind {
    None,          // 不检查（成功用例）
    ActionProblem, // 分支 1/2：缺少 action 或未知动作
    SetThParam,    // 分支 3a/3b：缺 value 或 value 非数字
    SetThRange,    // 分支 3c：越 ADC 量程
    SetThHys,      // 分支 3d：不严格大于 hys
    Any,           // 只要非空即可
};

bool whyMatchesKind(const QString &why, WhyKind kind)
{
    switch (kind) {
    case WhyKind::None:
        return true;
    case WhyKind::Any:
        return !why.isEmpty();
    case WhyKind::ActionProblem:
        // 覆盖 "缺少 action 字段"（分支 1）与 "未知动作：xxx"（分支 2）
        return why.contains(QStringLiteral("action"))
            || why.contains(QStringLiteral("未知动作"));
    case WhyKind::SetThParam:
        return why.contains(QStringLiteral("value"));
    case WhyKind::SetThRange:
        return why.contains(QStringLiteral("量程"));
    case WhyKind::SetThHys:
        return why.contains(QStringLiteral("回差"));
    }
    return false;
}

// 用例 id -> why 归属。新增用例时若忘了归类，会落到 Any（只要求非空）。
WhyKind whyKindFor(const QString &id)
{
    if (id.startsWith(QStringLiteral("action_missing"))
        || id.startsWith(QStringLiteral("action_empty_string"))
        || id.startsWith(QStringLiteral("action_null"))) {
        return WhyKind::ActionProblem;
    }
    if (id.startsWith(QStringLiteral("action_unknown"))) {
        return WhyKind::ActionProblem;
    }
    if (id.contains(QStringLiteral("missing_value")) || id.contains(QStringLiteral("value_null"))
        || id.contains(QStringLiteral("value_string")) || id.contains(QStringLiteral("value_bool"))) {
        return WhyKind::SetThParam;
    }
    if (id.contains(QStringLiteral("adc_max_plus_1")) || id.contains(QStringLiteral("negative"))
        || id.contains(QStringLiteral("huge"))) {
        return WhyKind::SetThRange;
    }
    return WhyKind::Any;
}

// ---------- 与 Python 侧比对用的记录 ----------
struct SnapRecord {
    QString id;
    bool    actual = false;
    QString why;
};

// 调 tools/check_validate_parity.py --emit-json，拿 protocol_schema.validate_cmd 的真实判定。
bool runPythonSnapshot(const QString &casesPath, QJsonArray *records, QString *err)
{
    const QString script = parityScriptFile();
    if (script.isEmpty()) {
        *err = QStringLiteral("找不到 tools/check_validate_parity.py");
        return false;
    }

    QProcess proc;
    proc.setProgram(pythonExe());
    proc.setArguments({script, casesPath, QStringLiteral("--emit-json")});
    proc.setProcessChannelMode(QProcess::SeparateChannels);
    proc.start();
    if (!proc.waitForStarted(15000)) {
        *err = QStringLiteral("无法启动 Python（%1）：%2；可设 QTPY 环境变量指定解释器")
                   .arg(pythonExe(), proc.errorString());
        return false;
    }
    if (!proc.waitForFinished(60000)) {
        proc.kill();
        proc.waitForFinished(3000);
        *err = QStringLiteral("Python 一致性脚本超时");
        return false;
    }
    if (proc.exitStatus() != QProcess::NormalExit || proc.exitCode() != 0) {
        *err = QStringLiteral("Python 一致性脚本退出码 %1；stderr:\n%2")
                   .arg(proc.exitCode())
                   .arg(QString::fromUtf8(proc.readAllStandardError()));
        return false;
    }

    const QByteArray out = proc.readAllStandardOutput();
    QJsonParseError pe{};
    const QJsonDocument doc = QJsonDocument::fromJson(out, &pe);
    if (pe.error != QJsonParseError::NoError || !doc.isObject()) {
        *err = QStringLiteral("Python 输出不是合法 JSON：%1；前 200 字节：%2")
                   .arg(pe.errorString(), QString::fromUtf8(out.left(200)));
        return false;
    }
    *records = doc.object().value(QStringLiteral("records")).toArray();
    if (records->isEmpty()) {
        *err = QStringLiteral("Python 快照 records 为空");
        return false;
    }
    return true;
}

} // namespace

class TestCommandEngine : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();

    // ---- 表驱动主用例 ----
    void validate_data();
    void validate();

    // ---- buildCmd ----
    void buildCmd_data();
    void buildCmd();

    // ---- ★ 跨语言一致性守卫 ----
    void parityWithPythonSchema();
    void paritySnapshotIsCurrent();
    void schemaVocabularyMatchesCpp();

private:
    QVector<Case> m_cases;
};

void TestCommandEngine::initTestCase()
{
    QString err;
    m_cases = loadCases(&err);
    QVERIFY2(!m_cases.isEmpty(), qPrintable(err));

    // 用例表本身的完整性：false 用例必须登记 rule + note，便于失败定位
    for (const Case &c : m_cases) {
        QVERIFY2(!c.rule.isEmpty(),
                 qPrintable(QStringLiteral("用例 %1 缺少 rule 字段").arg(c.id)));
    }

    qInfo().noquote() << "已加载用例表:" << testCaseFile();
    qInfo().noquote() << "用例条数:" << m_cases.size();
    qInfo().noquote() << "Python 解释器:" << pythonExe();
}

void TestCommandEngine::validate_data()
{
    QTest::addColumn<QString>("id");
    QTest::addColumn<QJsonObject>("cmd");
    QTest::addColumn<int>("hys");
    QTest::addColumn<bool>("expectOk");

    for (const Case &c : m_cases) {
        QTest::newRow(qPrintable(c.id))
            << c.id << c.cmd << c.hys << (c.expect == Expect::Ok);
    }
}

void TestCommandEngine::validate()
{
    QFETCH(QString, id);
    QFETCH(QJsonObject, cmd);
    QFETCH(int, hys);
    QFETCH(bool, expectOk);

    QString why;
    const bool ok = CommandEngine::validate(cmd, hys, &why);

    QVERIFY2(ok == expectOk,
             qPrintable(QStringLiteral("[%1] validate 期望 %2 实得 %3；why=%4；rule=%5；%6")
                            .arg(id,
                                 expectOk ? QStringLiteral("true") : QStringLiteral("false"),
                                 ok ? QStringLiteral("true") : QStringLiteral("false"),
                                 why.isEmpty() ? QStringLiteral("<空>") : why)));

    if (expectOk) {
        // 成功时不应写入 why
        QVERIFY2(why.isEmpty(),
                 qPrintable(QStringLiteral("[%1] 通过校验却写了 why=%2").arg(id, why)));
    } else {
        // 每条 false 用例都必须给出非空原因（否则 UI 无法提示）
        QVERIFY2(!why.isEmpty(),
                 qPrintable(QStringLiteral("[%1] 校验失败但 why 为空（UI 将无提示）").arg(id)));
        QVERIFY2(whyMatchesKind(why, whyKindFor(id)),
                 qPrintable(QStringLiteral("[%1] why 文案不属于期望分支：why=%2").arg(id, why)));
    }
}

void TestCommandEngine::buildCmd_data()
{
    QTest::addColumn<int>("nodeId");
    QTest::addColumn<QString>("action");
    QTest::addColumn<int>("value");
    QTest::addColumn<bool>("expectHasValue");
    QTest::addColumn<int>("expectValue");

    // set_th 必须带 value
    QTest::newRow("set_th_node1_2500") << 1 << QStringLiteral("set_th") << 2500 << true << 2500;
    QTest::newRow("set_th_node7_default") << 7 << QStringLiteral("set_th") << -1 << true << -1;
    QTest::newRow("set_th_node3_hys_default")
        << 3 << QStringLiteral("set_th") << Proto::TH_DEFAULT << true << Proto::TH_DEFAULT;
    // 其余 5 个动作不带 value，即使传了 value
    QTest::newRow("relay_on_no_value") << 1 << QStringLiteral("relay_on") << 2500 << false << 0;
    QTest::newRow("relay_off_no_value") << 2 << QStringLiteral("relay_off") << -1 << false << 0;
    QTest::newRow("auto_no_value") << 3 << QStringLiteral("auto") << 0 << false << 0;
    QTest::newRow("get_state_no_value") << 4 << QStringLiteral("get_state") << 999 << false << 0;
    QTest::newRow("alarm_test_no_value") << 5 << QStringLiteral("alarm_test") << 4095 << false
                                         << 0;
    // 未知动作按"其它动作"处理：只有 node/type/action
    QTest::newRow("unknown_no_value") << 6 << QStringLiteral("nope") << 100 << false << 0;
}

void TestCommandEngine::buildCmd()
{
    QFETCH(int, nodeId);
    QFETCH(QString, action);
    QFETCH(int, value);
    QFETCH(bool, expectHasValue);
    QFETCH(int, expectValue);

    const QJsonObject o = CommandEngine::buildCmd(nodeId, action, value);

    // node / type / action 三个字段必须正确
    QCOMPARE(o.value(QStringLiteral("node")).toInt(), nodeId);
    QCOMPARE(o.value(QStringLiteral("type")).toString(), QLatin1String(Proto::TYPE_CMD));
    QCOMPARE(o.value(QStringLiteral("action")).toString(), action);

    // value 字段的有无
    QCOMPARE(o.contains(QStringLiteral("value")), expectHasValue);
    if (expectHasValue) {
        QVERIFY2(o.value(QStringLiteral("value")).isDouble(),
                 "set_th 的 value 必须是 JSON 数字，不能是字符串");
        QCOMPARE(o.value(QStringLiteral("value")).toInt(), expectValue);
    }

    // 字段数：node/type/action 固定 3 个，set_th 额外带 value
    QCOMPARE(o.keys().size(), expectHasValue ? 4 : 3);
}

// =====================================================================
// ★ 跨语言一致性守卫
// =====================================================================

// 同一张用例表 -> C++ CommandEngine::validate 与
// Python protocol_schema.validate_cmd，逐条比对**判定**。
//
// ★ 比对口径（已拍板）★
//   - 判定不同 -> 必须 FAIL。这是契约：commandengine.h 承诺"校验规则完全一致"，
//     规则指判定。两侧对同一条指令给出不同结论就是真分叉，不允许放松。
//   - why 文案不同 -> 只作为 informational 输出（qInfo），**不判失败**。
//     why 是给人看的 UI 文案，不是协议语义；已知的固定文案差异台账记在
//     qtapp/tests/README.md「已知文案差异（非规则分叉，不影响判定）」一节。
//     不判失败 ≠ 不知道：差异仍然逐条打印，改了 why 文案在这里看得见。
// 任何一侧改规则都仍会让本用例红灯，**不改任何一侧去粉饰**。
void TestCommandEngine::parityWithPythonSchema()
{
    const QString casesPath = testCaseFile();
    QVERIFY2(!casesPath.isEmpty(), "找不到 tools/validate_cases.json");

    QJsonArray records;
    QString err;
    QVERIFY2(runPythonSnapshot(casesPath, &records, &err), qPrintable(err));

    QCOMPARE(records.size(), m_cases.size());

    QStringList verdictDiffs;      // P1：判定不一致（真 bug，FAIL）
    QStringList whyFailDiffs;      // P2：失败路径 why 文案不一致（informational）
    QStringList whyOkDiffs;        // P3：成功路径 why 写法约定不同（informational）
    QStringList missing;           // 用例表里有、Python 快照里没有（FAIL）

    QHash<QString, SnapRecord> pyById;
    for (const QJsonValue &v : records) {
        const QJsonObject r = v.toObject();
        SnapRecord rec;
        rec.id     = r.value(QStringLiteral("id")).toString();
        rec.actual = r.value(QStringLiteral("actual")).toBool();
        rec.why    = r.value(QStringLiteral("why")).toString();
        pyById.insert(rec.id, rec);
    }

    for (const Case &c : m_cases) {
        if (!pyById.contains(c.id)) {
            missing << c.id;
            continue;
        }
        const SnapRecord py = pyById.value(c.id);

        QString cppWhy;
        const bool cppOk = CommandEngine::validate(c.cmd, c.hys, &cppWhy);

        if (cppOk != py.actual) {
            verdictDiffs << QStringLiteral(
                                "用例 %1: C++ validate=%2 (why=%3) | Python validate_cmd=%4 (why=%5)"
                                " | hys=%6 | rule=%7 | note=%8")
                                .arg(c.id,
                                     cppOk ? QStringLiteral("true") : QStringLiteral("false"),
                                     cppWhy.isEmpty() ? QStringLiteral("<空>") : cppWhy,
                                     py.actual ? QStringLiteral("true") : QStringLiteral("false"),
                                     py.why)
                                .arg(c.hys)
                                .arg(c.rule, c.note);
        } else if (cppWhy != py.why) {
            const QString line = QStringLiteral("用例 %1: 判定同为 %2 —— C++ why=%3 | Python why=%4 | rule=%5")
                                     .arg(c.id,
                                          cppOk ? QStringLiteral("true") : QStringLiteral("false"),
                                          cppWhy.isEmpty() ? QStringLiteral("<空>") : cppWhy,
                                          py.why,
                                          c.rule);
            if (cppOk) {
                // 成功路径：C++ 约定"成功不写 why"，Python 约定"成功返回 ok"。
                // 这是返回值约定的差别，不是规则分叉。
                whyOkDiffs << line;
            } else {
                whyFailDiffs << line;
            }
        }
    }

    // ---- informational：why 文案差异（不算失败，但必须看得见）----
    if (!whyFailDiffs.isEmpty() || !whyOkDiffs.isEmpty()) {
        qInfo().noquote() << QStringLiteral("【已知文案差异（非规则分叉，不影响判定）】"
                                            " why 文案不同 %1 条；判定与协议语义无关，"
                                            "仅 UI 提示措辞不同：")
                                 .arg(whyFailDiffs.size() + whyOkDiffs.size());
        if (!whyFailDiffs.isEmpty()) {
            qInfo().noquote() << QStringLiteral("  · 失败路径 why 措辞不同：%1 条")
                                     .arg(whyFailDiffs.size());
            for (const QString &l : whyFailDiffs) {
                qInfo().noquote() << QStringLiteral("    - ") + l;
            }
        }
        if (!whyOkDiffs.isEmpty()) {
            qInfo().noquote() << QStringLiteral("  · 成功路径 why 写法约定不同"
                                                "（C++ 留空 / Python 返回 \"ok\"）：%1 条")
                                     .arg(whyOkDiffs.size());
            for (const QString &l : whyOkDiffs) {
                qInfo().noquote() << QStringLiteral("    - ") + l;
            }
        }
        qInfo().noquote() << QStringLiteral("  台账：qtapp/tests/README.md"
                                            "「已知文案差异（非规则分叉，不影响判定）」");
    } else {
        qInfo().noquote() << QStringLiteral("一致性守卫：%1 条用例的判定与 why 文本逐条一致")
                                 .arg(m_cases.size());
    }

    // ---- 硬门槛：判定必须一致 ----
    if (!missing.isEmpty()) {
        QFAIL(qPrintable(QStringLiteral("Python 快照缺少用例（用例表与快照不同步，"
                                        "请重跑 tools/check_validate_parity.py --write）：\n  %1")
                             .arg(missing.join(QStringLiteral("\n  ")))));
    }

    if (!verdictDiffs.isEmpty()) {
        QStringList msg;
        msg << QStringLiteral("★ 发现 C++ 与 Python 判定分叉（未做任何粉饰）★")
            << QStringLiteral("")
            << QStringLiteral("【判定不一致 · FAIL】%1 条 —— 这是真 bug，两侧对同一条指令"
                              "给出不同结论（契约就是判定）：")
                   .arg(verdictDiffs.size());
        msg << verdictDiffs;
        msg << QStringLiteral("")
            << QStringLiteral("（另有 %1 条 why 文案差异属 informational，不影响判定，详见上方 QINFO）")
                   .arg(whyFailDiffs.size() + whyOkDiffs.size());
        QFAIL(qPrintable(msg.join(QLatin1Char('\n'))));
    }

    qInfo().noquote() << QStringLiteral("一致性守卫：%1 条用例的**判定**逐条一致"
                                        "（why 文案差异另计，不判失败）")
                             .arg(m_cases.size());
}

// 快照文件必须与当前用例表 + protocol_schema.py 同步。
// 否则 parityWithPythonSchema 比对的可能是过期数据。
void TestCommandEngine::paritySnapshotIsCurrent()
{
    const QString snapPath = pythonSnapshotFile();
    QVERIFY2(!snapPath.isEmpty(), "找不到 tools/validate_parity_python.json 的位置");

    QFile f(snapPath);
    QVERIFY2(f.open(QIODevice::ReadOnly),
             qPrintable(QStringLiteral("缺少快照 %1；请运行：python tools/check_validate_parity.py --write")
                            .arg(snapPath)));
    const QByteArray committed = f.readAll();
    f.close();

    QJsonParseError pe{};
    const QJsonDocument doc = QJsonDocument::fromJson(committed, &pe);
    QVERIFY2(pe.error == QJsonParseError::NoError && doc.isObject(),
             qPrintable(QStringLiteral("快照不是合法 JSON：%1").arg(pe.errorString())));

    const QJsonObject obj = doc.object();
    QCOMPARE(obj.value(QStringLiteral("format")).toInt(), 1);
    QCOMPARE(obj.value(QStringLiteral("case_count")).toInt(), m_cases.size());
    QCOMPARE(obj.value(QStringLiteral("adc_max")).toInt(), Proto::ADC_MAX);
    QCOMPARE(obj.value(QStringLiteral("hys_default")).toInt(), Proto::HYS_DEFAULT);

    // 快照记录的用例 id 集合必须与用例表一致
    QStringList snapIds;
    for (const QJsonValue &v : obj.value(QStringLiteral("records")).toArray()) {
        snapIds << v.toObject().value(QStringLiteral("id")).toString();
    }
    QStringList tableIds;
    for (const Case &c : m_cases) {
        tableIds << c.id;
    }
    snapIds.sort();
    tableIds.sort();
    QVERIFY2(snapIds == tableIds,
             qPrintable(QStringLiteral("快照与用例表不同步（记录 %1 条 / 用例 %2 条），"
                                       "请运行：python tools/check_validate_parity.py --write")
                            .arg(snapIds.size())
                            .arg(tableIds.size())));

    // 再确认快照记的 why 就是当前 schema 会给出的 why（防止改了 py 没重跑快照）
    QJsonArray fresh;
    QString err;
    QVERIFY2(runPythonSnapshot(testCaseFile(), &fresh, &err), qPrintable(err));
    const QJsonArray stale = obj.value(QStringLiteral("records")).toArray();
    QCOMPARE(fresh.size(), stale.size());
    for (int i = 0; i < fresh.size(); ++i) {
        const QJsonObject a = fresh.at(i).toObject();
        const QJsonObject b = stale.at(i).toObject();
        QVERIFY2(a.value(QStringLiteral("id")).toString() == b.value(QStringLiteral("id")).toString(),
                 "快照记录顺序/id 与实时结果不符");
        QVERIFY2(a.value(QStringLiteral("actual")).toBool() == b.value(QStringLiteral("actual")).toBool()
                     && a.value(QStringLiteral("why")).toString() == b.value(QStringLiteral("why")).toString(),
                 qPrintable(QStringLiteral("快照已过期（用例 %1，实得 why=%2 / 快照 why=%3）；"
                                           "请运行：python tools/check_validate_parity.py --write")
                                .arg(a.value(QStringLiteral("id")).toString(),
                                     a.value(QStringLiteral("why")).toString(),
                                     b.value(QStringLiteral("why")).toString())));
    }
}

// 词汇/常量的一致性：protocol_actions.h 与 protocol_schema.py 必须同源。
//
// 注意这里探针问的是**动作词汇是否在 protocol_schema.ACTION_SET 里**，
// 所以探针指令必须是"只差动作名就合法"的完整指令：set_th 必须带上合法 value，
// 否则 protocol_schema.validate_cmd 会因为缺 value 而 false —— 那是"参数校验"的
// 结论，不是"动作名不认识"，会把本用例变成假阳性。
void TestCommandEngine::schemaVocabularyMatchesCpp()
{
    // 6 个合法动作，每个都构造成"其余字段齐备、只因动作名而通过/拒绝"的最小指令
    struct Vocab {
        const char *action;
        QJsonObject cmd;
    };
    const QVector<Vocab> known = {
        { Proto::ACTION_RELAY_ON,   {} },
        { Proto::ACTION_RELAY_OFF,  {} },
        { Proto::ACTION_AUTO,       {} },
        // set_th 必须给合法 value，否则 schema 会以"缺 value"拒绝
        { Proto::ACTION_SET_TH,     QJsonObject{ { QStringLiteral("value"), Proto::TH_DEFAULT } } },
        { Proto::ACTION_GET_STATE,  {} },
        { Proto::ACTION_ALARM_TEST, {} },
    };
    // 非法动作名（末尾空串代表 action 缺失）
    const QStringList unknown = { QStringLiteral("query"), QStringLiteral("Relay_On"),
                                  QStringLiteral("nope"),  QString() };

    QJsonArray probe;
    int i = 0;
    auto pushProbe = [&probe, &i](const QString &action, const QJsonObject &extra, bool isKnown) {
        QJsonObject cmd = extra;
        if (!action.isNull()) {
            cmd.insert(QStringLiteral("action"), action);
        }
        QJsonObject o;
        o.insert(QStringLiteral("id"), QStringLiteral("vocab_%1").arg(i++));
        o.insert(QStringLiteral("cmd"), cmd);
        o.insert(QStringLiteral("hys"), Proto::HYS_DEFAULT);
        o.insert(QStringLiteral("expect"), isKnown);   // 必须是 JSON 布尔
        o.insert(QStringLiteral("rule"), QStringLiteral("vocabulary-probe"));
        o.insert(QStringLiteral("note"), QStringLiteral("协议词汇一致性探针"));
        probe.append(o);
    };
    for (const Vocab &v : known) {
        pushProbe(QLatin1String(v.action), v.cmd, true);
    }
    for (const QString &a : unknown) {
        pushProbe(a, {}, false);
    }

    QTemporaryFile tmp(QDir::tempPath() + QStringLiteral("/validate_probe_XXXXXX.json"));
    tmp.setAutoRemove(true);
    QVERIFY(tmp.open());
    tmp.write(QJsonDocument(QJsonObject{ { QStringLiteral("cases"), probe } })
                  .toJson(QJsonDocument::Compact));
    tmp.flush();
    tmp.close();

    QJsonArray pyRecords;
    QString err;
    QVERIFY2(runPythonSnapshot(tmp.fileName(), &pyRecords, &err), qPrintable(err));
    QCOMPARE(pyRecords.size(), known.size() + unknown.size());

    // 前 6 条：Python 与 C++ 都必须接受；后 4 条：都必须拒绝
    for (int k = 0; k < known.size(); ++k) {
        const QString a = QLatin1String(known.at(k).action);
        const bool pyOk = pyRecords.at(k).toObject().value(QStringLiteral("actual")).toBool();
        const QString pyWhy = pyRecords.at(k).toObject().value(QStringLiteral("why")).toString();
        QVERIFY2(pyOk,
                 qPrintable(QStringLiteral("protocol_schema 不认动作 [%1]（protocol_actions.h 有而 schema 没有）；"
                                           "python why=%2")
                                .arg(a, pyWhy)));
        QVERIFY2(Proto::isValidAction(a),
                 qPrintable(QStringLiteral("Proto::isValidAction 拒绝了 [%1]").arg(a)));
    }
    for (int k = 0; k < unknown.size(); ++k) {
        const int idx = known.size() + k;
        const QString a = unknown.at(k).isNull() ? QStringLiteral("<缺失>") : unknown.at(k);
        const bool pyOk = pyRecords.at(idx).toObject().value(QStringLiteral("actual")).toBool();
        const QString pyWhy = pyRecords.at(idx).toObject().value(QStringLiteral("why")).toString();
        QVERIFY2(!pyOk,
                 qPrintable(QStringLiteral("protocol_schema 意外接受了非法 action [%1]；why=%2")
                                .arg(a, pyWhy)));
    }

    // 常量同源
    // （adc_max / hys_default 由 paritySnapshotIsCurrent 断言）
    QCOMPARE(Proto::ADC_MAX, 4095);
    QCOMPARE(Proto::HYS_DEFAULT, 150);
    QCOMPARE(Proto::TH_DEFAULT, 2000);
    QVERIFY(!Proto::isValidAction(QStringLiteral("query")));
    QVERIFY(Proto::isValidAction(QLatin1String(Proto::ACTION_GET_STATE)));
}

QTEST_APPLESS_MAIN(TestCommandEngine)

#include "test_commandengine.moc"
