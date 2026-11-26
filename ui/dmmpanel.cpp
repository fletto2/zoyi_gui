#include "dmmpanel.h"

#include <QComboBox>
#include <QDateTime>
#include <QDir>
#include <QFileDialog>
#include <QFont>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QSignalBlocker>
#include <QStandardPaths>
#include <QTimer>
#include <QVBoxLayout>

#include <cmath>
#include <limits>

namespace {

// dmm::Function order (include/openosc/measurement/dmm/function.hpp).
struct Fn {
    const char *label;
    const char *cmd;
    const char *unit;
};
constexpr Fn kFns[] = {
    {"V DC", "CONF:VOLT:DC", "V"},       {"V AC", "CONF:VOLT:AC", "V"},
    {"Ohm", "CONF:RES", "Ohm"},          {"Cont", "CONF:CONT", "Ohm"},
    {"Diode", "CONF:DIOD", "V"},         {"Cap", "CONF:CAP", "F"},
    {"A DC", "CONF:CURR:DC", "A"},       {"A AC", "CONF:CURR:AC", "A"},
    {"mV DC", "CONF:MVOL:DC", "V"},      {"mV AC", "CONF:MVOL:AC", "V"},
    {"mA DC", "CONF:MCUR:DC", "A"},      {"mA AC", "CONF:MCUR:AC", "A"},
    {"Temp", "CONF:TEMP", "C"},
};
constexpr int kFnCount = int(sizeof kFns / sizeof *kFns);

// DataStatus order (include/openosc/data/status.hpp).
const char *const kHealth[] = {"OK", "NO DATA", "BUSY", "BAD ARG", "UNSUPPORTED",
                               "BAD PACKET", "OVERLOAD", "STALE", "IO ERROR"};

// "12.345 m" style: mantissa 1..999, 5 significant digits.
QString siText(double v, QString *prefix)
{
    struct P { double f; const char *p; };
    static const P ps[] = {{1e9, "G"}, {1e6, "M"}, {1e3, "k"}, {1.0, ""},
                           {1e-3, "m"}, {1e-6, "u"}, {1e-9, "n"}, {1e-12, "p"}};
    const double a = std::abs(v);
    const P *use = &ps[3];
    if (a > 0.0) {
        for (const P &p : ps)
            if (a >= p.f * 0.9999) {
                use = &p;
                break;
            }
        if (a < 1e-12)
            use = &ps[7];
    }
    const double m = v / use->f;
    const double am = std::abs(m);
    const int dec = am >= 100.0 ? 2 : am >= 10.0 ? 3 : 4;
    *prefix = QString::fromLatin1(use->p);
    return QString::number(m, 'f', dec);
}

} // namespace

// Rolling trend of the primary reading, autoscaled, newest on the right.
class DmmTrend : public QWidget {
public:
    explicit DmmTrend(QWidget *parent) : QWidget(parent) { setMinimumHeight(140); }
    void push(double v)
    {
        m_pts.append(v);
        if (m_pts.size() > 600)
            m_pts.remove(0, m_pts.size() - 600);
        update();
    }
    void clear()
    {
        m_pts.clear();
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.fillRect(rect(), QColor(QStringLiteral("#0d1726")));
        p.setPen(QColor(QStringLiteral("#1e293b")));
        for (int i = 1; i < 4; ++i)
            p.drawLine(0, height() * i / 4, width(), height() * i / 4);
        double lo = std::numeric_limits<double>::infinity(), hi = -lo;
        for (double v : m_pts)
            if (std::isfinite(v)) {
                lo = std::min(lo, v);
                hi = std::max(hi, v);
            }
        if (!(hi >= lo))
            return;
        if (hi - lo < 1e-12) {
            hi += 0.5 * std::max(std::abs(hi), 1e-6);
            lo -= 0.5 * std::max(std::abs(lo), 1e-6);
        }
        const double pad = (hi - lo) * 0.08;
        lo -= pad;
        hi += pad;
        QPainterPath path;
        bool pen = false;
        const int n = m_pts.size();
        for (int i = 0; i < n; ++i) {
            const double v = m_pts.at(i);
            if (!std::isfinite(v)) {
                pen = false;
                continue;
            }
            const double x = width() - 1 - (n - 1 - i) * (width() - 1) / 599.0;
            const double y = (hi - v) / (hi - lo) * (height() - 1);
            if (pen)
                path.lineTo(x, y);
            else
                path.moveTo(x, y);
            pen = true;
        }
        p.setRenderHint(QPainter::Antialiasing, true);
        p.setPen(QPen(QColor(QStringLiteral("#facc15")), 1.5));
        p.drawPath(path);
        p.setPen(QColor(QStringLiteral("#94a3b8")));
        QString pre;
        const QString hiT = siText(hi, &pre) + QLatin1Char(' ') + pre;
        const QString loT = siText(lo, &pre) + QLatin1Char(' ') + pre;
        p.drawText(4, 14, hiT);
        p.drawText(4, height() - 4, loT);
    }

private:
    QVector<double> m_pts;
};

