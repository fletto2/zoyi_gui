#include "mainwindow.h"
#include "theme.h"

#include "core/framepump.h"
#include "core/scpiclient.h"
#include "controldock.h"
#include "dmmpanel.h"
#include "genpanel.h"
#include "measbar.h"
#include "overview.h"
#include "waveview.h"

#include <QCheckBox>
#include <QClipboard>
#include <QCloseEvent>
#include <QComboBox>
#include <QDateTime>
#include <QDialog>
#include <QDir>
#include <QDockWidget>
#include <QFile>
#include <QFileDialog>
#include <QFormLayout>
#include <QMenu>
#include <QSettings>
#include <QStackedWidget>
#include <QTextStream>
#include <QGuiApplication>
#include <QImage>
#include <QKeySequence>
#include <QShortcut>
#include <QStandardPaths>
#include <QFont>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMetaType>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QSerialPortInfo>
#include <QStatusBar>
#include <QStyle>
#include <QThread>
#include <QTime>
#include <QTimer>
#include <QVBoxLayout>

#include <cmath>
#include <cstdio>

namespace {

// Dynamic-property states repaint only after a repolish.
void repolish(QWidget *w)
{
    w->style()->unpolish(w);
    w->style()->polish(w);
}

const qint16 kZoyiVid = 0x0483; // STMicro CDC
const qint16 kZoyiPid = 0x572A;

} // namespace

