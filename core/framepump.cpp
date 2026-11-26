#include "framepump.h"

#include "lanczos.h"
#include "scpiclient.h"

#include <QDateTime>
#include <QSettings>
#include <cmath>
#include <qmath.h>

FramePump::FramePump(QObject *parent)
    : QObject(parent)
    // Timers are parented (like ScpiClient's): moveToThread only moves an
    // object and its children, so parentless members would keep UI-thread
    // affinity while the worker starts/stops them - unsupported and a
    // heap-corruption hazard.
    , m_retry(this)
    , m_setupRetry(this)
    , m_watchdog(this)
    , m_refresh(this)
    , m_pace(this)
    , m_statTimer(this)
    , m_idlePoll(this)
    , m_ovPos(this)
{
    // The view's saved render mode (WaveView reads the same key); renderSincChanged after.
    m_alignSinc = QSettings().value(QStringLiteral("trace/sinc"), true).toBool();
    // Bench override of the rate cap: ZOYI_GUI_MIN_CYCLE_MS=0 runs uncapped.
    bool capOk = false;
    const int cap = qEnvironmentVariableIntValue("ZOYI_GUI_MIN_CYCLE_MS", &capOk);
    if (capOk && cap >= 0)
        m_minCycleMs = cap;
    // These only set a flag; frameDone() does the work between cycles,
    // so nothing ever lands inside a frame cycle.
    m_statTimer.setInterval(500);
    connect(&m_statTimer, &QTimer::timeout, this, [this] { m_statDue = true; });
    m_idlePoll.setInterval(300);
    connect(&m_idlePoll, &QTimer::timeout, this, [this] {
        if (m_active && m_idle && m_client) {
            m_client->submit(QStringLiteral("TRIG:STAT?"), cookie(RStat, 0), true, false,
                             true);
            pollTick(); // knob turns show while stopped too
        }
    });
    // One overview refetch 400 ms after the last move: horizontal (pan,
    // POS, Center, offset box), vertical (channel settings, ground tag).
    m_ovPos.setSingleShot(true);
    m_ovPos.setInterval(400);
    connect(&m_ovPos, &QTimer::timeout, this, [this] {
        if (m_active && m_running && !m_idle)
            m_ovDue = true;
    });
    m_retry.setSingleShot(true);
    m_retry.setInterval(30);
    connect(&m_retry, &QTimer::timeout, this, &FramePump::kickCycle);
    // Setup could not finish (no channel on, base PRE unusable).
    // Read the channel switches again, whose answers lead to sendSetup().
    m_setupRetry.setSingleShot(true);
    m_setupRetry.setInterval(2000);
    connect(&m_setupRetry, &QTimer::timeout, this, [this] {
        if (!m_active || !m_client)
            return;
        m_dispGot = 0;
        m_setupMask = 0;
        m_client->submit(QStringLiteral("CHAN1:DISP?"), cookie(SDisp, 1), true, false,
                         true);
        m_client->submit(QStringLiteral("CHAN2:DISP?"), cookie(SDisp, 2), true, false,
                         true);
    });

    m_watchdog.setSingleShot(true);
    m_watchdog.setInterval(4000);
    connect(&m_watchdog, &QTimer::timeout, this, &FramePump::onWatchdog);

    m_refresh.setInterval(1500);
    connect(&m_refresh, &QTimer::timeout, this, &FramePump::refreshHoriz);

    // Back-pressure to the render rate: short screen-window frames would
    // otherwise cycle at 300+ f/s, hammering the device's SCPI parser and
    // the UI thread alike.
    m_pace.setSingleShot(true);
    connect(&m_pace, &QTimer::timeout, this, &FramePump::kickCycle);
}

void FramePump::setClient(ScpiClient *client)
{
    m_client = client;
}

void FramePump::setPoints(int points)
{
    m_points = qMax(0, points); // applied at the next context send
}

void FramePump::start()
{
    if (!m_client || m_active)
        return;
    m_active = true;
    ++m_gen;
    m_dispGot = 0;
    m_setupMask = 0;
    m_setupQuiet = false;
    m_refreshMask = 0;
    m_recPoll = false;
    m_recCheckAfterScal = false;
    m_recChanged = false;
    m_horizCheck = false;
    m_settle = 0;
    m_rolling = false;
    m_xorigFallbackLogged = false;
    m_sour = 0;
    m_seq = -1;
    m_newSeqClock.invalidate();
    m_preCache[0].clear();
    m_preCache[1].clear();
    m_preGen[0] = m_preGen[1] = 0;
    m_preAge[0] = m_preAge[1] = 0;
    m_cyclePending = 0;
    m_nanSkips = 0;
    m_manualWin = false;
    m_viewShift = 0.0;
    m_running = true;
    m_ovDue = true; // one overview at connect
    m_ovAfterSettle = false;
    m_statDue = m_deepDue = false;
    m_recBusy = m_recDeep = m_idle = false;
    m_recPending = 0;
    m_frames = 0;
    m_rate.start();
    m_statTimer.start();
    // Full state read and capability probe first (pump lane, quiet).
    // A missing query answers NAN and pushes -113: *CLS clears those so a
    // later SYST:ERR? check does not report them against a user command.
    m_state = DeviceState{};
    m_caps = DeviceCaps{};
    m_capGot = 0;
    m_pollIdx = 0;
    m_perfWarned = false;
    m_alignHit = m_alignMiss = 0;
    m_pollClock.start();
    m_measClock.invalidate();
    for (int i = 0; i < DeviceState::kPollCount; ++i)
        submitPoll(i, SPoll, false);
    static const char *const kCaps[] = {"WAV:POIN?", "SYST:STAL?", "SYST:BOOT?",
                                        "WAV:CROS?", "WAV:POIN:MODE?", "WAV:FRAM?",
                                        "WAV:ENV? 100", "WAV:MEAS?"};
    for (int i = 0; i < 5; ++i)
        m_client->submit(QLatin1String(kCaps[i]), cookie(SCap, i), true, false, true);
    // Keep the WAV:FRAM? / WAV:ENV? probes tiny instead of 128 KB if POIN was 0;
    // sendSetup() restores WAV:POIN 0 and the screen window right after.
    m_client->submit(QStringLiteral("WAV:STAR 1"), 0, false, false, true);
    m_client->submit(QStringLiteral("WAV:STOP 100"), 0, false, false, true);
    m_client->submit(QStringLiteral("WAV:POIN 100"), 0, false, false, true);
    m_client->submit(QLatin1String(kCaps[5]), cookie(SCap, 5), true, true, true);
    m_client->submit(QLatin1String(kCaps[6]), cookie(SCap, 6), true, true, true);
    m_client->submit(QLatin1String(kCaps[7]), cookie(SCap, 7), true, false, true);
    m_client->submit(QStringLiteral("*CLS"), 0, false, false, true);
    emit info(QStringLiteral(". pump start (%1 pts/ch cap)")
                  .arg(m_points > 0 ? QString::number(m_points)
                                    : QStringLiteral("full window")));
    // Which channels are displayed decides what gets read every cycle.
    m_client->submit(QStringLiteral("CHAN1:DISP?"), cookie(SDisp, 1), true, false,
                     false);
    m_client->submit(QStringLiteral("CHAN2:DISP?"), cookie(SDisp, 2), true, false,
                     false);
    m_setupRetry.start(); // Recover if startup resync drops CHAN*:DISP?
}

void FramePump::stop()
{
    if (!m_active)
        return;
    m_active = false;
    m_retry.stop();
    m_setupRetry.stop();
    m_watchdog.stop();
    m_refresh.stop();
    m_pace.stop();
    m_statTimer.stop();
    m_idlePoll.stop();
    m_ovPos.stop();
    m_idle = false;
    m_recBusy = false;
    m_rolling = false;
    emit info(QStringLiteral(". pump stop"));
}

void FramePump::sendSetup(bool quiet)
{
    const int first = m_chOn[0] ? 1 : (m_chOn[1] ? 2 : 0);
    if (!first) {
        emit info(QStringLiteral("! no channels displayed - waiting (turn a CH on)"));
        m_watchdog.stop(); // nothing to cycle: the setup retry polls instead
        m_setupRetry.start();
        return;
    }
    m_setupRetry.stop();
    // Horizontal truth for the screen window, then the WAV context. POIN 0
    // first so the barrier PRE reports the raw record (count + base
    // interval); the screen STAR/STOP follow once those are known.
    m_client->submit(QStringLiteral("TIM:SCAL?"), cookie(SScal, 0), true, false,
                     quiet);
    m_client->submit(QStringLiteral("TRIG:POS?"), cookie(SPos, 0), true, false,
                     quiet);
    m_client->submit(QStringLiteral("TRIG:STAT?"), cookie(RStat, 0), true, false,
                     quiet);
    m_client->submit(QStringLiteral("WAV:SOUR CHAN%1").arg(first), 0, false, false,
                     quiet);
    m_client->submit(QStringLiteral("WAV:MODE RAW"), 0, false, false, quiet);
    m_client->submit(QStringLiteral("WAV:FORM BYTE"), 0, false, false, quiet);
    if (m_caps.peak)
        m_client->submit(QStringLiteral("WAV:POIN:MODE PEAK"), 0, false, false, quiet);
    m_client->submit(QStringLiteral("WAV:POIN 0"), 0, false, false, quiet);
    m_client->submit(QStringLiteral("WAV:PRE?"), cookie(SBase, 0), true, false,
                     quiet);
    m_sour = first;
}

void FramePump::resyncSetup()
{
    ++m_gen;
    dropRecord();
    m_recPoll = false;
    m_dispGot = 0;
    m_setupMask = 0;
    m_setupQuiet = true;
    m_preCache[0].clear();
    m_preCache[1].clear();
    m_client->submit(QStringLiteral("CHAN1:DISP?"), cookie(SDisp, 1), true, false, true);
    m_client->submit(QStringLiteral("CHAN2:DISP?"), cookie(SDisp, 2), true, false, true);
    m_watchdog.start(4000);
}