DmmPanel::DmmPanel(QWidget *parent)
    : QFrame(parent)
{
    setObjectName(QStringLiteral("dmmPanel"));
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(24, 18, 24, 18);
    root->setSpacing(12);

    // Readout.
    auto *top = new QHBoxLayout;
    m_func = new QLabel(QStringLiteral("--"), this);
    m_func->setProperty("cls", QStringLiteral("cardTitle"));
    top->addWidget(m_func);
    top->addStretch(1);
    m_flags = new QLabel(this);
    m_flags->setTextFormat(Qt::RichText);
    top->addWidget(m_flags);
    root->addLayout(top);

    auto *big = new QHBoxLayout;
    big->addStretch(1);
    m_value = new QLabel(QStringLiteral("-----"), this);
    QFont f(QStringLiteral("Consolas"));
    f.setStyleHint(QFont::Monospace);
    f.setPixelSize(96);
    f.setBold(true);
    m_value->setFont(f);
    m_value->setStyleSheet(QStringLiteral("color:#facc15"));
    big->addWidget(m_value);
    m_unit = new QLabel(this);
    QFont fu = f;
    fu.setPixelSize(44);
    m_unit->setFont(fu);
    m_unit->setStyleSheet(QStringLiteral("color:#e2e8f0"));
    big->addWidget(m_unit, 0, Qt::AlignBottom);
    big->addStretch(1);
    root->addLayout(big);

    m_second = new QLabel(this);
    m_second->setAlignment(Qt::AlignCenter);
    QFont f2 = f;
    f2.setPixelSize(22);
    f2.setBold(false);
    m_second->setFont(f2);
    root->addWidget(m_second);

    // Function keys.
    auto *grid = new QGridLayout;
    grid->setSpacing(6);
    for (int i = 0; i < kFnCount; ++i) {
        auto *b = new QPushButton(QString::fromLatin1(kFns[i].label), this);
        b->setProperty("cls", QStringLiteral("seg"));
        b->setCheckable(true);
        b->setMinimumWidth(78);
        connect(b, &QPushButton::clicked, this, [this, i] {
            set(QString::fromLatin1(kFns[i].cmd));
            if (m_trend)
                m_trend->clear();
        });
        grid->addWidget(b, i / 7, i % 7);
        m_fnButtons << b;
    }
    root->addLayout(grid);

    // Range, hold, rel, secondary, temperature unit, stats, log.
    auto *row = new QHBoxLayout;
    row->setSpacing(8);
    m_auto = new QPushButton(tr("Auto"), this);
    m_auto->setProperty("cls", QStringLiteral("seg"));
    m_auto->setCheckable(true);
    m_auto->setToolTip(tr("Auto range. Picking a manual range turns it off"));
    connect(m_auto, &QPushButton::clicked, this,
            [this](bool on) { set(QStringLiteral("AUTO %1").arg(on ? 1 : 0)); });
    row->addWidget(m_auto);
    m_range = new QComboBox(this);
    m_range->setToolTip(tr("Manual measurement range"));
    m_range->setMinimumWidth(110);
    connect(m_range, QOverload<int>::of(&QComboBox::activated), this,
            [this](int idx) { set(QStringLiteral("RANG %1").arg(idx + 1)); });
    row->addWidget(m_range);
    m_hold = new QPushButton(tr("Hold"), this);
    m_rel = new QPushButton(tr("Rel"), this);
    m_freq = new QPushButton(tr("Hz"), this);
    for (QPushButton *b : {m_hold, m_rel, m_freq}) {
        b->setProperty("cls", QStringLiteral("seg"));
        b->setCheckable(true);
        row->addWidget(b);
    }
    m_hold->setToolTip(tr("Freeze the current reading"));
    m_rel->setToolTip(tr("Relative mode: zero on the current reading"));
    m_freq->setToolTip(tr("Show frequency as a secondary reading"));
    connect(m_hold, &QPushButton::clicked, this,
            [this](bool on) { set(QStringLiteral("SYST:DMM:HOLD %1").arg(on ? 1 : 0)); });
    connect(m_rel, &QPushButton::clicked, this,
            [this](bool on) { set(QStringLiteral("SYST:DMM:REL %1").arg(on ? 1 : 0)); });
    connect(m_freq, &QPushButton::clicked, this, [this](bool on) {
        set(on ? QStringLiteral("FUNC2 FREQ") : QStringLiteral("FUNC2 NONE"));
    });
    m_tunit = new QComboBox(this);
    m_tunit->addItems({QStringLiteral("deg C"), QStringLiteral("deg F")});
    m_tunit->setToolTip(tr("Temperature display unit"));
    connect(m_tunit, QOverload<int>::of(&QComboBox::activated), this, [this](int i) {
        set(QStringLiteral("SYST:DMM:TEMP:UNIT %1").arg(i ? QStringLiteral("F") : QStringLiteral("C")));
    });
    row->addWidget(m_tunit);
    row->addStretch(1);
    auto *reset = new QPushButton(tr("Reset stats"), this);
    connect(reset, &QPushButton::clicked, this,
            [this] { set(QStringLiteral("SYST:DMM:STAT:RES")); });
    row->addWidget(reset);
    m_log = new QPushButton(tr("Log to CSV"), this);
    m_log->setCheckable(true);
    m_log->setToolTip(tr("Write every new reading to a CSV file"));
    connect(m_log, &QPushButton::toggled, this, &DmmPanel::toggleLog);
    row->addWidget(m_log);
    root->addLayout(row);

    m_stats = new QLabel(QStringLiteral("--"), this);
    m_stats->setTextFormat(Qt::RichText);
    root->addWidget(m_stats);

    m_trend = new DmmTrend(this);
    root->addWidget(m_trend, 1);

    m_timer = new QTimer(this);
    m_timer->setInterval(250);
    connect(m_timer, &QTimer::timeout, this, &DmmPanel::poll);
}