MainWindow::MainWindow(const QString &autoPort, bool verbose, QWidget *parent)
    : QMainWindow(parent)
    , m_verbose(verbose)
{
    setWindowTitle(tr("ZOYI ZT703S / ZT706S"));
    resize(1440, 900);

    // --- Toolbar --------------------------------------
    auto *topbar = new QWidget(this);
    topbar->setObjectName(QStringLiteral("topbar"));
    topbar->setAttribute(Qt::WA_StyledBackground, true);
    auto *tlay = new QHBoxLayout(topbar);
    tlay->setContentsMargins(10, 7, 10, 7);
    tlay->setSpacing(10);

    m_chip = new QLabel(QStringLiteral("OFFLINE"), topbar);
    m_chip->setObjectName(QStringLiteral("stateChip"));
    m_chip->setAlignment(Qt::AlignCenter);
    setChipState(QStringLiteral("stop"), tr("OFFLINE"));
    tlay->addWidget(m_chip);

    // Scope chrome: Run/Stop, ARM, Center.
    m_run = new QPushButton(tr("Run"), topbar);
    m_run->setProperty("cls", QStringLiteral("primary"));
    m_run->setToolTip(tr("Start or stop capturing waveforms (Space)"));
    connect(m_run, &QPushButton::clicked, this,
            [this] { emit wantRunStop(!m_acqRunning); });
    tlay->addWidget(m_run);
    m_arm = new QPushButton(tr("Arm"), topbar);
    m_arm->setToolTip(tr("Arm one single capture; the meter holds it when triggered (A)"));
    connect(m_arm, &QPushButton::clicked, this, [this] { emit wantArm(); });
    tlay->addWidget(m_arm);
    m_center = new QPushButton(tr("Center"), topbar);
    m_center->setToolTip(tr("Center the trigger point horizontally (C)"));
    tlay->addWidget(m_center);
    // Force trigger, Autoscale.
    m_force = new QPushButton(tr("Force"), topbar);
    m_force->setToolTip(tr("Force one trigger now"));
    connect(m_force, &QPushButton::clicked, this, [this] { emit wantSet(QStringLiteral("TFOR")); });
    m_autoscale = new QPushButton(tr("Autoscale"), topbar);
    m_autoscale->setToolTip(tr("Let the meter pick V/div, timebase and trigger"));
    connect(m_autoscale, &QPushButton::clicked, this,
            [this] { emit wantSet(QStringLiteral("AUT")); });
    for (QPushButton *b : {m_force, m_autoscale})
        tlay->addWidget(b);
    // Three-dots menu: Screenshot, Remote, Save, Device, Auto-reconnect.
    m_more = new QPushButton(QStringLiteral("..."), topbar);
    m_more->setToolTip(tr("More actions (Screenshot, Remote, Save, Device, Reconnect)"));
    {
        auto *menu = new QMenu(m_more);
        m_shotAct = menu->addAction(tr("Screenshot (Ctrl+P)"), this, [this] {
            emit wantRequest(1, QStringLiteral("DISP:DATA?"), true);
        });
        m_remoteAct = menu->addAction(tr("Remote"));
        m_remoteAct->setCheckable(true);
        connect(m_remoteAct, &QAction::triggered, this, [this](bool on) {
            emit wantSet(QStringLiteral("SYST:REM %1").arg(on ? 1 : 0));
            emit wantRequest(2, QStringLiteral("SYST:REM?"), false);
        });
        m_saveMenu = menu->addMenu(tr("Save"));
        m_saveMenu->addAction(tr("Shown frame (CSV)..."), this, [this] { saveCsv(false); });
        m_saveMenu->addAction(tr("Whole record (CSV)..."), this, [this] { saveCsv(true); });
        m_deviceAct = menu->addAction(tr("Device..."), this, &MainWindow::showDeviceInfo);
        menu->addSeparator();
        m_autoReAct = menu->addAction(tr("Auto-reconnect"));
        m_autoReAct->setCheckable(true);
        m_autoReAct->setChecked(true);
        m_more->setMenu(menu);
    }
    tlay->addWidget(m_more);
    // 5/6: meter mode. A change restarts the meter's USB link, Reconnect
    // brings it back and SYST:MODE? then picks the page.
    m_mode = new QComboBox(topbar);
    m_mode->addItem(tr("Scope"), QStringLiteral("SCOP"));
    m_mode->addItem(tr("DMM"), QStringLiteral("DMM"));
    m_mode->addItem(tr("Generator"), QStringLiteral("GEN"));
    m_mode->setToolTip(tr("Switch meter mode. The USB connection restarts on a change"));
    connect(m_mode, QOverload<int>::of(&QComboBox::activated), this, [this](int i) {
        const QString want = m_mode->itemData(i).toString();
        if (!m_online || want == m_meterMode)
            return;
        statusBar()->showMessage(tr("Switching the meter to %1, the USB link restarts")
                                     .arg(m_mode->itemText(i)),
                                 6000);
        emit wantSet(QStringLiteral("SYST:MODE %1").arg(want));
    });
    tlay->insertWidget(1, m_mode);
    for (QPushButton *b : {m_run, m_arm, m_center, m_force, m_autoscale})
        b->setEnabled(false);
    m_shotAct->setEnabled(false);
    m_remoteAct->setEnabled(false);
    m_saveMenu->setEnabled(false);
    m_deviceAct->setEnabled(false);
    m_mode->setEnabled(false);

    // No "Port" label and no spacer (test pass 1, bug 2): the combo takes
    // the spare width and shrinks cleanly without overlapping Refresh/Connect.
    m_ports = new QComboBox(topbar);
    m_ports->setObjectName(QStringLiteral("portCombo"));
    m_ports->setToolTip(tr("Serial port of the meter"));
    m_ports->setMinimumWidth(80);
    m_ports->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    m_ports->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    m_ports->setMinimumContentsLength(6);
    tlay->addWidget(m_ports, 1);

    m_refresh = new QPushButton(tr("Refresh"), topbar);
    connect(m_refresh, &QPushButton::clicked, this, &MainWindow::refreshPorts);
    tlay->addWidget(m_refresh);

    m_connect = new QPushButton(tr("Connect"), topbar);
    m_connect->setProperty("cls", QStringLiteral("primary"));
    connect(m_connect, &QPushButton::clicked, this, &MainWindow::toggleConnect);
    tlay->addWidget(m_connect);

    m_reTimer = new QTimer(this);
    m_reTimer->setInterval(1500);
    connect(m_reTimer, &QTimer::timeout, this, [this] {
        if (m_online || m_userClose || !m_autoReAct->isChecked() || m_lastPort.isEmpty()) {
            m_reTimer->stop();
            return;
        }
        // Only when the port is back in the system list.
        for (const auto &p : QSerialPortInfo::availablePorts())
            if (p.portName() == m_lastPort) {
                emit wantOpen(m_lastPort);
                return;
            }
    });

    m_link = new QLabel(topbar);
    m_link->setObjectName(QStringLiteral("linkChip"));
    m_link->setProperty("cls", QStringLiteral("chip"));
    setLinkChip(QStringLiteral("#64748b"), tr("not connected"));
    // The link chip sits on the right of the status bar (added there below),
    // the toolbar has no room left for it on the Scope page.

    setMenuWidget(topbar);

    // --- Central waveform view ---------------------------------------
    // Plot over the buffer overview strip.
    auto *centerBox = new QWidget(this);
    auto *clay = new QVBoxLayout(centerBox);
    clay->setContentsMargins(0, 0, 0, 0);
    clay->setSpacing(0);
    m_horiz = new HorizBar(centerBox);
    clay->addWidget(m_horiz);
    m_view = new WaveView(centerBox);
    clay->addWidget(m_view, 1);
    clay->addWidget(new OverviewStrip(m_view, centerBox));
    // Bottom row: measurements and hardware counter.
    auto *bottom = new QWidget(centerBox);
    bottom->setObjectName(QStringLiteral("bottomRow"));
    bottom->setAttribute(Qt::WA_StyledBackground, true);
    auto *blay = new QHBoxLayout(bottom);
    blay->setContentsMargins(8, 3, 8, 3);
    blay->setSpacing(8);
    m_meas = new MeasBar(bottom);
    blay->addWidget(m_meas, 1);
    clay->addWidget(bottom);
    m_stack = new QStackedWidget(this);
    m_stack->addWidget(centerBox);
    m_dmm = new DmmPanel(m_stack);
    m_stack->addWidget(m_dmm);
    m_gen = new GenPanel(m_stack);
    m_gen->setActive(false);
    m_stack->addWidget(m_gen);
    setCentralWidget(m_stack);
    connect(m_dmm, &DmmPanel::wantRequest, this, &MainWindow::wantRequest);
    connect(m_dmm, &DmmPanel::wantSet, this, &MainWindow::wantSet);
    connect(m_gen, &GenPanel::wantRequest, this, &MainWindow::wantRequest);
    connect(m_gen, &GenPanel::wantSet, this, &MainWindow::wantSet);
    connect(m_center, &QPushButton::clicked, m_view, &WaveView::centerTrigger);
    connect(m_horiz, &HorizBar::timebaseStep, m_view, &WaveView::zoomStep);

    // --- Left control dock ------------------------
    m_controls = new ControlDock(this);
    auto *left = new QDockWidget(tr("Channels"), this);
    left->setObjectName(QStringLiteral("channelsDock"));
    m_leftDock = left;
    left->setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea);
    left->setFeatures(QDockWidget::DockWidgetMovable);
    left->setTitleBarWidget(new QWidget(left));
    // Scrolls when the cards (two channels + trigger) outgrow the window.
    auto *leftScroll = new QScrollArea(left);
    leftScroll->setFrameShape(QFrame::NoFrame);
    leftScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    leftScroll->setWidgetResizable(true);
    leftScroll->setWidget(m_controls);
    leftScroll->setFixedWidth(m_controls->width() + 10);
    left->setWidget(leftScroll);
    addDockWidget(Qt::LeftDockWidgetArea, left);

    // --- Status bar -----------------------------------
    auto *sb = new QStatusBar(this);
    sb->setObjectName(QStringLiteral("status"));
    sb->setSizeGripEnabled(false);
    m_statLed = new QLabel(sb);
    m_statTrig = new QLabel(sb);
    m_statAcq = new QLabel(sb);
    m_statCore = new QLabel(sb);
    m_statTrig->setToolTip(tr("Current trigger state"));
    m_statAcq->setToolTip(tr("Sample rate and memory depth"));
    m_statCore->setToolTip(tr("Meter processor clock (USB capture requires the high-speed clock profile)"));
    for (QLabel *l : {m_statLed, m_statTrig, m_statAcq, m_statCore})
        sb->addWidget(l);
    sb->addPermanentWidget(m_link);
    setStatusBar(sb);
    m_statLed->setText(QStringLiteral("<span style=\"color:#64748b\">&#9679;</span>"));
    m_statTrig->setText(tr("offline"));

    // --- SCPI console dock") ----------------
    auto *dock = new QDockWidget(tr("SCPI traffic (debug)"), this);
    dock->setObjectName(QStringLiteral("consoleDock"));
    dock->setAllowedAreas(Qt::BottomDockWidgetArea | Qt::TopDockWidgetArea);
    auto *body = new QWidget(dock);
    auto *lay = new QVBoxLayout(body);
    lay->setContentsMargins(6, 6, 6, 6);

    m_console = new QPlainTextEdit(body);
    m_console->setObjectName(QStringLiteral("console"));
    m_console->setReadOnly(true);
    m_console->setMaximumBlockCount(4000);
    m_console->setFont(theme::monoFont());
    lay->addWidget(m_console);

    auto *row = new QHBoxLayout;
    m_input = new QLineEdit(body);
    m_input->setObjectName(QStringLiteral("cmdInput"));
    m_input->setPlaceholderText(tr("SCPI command (Enter to send), e.g. *IDN?"));
    connect(m_input, &QLineEdit::returnPressed, this, &MainWindow::sendManual);
    row->addWidget(m_input, 1);
    auto *send = new QPushButton(tr("Send"), body);
    connect(send, &QPushButton::clicked, this, &MainWindow::sendManual);
    row->addWidget(send);
    lay->addLayout(row);

    dock->setWidget(body);
    addDockWidget(Qt::BottomDockWidgetArea, dock);

    // --- Worker thread + client + frame pump ------------------------------
    qRegisterMetaType<WaveFrame>("WaveFrame");
    qRegisterMetaType<DeviceState>("DeviceState");
    qRegisterMetaType<DeviceCaps>("DeviceCaps");
    qRegisterMetaType<meas::Result>("meas::Result");
    m_thread = new QThread(this);
    m_client = new ScpiClient; // no parent: lives in m_thread
    m_pump = new FramePump;    // ditto; drives the client directly in-thread
    m_pump->setClient(m_client);
    m_client->moveToThread(m_thread);
    m_pump->moveToThread(m_thread);
    connect(m_thread, &QThread::finished, m_client, &QObject::deleteLater);
    connect(m_thread, &QThread::finished, m_pump, &QObject::deleteLater);

    connect(this, &MainWindow::wantOpen, m_client, &ScpiClient::openPort);
    connect(this, &MainWindow::wantClose, m_client, &ScpiClient::closePort);
    connect(this, &MainWindow::wantCommand, m_client, &ScpiClient::command);
    connect(this, &MainWindow::wantQuery, m_client, &ScpiClient::query);

    connect(m_client, &ScpiClient::opened, this, &MainWindow::onOpened);
    connect(m_client, &ScpiClient::closed, this, &MainWindow::onClosed);
    connect(m_client, &ScpiClient::failed, this, &MainWindow::onFailed);
    connect(m_client, &ScpiClient::traffic, this, &MainWindow::onTraffic);
    connect(m_client, &ScpiClient::response, this, &MainWindow::onResponse);
    connect(m_client, &ScpiClient::scpiError, this, &MainWindow::onScpiError);

    // Same-thread (direct) wiring inside the worker: the pump listens to
    // client responses and follows the link state.
    connect(m_client, &ScpiClient::response, m_pump, &FramePump::onText);
    connect(m_client, &ScpiClient::rawResponse, m_pump, &FramePump::onRaw);
    // 5: the pump starts once SYST:MODE? says SCOP (setMeterMode).
    connect(this, &MainWindow::wantPumpStart, m_pump, &FramePump::start);
    connect(m_client, &ScpiClient::closed, m_pump, &FramePump::stop);
    connect(m_client, &ScpiClient::failed, m_pump, &FramePump::stop);

    // Frames and console notes cross back to the UI thread (queued).
    connect(m_pump, &FramePump::frame, m_view, &WaveView::setFrame);
    connect(m_pump, &FramePump::record, m_view, &WaveView::setRecord);
    connect(m_pump, &FramePump::frame, m_meas, &MeasBar::setFrame);
    connect(m_pump, &FramePump::measReady, m_meas, &MeasBar::setFirmwareMeas);
    connect(m_pump, &FramePump::status, this, &MainWindow::onAcqStatus);
    connect(this, &MainWindow::wantRunStop, m_pump, &FramePump::runStop);
    connect(this, &MainWindow::wantArm, m_pump, &FramePump::arm);
    connect(m_view, &WaveView::centerRequested, m_pump, &FramePump::center);
    connect(m_pump, &FramePump::info, this, &MainWindow::onTraffic);
    // One-off requests and sets from the UI (screenshot, remote, ...).
    connect(this, &MainWindow::wantRequest, m_pump, &FramePump::request);
    connect(this, &MainWindow::wantSet, m_pump, &FramePump::sendCommand);
    connect(m_pump, &FramePump::reply, this, &MainWindow::onPumpReply);
    auto *shotKey = new QShortcut(QKeySequence(QStringLiteral("Ctrl+P")), this);
    connect(shotKey, &QShortcut::activated, this, [this] {
        if (m_shotAct->isEnabled())
            m_shotAct->trigger();
    });
    // Shortcuts. Line edits and spin boxes keep their own keys (they
    // accept the ShortcutOverride for text keys).
    auto key = [this](const char *seq, QPushButton *b) {
        auto *sc = new QShortcut(QKeySequence(QString::fromLatin1(seq)), this);
        connect(sc, &QShortcut::activated, this, [this, b] {
            if (b->isEnabled() && m_stack->currentIndex() == 0)
                b->click();
        });
    };
    key("Space", m_run);
    key("A", m_arm);
    key("C", m_center);
    // Clean view (OBS capture): plot only, no toolbar, docks,
    // status bar or rows. F11 or the plot's context menu toggles it.
    auto cleanToggle = [this, bottom, dock] {
        const bool clean = menuWidget()->isVisible();
        if (clean && m_stack->currentIndex() == 0)
            m_normalState = saveState();
        m_clean = clean;
        menuWidget()->setVisible(!clean);
        statusBar()->setVisible(!clean);
        bottom->setVisible(!clean);
        m_horiz->setVisible(!clean);
        if (clean)
            dock->hide();
        if (m_leftDock)
            m_leftDock->setVisible(!clean && (m_meterMode.isEmpty() || m_meterMode == QLatin1String("SCOP")));
    };
    m_modeTimer = new QTimer(this);
    m_modeTimer->setSingleShot(true);
    connect(m_modeTimer, &QTimer::timeout, this, [this] {
        if (m_online && m_meterMode.isEmpty())
            setMeterMode(QStringLiteral("SCOP"));
    });
    auto *cleanKey = new QShortcut(QKeySequence(Qt::Key_F11), this);
    connect(cleanKey, &QShortcut::activated, this, cleanToggle);
    connect(m_view, &WaveView::cleanViewToggled, this, cleanToggle);
    auto *saveKey = new QShortcut(QKeySequence::Save, this);
    connect(saveKey, &QShortcut::activated, this, [this] {
        if (m_saveMenu->isEnabled() && m_stack->currentIndex() == 0)
            saveCsv(false);
    });

    // Device state model: the dock shows read-back values, its sets and
    // the ground tag drags go through the pump (set + read-back barrier).
    connect(m_pump, &FramePump::stateChanged, m_controls, &ControlDock::setState);
    connect(m_controls, &ControlDock::channelValue, m_pump, &FramePump::setChannelValue);
    // Trigger: dock + level tag drag -> pump; model -> view + status bar.
    connect(m_controls, &ControlDock::triggerValue, m_pump, &FramePump::setTriggerValue);
    connect(m_controls, &ControlDock::trigAlignChanged, m_pump, &FramePump::setTrigAlign);
    connect(m_controls, &ControlDock::cursorModeChanged, m_view, &WaveView::setCursorMode);
    connect(m_controls, &ControlDock::cursorChannelChanged, m_view, &WaveView::setCursorChannel);
    connect(m_view, &WaveView::cursorsChanged, m_controls, &ControlDock::setCursorReadout);
    connect(m_view, &WaveView::triggerLevelRequested, m_pump, [this](double v) {
        m_pump->setTriggerValue(DeviceState::TLev, v);
    });
    connect(m_pump, &FramePump::stateChanged, this, [this](const DeviceState &s) {
        m_view->setTrigger(s.trig.source, s.trig.level,
                           s.trig.known & (1 << DeviceState::TLev));
        setStatusTele(s.tele.srat, s.tele.mdep, s.tele.coreHz, s.tele.known);
        m_horiz->setRoll(s.tele.known & (1 << DeviceState::YRoll), s.tele.roll);
        m_meas->setCounter(s.tele.known & (1 << DeviceState::YFcnt), s.tele.fcnt,
                           (s.tele.known & (1 << DeviceState::YFqual)) ? s.tele.fqual : -1,
                           s.trig.source);
    });
    connect(m_horiz, &HorizBar::rollChanged, m_pump, &FramePump::setRoll);
    connect(m_view, &WaveView::channelOffsetRequested, m_pump,
            [this](int ch, double v) {
                m_pump->setChannelValue(ch, DeviceState::Offs, v);
            });
    connect(m_pump, &FramePump::frame, m_horiz,
            [this](const WaveFrame &f) { m_horiz->setHoriz(f.scal, f.pos); });

    // View-is-the-device: the wheel becomes TIM:SCAL, a drag a pan in
    // the record (running), or STAR/STOP re-fetches (stopped); coalesced
    // UI-lane groups in the pump. Render stats go to the console (debug
    // window).
    connect(m_view, &WaveView::scaleChangeRequested, m_pump,
            &FramePump::stepTimeScale);
    connect(m_view, &WaveView::viewShiftRequested, m_pump, &FramePump::setViewShift);
    connect(m_view, &WaveView::viewWindowChanged, m_pump,
            &FramePump::setViewWindow);
    connect(m_view, &WaveView::renderStats, this,
            [this](double fps, qreal ms, qreal drawMs) {
                onTraffic(QStringLiteral(". render %1 f/s, paint %2 ms, traces %3 ms")
                              .arg(fps, 0, 'f', 1)
                              .arg(ms, 0, 'f', 1)
                              .arg(drawMs, 0, 'f', 1));
            });
    m_thread->start();

    // Window layout, Reconnect and the last port (render options are
    // kept by WaveView itself, trace/*).
    {
        QSettings s;
        restoreGeometry(s.value(QStringLiteral("window/geometry")).toByteArray());
        restoreState(s.value(QStringLiteral("window/state")).toByteArray());
        dock->hide();
        auto syncLeftWidth = [this](bool collapsed) {
            const int w = collapsed ? 148 : 272;
            m_leftDock->setFixedWidth(w);
            resizeDocks({m_leftDock}, {w}, Qt::Horizontal);
        };
        const bool collapsed = s.value(QStringLiteral("ui/controlsCollapsed"), false).toBool();
        m_controls->setCollapsed(collapsed);
        syncLeftWidth(collapsed);
        connect(m_controls, &ControlDock::collapsedChanged, this, [syncLeftWidth](bool c) {
            QSettings().setValue(QStringLiteral("ui/controlsCollapsed"), c);
            syncLeftWidth(c);
        });
        m_autoReAct->setChecked(s.value(QStringLiteral("link/reconnect"), true).toBool());
    }
    refreshPorts();
    {
        const QString last = QSettings().value(QStringLiteral("link/port")).toString();
        const int idx = last.isEmpty() ? -1 : m_ports->findData(last);
        if (idx >= 0)
            m_ports->setCurrentIndex(idx);
    }
    if (!autoPort.isEmpty()) {
        const int idx = m_ports->findData(autoPort);
        if (idx >= 0)
            m_ports->setCurrentIndex(idx);
    }
    if (m_ports->count() > 0 && !m_ports->currentData().toString().isEmpty())
        QTimer::singleShot(250, this, &MainWindow::toggleConnect);
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    QSettings s;
    s.setValue(QStringLiteral("window/geometry"), saveGeometry());
    // Do not persist docks hidden by Clean view or DMM/GEN pages.
    if (!m_clean && m_stack->currentIndex() == 0)
        s.setValue(QStringLiteral("window/state"), saveState());
    else if (!m_normalState.isEmpty())
        s.setValue(QStringLiteral("window/state"), m_normalState);
    s.setValue(QStringLiteral("link/reconnect"), m_autoReAct->isChecked());
    if (!m_lastPort.isEmpty())
        s.setValue(QStringLiteral("link/port"), m_lastPort);
    m_userClose = true;
    m_reTimer->stop();
    m_modeTimer->stop();
    QMainWindow::closeEvent(event);
}