void FramePump::sendWindow(const QString &leadSet, bool ahead, bool checkRec)
{
    // WAV:STAR/STOP/POIN from the current window state (m_winStart/Count,
    // set by computeScreenWindow() or the stopped-mode view), plus an
    // optional leading TIM:SCAL/TIM:OFFS set. The trailing WAV:STAR? barrier
    // proves the window applied and re-kicks cycling without touching the
    // frame lease (a repeated WAV:PRE? releases the lease and stalls 1 s in
    // Normal mode). `ahead` routes the whole group through the UI lane under
    // the HORIZ key, so view-driven bursts coalesce to the newest group.
    // Every window group rides the UI lane under HORIZ, in order (test run
    // 10): an internal resend (record change, refresh) used the pump lane,
    // so it ran AFTER a newer wheel group that jumped ahead, left the meter
    // on the old window, and its barrier answer was dropped as stale. The
    // trace then mapped off screen and vanished. Only view-driven groups
    // (`ahead`) drop the queued ones; internal ones just queue behind.
    const QString key = QStringLiteral("HORIZ");
    if (ahead) {
        m_client->dropQueued(key);
        // A queued record chain would set STAR 1 / STOP recN after this
        // window: drop it too. A deep fetch stays due (m_deepDue).
        dropRecord();
    }
    ahead = true;
    if (!leadSet.isEmpty())
        m_client->submit(leadSet, 0, false, false, true, key, ahead);
    m_client->submit(QStringLiteral("WAV:STAR %1").arg(m_winStart), 0, false,
                     false, true, key, ahead);
    m_client->submit(QStringLiteral("WAV:STOP %1").arg(m_winStart + m_winCount - 1),
                     0, false, false, true, key, ahead);
    // Record check: with POIN 0, PRE? reports the whole record (count,
    // sample interval, record start) whatever the window (probe
    // 2026-11-19, bve 0.13.1). Sent only when the record may have changed
    // (timebase step), so restoring a window never triggers a repeated PRE?.
    if (checkRec) {
        m_client->submit(QStringLiteral("WAV:POIN 0"), 0, false, false, true, key, ahead);
        m_client->submit(QStringLiteral("WAV:PRE?"), cookie(SRec, 0), true, false, true,
                         key, ahead);
    }
    // Windows that fit the cap are fetched raw (POIN 0, stride 1): sinc
    // reconstruction is only legal on raw samples, and the display never
    // zooms into decimated points (Architecture).
    const int poin = m_winCount <= m_points ? 0 : m_points;
    m_winPoin = poin;
    if (!checkRec || poin > 0)
        m_client->submit(QStringLiteral("WAV:POIN %1").arg(poin), 0, false, false,
                         true, key, ahead);
    m_client->submit(QStringLiteral("WAV:STAR?"), cookie(SWin, 0), true, false, true,
                     key, ahead);
    emit info(QStringLiteral(". window STAR %1 STOP %2 of %3 (%4 s/div, trig %5%)")
                  .arg(m_winStart)
                  .arg(m_winStart + m_winCount - 1)
                  .arg(m_recN)
                  .arg(m_scal)
                  .arg(m_pos, 0, 'f', 1));
}

double FramePump::recordStart(double preT0, int count, double xinc)
{
    // PRE? xorig is normally -POS% of the record span (measured on the meter). Build 40
    // in Auto with no trigger event answered xorig 0 at POS 35.1 (seen
    // 2026-11-20), which put the screen window about 113 us past
    // the trigger and left the plot blank. A record start of exactly 0
    // with POS above 0 cannot hold the trigger at POS%, so derive it.
    if (m_pos <= 0.0 || count <= 0 || xinc <= 0.0 || std::abs(preT0) > 0.5 * xinc)
        return preT0;
    if (!m_xorigFallbackLogged) {
        m_xorigFallbackLogged = true;
        emit info(QStringLiteral(". PRE xorig 0 at POS %1%, record start taken from POS")
                      .arg(m_pos, 0, 'f', 1));
    }
    return -(m_pos / 100.0) * double(count) * xinc;
}

void FramePump::computeScreenWindow()
{
    // Screen window in raw samples. The trigger sits at POS% across the
    // 10-division screen; the record holds it at POS% of its own span
    // (verified: TRIG:POS 25 -> PRE xorig = -25% of the record span).
    const double posFrac = qBound(0.0, m_pos / 100.0, 1.0);
    double screenN = m_baseInt > 0.0 && m_scal > 0.0 ? (10.0 * m_scal) / m_baseInt
                                                      : double(m_recN);
    const int n = qRound(qBound(1.0, screenN, double(m_recN)));
    int start = 1 + qRound(posFrac * double(m_recN - n)); // STAR is 1-based
    // Running pan: dragging scrolls through the record,
    // the trigger stays put. Same record, the window just slides.
    if (m_viewShift != 0.0 && m_baseInt > 0.0)
        start = qBound(1, start + qRound(m_viewShift / m_baseInt), qMax(1, m_recN - n + 1));
    // Margin beyond both screen edges (fixed 2026-11-19): the sinc
    // kernel needs real neighbors at the visible edges (edge wiggle), and
    // a drag preview needs data past the edge (gap after release). Half a
    // screen each side while that stays within the point cap, at least 16
    // raw samples (more than the 10-lobe kernel) otherwise.
    int margin = n / 2;
    if (n + 2 * margin > m_points)
        margin = qMax(16, (m_points - n) / 2);
    const int first = qBound(1, start - margin, m_recN);
    const int last = qBound(first, start + n - 1 + margin, m_recN);
    m_winStart = first;
    m_winCount = last - first + 1;
}

void FramePump::kickCycle()
{
    if (!m_active || !m_client)
        return;
    m_pace.stop(); // a window barrier may also kick; never two cycles
    m_retry.stop(); // ditto for a pending NAN retry
    const bool roll = rolling();
    if (roll != m_rolling) {
        m_rolling = roll;
        emit info(roll ? QStringLiteral(". roll mode on (%1 s/div, Auto)").arg(m_scal)
                       : QStringLiteral(". roll mode off"));
        emit status(roll ? QStringLiteral("ROLL") : QStringLiteral("RUN"));
        m_preCache[0].clear();
        m_preCache[1].clear();
        if (!roll && m_running) {
            // Back to triggered frames: read the record fresh (SBase), which
            // sends the screen window; its SWin barrier re-kicks cycling.
            m_ovDue = true;
            m_client->submit(QStringLiteral("WAV:POIN 0"), 0, false, false, true);
            m_client->submit(QStringLiteral("WAV:PRE?"), cookie(SBase, 0), true, false, true);
            m_watchdog.start(4000);
            return;
        }
    }
    m_cycle = WaveFrame{};
    m_cycle.snapMs = QDateTime::currentMSecsSinceEpoch();
    m_cycleCh = -1;
    m_cycleStopwatch.restart();
    m_watchdog.start(4000);
    m_client->submit(QStringLiteral("WAV:SNAP"), 0, false, false, true);
    if (m_caps.frame && !m_rolling)
        m_client->submit(QStringLiteral("WAV:FRAM?"), cookie(CFram, 0), true, true, true);
    else
        m_client->submit(QStringLiteral("WAV:SEQ?"), cookie(SSeq, 0), true, false, true);
}

void FramePump::request(int tag, const QString &cmd, bool raw)
{
    if (!m_client)
        return;
    // Bit 40 marks a request cookie: valid in any gen, even before start().
    m_client->submit(cmd, (qint64(1) << 40) | (SUser << 8) | (tag & 0xff), true, raw, true,
                     QString(), true);
}

void FramePump::sendCommand(const QString &cmd)
{
    if (!m_client)
        return;
    m_client->submit(cmd, 0, false, false, false, QString(), true);
}

