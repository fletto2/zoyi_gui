#include "genpanel.h"

#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QStyle>
#include <QVBoxLayout>

namespace {
// SOUR:FUNC? replies, in firmware Shape order (src/scpi/generator.cpp).
const char *const kShapes[] = {"SIN", "SQU", "TRI", "HALF", "POS", "RAMP"};
const char *const kShapeNames[] = {"Sine", "Square", "Triangle", "Half sine", "Positive sine",
                                   "Ramp"};
const char *const kProfs[] = {"DED", "LOW", "HIGH", "CUST"};
const char *const kProfNames[] = {"Dedicated", "Low", "High", "Custom"};
} // namespace

GenPanel::GenPanel(QWidget *parent)
    : QFrame(parent)
{
    setObjectName(QStringLiteral("genPanel"));
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(24, 18, 24, 18);
    root->setSpacing(14);

    auto *title = new QLabel(tr("GENERATOR"), this);
    title->setProperty("cls", QStringLiteral("cardTitle"));
    root->addWidget(title);

    m_out = new QPushButton(tr("Output OFF"), this);
    m_out->setCheckable(true);
    m_out->setMinimumHeight(48);
    m_out->setProperty("cls", QStringLiteral("primary"));
    connect(m_out, &QPushButton::clicked, this,
            [this](bool on) { set(QStringLiteral("OUTP %1").arg(on ? 1 : 0)); });
    root->addWidget(m_out);

    auto *form = new QFormLayout;
    form->setLabelAlignment(Qt::AlignRight);
    form->setHorizontalSpacing(14);
    form->setVerticalSpacing(8);

    m_shape = new QComboBox(this);
    for (const char *n : kShapeNames)
        m_shape->addItem(tr(n));
    connect(m_shape, QOverload<int>::of(&QComboBox::activated), this, [this](int i) {
        if (!m_active)
            return;
        const QString cmd = QStringLiteral("SOUR:FUNC %1").arg(QString::fromLatin1(kShapes[i]));
        if (m_outOn) {
            emit wantSet(QStringLiteral("OUTP 0"));
            emit wantSet(cmd);
            emit wantSet(QStringLiteral("OUTP 1"));
            readAll();
        } else {
            set(cmd);
        }
    });
    form->addRow(tr("Shape"), m_shape);

    m_freq = new QDoubleSpinBox(this);
    m_freq->setDecimals(3);
    m_freq->setRange(0.001, 10e6);
    m_freq->setSuffix(QStringLiteral(" Hz"));
    m_freq->setKeyboardTracking(false);
    m_freq->setToolTip(tr("Output frequency (capped at each waveform's limit; sine snaps "
                          "to meter steps above 200 kHz)"));
    connect(m_freq, &QDoubleSpinBox::editingFinished, this, [this] {
        set(QStringLiteral("SOUR:FREQ %1").arg(m_freq->value(), 0, 'f', 3));
    });
    form->addRow(tr("Frequency"), m_freq);

    m_duty = new QDoubleSpinBox(this);
    m_duty->setDecimals(1);
    m_duty->setRange(0.1, 99.9);
    m_duty->setSuffix(QStringLiteral(" %"));
    m_duty->setKeyboardTracking(false);
    m_duty->setToolTip(tr("Square and pulse shapes only"));
    connect(m_duty, &QDoubleSpinBox::editingFinished, this, [this] {
        set(QStringLiteral("SOUR:PULS:DCYC %1").arg(m_duty->value(), 0, 'f', 1));
    });
    form->addRow(tr("Duty"), m_duty);

    m_prof = new QComboBox(this);
    for (const char *n : kProfNames)
        m_prof->addItem(tr(n));
    m_prof->setToolTip(tr("Output level preset (the meter sets amplitude in DAC steps, "
                          "not volts)"));
    connect(m_prof, QOverload<int>::of(&QComboBox::activated), this, [this](int i) {
        set(QStringLiteral("SOUR:LEV:PROF %1").arg(QString::fromLatin1(kProfs[i])));
    });
    form->addRow(tr("Level"), m_prof);

    auto *codes = new QHBoxLayout;
    m_low = new QSpinBox(this);
    m_high = new QSpinBox(this);
    for (QSpinBox *s : {m_low, m_high}) {
        s->setRange(0, 4095);
        s->setKeyboardTracking(false);
        codes->addWidget(s);
    }
    codes->addStretch(1);
    m_low->setToolTip(tr("Low DAC code 0-4095 (sets the profile to Custom)"));
    m_high->setToolTip(tr("High DAC code 0-4095 (sets the profile to Custom)"));
    connect(m_low, &QSpinBox::editingFinished, this,
            [this] { set(QStringLiteral("SOUR:LEV:LOW %1").arg(m_low->value())); });
    connect(m_high, &QSpinBox::editingFinished, this,
            [this] { set(QStringLiteral("SOUR:LEV:HIGH %1").arg(m_high->value())); });
    form->addRow(tr("Codes low / high"), codes);

    m_drive = new QComboBox(this);
    m_drive->addItems({tr("Factory"), tr("PWM")});
    m_drive->setToolTip(tr("Dedicated square drive: Factory = DAC below 1 kHz, PWM from 1 kHz"));
    connect(m_drive, QOverload<int>::of(&QComboBox::activated), this, [this](int i) {
        set(QStringLiteral("SOUR:SQU:DRIV %1").arg(i ? QStringLiteral("PWM") : QStringLiteral("FACT")));
    });
    form->addRow(tr("Square drive"), m_drive);

    m_path = new QLabel(QStringLiteral("--"), this);
    m_path->setToolTip(tr("Active hardware output path"));
    form->addRow(tr("Path"), m_path);

    m_startup = new QComboBox(this);
    m_startup->addItems({tr("Off"), tr("Restore last")});
    m_startup->setToolTip(tr("Output state when entering generator mode"));
    connect(m_startup, QOverload<int>::of(&QComboBox::activated), this, [this](int i) {
        set(QStringLiteral("OUTP:STAR %1").arg(i ? QStringLiteral("REST") : QStringLiteral("OFF")));
    });
    form->addRow(tr("At startup"), m_startup);
    root->addLayout(form);

    auto *note = new QLabel(tr("The output keeps running when the app disconnects or closes. "
                               "Turn it off here or on the meter."),
                            this);
    note->setProperty("cls", QStringLiteral("dim"));
    note->setWordWrap(true);
    root->addWidget(note);
    root->addStretch(1);
}