void DmmPanel::setActive(bool on)
{
    m_active = on;
    setEnabled(on);
    if (on) {
        m_fn = -1;
        m_tick = 0;
        emit wantRequest(TUnit, QStringLiteral("SYST:DMM:TEMP:UNIT?"), false);
        poll();
        m_timer->start();
    } else {
        m_timer->stop();
        m_value->setText(QStringLiteral("-----"));
        m_unit->clear();
        m_second->clear();
        m_flags->clear();
        if (m_log->isChecked())
            m_log->setChecked(false);
    }
}

void DmmPanel::poll()
{
    if (!m_active)
        return;
    emit wantRequest(TRes, QStringLiteral("SYST:DMM:RES?"), false);
    if ((m_tick++ & 3) == 0)
        emit wantRequest(TStat, QStringLiteral("SYST:DMM:STAT?"), false);
}

void DmmPanel::set(const QString &cmd)
{
    if (!m_active)
        return;
    emit wantSet(cmd);
    emit wantRequest(TRes, QStringLiteral("SYST:DMM:RES?"), false);
}

QString DmmPanel::unitFor(int fn) const
{
    if (fn < 0 || fn >= kFnCount)
        return QString();
    if (fn == 12)
        return m_tunit->currentIndex() ? QStringLiteral("deg F") : QStringLiteral("deg C");
    return QString::fromLatin1(kFns[fn].unit);
}