MainWindow::~MainWindow()
{
    m_thread->quit();
    m_thread->wait(2000);
}

void MainWindow::refreshPorts()
{
    m_ports->clear();
    // ZOYI CDC ports first, everything else after, in system order.
    const auto ports = QSerialPortInfo::availablePorts();
    for (const auto &p : ports) {
        if (p.vendorIdentifier() == kZoyiVid && p.productIdentifier() == kZoyiPid) {
            m_ports->insertItem(0, QStringLiteral("%1 - ZOYI %2")
                                              .arg(p.portName(), p.description()),
                                p.portName());
        }
    }
    for (const auto &p : ports) {
        if (p.vendorIdentifier() == kZoyiVid && p.productIdentifier() == kZoyiPid)
            continue;
        m_ports->addItem(QStringLiteral("%1 - %2").arg(p.portName(), p.description()),
                         p.portName());
    }
}

void MainWindow::toggleConnect()
{
    if (m_online) {
        m_userClose = true;
        m_reTimer->stop();
        emit wantClose();
        return;
    }
    const QString port = m_ports->currentData().toString();
    if (port.isEmpty())
        return;
    m_userClose = false;
    m_reTimer->stop();
    emit wantOpen(port);
}

void MainWindow::onOpened(const QString &port)
{
    m_online = true;
    m_port = port;
    m_connect->setText(tr("Disconnect"));
    m_connect->setProperty("cls", QStringLiteral("stop"));
    repolish(m_connect);
    m_ports->setEnabled(false);
    m_refresh->setEnabled(false);
    setChipState(QStringLiteral("ok"), tr("ONLINE"));
    setLinkChip(QStringLiteral("#4ade80"), port);
    m_shotAct->setEnabled(true);
    m_remoteAct->setEnabled(true);
    m_deviceAct->setEnabled(true);
    m_mode->setEnabled(true);
    m_lastPort = port;
    m_userClose = false;
    m_reTimer->stop();
    QSettings().setValue(QStringLiteral("link/port"), port);
    emit wantQuery(QStringLiteral("*IDN?"), ++m_cookie);
    emit wantRequest(2, QStringLiteral("SYST:REM?"), false);
    // 5: the mode picks the page and whether the scope pump runs. Firmware
    // with no SYST:MODE? (or a lost reply): scope after 2.5 s.
    m_meterMode.clear();
    emit wantRequest(3, QStringLiteral("SYST:MODE?"), false);
    m_modeTimer->start(2500);
}

