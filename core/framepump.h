#pragma once

#include <QElapsedTimer>
#include <QObject>
#include <QTimer>
#include <QVector>

#include "devicestate.h"
#include "measure.h"

class ScpiClient;

// One acquired channel: volts per point plus the WAV:PRE? scaling.
// Time of point i is xorig + i*xinc (xinc already carries the WAV:POIN
// stride; xorig is trigger-relative, verified on hardware 2026-11-19).
struct ChannelFrame {
    bool valid = false;
    QVector<float> v;      // volts (or bucket min when isPeak())
    QVector<float> vHi;    // WAV:POIN:MODE PEAK: bucket max volts (empty otherwise)
    double xinc = 0.0;     // s between points
    double xorig = 0.0;    // t of point 0
    double yinc = 0.0;     // V per LSB (V/div = 32*yinc: 256 codes = 8 divs)
    double yorg = 0.0;
    double yref = 0.0;
    int count = 0;
    int stride = 1;        // xinc / raw sample interval (1 = raw samples)
    bool isPeak() const { return !vHi.isEmpty() && vHi.size() == v.size(); }
    // Vertical state of this frame, from its own PRE (measured on the meter): V/div =
    // yinc * 80/3 codes per division, offset = yorg * yinc (== CHAN:OFFS).
    // Screen divisions of a sample = (v + offs) / vdiv.
    double vdiv = 0.0;
    double offs = 0.0;
    // Sub-sample trigger alignment: xorig already includes this shift
    // (s). xorig - tshift is the meter's own time base, which the view keys
    // its phosphor on, so a per-frame shift does not clear the ghosts.
    double tshift = 0.0;
};

// Codes per vertical division (measured on the meter, 0.2-5 V/div at 10x).
constexpr double kCodesPerDiv = 80.0 / 3.0;

// One SNAP lease: both channels of the same trigger frame, plus the
// horizontal context the view needs: the device screen window
// (scal/pos), whether the scope is running, and the raw record's time
// bounds (for stopped-mode zoom/pan clamping).
struct WaveFrame {
    qint64 stampMs = 0;
    int seq = -1;       // WAV:SEQ? of the lease
    ChannelFrame ch[2]; // [0] = CHAN1, [1] = CHAN2
    bool running = true;  // TRIG:STAT? != STOP
    double scal = 0.0;    // s/div
    double pos = 50.0;    // trigger position, % (== TIM:OFFS in disguise)
    double recT0 = 0.0;   // time of raw record sample 1 (trigger-relative)
    double recT1 = 0.0;   // time of the last raw sample
    bool settled = true;  // false: fetched on a guessed window after a
                          // wheel step; the view keeps its last good frame
    double baseInt = 0.0; // s between raw record samples
    int recN = 0;         // raw record length in samples
    bool full = false;    // true: the whole raw record (overview / deep fetch)
    bool roll = false;    // Roll frame, newest sample on the right edge
    qint64 snapMs = 0;    // PC time the cycle's WAV:SNAP went out (roll clock)
};
Q_DECLARE_METATYPE(WaveFrame)

// UI-free acquisition loop. Per cycle: WAV:SNAP, WAV:SEQ? (NAN = DMA race,
// skip), then for each displayed channel a SOUR switch when needed,
// WAV:PRE? (read every frame, so knob turns on the device are picked up
// live) and a raw WAV:DATA? block read; then frame() is emitted and the
// next cycle chains immediately. Exactly one cycle is in flight - that is
// the back-pressure: nothing is ever queued ahead of the renderer.
//
// The fetch window is the SCREEN window (there is no screen mode in the
// firmware - WAV:MODE NORM is -224; batch 3's suggestion shipped as the
// WAV:POIN stride instead): STAR/STOP are computed from TIM:SCAL?,
// TRIG:POS? and the base interval (PRE with POIN 0), and WAV:POIN
// decimates that window to at most `points` samples. SCAL/POS are
// re-queried periodically; the queries ride the task queue between
// cycles, and a change re-sends STAR/STOP before the next cycle. Fetching
// the whole 64K record instead decimates
// a 1/6500th-of-record screen window into alias mush.
//
// Lives in the same worker thread as the ScpiClient and drives it through
// direct submit() calls (quiet: no per-exchange echo, no SYST:ERR?
// follow-ups - several exchanges run per frame). Setup commands go through
// unquiet submits so one-time context errors are visible in the console.
class FramePump : public QObject {
    Q_OBJECT
public:
    explicit FramePump(QObject *parent = nullptr);