void FramePump::onText(qint64 cookieV, const QString &cmd, const QString &answer)
{
    Q_UNUSED(cmd);
    const int step = int((cookieV >> 8) & 0xff);
    const int ch = int(cookieV & 0xff);
    if ((cookieV >> 40) == 1 && step == SUser) {
        emit reply(ch, answer.trimmed(), QByteArray());
        return;
    }
    if (!m_active)
        return;
    // State answers are plain reads, valid whatever the gen: a window
    // barrier bumps gen while the connect-time reads are still queued.
    if (step == SPoll || step == SSet) {
        const bool changed = applyPoll(ch, answer.trimmed());
        if (changed) {
            // Something changed on the meter (V/div, offset, timebase):
            // the cached PRE? scaling may be stale, read it fresh.
            m_preCache[0].clear();
            m_preCache[1].clear();
        }
        // A set's read-back always reports: a refused step reads
        // back the old value, and the dock has to snap back to it.
        if (changed || step == SSet)
            emit stateChanged(m_state);
        if (step == SSet) {
            // The set released the frame lease: restart the cycle (idle:
            // refetch the held record so the new vertical state shows).
            if (m_idle) {
                m_idle = false;
                m_deepDue = true;
                m_idlePoll.stop();
                startRecordFetch(true);
            } else if (m_recPoll && !m_rolling) {
                // Record depth/rate changed (e.g. CH1/CH2 toggled: dual
                // channel halves record depth). Re-read record & window
                // before cycling so WAV:STAR/STOP stay in range for WAV:FRAM?.
                resyncSetup();
            } else {
                ++m_gen;
                restartAfterBarrier();
            }
        }
        return;
    }
    if (step == SCap) {
        const bool have = !answer.trimmed().startsWith(QLatin1String("NAN"));
        bool *flags[] = {&m_caps.poin, &m_caps.stall, &m_caps.boot, &m_caps.cros,
                         &m_caps.peak, nullptr, nullptr, &m_caps.meas};
        if (ch >= 0 && ch < 8 && flags[ch])
            *flags[ch] = have;
        if (++m_capGot == 8) {
            m_caps.done = true;
            emit info(QStringLiteral(". caps: POIN %1, STALl %2, BOOT %3, CROS %4, PEAK %5, FRAMe %6, ENV %7, MEAS %8")
                          .arg(m_caps.poin ? "yes" : "no", m_caps.stall ? "yes" : "no",
                               m_caps.boot ? "yes" : "no", m_caps.cros ? "yes" : "no",
                               m_caps.peak ? "yes" : "no", m_caps.frame ? "yes" : "no",
                               m_caps.env ? "yes" : "no", m_caps.meas ? "yes" : "no"));
            emit capsKnown(m_caps);
        }
        return;
    }
    if (step == CMeas) {
        const QString trimmed = answer.trimmed();
        if (trimmed.isEmpty() || trimmed.startsWith(QLatin1String("NAN")))
            return;
        const QStringList f = trimmed.split(QLatin1Char(','));
        if (f.size() < meas::Count + 2 || ch < 1 || ch > 2)
            return;
        meas::Result res;
        for (int i = 0; i < meas::Count; ++i) {
            bool ok = false;
            const double val = f.at(i).toDouble(&ok);
            if (ok && std::isfinite(val)) {
                res.v[i] = val;
                res.mask |= (1u << i);
            }
        }
        const bool clipped = f.at(meas::Count).toInt() != 0;
        res.clipLow = clipped;
        res.clipHigh = clipped;
        res.spectrum = (f.at(meas::Count + 1).trimmed() == QLatin1String("SPEC"));
        res.valid = (res.mask != 0);
        emit measReady(ch - 1, res);
        return;
    }
    if (!current(cookieV))
        return;
    if (!m_idle)
        m_watchdog.start(4000); // idle: only the status poll talks, no stall

    if (step == SDisp) {
        m_chOn[ch - 1] = answer.startsWith(QLatin1Char('1')) || answer == QLatin1String("ON")
                          || answer == QLatin1String("on");
        if (++m_dispGot == 2)
            sendSetup(m_setupQuiet);
    } else if (step == SScal || step == RScal) {
        const double v = answer.toDouble();
        if (v > 0.0) {
            if (step == RScal && m_scal > 0.0 && std::abs(v - m_scal) > 1e-9 * v)
                m_recPoll = true;
            m_scal = v;
            m_setupMask |= 1;
            m_refreshMask |= 1;
        }
    } else if (step == SPos || step == RPos) {
        const double v = answer.toDouble();
        if (v >= 0.0 && v <= 100.0) {
            if (step == RPos && std::abs(v - m_pos) >= 0.05)
                m_ovPos.start(); // moved on the meter: the record moved too
            m_pos = v;
            if (m_scal > 0.0)
                m_offs = (0.5 - m_pos / 100.0) * 10.0 * m_scal;
            m_setupMask |= 2;
            m_refreshMask |= 2;
        }
    } else if (step == RStat || step == SRun) {
        onStatus(answer.trimmed(), step == SRun);
    } else if (step == KSeq) {
        if (!m_recBusy)
            return; // the chain was cancelled while its SEQ? was in flight
        bool ok = false;
        m_rec.seq = answer.toInt(&ok);
        if (!ok) {
            // No finished frame to lease: give up this round. A deep fetch
            // stays due and is retried after the next cycle.
            m_recBusy = false;
            kickCycle();
            return;
        }
        // Per displayed channel: the whole record raw (POIN 0, STAR 1 ..
        // STOP recN), PRE? for its scaling, then the block. UI lane under
        // its own key "REC": a wheel or drag group, a set read-back,
        // a RUN/STOP barrier or the watchdog drop the rest of the chain
        // and re-send the screen window (dropRecord()).
        const QString key = QStringLiteral("REC");
        m_recPending = 0;
        for (int i = 0; i < 2; ++i) {
            if (!m_chOn[i])
                continue;
            const int c = i + 1;
            m_client->submit(QStringLiteral("WAV:SOUR CHAN%1").arg(c), 0, false, false,
                             true, key, true);
            m_client->submit(QStringLiteral("WAV:STAR 1"), 0, false, false, true, key,
                             true);
            m_client->submit(QStringLiteral("WAV:STOP %1").arg(m_recN), 0, false, false,
                             true, key, true);
            m_client->submit(QStringLiteral("WAV:POIN 0"), 0, false, false, true, key,
                             true);
            m_client->submit(QStringLiteral("WAV:PRE?"), cookie(KPre, c), true, false,
                             true, key, true);
            m_client->submit(QStringLiteral("WAV:DATA?"), cookie(KData, c), true, true,
                             true, key, true);
            ++m_recPending;
        }
        if (m_recPending == 0) {
            m_recBusy = false;
            kickCycle();
        }
    } else if (step == KPre) {
        const QStringList f = answer.split(QLatin1Char(','));
        ChannelFrame &cf = m_rec.ch[ch - 1];
        if (f.size() >= 10) {
            // POIN 0: PRE? describes the whole record (measured on the meter), which is
            // exactly what this chain fetches.
            cf.count = f.at(2).toInt();
            cf.xinc = f.at(4).toDouble();
            cf.xorig = recordStart(f.at(5).toDouble(), cf.count, cf.xinc);
            cf.yinc = f.at(7).toDouble();
            cf.yorg = f.at(8).toDouble();
            cf.yref = f.at(9).toDouble();
            cf.stride = 1;
            cf.vdiv = cf.yinc * kCodesPerDiv;
            cf.offs = cf.yorg * cf.yinc;
        }
    } else if (step == SBase) {
        bool baseOk = false;
        const QStringList f = answer.split(QLatin1Char(','));
        if (f.size() >= 10) {
            const int count = f.at(2).toInt();
            const double xinc = f.at(4).toDouble();
            if (count > 0 && xinc > 0.0) {
                if (m_setupQuiet
                    && (count != m_recN || std::abs(xinc - m_baseInt) > 1e-6 * xinc)) {
                    emit info(QStringLiteral(". record now %1 pts, %2 s/pt (was %3, %4)")
                                  .arg(count)
                                  .arg(xinc)
                                  .arg(m_recN)
                                  .arg(m_baseInt));
                }
                m_recN = count;
                m_baseInt = xinc;
                m_setupMask |= 4;
                baseOk = true;
            }
        }
        if (baseOk && (m_setupMask & 7) == 7) {
            computeScreenWindow();
            sendWindow(QString(), false, false);
        } else {
            // Nothing would ever restart the pump. Redo the setup.
            emit info(QStringLiteral("! base PRE unusable (%1), setup again in 2 s")
                          .arg(answer));
            m_watchdog.stop();
            m_setupRetry.start();
        }
    } else if (step == SRec || step == RRec) {
        const QStringList f = answer.split(QLatin1Char(','));
        if (f.size() >= 10) {
            const int count = f.at(2).toInt();
            const double xinc = f.at(4).toDouble();
            const bool sameRate = std::abs(xinc - m_baseInt) <= 1e-6 * xinc;
            // Roll timebase: the record grows sample by sample, so a
            // count change at the same rate is not a new record. Redoing
            // the window on it looped forever at 500 ms/div.
            const bool growing = sameRate && m_scal >= 0.2 * (1.0 - 1e-6);
            if (rolling() || growing) {
                if (count > 0)
                    m_recN = count;
            } else if (count > 0 && xinc > 0.0 && (count != m_recN || !sameRate)) {
                emit info(QStringLiteral(". record now %1 pts, %2 s/pt (was %3, %4)")
                              .arg(count)
                              .arg(xinc)
                              .arg(m_recN)
                              .arg(m_baseInt));
                m_recN = count;
                m_baseInt = xinc;
                if (step == SRec) {
                    m_recChanged = true; // SWin redoes the window
                } else if (m_running && !m_manualWin) {
                    m_ovDue = true; // the strip shows the old record: refetch
                    // Late record change (bug m, 50-100 ms/div): the meter
                    // finished the new-rate record after the window group's
                    // check. Redo the window now. Stopped mode just keeps
                    // the new interval for the view's next window.
                    computeScreenWindow();
                    sendWindow(QString(), false, false);
                }
            }
        }
    } else if (step == SWin) {
        // Window applied. Drop anything a mid-flight cycle still expects
        // (gen bump) and start a fresh cycle; a refresh-driven window send
        // lands between cycles and just re-kicks the next one.
        ++m_gen;
        // Any record chain is over (done or dropped by a view group). One
        // queued behind this window would move STAR/STOP again: drop it.
        dropRecord();
        if (m_idle)
            return; // stopped with the deep record: no cycles until RUN
        if (m_settle == 2 && !m_recChanged && !m_horizCheck)
            m_settle = 0; // the corrected window is in: frames are good
        if (m_recChanged) {
            // The window was computed on the old record: redo it once. The
            // record check does not depend on the window, so this settles.
            m_recChanged = false;
            if (m_running && !m_manualWin && !rolling()) {
                computeScreenWindow();
                sendWindow(QString(), false, false);
                return;
            }
        }
        if (m_horizCheck) {
            // Queued ahead of the cycle's SNAP, under the new gen, so the
            // answers land before frameDone() and it re-sends the window.
            m_horizCheck = false;
            refreshHoriz();
        }
        kickCycle();
        m_refresh.start(1500);
    } else if (step == SRollScal) {
        // (C) barrier: the roll timebase step is in. kickCycle() picks roll
        // on or off from the new scale.
        const double v = answer.toDouble();
        if (v > 0.0) {
            m_scal = v;
            m_offs = (0.5 - m_pos / 100.0) * 10.0 * m_scal;
        }
        ++m_gen;
        m_preCache[0].clear();
        m_preCache[1].clear();
        kickCycle();
        m_refresh.start(1500);
    } else if (step == SPre) {
        kickCycle(); // initial barrier (kept for compatibility)
    } else if (step == SSeq) {
        bool ok = false;
        const int seq = answer.toInt(&ok);
        if (!ok) {
            // NAN: the meter has no finished frame yet (filling the record
            // at slow timebases, or the DMA copy race). Skip and retry. Log
            // the first skip of a run only, the count when it ends.
            if (m_nanSkips++ == 0)
                emit info(QStringLiteral(". seq %1 - frame skipped").arg(answer));
            m_retry.start();
            return;
        }
        const bool waited = m_nanSkips > 0;
        if (m_nanSkips > 1)
            emit info(QStringLiteral(". seq NAN run ended, %1 frames skipped")
                          .arg(m_nanSkips));
        m_nanSkips = 0;
        if (seq != m_seq) {
            if (m_running
                && (waited || (m_newSeqClock.isValid() && m_newSeqClock.elapsed() >= 500)))
                m_ovDue = true; // new capture after a wait: refresh the strip
            m_newSeqClock.restart();
        }
        m_seq = seq;
        if (m_rolling) {
            // The record length changes every SNAP: read it first.
            m_client->submit(QStringLiteral("WAV:POIN 0"), 0, false, false, true);
            m_client->submit(QStringLiteral("WAV:PRE?"), cookie(SRollN, 0), true, false, true);
            m_winPoin = 0;
        } else {
            readChannel();
        }
    } else if (step == SRollN) {
        readRoll(answer);
    } else if (step == CPre) {
        const bool ok = applyCyclePre(ch, answer);
        m_cyclePreOk[ch - 1] = ok;
        m_preCache[ch - 1] = ok ? answer : QString();
        m_preGen[ch - 1] = m_gen;
        m_preAge[ch - 1] = 0;
    }
}

bool FramePump::applyCyclePre(int ch, const QString &answer)
{
    {
        const QStringList f = answer.split(QLatin1Char(','));
        ChannelFrame &cf = m_cycle.ch[ch - 1];
        if (f.size() < 10) {
            emit info(QStringLiteral("! short PRE (%1)").arg(answer));
            return false;
        }
        m_cyclePeak[ch - 1] = f.at(1).toInt() == 1;
        cf.count = f.at(2).toInt();
        cf.xinc = f.at(4).toDouble();
        cf.xorig = f.at(5).toDouble();
        cf.yinc = f.at(7).toDouble();
        cf.yorg = f.at(8).toDouble();
        cf.yref = f.at(9).toDouble();
        cf.vdiv = cf.yinc * kCodesPerDiv;
        cf.offs = cf.yorg * cf.yinc;
        if (cf.count <= 0 || cf.xinc <= 0.0 || cf.yinc <= 0.0) {
            emit info(QStringLiteral("! bad PRE (%1) - channel %2 skipped")
                          .arg(answer)
                          .arg(ch));
            return false;
        }
        // PRE? semantics, checked 2026-11-19 on OpenOSC 0.13.1:
        //  - POIN 0: PRE? describes the WHOLE record whatever STAR/STOP
        //    (count = record, xinc = sample interval, xorig = record start).
        //    The window's time base is derived from the window we set.
        //  - POIN > 0: PRE? describes the decimated window exactly
        //    (count = points returned, xinc = stride x interval, xorig =
        //    window start). Taken as is. Mixing the two drew decimated
        //    windows far off screen (blank trace at wide zoom, test run 4).
        if (m_rolling) {
            // Roll: PRE xorig is 0 by design (no trigger). onRaw() places
            // the block with its newest sample on the right screen edge.
            if (m_winPoin == 0 || m_baseInt <= 0.0) {
                cf.xinc = m_baseInt > 0.0 ? m_baseInt : cf.xinc;
                cf.stride = 1;
            } else {
                cf.stride = qMax(1, qRound(cf.xinc / m_baseInt));
            }
            return true;
        }
        if (m_winPoin == 0) {
            m_recT0 = recordStart(cf.xorig, cf.count, cf.xinc);
            if (m_baseInt > 0.0) {
                cf.xorig = m_recT0 + double(m_winStart - 1) * m_baseInt;
                cf.xinc = m_baseInt;
            }
            cf.stride = 1;
        } else {
            cf.stride = m_baseInt > 0.0 ? qMax(1, qRound(cf.xinc / m_baseInt)) : 1;
            if (m_baseInt > 0.0) {
                const double t0 = cf.xorig - double(m_winStart - 1) * m_baseInt;
                m_recT0 = recordStart(t0, m_recN, m_baseInt);
                cf.xorig += m_recT0 - t0; // same shift for the window
            }
        }
    }
    return true;
}