void MainWindow::onClosed()
{
    m_online = false;
    m_port.clear();
    m_modeTimer->stop();
    m_connect->setText(tr("Connect"));
    m_connect->setProperty("cls", QStringLiteral("primary"));
    repolish(m_connect);
    m_ports->setEnabled(true);
    m_refresh->setEnabled(true);
    setChipState(QStringLiteral("stop"), tr("OFFLINE"));
    setLinkChip(QStringLiteral("#64748b"), tr("not connected"));
    m_remoteAct->setChecked(false);
    const bool onScope = m_stack->currentIndex() == 0;
    for (QPushButton *b : {m_run, m_arm, m_center, m_force, m_autoscale}) {
        b->setEnabled(false);
        b->setVisible(onScope);
    }
    m_saveMenu->setEnabled(false);
    m_shotAct->setEnabled(false);
    m_remoteAct->setEnabled(false);
    m_deviceAct->setEnabled(false);
    m_mode->setEnabled(false);
    m_meterMode.clear();
    m_dmm->setActive(false);
    m_gen->setActive(false);
    if (!m_userClose && m_autoReAct->isChecked() && !m_lastPort.isEmpty())
        m_reTimer->start();
    m_controls->setOnline(false);
    m_horiz->setOnline(false);
    m_meas->setOnline(false);
    m_view->setTrigger(1, 0.0, false);
    m_statLed->setText(QStringLiteral("<span style=\"color:#64748b\">&#9679;</span>"));
    m_statTrig->setText(tr("offline"));
    m_statAcq->clear();
    m_statCore->clear();
}