void DmmPanel::onReply(int tag, const QString &text)
{
    if (!m_active)
        return;
    switch (tag) {
    case TRes:
        applyResult(text);
        break;
    case TStat:
        applyStats(text);
        break;
    case TRanges:
        applyRanges(text);
        break;
    case TUnit: {
        const QSignalBlocker b(m_tunit);
        m_tunit->setCurrentIndex(text.contains(QLatin1Char('F')) ? 1 : 0);
        break;
    }
    default:
        break;
    }
}

// SYST:DMM:RES? = fn,primary,health,secondary,qty2,rangeIdx,auto,hold,rel,seq,ts
void DmmPanel::applyResult(const QString &text)
{
    const QStringList p = text.split(QLatin1Char(','));
    if (p.size() < 10)
        return;
    bool ok = false;
    const int fn = p.at(0).toInt(&ok);
    if (!ok)
        return;
    if (fn != m_fn) {
        m_fn = fn;
        m_ranges.clear();
        m_range->clear();
        emit wantRequest(TRanges, QStringLiteral("SYST:DMM:RANG:LIST?"), false);
        if (m_trend)
            m_trend->clear();
    }
    for (int i = 0; i < m_fnButtons.size(); ++i)
        m_fnButtons.at(i)->setChecked(i == fn);
    m_func->setText(fn >= 0 && fn < kFnCount ? QString::fromLatin1(kFns[fn].label).toUpper()
                                             : tr("UNKNOWN FUNCTION"));

    const double v = p.at(1).toDouble(&ok);
    const int health = p.at(2).toInt();
    const QString unit = unitFor(fn);
    double logged = std::numeric_limits<double>::quiet_NaN();
    if (health == 6) {
        m_value->setText(QStringLiteral("OL"));
        m_unit->setText(unit);
    } else if (ok && std::isfinite(v)) {
        if (fn == 12) { // temperature: no SI prefix
            m_value->setText(QString::number(v, 'f', 1));
            m_unit->setText(unit);
        } else {
            QString pre;
            m_value->setText(siText(v, &pre));
            m_unit->setText(pre + unit);
        }
        logged = v;
    } else {
        m_value->setText(QStringLiteral("-----"));
        m_unit->setText(unit);
    }

    const double v2 = p.at(3).toDouble(&ok);
    if (ok && std::isfinite(v2) && p.at(4).toInt() == 5) {
        QString pre;
        m_second->setText(siText(v2, &pre) + QLatin1Char(' ') + pre + QStringLiteral("Hz"));
    } else {
        m_second->clear();
    }

    const int rIdx = p.at(5).toInt();
    const int autoR = p.at(6).toInt();
    const bool hold = p.at(7).toInt() != 0;
    const bool rel = p.at(8).toInt() != 0;
    {
        const QSignalBlocker b1(m_auto), b2(m_hold), b3(m_rel), b4(m_range);
        m_auto->setChecked(autoR == 1);
        m_hold->setChecked(hold);
        m_rel->setChecked(rel);
        if (rIdx >= 1 && rIdx <= m_range->count())
            m_range->setCurrentIndex(rIdx - 1);
    }
    {
        const QSignalBlocker b(m_freq);
        m_freq->setChecked(p.at(4).toInt() == 5);
    }
    QStringList flags;
    const QString chip = QStringLiteral("<span style=\"color:%1\"><b>%2</b></span>");
    if (autoR == 1)
        flags << chip.arg(QStringLiteral("#4ade80"), tr("AUTO"));
    else if (autoR == 0)
        flags << chip.arg(QStringLiteral("#fbbf24"), tr("MANUAL"));
    if (hold)
        flags << chip.arg(QStringLiteral("#f87171"), tr("HOLD"));
    if (rel)
        flags << chip.arg(QStringLiteral("#38bdf8"), tr("REL"));
    if (health != 0 && health != 6 && health >= 0 && health < 9)
        flags << chip.arg(QStringLiteral("#94a3b8"), QString::fromLatin1(kHealth[health]));
    m_flags->setText(flags.join(QStringLiteral("&nbsp;&nbsp;")));

    // Trend and log once per new meter reading (sequence).
    const unsigned long seq = p.at(9).toULong();
    if (seq != m_lastSeq) {
        m_lastSeq = seq;
        m_trend->push(logged);
        if (m_logFile.isOpen()) {
            m_logOut << QDateTime::currentDateTime().toString(Qt::ISODateWithMs) << ','
                     << (fn >= 0 && fn < kFnCount ? kFns[fn].label : "?") << ','
                     << (health == 6 ? QStringLiteral("OL")
                         : std::isfinite(logged) ? QString::number(logged, 'g', 10)
                                                 : QString())
                     << ',' << unit << '\n';
            m_logOut.flush();
        }
    }
}