    void setClient(ScpiClient *client); // same thread as the client only
    void setPoints(int points);         // WAV:POIN n (max pts/channel); 0 = full window

signals:
    void frame(const WaveFrame &f);
    // Whole raw record, both displayed channels. While running about
    // once a second for the overview strip. When stopped it is the deep
    // fetch (running == false): the view then zooms and pans in it locally.
    void record(const WaveFrame &rec);
    void status(const QString &trigStat); // raw TRIG:STAT? answer (TD/WAIT/RUN/AUTO/STOP)
    void info(const QString &line); // "." / "!" lines for the console
    void stateChanged(const DeviceState &s); // Any polled setting changed
    // Answer to request(tag, ...). text for a line reply, raw for a block.
    void reply(int tag, const QString &text, const QByteArray &raw);
    void capsKnown(const DeviceCaps &caps);  // Capability probe done
    void measReady(int ch, const meas::Result &res); // WAV:MEAS? firmware readouts

public slots:
    void start();
    void stop();
    // Incoming client signals (same-thread wiring; cookie-routed).
    void onText(qint64 cookie, const QString &cmd, const QString &answer);
    void onRaw(qint64 cookie, const QString &cmd, const QByteArray &data);

    // View-is-the-device inputs, queued from the UI thread. All send
    // through the UI lane under one coalesce key, so a spin or drag burst
    // collapses to the newest value and lands as: set(s) + window + barrier.
    void stepTimeScale(double s);          // TIM:SCAL (absolute s/div)
    void setViewWindow(double center, double span); // stopped-mode STAR/STOP

    // Scope chrome.
    void runStop(bool run); // RUN / STOP
    void arm();             // SING: one capture, then the meter holds
    void center();          // TIM:OFFS 0 (trigger point to screen center)
    // Running: look at another part of the record without moving the
    // trigger. s = screen center shift from the device screen, seconds.
    void setViewShift(double s);

    // Vertical + horizontal controls. field = DeviceState::Field,
    // ch 1/2. Coup: value != 0 means AC. Each set is followed by a quiet
    // read-back of the same setting: it confirms the model and is the
    // barrier that restarts the cycle (a set releases the frame lease).
    void setChannelValue(int ch, int field, double value);
    void setTimeOffset(double s); // absolute TIM:OFFS (Position box, zero)

    // Trigger controls. field = DeviceState::TrigField. TSour: 1/2,
    // TSlop: != 0 rising, TLev: V, TSwe: 0 AUTO/1 NORM/2 SING, TNrej: != 0
    // on. Same set + quiet read-back barrier as setChannelValue.
    void setTriggerValue(int field, double value);
    // PC-side sub-sample trigger alignment, default on.
    void setTrigAlign(bool on);
    // ACQ:ROLL: the meter rolls at >= 200 ms/div in Auto only
    // while this is on. Same set + quiet read-back barrier.
    void setRoll(bool on);
    // generic UI lane traffic for panels (screenshot, remote, DMM,
    // generator). request() answers through reply(tag, ...), tag 0-255.
    // sendCommand() is a set with the usual SYST:ERR? check (console).
    void request(int tag, const QString &cmd, bool raw);
    void sendCommand(const QString &cmd);

private slots:
    void kickCycle();            // begin the next SNAP cycle (also the retry path)
    void onWatchdog();           // no progress for 4 s -> resync the cycle
    void refreshHoriz();         // periodic TIM:SCAL?/TRIG:POS?/TRIG:STAT? re-query

private:
    // Response cookies: gen<<16 | step<<8 | (channel, 0 for cycle-wide).
    // gen invalidates stale responses after a restart or watchdog resync.
    enum Step {
        SDisp = 1, SPre, SSeq, CPre, CData, CFram,
        SScal, SPos, SBase, SWin, RScal, RPos, RStat, SRec, RRec,
        SRun,               // TRIG:STAT? barrier after RUN/STOP/SING
        KSeq, KPre, KData, KEnv, // whole-record fetch (overview / deep)
        CMeas,              // WAV:MEAS? firmware readouts
        SPoll,              // state poll answer (ch = poll index)
        SSet,               // read-back after a UI set (ch = poll index)
        SCap,               // capability probe answer (ch = probe index)
        SRollN,             // Record count (POIN 0 PRE?) of a roll cycle
        SRollScal,          // TIM:SCAL? barrier after a timebase step while rolling
        SUser               // Request() answer (ch = tag), any gen
    };
    // The meter rolls when ACQ:ROLL is on, TIM:SCAL >= 200 ms/div and
    // the sweep is AUTO (firmware request.hpp rolling()).
    bool rolling() const;
    void readRoll(const QString &pre); // roll cycle: newest screen of the record
    void submitPoll(int idx, Step step, bool ahead, const QString &key = QString());
    bool applyPoll(int idx, const QString &answer); // true: the model changed
    bool applyTrigPoll(int idx, const QString &answer); // indices 10-17
    void pollTick(); // one round-robin query, rate-limited (frame gaps / idle)
    qint64 cookie(Step s, int ch) const { return (qint64(m_gen) << 16) | (s << 8) | ch; }
    bool current(qint64 c) const { return (c >> 16) == qint64(m_gen); }