void FramePump::onRaw(qint64 cookieV, const QString &cmd, const QByteArray &data)
{
    Q_UNUSED(cmd);
    if ((cookieV >> 40) == 1 && ((cookieV >> 8) & 0xff) == SUser) {
        emit reply(int(cookieV & 0xff), QString(), data);
        return;
    }
    if (!m_active)
        return;
    if (((cookieV >> 8) & 0xff) == SCap) {
        const int idx = int(cookieV & 0xff);
        if (idx == 5)
            m_caps.frame = !data.isEmpty();
        else if (idx == 6)
            m_caps.env = !data.isEmpty();
        if (++m_capGot == 8) {
            m_caps.done = true;
            emit info(QStringLiteral(". caps: POIN %1, STALl %2, BOOT %3, CROS %4, PEAK %5, FRAMe %6, ENV %7, MEAS %8")
                          .arg(m_caps.poin ? "yes" : "no", m_caps.stall ? "yes" : "no",
                               m_caps.boot ? "yes" : "no", m_caps.cros ? "yes" : "no",
                               m_caps.peak ? "yes" : "no", m_caps.frame ? "yes" : "no",
                               m_caps.env ? "yes" : "no", m_caps.meas ? "yes" : "no"));
            emit capsKnown(m_caps);
        }
        return;
    }
    if (!current(cookieV))
        return;
    m_watchdog.start(4000);
    const int step = int((cookieV >> 8) & 0xff);
    const int ch = int(cookieV & 0xff);
    if (step == CFram) {
        handleFrameBlock(data);
        return;
    }
    if (step == KEnv) {
        handleEnvBlock(data);
        return;
    }
    if (step == KData) {
        if (!m_recBusy)
            return; // chain cancelled by a window group meanwhile
        ChannelFrame &rf = m_rec.ch[ch - 1];
        const int n = data.size();
        if (rf.yinc > 0.0 && rf.xinc > 0.0 && n > 0) {
            rf.v.resize(n);
            const float yorg = float(rf.yorg), yref = float(rf.yref),
                        yinc = float(rf.yinc);
            for (int i = 0; i < n; ++i)
                rf.v[i] = (float(quint8(data.at(i))) - yorg - yref) * yinc;
            rf.count = n;
            rf.valid = true;
        }
        if (--m_recPending <= 0)
            finishRecord();
        return;
    }
    ChannelFrame &cf = m_cycle.ch[ch - 1];
    if (!m_cyclePreOk[ch - 1]) {
        cf.valid = false; // PRE? was unusable: the block cannot be scaled
        channelDone();
        return;
    }
    const bool peak = m_cyclePeak[ch - 1];
    const int n = peak ? data.size() / 2 : data.size();
    cf.v.resize(n);
    const float yorg = float(cf.yorg), yref = float(cf.yref), yinc = float(cf.yinc);
    if (peak) {
        cf.vHi.resize(n);
        for (int i = 0; i < n; ++i) {
            cf.v[i] = (float(quint8(data.at(2 * i))) - yorg - yref) * yinc;
            cf.vHi[i] = (float(quint8(data.at(2 * i + 1))) - yorg - yref) * yinc;
        }
    } else {
        cf.vHi.clear();
        for (int i = 0; i < n; ++i)
            cf.v[i] = (float(quint8(data.at(i))) - yorg - yref) * yinc;
    }
    cf.count = n;
    cf.valid = n > 0;
    // Raw window shorter than asked (record end): keep raw xinc/stride.
    // Decimated windows already carry PRE?'s exact stride and xinc (see CPre).
    if (m_rolling && n > 0) {
        // Newest sample on the right screen edge. The screen spans
        // -POS% .. (100-POS)% of 10 divisions around t = 0.
        const double right = (1.0 - m_pos / 100.0) * 10.0 * m_scal;
        cf.xorig = right - double(n - 1) * cf.xinc;
        cf.tshift = 0.0;
    }
    channelDone();
}

bool FramePump::rolling() const
{
    // Firmware rule (request.hpp): ACQ:ROLL on, TIM:SCAL >= 200 ms/div,
    // sweep AUTO. Unknown ROLL counts as on (the firmware default).
    const TriggerState &t = m_state.trig;
    const Telemetry &y = m_state.tele;
    const bool rollOn = !(y.known & (1 << DeviceState::YRoll)) || y.roll;
    return m_running && !m_idle && rollOn && m_scal >= 0.2 * (1.0 - 1e-6)
           && (t.known & (1 << DeviceState::TSwe)) && t.sweep == 0;
}

void FramePump::readRoll(const QString &pre)
{
    // probe_2d: with POIN 0, PRE? count is the samples so far (grows from
    // the roll start, then holds at the record size as a sliding window,
    // newest last). Fetch the newest screen width of it.
    const QStringList f = pre.split(QLatin1Char(','));
    const int count = f.size() >= 10 ? f.at(2).toInt() : 0;
    const double xinc = f.size() >= 10 ? f.at(4).toDouble() : 0.0;
    if (count < 2 || xinc <= 0.0) {
        m_retry.start(); // nothing rolled yet: try again shortly
        return;
    }
    m_baseInt = xinc;
    m_recN = count;
    // Plus kRollMarginS of older samples: the view draws the trace a fetch
    // interval late and slides it left between frames (smooth scroll), so
    // the left edge needs data from before the screen.
    const int screenN = qMax(1, qRound((10.0 * m_scal + kRollMarginS) / m_baseInt));
    const int n = qMin(count, screenN);
    m_winStart = count - n + 1;
    m_winCount = n;
    m_winPoin = n <= m_points ? 0 : m_points;
    const double right = (1.0 - m_pos / 100.0) * 10.0 * m_scal;
    m_recT0 = right - double(count - 1) * m_baseInt;
    m_cyclePending = 0;
    if (m_caps.frame) {
        m_client->submit(QStringLiteral("WAV:STAR %1").arg(m_winStart), 0, false, false, true);
        m_client->submit(QStringLiteral("WAV:STOP %1").arg(count), 0, false, false, true);
        m_client->submit(QStringLiteral("WAV:POIN %1").arg(m_winPoin), 0, false, false, true);
        m_client->submit(QStringLiteral("WAV:FRAM?"), cookie(CFram, 0), true, true, true);
        return;
    }
    for (int i = 0; i < 2; ++i) {
        if (!m_chOn[i])
            continue;
        const int ch = i + 1;
        m_client->submit(QStringLiteral("WAV:SOUR CHAN%1").arg(ch), 0, false, false, true);
        m_sour = ch;
        m_client->submit(QStringLiteral("WAV:STAR %1").arg(m_winStart), 0, false, false, true);
        m_client->submit(QStringLiteral("WAV:STOP %1").arg(count), 0, false, false, true);
        m_client->submit(QStringLiteral("WAV:POIN %1").arg(m_winPoin), 0, false, false, true);
        m_client->submit(QStringLiteral("WAV:PRE?"), cookie(CPre, ch), true, false, true);
        m_client->submit(QStringLiteral("WAV:DATA?"), cookie(CData, ch), true, true, true);
        ++m_cyclePending;
    }
    if (m_cyclePending == 0)
        frameDone();
}

bool FramePump::parseBlock(const QByteArray &data, int &seq, ChannelFrame (&out)[2],
                           bool &parsedAny, QString *err)
{
    parsedAny = false;
    int pos = data.indexOf('\n');
    if (pos < 0)
        return false;
    const QList<QByteArray> hdr = data.left(pos).trimmed().split(',');
    bool seqOk = false, nchOk = false;
    seq = hdr.size() >= 2 ? hdr.at(0).toInt(&seqOk) : -1;
    const int nch = hdr.size() >= 2 ? hdr.at(1).toInt(&nchOk) : 0;
    if (!seqOk || !nchOk || nch < 1 || nch > 2) {
        if (err)
            *err = QStringLiteral("! bad WAV:FRAM? header");
        return false;
    }
    ++pos;

    struct Parsed {
        int idx = -1;
        int type = 0;
        int pts = 0;
    };
    Parsed parsed[2];
    for (int k = 0; k < nch; ++k) {
        const int nl = data.indexOf('\n', pos);
        if (nl < 0)
            return false;
        const QString line = QString::fromLatin1(data.constData() + pos, nl - pos).trimmed();
        pos = nl + 1;
        const QStringList f = line.split(QLatin1Char(','));
        if (f.size() < 11 || !f.at(0).startsWith(QLatin1String("CHAN"))) {
            if (err)
                *err = QStringLiteral("! bad block channel line (%1)").arg(line);
            return false;
        }
        const int c = f.at(0).mid(4).toInt();
        if (c < 1 || c > 2)
            return false;
        const int idx = c - 1;
        ChannelFrame &cf = out[idx];
        parsed[k].idx = idx;
        parsed[k].type = f.at(2).toInt();
        parsed[k].pts = f.at(3).toInt();
        cf.count = parsed[k].pts;
        cf.xinc = f.at(5).toDouble();
        cf.xorig = f.at(6).toDouble();
        cf.yinc = f.at(8).toDouble();
        cf.yorg = f.at(9).toDouble();
        cf.yref = f.at(10).toDouble();
        cf.vdiv = cf.yinc * kCodesPerDiv;
        cf.offs = cf.yorg * cf.yinc;
        if (cf.count <= 0 || cf.xinc <= 0.0 || cf.yinc <= 0.0)
            return false;
    }
    parsedAny = true;

    for (int k = 0; k < nch; ++k) {
        const int idx = parsed[k].idx;
        const int pts = parsed[k].pts;
        const int bpp = parsed[k].type == 1 ? 2 : 1;
        const int need = pts * bpp;
        if (pos + need > data.size()) {
            if (err)
                *err = QStringLiteral("! short block payload (%1 < %2)")
                           .arg(data.size() - pos)
                           .arg(need);
            return false;
        }
        ChannelFrame &cf = out[idx];
        cf.v.resize(pts);
        const float yorg = float(cf.yorg), yref = float(cf.yref), yinc = float(cf.yinc);
        const char *src = data.constData() + pos;
        if (bpp == 2) {
            cf.vHi.resize(pts);
            for (int i = 0; i < pts; ++i) {
                cf.v[i] = (float(quint8(src[2 * i])) - yorg - yref) * yinc;
                cf.vHi[i] = (float(quint8(src[2 * i + 1])) - yorg - yref) * yinc;
            }
        } else {
            cf.vHi.clear();
            for (int i = 0; i < pts; ++i)
                cf.v[i] = (float(quint8(src[i])) - yorg - yref) * yinc;
        }
        pos += need;
        cf.count = pts;
        cf.valid = pts > 0;
    }
    return true;
}