void GenPanel::setActive(bool on)
{
    m_active = on;
    setEnabled(on);
    if (on)
        readAll();
}

void GenPanel::readAll()
{
    if (!m_active)
        return;
    static const char *const q[] = {"SOUR:FUNC?",     "SOUR:FREQ?",     "SOUR:PULS:DCYC?",
                                    "SOUR:LEV:PROF?", "SOUR:LEV:LOW?",  "SOUR:LEV:HIGH?",
                                    "SOUR:SQU:DRIV?", "SOUR:LEV:PATH?", "OUTP?",
                                    "OUTP:STAR?"};
    for (int i = 0; i < 10; ++i)
        emit wantRequest(TFunc + i, QString::fromLatin1(q[i]), false);
}

void GenPanel::set(const QString &cmd)
{
    if (!m_active)
        return;
    emit wantSet(cmd);
    readAll();
}

void GenPanel::onReply(int tag, const QString &text)
{
    if (!m_active)
        return;
    const QString t = text.trimmed().toUpper();
    bool ok = false;
    switch (tag) {
    case TFunc:
        for (int i = 0; i < 6; ++i)
            if (t == QLatin1String(kShapes[i])) {
                const QSignalBlocker b(m_shape);
                m_shape->setCurrentIndex(i);
            }
        break;
    case TFreq: {
        const double v = t.toDouble(&ok);
        if (ok) {
            const QSignalBlocker b(m_freq);
            m_freq->setValue(v);
        }
        break;
    }
    case TDuty: {
        const double v = t.toDouble(&ok);
        if (ok) {
            const QSignalBlocker b(m_duty);
            m_duty->setValue(v);
        }
        break;
    }
    case TProf:
        for (int i = 0; i < 4; ++i)
            if (t.startsWith(QLatin1String(kProfs[i]))) {
                const QSignalBlocker b(m_prof);
                m_prof->setCurrentIndex(i);
            }
        break;
    case TLow:
    case THigh: {
        QSpinBox *s = tag == TLow ? m_low : m_high;
        const int v = t.toInt(&ok);
        if (ok) {
            const QSignalBlocker b(s);
            s->setValue(v);
        }
        break;
    }
    case TDrive: {
        const QSignalBlocker b(m_drive);
        m_drive->setCurrentIndex(t.startsWith(QLatin1String("PWM")) ? 1 : 0);
        break;
    }
    case TPath:
        m_path->setText(t.isEmpty() ? QStringLiteral("--") : t);
        break;
    case TOut: {
        m_outOn = t.startsWith(QLatin1Char('1'));
        const QSignalBlocker b(m_out);
        m_out->setChecked(m_outOn);
        m_out->setText(m_outOn ? tr("Output ON") : tr("Output OFF"));
        m_out->setProperty("cls", m_outOn ? QStringLiteral("stop") : QStringLiteral("primary"));
        m_out->style()->unpolish(m_out);
        m_out->style()->polish(m_out);
        break;
    }
    case TStart: {
        const QSignalBlocker b(m_startup);
        m_startup->setCurrentIndex(t.startsWith(QLatin1String("REST")) ? 1 : 0);
        break;
    }
    default:
        break;
    }
}