void MainWindow::setStatusTele(double srat, int mdep, double coreHz, int known)
{
    QStringList acq;
    if ((known & (1 << DeviceState::YSrat)) && srat > 0.0) {
        const double a = srat;
        acq << (a >= 1e6   ? QStringLiteral("%1 MS/s").arg(a / 1e6, 0, 'f', 1)
                : a >= 1e3 ? QStringLiteral("%1 kS/s").arg(a / 1e3, 0, 'f', 1)
                           : QStringLiteral("%1 S/s").arg(a, 0, 'f', 0));
    }
    if ((known & (1 << DeviceState::YMdep)) && mdep > 0)
        acq << (mdep >= 1024 && mdep % 1024 == 0 ? QStringLiteral("%1K").arg(mdep / 1024)
                                                 : QString::number(mdep))
                   + tr(" pts");
    if ((known & (1 << DeviceState::YSrat)) && srat > 0.0)
        acq << fmt::seconds(1.0 / srat) + tr("/pt");
    m_statAcq->setText(acq.join(QStringLiteral(", ")));
    if (known & (1 << DeviceState::YPerf)) {
        const bool ok = std::abs(coreHz - 200e6) <= 0.5;
        m_statCore->setText(QStringLiteral("core <b style=\"color:%1\">%2 MHz</b>")
                                .arg(ok ? QStringLiteral("#e2e8f0") : QStringLiteral("#f87171"))
                                .arg(coreHz / 1e6, 0, 'f', 0));
    } else {
        m_statCore->clear();
    }
}