void FramePump::handleFrameBlock(const QByteArray &data)
{
    // 0.18.0 WAV:FRAM? block: "seq,nch\n", then nch lines of
    // "CHANn,0,type,points,1,xinc,xorig,0,yinc,yorg,yref\n", then nch
    // contiguous raw byte blocks (exact window preamble, even with POIN 0).
    if (data.isEmpty()) {
        if (!m_rolling) {
            const bool fast = m_cycleStopwatch.elapsed() < 250;
            if (m_nanSkips++ == 0)
                emit info(QStringLiteral(". frame NAN - frame skipped"));
            // A fast NAN (< 250 ms, not a 1 s waitFrame timeout) means
            // WAV:FRAM? failed immediately: typically -222 because the
            // record shrank (CH2 toggled, timebase/depth changed on meter)
            // or -230 because no channel is enabled. Re-read disp + record.
            if (fast && (m_nanSkips == 1 || m_nanSkips % 10 == 0)) {
                resyncSetup();
                return;
            }
        }
        m_retry.start();
        return;
    }

    int seq = -1;
    bool parsedAny = false;
    QString err;
    if (!parseBlock(data, seq, m_cycle.ch, parsedAny, &err)) {
        if (!err.isEmpty())
            emit info(err);
        m_retry.start();
        return;
    }

    if (!m_rolling) {
        const bool waited = m_nanSkips > 0;
        if (m_nanSkips > 1)
            emit info(QStringLiteral(". seq NAN run ended, %1 frames skipped")
                          .arg(m_nanSkips));
        m_nanSkips = 0;
        if (seq != m_seq) {
            if (m_running
                && (waited || (m_newSeqClock.isValid() && m_newSeqClock.elapsed() >= 500)))
                m_ovDue = true;
            m_newSeqClock.restart();
        }
        m_seq = seq;
    }

    for (int idx = 0; idx < 2; ++idx) {
        ChannelFrame &cf = m_cycle.ch[idx];
        if (!cf.valid)
            continue;
        if (m_rolling) {
            if (m_winPoin == 0 || m_baseInt <= 0.0) {
                cf.xinc = m_baseInt > 0.0 ? m_baseInt : cf.xinc;
                cf.stride = 1;
            } else {
                cf.stride = qMax(1, qRound(cf.xinc / m_baseInt));
            }
            const double right = (1.0 - m_pos / 100.0) * 10.0 * m_scal;
            cf.xorig = right - double(cf.count - 1) * cf.xinc;
            cf.tshift = 0.0;
        } else {
            cf.stride = m_baseInt > 0.0 ? qMax(1, qRound(cf.xinc / m_baseInt)) : 1;
            if (m_baseInt > 0.0) {
                const double t0 = cf.xorig - double(m_winStart - 1) * m_baseInt;
                m_recT0 = recordStart(t0, m_recN, m_baseInt);
                cf.xorig += m_recT0 - t0;
            }
        }
    }
    frameDone();
}

void FramePump::handleEnvBlock(const QByteArray &data)
{
    if (!m_recBusy)
        return;
    if (data.isEmpty()) {
        m_recBusy = false;
        m_ovDue = true;
        kickCycle();
        return;
    }
    int seq = -1;
    bool parsedAny = false;
    QString err;
    if (!parseBlock(data, seq, m_rec.ch, parsedAny, &err)) {
        if (!err.isEmpty())
            emit info(err);
        m_recBusy = false;
        m_ovDue = true;
        kickCycle();
        return;
    }
    m_rec.seq = seq;
    for (int idx = 0; idx < 2; ++idx) {
        ChannelFrame &cf = m_rec.ch[idx];
        if (!cf.valid)
            continue;
        cf.stride = m_baseInt > 0.0 ? qMax(1, qRound(cf.xinc / m_baseInt)) : 1;
        cf.xorig = recordStart(cf.xorig, cf.count, cf.xinc);
    }
    finishRecord();
    kickCycle();
}

void FramePump::readChannel()
{
    // The whole cycle goes out at once and the client pipelines it
    // (probe_2c: 2 channels 23.6 ms/frame pipelined vs about 46 ms chained).
    // Answers come back in order: PRE? (when sent) before its DATA?.
    m_cyclePending = 0;
    for (int i = 0; i < 2; ++i) {
        if (!m_chOn[i])
            continue;
        m_cycleCh = i;
        const int ch = i + 1;
        if (m_sour != ch) {
            m_client->submit(QStringLiteral("WAV:SOUR CHAN%1").arg(ch), 0, false,
                             false, true);
            m_sour = ch;
        }
        if (!m_preCache[i].isEmpty() && m_preGen[i] == m_gen && m_preAge[i] < kPreEvery) {
            ++m_preAge[i];
            m_cyclePreOk[i] = applyCyclePre(ch, m_preCache[i]);
        } else {
            m_client->submit(QStringLiteral("WAV:PRE?"), cookie(CPre, ch), true, false,
                             true);
        }
        m_client->submit(QStringLiteral("WAV:DATA?"), cookie(CData, ch), true, true,
                         true);
        ++m_cyclePending;
    }
    if (m_cyclePending == 0)
        frameDone();
}

void FramePump::channelDone()
{
    if (--m_cyclePending <= 0)
        frameDone();
}

void FramePump::frameDone()
{
    m_cycle.seq = m_seq;
    m_cycle.stampMs = QDateTime::currentMSecsSinceEpoch();
    m_cycle.running = m_running;
    m_cycle.scal = m_scal;
    m_cycle.pos = m_pos;
    m_cycle.recT0 = m_recT0;
    m_cycle.recT1 = m_recT0 + double(m_recN - 1) * m_baseInt;
    if (m_rolling && m_scal > 0.0) {
        // Roll: the view is the screen, newest sample on the right edge.
        // A record still shorter than the screen made the view center on
        // it, so the trace grew from the middle.
        m_cycle.recT1 = (1.0 - m_pos / 100.0) * 10.0 * m_scal;
        m_cycle.recT0 = m_cycle.recT1 - 10.0 * m_scal;
    }
    m_cycle.roll = m_rolling;
    m_cycle.baseInt = m_baseInt;
    m_cycle.recN = m_recN;

    // A completed SCAL/POS refresh pair re-sends the screen window before
    // the next cycle (only while running: stopped keeps the view's manual
    // window); the barrier PRE (SWin) re-kicks cycling. Otherwise chain
    // on, paced to the rate cap. Decided before the emit: after a wheel
    // step this frame is good only if the meter's answer confirms the
    // window it was fetched with.
    bool resend = false;
    if (m_refreshMask == 3) {
        m_refreshMask = 0;
        if (m_running && !m_rolling) {
            const int oldStart = m_winStart, oldCount = m_winCount;
            computeScreenWindow();
            resend = m_winStart != oldStart || m_winCount != oldCount;
            if (m_settle == 1)
                m_settle = resend ? 2 : 0;
        }
    }
    if (!m_running)
        m_settle = 0;
    if (m_rolling) {
        // Roll: no trigger, no overview strip, no record re-read (the
        // count changes every frame by design).
        m_settle = 0;
        m_recPoll = false;
        m_ovDue = m_ovAfterSettle = false;
    }
    m_cycle.settled = m_settle == 0;
    if (m_align && !m_rolling)
        alignTrigger();
    emit frame(m_cycle);
    ++m_frames;
    rateTick();
    if (resend) {
        sendWindow();
        return;
    }
    // Whole-record fetches take the place of the next cycle: the deep
    // one once after a stop, the overview one event-driven while running
    // (on the current frame lease, so it runs before any m_recPoll PRE?).
    if (!m_running && m_deepDue) {
        startRecordFetch(true);
        return;
    }
    if (m_ovAfterSettle && m_settle == 0) {
        // One fetch per wheel burst: every step re-arms m_settle, so this
        // fires only after the last step's window is confirmed.
        m_ovAfterSettle = false;
        m_ovDue = true;
    }
    if (m_running && m_ovDue) {
        m_ovDue = false;
        // From about 20 ms/div the screen window is the whole record, and
        // every capture comes 500 ms or more after the last, so a fetch
        // here ran on every frame: 0.4 f/s at 50 ms/div where the meter
        // gives 2.1 (measured 2026-11-20). The frame already holds
        // the record: hand it to the strip as is.
        const int edge = m_recN / 100;
        if (m_recN > 0 && m_winStart <= 1 + edge
            && m_winStart + m_winCount - 1 >= m_recN - edge) {
            WaveFrame r = m_cycle;
            r.full = true;
            r.settled = true;
            emit record(r);
        } else {
            startRecordFetch(false);
            return;
        }
    }
    if (m_recPoll) {
        // Record re-read between cycles (bug m): POIN 0 makes PRE? describe
        // the whole record, then the window's POIN is restored before the
        // next SNAP. A change is handled in the RRec answer.
        m_recPoll = false;
        m_client->submit(QStringLiteral("WAV:POIN 0"), 0, false, false, true);
        m_client->submit(QStringLiteral("WAV:PRE?"), cookie(RRec, 0), true, false, true);
        if (m_winPoin > 0)
            m_client->submit(QStringLiteral("WAV:POIN %1").arg(m_winPoin), 0, false,
                             false, true);
    }
    if (m_statDue) {
        m_statDue = false;
        m_client->submit(QStringLiteral("TRIG:STAT?"), cookie(RStat, 0), true, false,
                         true);
    }
    if (m_caps.meas && !m_rolling && m_cycle.settled
        && (!m_measClock.isValid() || m_measClock.elapsed() >= 250)) {
        bool any = false;
        for (int i = 0; i < 2; ++i) {
            if (!m_chOn[i] || !m_cycle.ch[i].valid || m_cycle.ch[i].stride <= 1)
                continue;
            if (!any) {
                m_measClock.restart();
                any = true;
            }
            const int c = i + 1;
            if (m_sour != c) {
                m_client->submit(QStringLiteral("WAV:SOUR CHAN%1").arg(c), 0, false,
                                 false, true);
                m_sour = c;
            }
            m_client->submit(QStringLiteral("WAV:MEAS?"), cookie(CMeas, c), true, false,
                             true);
        }
    }
    pollTick(); // At most one state query per frame gap

    const qint64 used = m_cycleStopwatch.elapsed();
    const int minMs = m_rolling ? qMax(m_minCycleMs, kRollCycleMs) : m_minCycleMs;
    if (used < minMs)
        m_pace.start(int(minMs - used));
    else
        kickCycle();
}

void FramePump::refreshHoriz()
{
    if (!m_active || !m_client)
        return;
    m_refreshMask = 0;
    if (m_recCheckAfterScal) {
        m_recCheckAfterScal = false;
        m_recPoll = true; // one late record check after TIM:SCAL (bug m)
    }
    m_client->submit(QStringLiteral("TIM:SCAL?"), cookie(RScal, 0), true, false, true);
    m_client->submit(QStringLiteral("TRIG:POS?"), cookie(RPos, 0), true, false, true);
    m_client->submit(QStringLiteral("TRIG:STAT?"), cookie(RStat, 0), true, false,
                     true);
}