// SYST:DMM:STAT? = count,min,max,avg,qty,seq
void DmmPanel::applyStats(const QString &text)
{
    const QStringList p = text.split(QLatin1Char(','));
    if (p.size() < 4)
        return;
    const QString unit = unitFor(m_fn);
    auto val = [&](const QString &s) {
        bool ok = false;
        const double v = s.toDouble(&ok);
        if (!ok || !std::isfinite(v))
            return QStringLiteral("--");
        if (m_fn == 12)
            return QString::number(v, 'f', 1) + QLatin1Char(' ') + unit;
        QString pre;
        return siText(v, &pre) + QLatin1Char(' ') + pre + unit;
    };
    const QString tag = QStringLiteral("<span style=\"color:#94a3b8\">%1</span> %2");
    m_stats->setText(QStringList{tag.arg(tr("n"), p.at(0)), tag.arg(tr("min"), val(p.at(1))),
                                 tag.arg(tr("max"), val(p.at(2))), tag.arg(tr("avg"), val(p.at(3)))}
                         .join(QStringLiteral("&nbsp;&nbsp;&nbsp;&nbsp;")));
}

// SYST:DMM:RANG:LIST? = count,max1,max2...
void DmmPanel::applyRanges(const QString &text)
{
    const QStringList p = text.split(QLatin1Char(','));
    const QSignalBlocker b(m_range);
    m_range->clear();
    m_ranges.clear();
    const QString unit = unitFor(m_fn);
    for (int i = 1; i < p.size(); ++i) {
        bool ok = false;
        const double v = p.at(i).toDouble(&ok);
        if (!ok)
            continue;
        m_ranges << v;
        QString pre;
        QString t = siText(v, &pre);
        while (t.contains(QLatin1Char('.')) && (t.endsWith(QLatin1Char('0')) || t.endsWith(QLatin1Char('.'))))
            t.chop(1);
        m_range->addItem(t + QLatin1Char(' ') + pre + unit);
    }
    m_range->setEnabled(m_range->count() > 0);
}

void DmmPanel::toggleLog(bool on)
{
    if (!on) {
        if (m_logFile.isOpen()) {
            m_logOut.flush();
            m_logFile.close();
        }
        return;
    }
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    const QString def = QDir(dir).filePath(
        QStringLiteral("zoyi_dmm_%1.csv")
            .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd_HHmmss"))));
    const QString path =
        QFileDialog::getSaveFileName(this, tr("Log DMM readings"), def, tr("CSV (*.csv)"));
    if (path.isEmpty()) {
        const QSignalBlocker b(m_log);
        m_log->setChecked(false);
        return;
    }
    m_logFile.setFileName(path);
    if (!m_logFile.open(QIODevice::WriteOnly | QIODevice::Text)) {
        const QSignalBlocker b(m_log);
        m_log->setChecked(false);
        return;
    }
    m_logOut.setDevice(&m_logFile);
    m_logOut << "time,function,value,unit\n";
}