void MainWindow::onAcqStatus(const QString &stat)
{
    if (!m_online)
        return;
    // TRIG:STAT? -> chip states. AUTO = Auto sweep running untriggered.
    m_acqRunning = !stat.startsWith(QLatin1String("STOP"));
    if (stat.startsWith(QLatin1String("TD")))
        setChipState(QStringLiteral("ok"), tr("TRIGGERED"));
    else if (stat.startsWith(QLatin1String("WAIT")))
        setChipState(QStringLiteral("wait"), tr("WAITING"));
    else if (stat.startsWith(QLatin1String("AUTO")))
        setChipState(QStringLiteral("wait"), tr("AUTO"));
    else if (stat.startsWith(QLatin1String("RUN")))
        setChipState(QStringLiteral("ok"), tr("RUNNING"));
    else if (stat.startsWith(QLatin1String("ROLL")))
        setChipState(QStringLiteral("ok"), tr("ROLL")); // Pump-made state
    else
        setChipState(QStringLiteral("stop"), tr("STOP"));
    // Status bar: LED green when triggered, amber waiting/auto, gray else.
    const QString led = stat.startsWith(QLatin1String("TD"))      ? QStringLiteral("#4ade80")
                        : stat.startsWith(QLatin1String("WAIT"))
                                || stat.startsWith(QLatin1String("AUTO"))
                            ? QStringLiteral("#fbbf24")
                            : QStringLiteral("#64748b");
    m_statLed->setText(QStringLiteral("<span style=\"color:%1\">&#9679;</span>").arg(led));
    m_statTrig->setText(QStringLiteral("<b>%1</b>").arg(m_chip->text()));
    const QString cls = m_acqRunning ? QStringLiteral("primary") : QStringLiteral("stop");
    const QString text = stat.startsWith(QLatin1String("WAIT")) ? tr("Armed")
                         : m_acqRunning                        ? tr("Run")
                                                               : tr("Stopped");
    if (m_run->text() != text || m_run->property("cls").toString() != cls) {
        m_run->setText(text);
        m_run->setProperty("cls", cls);
        repolish(m_run);
    }
}

void MainWindow::onFailed(const QString &what)
{
    setChipState(QStringLiteral("wait"), tr("ERROR"));
    setLinkChip(QStringLiteral("#f87171"), tr("link error"));
    const QString w = what.toLower();
    const bool busy = w.contains(QLatin1String("denied")) || w.contains(QLatin1String("busy"))
                      || w.contains(QLatin1String("in use"));
    statusBar()->showMessage(busy ? tr("Port in use by another program") : what, 8000);
    if (!m_userClose && m_autoReAct->isChecked() && !m_lastPort.isEmpty())
        m_reTimer->start();
}