void FramePump::syncFromOffset()
{
    // TIM:OFFS and TRIG:POS are the same knob (measured on the meter): keep both
    // consistent and clamped to the +-half-screen range POS allows.
    const double span = 10.0 * m_scal;
    if (span <= 0.0)
        return;
    m_offs = qBound(-span / 2.0, m_offs, span / 2.0);
    m_pos = qBound(0.0, 50.0 - 100.0 * m_offs / span, 100.0);
    m_offs = (0.5 - m_pos / 100.0) * span;
}

double FramePump::ladderStep(double cur, int dir)
{
    // 1-2-5 ladder, 10 ns .. 20 s (the firmware's TIM:SCAL range). the
    // loop ran one step past it, to 50 s/div.
    static const QVector<double> ladder = [] {
        QVector<double> v;
        for (int e = -8; e <= 1; ++e) {
            const double base = std::pow(10.0, e);
            v << base << 2.0 * base;
            if (e < 1)
                v << 5.0 * base;
        }
        return v;
    }();
    int idx = 0;
    double best = -1.0;
    for (int i = 0; i < ladder.size(); ++i) {
        const double r = ladder.at(i) / (cur > 0.0 ? cur : 1e-8);
        const double dist = r > 1.0 ? r : 1.0 / r;
        if (best < 0.0 || dist < best) {
            best = dist;
            idx = i;
        }
    }
    return ladder.at(qBound(0, idx + dir, ladder.size() - 1));
}

void FramePump::stepTimeScale(double s)
{
    // View requested a new timebase (wheel). Send SCAL + window as one
    // coalesced group; local state updates immediately so the preview and
    // the next window agree even before the device answers.
    if (!m_active || !m_client || !m_running)
        return;
    if (s <= 0.0)
        return;
    m_scal = ladderStep(s, 0); // snap: firmware range is the 1-2-5 ladder
    // Answers already in flight describe the old timebase: a late RScal
    // would undo m_scal and a stale refresh pair would end the hold early.
    // New gen drops them (the window group below is sent under it).
    ++m_gen;
    m_refreshMask = 0;
    // The meter keeps TRIG:POS (%) across a SCAL change, not TIM:OFFS:
    // every guess made by keeping the offset was corrected back to the
    // old percent (seen 2026-11-19). So keep m_pos, derive m_offs.
    m_offs = (0.5 - m_pos / 100.0) * 10.0 * m_scal;
    syncFromOffset();
    if (m_rolling || rolling()) {
        // (C): while rolling, or rolling after the step, the meter restarts
        // its roll record and a window guessed on the old one gets -222.
        // Send only the scale, then a TIM:SCAL? barrier: kickCycle() then
        // goes on rolling, or leaves roll through SBase and the window.
        const QString key = QStringLiteral("HORIZ");
        m_client->dropQueued(key);
        dropRecord();
        m_client->submit(QStringLiteral("TIM:SCAL %1").arg(m_scal, 0, 'e', 2), 0, false,
                         false, true, key, true);
        m_client->submit(QStringLiteral("TIM:SCAL?"), cookie(SRollScal, 0), true, false,
                         true, key, true);
        m_settle = 0;
        return;
    }
    computeScreenWindow();
    sendWindow(QStringLiteral("TIM:SCAL %1").arg(m_scal, 0, 'e', 2), true, true);
    // The window above is a guess (meter's position rule across a SCAL
    // change not probed). Re-read SCAL/POS right after the barrier instead
    // of waiting up to 1.5 s for refreshHoriz: the stale window was the
    // 1-1.5 s blank / half trace after a wheel step (test run 3).
    m_horizCheck = true;
    m_recCheckAfterScal = true;
    m_settle = 1;
    m_ovAfterSettle = true;
}

void FramePump::setViewShift(double s)
{
    // Running pan: only WAV:STAR/STOP move, no TRIG:POS or TIM:OFFS, so the
    // meter's capture (and its record) is unchanged. Ignored in roll mode.
    if (!m_active || !m_client || !m_running || m_idle || m_rolling)
        return;
    m_viewShift = s;
    computeScreenWindow();
    sendWindow(QString(), true, false);
    m_ovPos.start(); // refresh the strip once the pan ends
}

void FramePump::setViewWindow(double center, double span)
{
    // Stopped-mode deep zoom/pan: the view owns STAR/STOP directly and
    // every change re-fetches real samples (POIN 0 when it fits, so the
    // window is never decimated).
    if (!m_active || !m_client || m_running || m_baseInt <= 0.0)
        return;
    const int start =
        1 + qRound((center - span / 2.0 - m_recT0) / m_baseInt);
    const int count = qRound(span / m_baseInt);
    m_winStart = qBound(1, start, m_recN);
    m_winCount = qBound(1, count, m_recN - m_winStart + 1);
    m_manualWin = true;
    sendWindow(QString(), true, false);
}

void FramePump::rateTick()
{
    const qint64 ms = m_rate.elapsed();
    if (ms >= 2000) {
        int pts = 0;
        for (const auto &c : m_cycle.ch)
            if (c.valid)
                pts = qMax(pts, c.count);
        QString align;
        if (m_align && m_alignHit + m_alignMiss > 0)
            align = QStringLiteral(", trig align %1/%2")
                        .arg(m_alignHit)
                        .arg(m_alignHit + m_alignMiss);
        emit info(QStringLiteral(". pump %1 f/s (%2 pts/ch%3)")
                      .arg(m_frames * 1000.0 / ms, 0, 'f', 1)
                      .arg(pts)
                      .arg(align));
        m_frames = 0;
        m_alignHit = m_alignMiss = 0;
        m_rate.restart();
    }
}

void FramePump::onStatus(const QString &answer, bool barrier)
{
    // TRIG:STAT? answers TD, WAIT, RUN, AUTO or STOP (src/scpi/scope.cpp;
    // STOP = the meter holds a frame). Rolling reads RUN (probe_2d): the
    // chip shows ROLL instead.
    emit status(m_rolling && answer.startsWith(QLatin1String("RUN")) ? QStringLiteral("ROLL")
                                                                     : answer);
    const bool run = !answer.startsWith(QLatin1String("STOP"));
    bool resumed = false;
    if (run != m_running) {
        m_running = run;
        emit info(run ? QStringLiteral(". acquisition running (%1)").arg(answer)
                      : QStringLiteral(". acquisition stopped"));
        if (run) {
            m_deepDue = false;
            if (m_idle || m_manualWin) {
                resumeFromIdle();
                resumed = true;
            } else if (m_viewShift != 0.0) {
                // A running pan was held through a short stop. RUN
                // starts on the device screen, as the view does.
                m_viewShift = 0.0;
                ++m_gen;
                dropRecord();
                computeScreenWindow();
                sendWindow(QString(), false, false); // the SWin barrier re-kicks cycling
                resumed = true;
            }
        } else {
            m_deepDue = true; // next frameDone() fetches the whole record
        }
    }
    if (barrier && !resumed && !m_idle) {
        // RUN, STOP and SING release the meter's frame lease, so a cycle in
        // flight reads a dropped lease. Drop its answers and start over.
        ++m_gen;
        restartAfterBarrier();
    }
}

void FramePump::resumeFromIdle()
{
    m_idle = false;
    m_idlePoll.stop();
    m_manualWin = false;
    m_viewShift = 0.0; // RUN starts on the device screen
    computeScreenWindow();
    sendWindow(QString(), false, false); // the SWin barrier re-kicks cycling
    m_refresh.start(1500);
}

void FramePump::startRecordFetch(bool deep)
{
    m_recBusy = true;
    m_recDeep = deep;
    m_rec = WaveFrame{};
    m_recPending = 0;
    m_sour = 0; // the chain moves WAV:SOUR: the next cycle sets it again
    const QString key = QStringLiteral("REC");
    if (!deep && m_recN > 0 && m_baseInt > 0.0 && m_caps.env) {
        m_client->submit(QStringLiteral("WAV:ENV? 400"), cookie(KEnv, 0), true, true,
                         true, key, true);
        m_watchdog.start(4000);
        return;
    }
    if (!deep && m_recN > 0 && m_baseInt > 0.0) {
        // Running overview from frameDone(): read the whole record from the
        // frame ALREADY leased by this cycle (no WAV:SNAP, no repeated
        // WAV:PRE?). Sending WAV:SNAP or a second WAV:PRE? in Normal trigger
        // mode releases the lease and blocks 1 s -> NAN (-230) when no new
        // trigger arrives, dropping the overview refresh.
        m_rec.seq = m_cycle.seq;
        for (int i = 0; i < 2; ++i) {
            if (!m_chOn[i] || !m_cycle.ch[i].valid)
                continue;
            ChannelFrame &rf = m_rec.ch[i];
            const ChannelFrame &cf = m_cycle.ch[i];
            rf.count = m_recN;
            rf.xinc = m_baseInt;
            rf.xorig = m_recT0;
            rf.yinc = cf.yinc;
            rf.yorg = cf.yorg;
            rf.yref = cf.yref;
            rf.stride = 1;
            rf.vdiv = cf.vdiv;
            rf.offs = cf.offs;
            const int c = i + 1;
            m_client->submit(QStringLiteral("WAV:SOUR CHAN%1").arg(c), 0, false, false,
                             true, key, true);
            m_client->submit(QStringLiteral("WAV:STAR 1"), 0, false, false, true, key,
                             true);
            m_client->submit(QStringLiteral("WAV:STOP %1").arg(m_recN), 0, false, false,
                             true, key, true);
            m_client->submit(QStringLiteral("WAV:POIN 0"), 0, false, false, true, key,
                             true);
            m_client->submit(QStringLiteral("WAV:DATA?"), cookie(KData, c), true, true,
                             true, key, true);
            ++m_recPending;
        }
        if (m_recPending == 0) {
            m_recBusy = false;
            kickCycle();
            return;
        }
        m_watchdog.start(4000);
        return;
    }
    // Stopped deep fetch: STOP or SSet released the lease, and the stopped
    // meter holds the frame permanently, so WAV:SNAP + SEQ? + PRE? return
    // immediately.
    m_client->submit(QStringLiteral("WAV:SNAP"), 0, false, false, true, key, true);
    m_client->submit(QStringLiteral("WAV:SEQ?"), cookie(KSeq, 0), true, false, true, key,
                     true);
    // A chain that never answers must not stall the pump for good.
    m_watchdog.start(4000);
}

bool FramePump::dropRecord()
{
    if (!m_recBusy)
        return false;
    m_client->dropQueued(QStringLiteral("REC"));
    m_recBusy = false;
    if (!m_recDeep)
        m_ovDue = true; // the overview was cut short: fetch it again later
    return true;
}

void FramePump::restartAfterBarrier()
{
    // The caller bumped m_gen. A record chain may have left STAR 1 / STOP
    // recN on the meter: put the window back, its SWin barrier re-kicks.
    if (dropRecord()) {
        sendWindow(QString(), false, false); // m_winStart/m_winCount still hold the last window
    } else {
        kickCycle();
    }
}