    void sendSetup(bool quiet = false); // context + base probe; leads to sendWindow()
    void resyncSetup();     // quiet re-read of DISP + record size (e.g. CH2 toggled, -222)
    void sendWindow(const QString &leadSet = QString(), bool ahead = false,
                    bool checkRec = true);
    void computeScreenWindow(); // STAR/STOP from SCAL/POS + base interval
    void readChannel();     // every displayed channel: SOUR/PRE/DATA at once
    bool parseBlock(const QByteArray &data, int &seq, ChannelFrame (&out)[2],
                    bool &parsedAny, QString *err = nullptr);
    void handleFrameBlock(const QByteArray &data); // 0.18.0 WAV:FRAM? single block
    void handleEnvBlock(const QByteArray &data);   // WAV:ENV? whole-record envelope
    bool applyCyclePre(int ch, const QString &answer); // false: channel unusable
    void channelDone();
    void frameDone();
    void rateTick();
    void syncFromOffset();  // keep m_pos/m_offs consistent + clamped
    void alignTrigger();    // Shift m_cycle by the interpolated crossing
    double recordStart(double preT0, int count, double xinc); // PRE xorig 0 fallback
    void onStatus(const QString &answer, bool barrier); // RStat / SRun answers
    void startRecordFetch(bool deep); // SNAP + SEQ?, then per channel the whole record
    void finishRecord();
    // A record chain still queued (key "REC") is dropped. True when one
    // was busy: the caller must then re-send the screen window, because the
    // chain may already have set STAR 1 / STOP recN on the meter.
    bool dropRecord();
    void restartAfterBarrier(); // gen bumped by the caller: window or new cycle
    void resumeFromIdle();  // stopped -> running: screen window, cycles again
    static double ladderStep(double cur, int dir); // 1-2-5, 10 ns .. 20 s

    ScpiClient *m_client = nullptr;
    QTimer m_retry;     // 30 ms after a skipped frame
    QTimer m_setupRetry; // 2 s: re-read CHAN DISP? and redo the setup (no channel / bad base PRE)
    QTimer m_watchdog;  // 4 s without a response -> resync
    QTimer m_refresh;   // 1.5 s SCAL/POS/STAT re-query while cycling
    QTimer m_pace;      // enforces the minimum cycle interval (rate cap)
    QTimer m_statTimer; // 500 ms: TRIG:STAT? between cycles (status chip)
    QTimer m_idlePoll;  // 300 ms: TRIG:STAT? while stopped and idle
    QTimer m_ovPos;     // 400 ms after the last position change: refetch the record
    QElapsedTimer m_rate;
    QElapsedTimer m_cycleStopwatch;