void MainWindow::onPumpReply(int tag, const QString &text, const QByteArray &raw)
{
    if (tag == 1) {
        saveScreenshot(raw);
    } else if (tag == 2) {
        const QSignalBlocker block(m_remoteAct);
        m_remoteAct->setChecked(text.startsWith(QLatin1Char('1')));
    } else if (tag == 3) {
        const QString t = text.trimmed().toUpper();
        if (t.startsWith(QLatin1String("DMM")))
            setMeterMode(QStringLiteral("DMM"));
        else if (t.startsWith(QLatin1String("GEN")))
            setMeterMode(QStringLiteral("GEN"));
        else
            setMeterMode(QStringLiteral("SCOP"));
    } else if (tag >= DmmPanel::kTagFirst && tag <= DmmPanel::kTagLast) {
        m_dmm->onReply(tag, text);
    } else if (tag >= GenPanel::kTagFirst && tag <= GenPanel::kTagLast) {
        m_gen->onReply(tag, text);
    } else if (tag >= 60 && tag < 60 + m_infoVals.size()) {
        // ", " so the label can wrap the long comma lists (PERF, BOOT).
        QString t = text;
        t.replace(QLatin1Char(','), QLatin1String(", "));
        m_infoVals.at(tag - 60)->setText(t.isEmpty() ? QStringLiteral("--") : t);
        if (m_info)
            m_info->adjustSize(); // grow for the wrapped lines

    }
}

void MainWindow::setMeterMode(const QString &mode)
{
    m_modeTimer->stop();
    if (!m_online || mode == m_meterMode)
        return;
    const bool scope = mode == QLatin1String("SCOP");
    if (!scope && !m_clean && m_stack->currentIndex() == 0)
        m_normalState = saveState();
    m_meterMode = mode;
    {
        const QSignalBlocker b(m_mode);
        m_mode->setCurrentIndex(qMax(0, m_mode->findData(mode)));
    }
    // Scope-only buttons are hidden on the DMM and GEN pages, not just
    // disabled: the QSS primary style made a disabled Run look live (test
    // pass 1, bug 3) and the bar ran out of room (bug 2).
    for (QPushButton *b : {m_run, m_arm, m_center, m_force, m_autoscale}) {
        b->setEnabled(scope);
        b->setVisible(scope);
    }
    m_saveMenu->setEnabled(scope);
    m_controls->setOnline(scope);
    m_horiz->setOnline(scope);
    m_leftDock->setVisible(scope);
    m_dmm->setActive(mode == QLatin1String("DMM"));
    m_gen->setActive(mode == QLatin1String("GEN"));
    m_stack->setCurrentIndex(scope ? 0 : mode == QLatin1String("DMM") ? 1 : 2);
    if (scope) {
        emit wantPumpStart();
    } else {
        setChipState(QStringLiteral("ok"), mode == QLatin1String("DMM") ? tr("DMM") : tr("GEN"));
        m_statTrig->setText(QStringLiteral("<b>%1</b>").arg(m_chip->text()));
    }
}