void FramePump::finishRecord()
{
    m_recBusy = false;
    int first = -1;
    for (int i = 0; i < 2; ++i)
        if (m_rec.ch[i].valid && first < 0)
            first = i;
    if (first >= 0) {
        const ChannelFrame &cf = m_rec.ch[first];
        m_rec.stampMs = QDateTime::currentMSecsSinceEpoch();
        m_rec.running = !(m_recDeep && !m_running);
        m_rec.scal = m_scal;
        m_rec.pos = m_pos;
        m_rec.recT0 = cf.xorig;
        m_rec.recT1 = cf.xorig + double(cf.count - 1) * cf.xinc;
        m_rec.baseInt = cf.xinc;
        m_rec.recN = cf.count;
        m_rec.full = true;
        m_rec.settled = true;
        if (!m_rec.running) {
            // Stopped: the held capture keeps the V/div and offset it was
            // taken with in its PRE, but a V/div or position change on the
            // stopped meter must rescale the held trace, as on the meter.
            // DATA is absolute volts, so drawing on the model's values is
            // exact.
            const int need = (1 << DeviceState::Scal) | (1 << DeviceState::Offs);
            for (int i = 0; i < 2; ++i) {
                const ChannelState &cs = m_state.ch[i];
                if (m_rec.ch[i].valid && (cs.known & need) == need && cs.scal > 0.0) {
                    m_rec.ch[i].vdiv = cs.scal;
                    m_rec.ch[i].offs = cs.offs;
                }
            }
        }
        if (m_rec.running)
            emit info(QStringLiteral(". overview record %1 pts").arg(cf.count));
        emit record(m_rec);
    }
    if (m_recDeep && !m_running) {
        m_deepDue = false;
        if (first < 0) {
            emit info(QStringLiteral("! stopped: whole-record fetch returned no data"));
            kickCycle(); // keep the stopped path (window re-fetches) going
            return;
        }
        m_idle = true;
        m_watchdog.stop();
        m_pace.stop();
        m_retry.stop();
        m_idlePoll.start();
        emit info(QStringLiteral(". stopped: whole record held (%1 pts/ch), pump idle")
                      .arg(m_rec.ch[first].count));
        return;
    }
    sendWindow(QString(), false, false); // restore the screen window; its SWin barrier re-kicks cycling
}

void FramePump::runStop(bool run)
{
    if (!m_active || !m_client)
        return;
    // Unquiet: echoed in the console with its SYST:ERR? check. The status
    // query after it is the barrier that restarts the cycle (onStatus).
    m_client->submit(run ? QStringLiteral("RUN") : QStringLiteral("STOP"), 0, false,
                     false, false, QString(), true);
    m_client->submit(QStringLiteral("TRIG:STAT?"), cookie(SRun, 0), true, false, true,
                     QString(), true);
}

void FramePump::arm()
{
    if (!m_active || !m_client)
        return;
    m_client->submit(QStringLiteral("SING"), 0, false, false, false, QString(), true);
    m_client->submit(QStringLiteral("TRIG:STAT?"), cookie(SRun, 0), true, false, true,
                     QString(), true);
}

void FramePump::center()
{
    // Running only (not roll): stopped, the view centers locally inside the held record.
    if (!m_active || !m_client || !m_running || m_idle || m_rolling)
        return;
    m_offs = 0.0;
    m_viewShift = 0.0;
    syncFromOffset();
    computeScreenWindow();
    sendWindow(QStringLiteral("TIM:OFFS 0"), true, false);
    m_ovPos.start();
}

namespace {
// Poll index i: channel i / FieldCount + 1, field i % FieldCount.
const char *const kFieldCmd[DeviceState::FieldCount] = {"DISP", "SCAL", "OFFS", "COUP",
                                                        "PROB"};
} // namespace

namespace {
// Poll indices 10-17: trigger, then telemetry (DeviceState layout).
const char *const kTrigCmd[DeviceState::TrigFieldCount] = {
    "TRIG:EDGE:SOUR", "TRIG:EDGE:SLOP", "TRIG:EDGE:LEV", "TRIG:SWE", "TRIG:EDGE:NREJ"};
const char *const kTeleCmd[DeviceState::TeleFieldCount] = {
    "ACQ:SRAT", "ACQ:MDEP", "SYST:PERF", "ACQ:ROLL", "MEAS:FCNT", "MEAS:FCNT:QUAL"};
// Gap between poll queries. 18 queries at 60 ms: a knob turn on the meter
// shows within about 1.1 s,.
constexpr int kPollGapMs = 60;
} // namespace

void FramePump::submitPoll(int idx, Step step, bool ahead, const QString &key)
{
    QString cmd;
    if (idx < DeviceState::kTrigBase) {
        const int c = idx / DeviceState::FieldCount + 1;
        const int f = idx % DeviceState::FieldCount;
        cmd = QStringLiteral("CHAN%1:%2").arg(c).arg(QLatin1String(kFieldCmd[f]));
    } else if (idx < DeviceState::kTeleBase) {
        cmd = QLatin1String(kTrigCmd[idx - DeviceState::kTrigBase]);
    } else if (idx < DeviceState::kPollCount) {
        cmd = QLatin1String(kTeleCmd[idx - DeviceState::kTeleBase]);
    } else {
        return;
    }
    m_client->submit(cmd + QLatin1Char('?'), cookie(step, idx), true, false, true, key, ahead);
}

bool FramePump::applyPoll(int idx, const QString &answer)
{
    if (idx < 0 || idx >= DeviceState::kPollCount)
        return false;
    if (answer.startsWith(QLatin1String("NAN"))) {
        // Clear counter/quality known bits on NAN so MeasBar reverts to "--".
        const int f = idx - DeviceState::kTeleBase;
        if (f == DeviceState::YFcnt || f == DeviceState::YFqual) {
            const int bit = 1 << f;
            if (m_state.tele.known & bit) {
                m_state.tele.known &= ~bit;
                return true;
            }
        }
        return false;
    }
    if (idx >= DeviceState::kTrigBase)
        return applyTrigPoll(idx, answer);
    const int c = idx / DeviceState::FieldCount;
    const int f = idx % DeviceState::FieldCount;
    ChannelState &cs = m_state.ch[c];
    const bool first = !(cs.known & (1 << f));
    bool changed = false;
    auto setD = [&](double &dst) {
        bool ok = false;
        const double v = answer.toDouble(&ok);
        if (!ok)
            return false;
        if (first || std::abs(v - dst) > 1e-9 * qMax(1.0, std::abs(v)))
            changed = true;
        dst = v;
        return true;
    };
    bool ok = true;
    switch (f) {
    case DeviceState::Disp: {
        const bool v = answer.startsWith(QLatin1Char('1')) || answer == QLatin1String("ON");
        changed = first || v != cs.disp;
        cs.disp = v;
        if (m_chOn[c] != v) {
            // A channel turned on/off: the record length changes (two
            // channels halve it), so re-read it and refresh the strip.
            m_chOn[c] = v;
            m_recPoll = true;
            if (m_running)
                m_ovDue = true;
            emit info(QStringLiteral(". CH%1 %2").arg(c + 1).arg(v ? "on" : "off"));
        }
        break;
    }
    case DeviceState::Scal:
        ok = setD(cs.scal);
        break;
    case DeviceState::Offs:
        ok = setD(cs.offs);
        break;
    case DeviceState::Coup: {
        const bool v = answer.startsWith(QLatin1String("AC"));
        changed = first || v != cs.ac;
        cs.ac = v;
        break;
    }
    case DeviceState::Prob:
        ok = setD(cs.prob);
        break;
    }
    if (ok)
        cs.known |= 1 << f;
    return ok && changed;
}

bool FramePump::applyTrigPoll(int idx, const QString &answer)
{
    bool changed = false;
    auto setD = [&](double &dst, double v, bool first) {
        if (first || std::abs(v - dst) > 1e-9 * qMax(1.0, std::abs(v)))
            changed = true;
        dst = v;
    };
    if (idx < DeviceState::kTeleBase) {
        const int f = idx - DeviceState::kTrigBase;
        TriggerState &t = m_state.trig;
        const bool first = !(t.known & (1 << f));
        switch (f) {
        case DeviceState::TSour: {
            int s = 0;
            if (answer.startsWith(QLatin1String("CHAN")))
                s = answer.mid(4).toInt();
            if (s != 1 && s != 2)
                return false;
            changed = first || s != t.source;
            t.source = s;
            break;
        }
        case DeviceState::TSlop: {
            bool r;
            if (answer.startsWith(QLatin1String("POS")))
                r = true;
            else if (answer.startsWith(QLatin1String("NEG")))
                r = false;
            else
                return false;
            changed = first || r != t.rising;
            t.rising = r;
            break;
        }
        case DeviceState::TLev: {
            bool ok = false;
            const double v = answer.toDouble(&ok);
            if (!ok)
                return false;
            setD(t.level, v, first);
            break;
        }
        case DeviceState::TSwe: {
            int s;
            if (answer.startsWith(QLatin1String("AUTO")))
                s = 0;
            else if (answer.startsWith(QLatin1String("NORM")))
                s = 1;
            else if (answer.startsWith(QLatin1String("SING")))
                s = 2;
            else
                return false;
            changed = first || s != t.sweep;
            t.sweep = s;
            break;
        }
        case DeviceState::TNrej: {
            const bool v = answer.startsWith(QLatin1Char('1'));
            if (!v && !answer.startsWith(QLatin1Char('0')))
                return false;
            changed = first || v != t.nrej;
            t.nrej = v;
            break;
        }
        default:
            return false;
        }
        t.known |= 1 << f;
        return changed;
    }
    const int f = idx - DeviceState::kTeleBase;
    Telemetry &y = m_state.tele;
    const bool first = !(y.known & (1 << f));
    switch (f) {
    case DeviceState::YSrat: {
        bool ok = false;
        const double v = answer.toDouble(&ok);
        if (!ok)
            return false;
        setD(y.srat, v, first);
        if (!first && changed)
            m_recPoll = true;
        break;
    }
    case DeviceState::YMdep: {
        bool ok = false;
        const int v = int(answer.toDouble(&ok));
        if (!ok)
            return false;
        changed = first || v != y.mdep;
        y.mdep = v;
        if (!first && changed)
            m_recPoll = true;
        break;
    }
    case DeviceState::YPerf: {
        // uptimeMs,coreHz,captures,projections,flushes,elapsedCycles,uiCycles
        const QStringList p = answer.split(QLatin1Char(','));
        bool ok = false;
        const double hz = p.size() >= 2 ? p.at(1).toDouble(&ok) : 0.0;
        if (!ok)
            return false;
        setD(y.coreHz, hz, first);
        if (std::abs(hz - 200e6) > 0.5 && !m_perfWarned) {
            m_perfWarned = true;
            emit info(QStringLiteral("! core clock %1 MHz, not 200: USB data needs the "
                                     "200/100 profile")
                          .arg(hz / 1e6, 0, 'f', 1));
        }
        break;
    }
    case DeviceState::YRoll: {
        const bool v = answer.startsWith(QLatin1Char('1')) || answer.startsWith(QLatin1String("ON"));
        if (!v && !answer.startsWith(QLatin1Char('0')) && !answer.startsWith(QLatin1String("OFF")))
            return false;
        changed = first || v != y.roll;
        y.roll = v;
        break;
    }
    case DeviceState::YFcnt: {
        bool ok = false;
        const double v = answer.toDouble(&ok);
        if (!ok || !std::isfinite(v))
            return false;
        setD(y.fcnt, v, first);
        break;
    }
    case DeviceState::YFqual: {
        static const char *const kQual[5] = {"CLEAN", "FILTERED", "UNVERIFIED", "UNCERTAIN",
                                             "CHANGING"};
        int q = -1;
        for (int i = 0; i < 5; ++i)
            if (answer.startsWith(QLatin1String(kQual[i])))
                q = i;
        if (q < 0)
            return false;
        changed = first || q != y.fqual;
        y.fqual = q;
        break;
    }
    default:
        return false;
    }
    y.known |= 1 << f;
    return changed;
}