    bool m_active = false;
    unsigned m_gen = 0;
    int m_points = 4000;
    bool m_chOn[2] = {true, true};
    int m_dispGot = 0;
    int m_setupMask = 0;   // 1=scal 2=pos 4=base -> sendWindow()
    bool m_setupQuiet = false;
    int m_refreshMask = 0; // 1=scal 2=pos -> maybe re-send window
    double m_scal = 0.0;   // s/div
    double m_pos = 50.0;   // trigger position, % of screen == % of record
    double m_offs = 0.0;   // TIM:OFFS, s: (0.5 - pos/100) * 10*scal
    double m_viewShift = 0.0; // running: fetched window shift from the device screen, s
    double m_baseInt = 0.0;// s between raw samples (PRE with POIN 0)
    bool m_xorigFallbackLogged = false;
    int m_recN = 65536;    // raw record length in samples
    bool m_running = true; // TRIG:STAT? says the acquisition is running
    int m_winStart = 1;    // WAV:STAR currently sent
    int m_winCount = 0;    // window length in raw samples
    int m_winPoin = 0;     // WAV:POIN sent with the window (0 = raw)
    bool m_recChanged = false; // SRec saw a new record length/interval
    bool m_recPoll = false;    // refresh tick: re-read the record between cycles
    bool m_recCheckAfterScal = false; // one late record check after TIM:SCAL (bug m)
    int m_nanSkips = 0;        // frames skipped in the current run of seq NAN
    bool m_manualWin = false; // stopped: view drives STAR/STOP directly
    bool m_horizCheck = false; // wheel sent TIM:SCAL: re-read SCAL/POS at once
    // After a wheel step: 1 = waiting for the meter's SCAL/POS answer,
    // 2 = corrected window sent, waiting for its barrier. Frames are
    // emitted unsettled until it is back to 0.
    int m_settle = 0;
    double m_recT0 = 0.0;  // time of raw record sample 1 (trigger-relative)
    int m_sour = 0;        // channel WAV:SOUR currently points at (1/2, 0 unknown)
    int m_seq = -1;
    // Time since the last NEW frame (seq changed). A new frame after a
    // wait of 500 ms or more (Normal trigger, single shot) refreshes the
    // overview.
    QElapsedTimer m_newSeqClock;
    int m_cycleCh = -1;    // 0-based index of the channel being read
    // One cycle goes out at once (SOUR, PRE? when due, DATA? per
    // channel). PRE? answers are cached per channel and reused while the
    // gen holds, the state poll saw no change and fewer than kPreEvery
    // frames have passed.
    static constexpr int kPreEvery = 15;
    QString m_preCache[2];
    unsigned m_preGen[2] = {0, 0};
    int m_preAge[2] = {0, 0};
    bool m_cyclePreOk[2] = {false, false};
    bool m_cyclePeak[2] = {false, false};
    int m_cyclePending = 0; // DATA? answers still due this cycle
    int m_minCycleMs = 33; // rate cap (~30 f/s) - device + render politeness
    // Roll mode: the record grows (then slides) every SNAP. Each cycle
    // reads the count first, then fetches the newest screen width and the
    // frame is placed with its last sample on the right screen edge.
    bool m_rolling = false;
    static constexpr int kRollCycleMs = 50;
    static constexpr double kRollMarginS = 0.4;
    WaveFrame m_cycle;     // frame under assembly
    // Whole-record fetch.
    // Overview fetches are event-driven: at connect,
    // after a timebase step settles, on a late record change, and the deep
    // fetch on stop, and once after a position change ends (the record
    // moves with TRIG:POS, 2026-11-20). No periodic refresh (about 7 f/s).
    bool m_ovDue = false;   // fetch the whole record at the next frameDone
    bool m_ovAfterSettle = false; // timebase stepped: fetch once m_settle is 0
    bool m_statDue = false; // status timer fired: TRIG:STAT? at the next frameDone
    bool m_deepDue = false; // stopped: fetch the whole record, then go idle
    bool m_recBusy = false; // a record fetch chain is in flight
    bool m_recDeep = false; // the chain in flight is the deep (stopped) one
    int m_recPending = 0;   // channels still to arrive in the chain
    bool m_idle = false;    // stopped with the deep record held: no cycles
    WaveFrame m_rec;        // record under assembly
    qint64 m_frames = 0;
    // State model.
    DeviceState m_state;
    DeviceCaps m_caps;
    int m_capGot = 0;
    int m_pollIdx = 0;
    QElapsedTimer m_pollClock; // >= kPollGapMs between poll queries
    QElapsedTimer m_measClock; // >= 250 ms between WAV:MEAS? queries
    bool m_perfWarned = false; // core clock warning logged once per connect
    // Trigger alignment.
    bool m_align = true;
    int m_alignHit = 0, m_alignMiss = 0; // per 2 s, logged with the rate line
};