void MainWindow::saveCsv(bool whole)
{
    if (whole ? !m_view->hasRecord() : !m_view->hasFrame()) {
        statusBar()->showMessage(whole ? tr("No whole record fetched yet") : tr("No frame yet"),
                                 6000);
        return;
    }
    const WaveFrame f = whole ? m_view->record() : m_view->frame(); // copy: frames keep coming
    // One time column when the channels share their sampling (always the
    // case for one SNAP lease), one per channel otherwise.
    QList<int> chans;
    for (int c = 0; c < 2; ++c)
        if (f.ch[c].valid && !f.ch[c].v.isEmpty())
            chans << c;
    if (chans.isEmpty()) {
        statusBar()->showMessage(tr("The frame holds no channel data"), 6000);
        return;
    }
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    const QString def = QDir(dir).filePath(
        QStringLiteral("zoyi_%1_%2.csv")
            .arg(whole ? QStringLiteral("record") : QStringLiteral("frame"),
                 QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd_HHmmss"))));
    const QString path = QFileDialog::getSaveFileName(
        this, whole ? tr("Save whole record") : tr("Save shown frame"), def, tr("CSV (*.csv)"));
    if (path.isEmpty())
        return;
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        statusBar()->showMessage(tr("Could not write %1").arg(path), 8000);
        return;
    }
    QTextStream out(&file);
    const ChannelFrame &a = f.ch[chans.first()];
    bool shared = true;
    for (int c : chans) {
        const ChannelFrame &b = f.ch[c];
        if (b.v.size() != a.v.size() || std::abs(b.xinc - a.xinc) > 1e-15
            || std::abs(b.xorig - a.xorig) > 1e-12)
            shared = false;
    }
    out << "# ZOYI " << (whole ? "whole record" : "shown frame") << ", "
        << QDateTime::currentDateTime().toString(Qt::ISODate) << ", timebase "
        << QString::number(f.scal, 'g', 6) << " s/div, seq " << f.seq << '\n';
    QStringList head;
    if (shared)
        head << QStringLiteral("time_s");
    for (int c : chans) {
        if (!shared)
            head << QStringLiteral("time%1_s").arg(c + 1);
        if (f.ch[c].isPeak())
            head << QStringLiteral("ch%1_min_V").arg(c + 1)
                 << QStringLiteral("ch%1_max_V").arg(c + 1);
        else
            head << QStringLiteral("ch%1_V").arg(c + 1);
    }
    out << head.join(QLatin1Char(',')) << '\n';
    int rows = 0;
    for (int c : chans)
        rows = qMax(rows, int(f.ch[c].v.size()));
    for (int i = 0; i < rows; ++i) {
        QStringList cells;
        if (shared)
            cells << QString::number(a.xorig + i * a.xinc, 'g', 12);
        for (int c : chans) {
            const ChannelFrame &ch = f.ch[c];
            const bool have = i < ch.v.size();
            if (!shared)
                cells << (have ? QString::number(ch.xorig + i * ch.xinc, 'g', 12) : QString());
            cells << (have ? QString::number(ch.v.at(i), 'g', 7) : QString());
            if (ch.isPeak())
                cells << (have ? QString::number(ch.vHi.at(i), 'g', 7) : QString());
        }
        out << cells.join(QLatin1Char(',')) << '\n';
    }
    out.flush();
    statusBar()->showMessage(tr("Saved %1 rows to %2").arg(rows).arg(path), 8000);
}

void MainWindow::showDeviceInfo()
{
    static const char *const names[] = {"Identity (*IDN?)",
                                        "Performance (SYST:PERF?)",
                                        "Memory (SYST:MEM?)",
                                        "Boot timing (SYST:BOOT?)",
                                        "Stalls (SYST:STAL?)",
                                        "Core clock MHz (SYST:CLOC:MODE?)",
                                        "SCPI version (SYST:VERS?)"};
    static const char *const cmds[] = {"*IDN?",      "SYST:PERF?",      "SYST:MEM?", "SYST:BOOT?",
                                       "SYST:STAL?", "SYST:CLOC:MODE?", "SYST:VERS?"};
    constexpr int n = int(sizeof cmds / sizeof *cmds);
    if (!m_info) {
        m_info = new QDialog(this);
        m_info->setWindowTitle(tr("Device"));
        auto *form = new QFormLayout(m_info);
        for (int i = 0; i < n; ++i) {
            auto *v = new QLabel(QStringLiteral("--"), m_info);
            v->setTextInteractionFlags(Qt::TextSelectableByMouse);
            v->setFont(theme::monoFont());
            v->setWordWrap(true);
            v->setMinimumWidth(420);
            form->addRow(tr(names[i]), v);
            m_infoVals << v;
        }
        auto *again = new QPushButton(tr("Refresh"), m_info);
        connect(again, &QPushButton::clicked, this, &MainWindow::showDeviceInfo);
        form->addRow(QString(), again);
        auto *note = new QLabel(tr("Fields: PERF = uptime ms, core Hz, captures, projections, "
                                   "flushes, cycles. MEM = valid, total, used, peak, free, "
                                   "largest free, fragmentation %. Backlight, power and file "
                                   "settings have no SCPI command yet."),
                                m_info);
        note->setWordWrap(true);
        note->setProperty("cls", QStringLiteral("dim"));
        form->addRow(note);
    }
    for (QLabel *l : m_infoVals)
        l->setText(QStringLiteral("..."));
    if (m_online)
        for (int i = 0; i < n; ++i)
            emit wantRequest(60 + i, QString::fromLatin1(cmds[i]), false);
    m_info->show();
    m_info->raise();
}

void MainWindow::saveScreenshot(const QByteArray &bmp)
{
    QImage img = QImage::fromData(bmp, "BMP");
    if (img.isNull()) {
        statusBar()->showMessage(tr("Screenshot failed: no image from the meter"), 8000);
        return;
    }
    QGuiApplication::clipboard()->setImage(img);
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::PicturesLocation);
    const QString def = QDir(dir).filePath(
        QStringLiteral("zoyi_%1.png")
            .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd_HHmmss"))));
    const QString path = QFileDialog::getSaveFileName(this, tr("Save meter screenshot"), def,
                                                      tr("PNG image (*.png)"));
    if (path.isEmpty()) {
        statusBar()->showMessage(tr("Screenshot copied to the clipboard"), 5000);
        return;
    }
    if (img.save(path, "PNG"))
        statusBar()->showMessage(tr("Screenshot saved and copied: %1").arg(path), 8000);
    else
        statusBar()->showMessage(tr("Could not save %1").arg(path), 8000);
}

void MainWindow::onTraffic(const QString &line)
{
    QString color = QStringLiteral("#94a3b8"); // "->" sent
    if (line.startsWith(QLatin1String("<-")))
        color = QStringLiteral("#e2e8f0");
    else if (line.startsWith(QLatin1Char('!')))
        color = QStringLiteral("#f87171");
    else if (line.startsWith(QLatin1Char('.')))
        color = QStringLiteral("#64748b");
    m_console->appendHtml(QStringLiteral("<span style=\"color:%1\">[%2] %3</span>")
                              .arg(color,
                                   QTime::currentTime().toString(QStringLiteral("HH:mm:ss.zzz")),
                                   line.toHtmlEscaped()));
    if (m_verbose)
        std::fprintf(stderr, "%s\n", line.toLocal8Bit().constData());
}

void MainWindow::onResponse(qint64 cookie, const QString &cmd, const QString &answer)
{
    Q_UNUSED(cookie);
    if (cmd == QLatin1String("*IDN?")) {
        // IDN: vendor,model,serial,version -> "COM28 - vendor version"
        const QStringList parts = answer.split(QLatin1Char(','));
        if (parts.size() >= 4 && !m_port.isEmpty())
            setLinkChip(QStringLiteral("#4ade80"),
                        QStringLiteral("%1 - %2 %3")
                            .arg(m_port, parts.at(0).trimmed(), parts.at(3).trimmed()));
    }
}

void MainWindow::onScpiError(const QString &err)
{
    Q_UNUSED(err); // already visible in the console as "! SYST:ERR ..."
}

void MainWindow::sendManual()
{
    const QString cmd = m_input->text().trimmed();
    if (cmd.isEmpty())
        return;
    m_input->clear();
    if (cmd.endsWith(QLatin1Char('?')))
        emit wantQuery(cmd, ++m_cookie);
    else
        emit wantCommand(cmd, ++m_cookie);
}

void MainWindow::setChipState(const QString &cls, const QString &text)
{
    m_chip->setText(text);
    m_chip->setProperty("cls", cls);
    repolish(m_chip);
}

void MainWindow::setLinkChip(const QString &ledColor, const QString &text)
{
    m_link->setText(QStringLiteral("<span style=\"color:%1\">&#9679;</span>  %2")
                        .arg(ledColor, text.toHtmlEscaped()));
}