void FramePump::pollTick()
{
    if (!m_active || !m_client || m_pollClock.elapsed() < kPollGapMs)
        return;
    m_pollClock.restart();
    submitPoll(m_pollIdx, SPoll, false);
    m_pollIdx = (m_pollIdx + 1) % DeviceState::kPollCount;
}

void FramePump::setTriggerValue(int field, double value)
{
    if (!m_active || !m_client || field < 0 || field >= DeviceState::TrigFieldCount)
        return;
    QString arg;
    switch (field) {
    case DeviceState::TSour: {
        const int ch = value >= 1.5 ? 2 : 1;
        // The meter refuses (-221) a source that is not displayed.
        if (!m_chOn[ch - 1]) {
            emit info(QStringLiteral(". TRIG:EDGE:SOUR CHAN%1 not sent: CH%1 is off").arg(ch));
            return;
        }
        arg = QStringLiteral("CHAN%1").arg(ch);
        break;
    }
    case DeviceState::TSlop:
        arg = value != 0.0 ? QStringLiteral("POS") : QStringLiteral("NEG");
        break;
    case DeviceState::TLev: {
        // Keep the level on screen (+-4 div of the trigger channel, its
        // CHAN:SCAL and CHAN:OFFS from the model). The meter's own limit is
        // ADC code -128..127 (-222 beyond), a little wider than the screen.
        const int c = qBound(1, m_state.trig.source, 2) - 1;
        const ChannelState &cs = m_state.ch[c];
        const int need = (1 << DeviceState::Scal) | (1 << DeviceState::Offs);
        if ((cs.known & need) == need && cs.scal > 0.0) {
            const double lo = -4.0 * cs.scal - cs.offs, hi = 4.0 * cs.scal - cs.offs;
            if (value < lo || value > hi) {
                emit info(QStringLiteral(". TRIG:EDGE:LEV %1 clamped to the screen")
                              .arg(value, 0, 'g', 6));
                value = qBound(lo, value, hi);
            }
        }
        arg = QString::number(value, 'g', 6);
        break;
    }
    case DeviceState::TSwe: {
        static const char *const kSwe[3] = {"AUTO", "NORM", "SING"};
        arg = QLatin1String(kSwe[qBound(0, int(value), 2)]);
        break;
    }
    case DeviceState::TNrej:
        arg = value != 0.0 ? QStringLiteral("1") : QStringLiteral("0");
        break;
    }
    const QString key = QStringLiteral("TRIG:%1").arg(field);
    m_client->dropQueued(key);
    m_client->submit(QStringLiteral("%1 %2").arg(QLatin1String(kTrigCmd[field]), arg), 0,
                     false, false, false, key, true);
    submitPoll(DeviceState::kTrigBase + field, SSet, true, key);
    // A new source has its own level in volts: read it right away.
    if (field == DeviceState::TSour)
        submitPoll(DeviceState::kTrigBase + DeviceState::TLev, SPoll, true, key);
    if (m_running && !m_idle)
        m_ovPos.start();
}

void FramePump::setRoll(bool on)
{
    if (!m_active || !m_client)
        return;
    // kickCycle re-evaluates rolling() from the read-back, so the pump
    // switches between the roll and the normal cycle on its own.
    const QString key = QStringLiteral("ROLL");
    m_client->dropQueued(key);
    m_client->submit(QStringLiteral("ACQ:ROLL %1").arg(on ? 1 : 0), 0, false, false, false, key,
                     true);
    submitPoll(DeviceState::kTeleBase + DeviceState::YRoll, SSet, true, key);
}

void FramePump::setTrigAlign(bool on)
{
    m_align = on;
    emit info(QStringLiteral(". trigger alignment %1").arg(on ? "on" : "off"));
}

void FramePump::setTrigAlignSinc(bool on)
{
    m_alignSinc = on;
}

void FramePump::alignTrigger()
{
    // Sub-sample trigger alignment: the meter puts the trigger
    // on a whole sample, so a frame lands up to one sample left or right of
    // the last one. Find the level crossing with the trigger slope next to
    // t = 0, interpolate it, and shift both channels so it sits at t = 0.
    // In sinc mode the crossing is taken on the drawn curve, as the meter's
    // own screen does: a straight-line crossing differs from it by up to a
    // third of a sample on fast edges, and that moved the trace every frame.
    // Raw samples only (stride 1): a decimated window has no single sample
    // at the trigger. Shifts over 1.5 samples are left alone (no crossing
    // there, so the trigger came from somewhere else, such as Auto).
    const TriggerState &t = m_state.trig;
    const int need = (1 << DeviceState::TSour) | (1 << DeviceState::TSlop)
                     | (1 << DeviceState::TLev);
    if ((t.known & need) != need || !std::isfinite(t.level))
        return;
    const ChannelFrame &cf = m_cycle.ch[qBound(1, t.source, 2) - 1];
    const int n = cf.v.size();
    if (!cf.valid || cf.stride != 1 || cf.xinc <= 0.0 || n < 4)
        return;
    const double i0 = (0.0 - cf.xorig) / cf.xinc; // t = 0 in samples
    const int base = int(std::floor(i0));
    const float L = float(t.level);
    double best = 1e9, cross = 0.0;
    int bestI = -1;
    // Sinc only where the view draws it (not a PEAK envelope). The curve can
    // ring across the level more than once between two samples: scan the pair
    // in sub-steps and refine the crossing nearest t = 0.
    const bool onCurve = m_alignSinc && !cf.isPeak();
    const auto starting = [&](double u) {
        const double y = lanczosCurve(cf.v, u);
        return t.rising ? y <= L : y >= L;
    };
    for (int i = base - 2; i <= base + 2; ++i) {
        if (i < 0 || i + 1 >= n)
            continue;
        const float a = cf.v.at(i), b = cf.v.at(i + 1);
        const bool hit = t.rising ? (a < L && b >= L) : (a > L && b <= L);
        if (!hit || a == b)
            continue;
        double at = double(i) + double(L - a) / double(b - a);
        if (onCurve) {
            constexpr int kScan = 8;
            double found = 1e9;
            for (int k = 0; k < kScan; ++k) {
                double lo = i + double(k) / kScan, hi = i + double(k + 1) / kScan;
                if (!starting(lo) || starting(hi))
                    continue;
                for (int step = 0; step < 17; ++step) {
                    const double mid = 0.5 * (lo + hi);
                    (starting(mid) ? lo : hi) = mid;
                }
                const double c = 0.5 * (lo + hi);
                if (std::abs(c - i0) < std::abs(found - i0))
                    found = c;
            }
            if (found < 1e8)
                at = found;
        }
        if (std::abs(at - i0) < std::abs(best - i0)) {
            best = at;
            bestI = i;
        }
    }
    if (bestI >= 0)
        cross = cf.xorig + best * cf.xinc;
    if (best > 1e8 || std::abs(cross) > 1.5 * cf.xinc) {
        ++m_alignMiss;
        return;
    }
    ++m_alignHit;
    for (ChannelFrame &c : m_cycle.ch) {
        if (!c.valid)
            continue;
        c.xorig -= cross;
        c.tshift = -cross;
    }
}

void FramePump::setChannelValue(int ch, int field, double value)
{
    if (!m_active || !m_client || ch < 1 || ch > 2 || field < 0
        || field >= DeviceState::FieldCount)
        return;
    QString arg;
    switch (field) {
    case DeviceState::Disp:
        arg = value != 0.0 ? QStringLiteral("1") : QStringLiteral("0");
        break;
    case DeviceState::Coup:
        arg = value != 0.0 ? QStringLiteral("AC") : QStringLiteral("DC");
        break;
    case DeviceState::Offs: {
        // The meter refuses (-222) an offset beyond +-4 div of its own
        // V/div (channel.cpp: position 0..160). The view and dock clamp on
        // their idea of V/div, which can lag a knob turn or a channel
        // switch. Clamp again on the model's
        // CHAN:SCAL, just inside the limit so 'g' 6 rounding stays legal.
        const ChannelState &cs = m_state.ch[ch - 1];
        if ((cs.known & (1 << DeviceState::Scal)) && cs.scal > 0.0) {
            const double lim = 4.0 * cs.scal * (1.0 - 1e-5);
            if (std::abs(value) > lim) {
                emit info(QStringLiteral(". CHAN%1:OFFS %2 clamped to +-4 div (%3)")
                              .arg(ch)
                              .arg(value, 0, 'g', 6)
                              .arg(lim, 0, 'g', 6));
                value = qBound(-lim, value, lim);
            }
        }
        arg = QString::number(value, 'g', 6);
        break;
    }
    default:
        arg = QString::number(value, 'g', 6);
        break;
    }
    // One key per setting: a burst (arrow repeat, tag drag) collapses to
    // the newest set + read-back still waiting in the UI lane.
    const QString key = QStringLiteral("CH%1:%2").arg(ch).arg(field);
    m_client->dropQueued(key);
    m_client->submit(QStringLiteral("CHAN%1:%2 %3")
                         .arg(ch)
                         .arg(QLatin1String(kFieldCmd[field]), arg),
                     0, false, false, false, key, true);
    if (field == DeviceState::Scal || field == DeviceState::Prob) {
        // The meter keeps position in divisions and the trigger level as an
        // ADC code, so CHAN:OFFS and TRIG:EDGE:LEV volts follow a V/div or
        // probe change. Re-read both ahead of the barrier, so a stopped
        // redraw uses the new values.
        submitPoll((ch - 1) * DeviceState::FieldCount + DeviceState::Offs, SPoll, true, key);
        submitPoll(DeviceState::kTrigBase + DeviceState::TLev, SPoll, true, key);
    }
    submitPoll((ch - 1) * DeviceState::FieldCount + field, SSet, true, key);
    // Running: refresh the strip once the change ends.
    // Stopped: the SSet read-back refetches the held record already.
    if (m_running && !m_idle)
        m_ovPos.start();
}

void FramePump::setTimeOffset(double s)
{
    if (!m_active || !m_client || !m_running || m_idle)
        return;
    m_offs = s;
    syncFromOffset();
    computeScreenWindow();
    sendWindow(QStringLiteral("TIM:OFFS %1").arg(m_offs, 0, 'e', 6), true, false);
    m_ovPos.start();
}

void FramePump::onWatchdog()
{
    if (!m_active || m_idle)
        return;
    emit info(QStringLiteral("! pump stall - resync"));
    ++m_gen; // drop any in-flight response
    restartAfterBarrier(); // a stuck record chain is dropped, the window re-sent
}
