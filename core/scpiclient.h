#pragma once

#include <QByteArray>
#include <QObject>
#include <QSerialPort>
#include <QTimer>
#include <deque>

// UI-free SCPI-over-CDC-serial client for the ZT703S/ZT706S.
//
// Lives in a worker thread (moveToThread): slots run there, signals are
// queued to listeners. Serial framing: 115200 8N1, DTR asserted, LF line endings, input flushed
// 200 ms after open.
//
// Task flow: command()/query()/submit() enqueue; the head task is written,
// a per-task timeout arms, and the first complete reply finishes a query.
// Replies are text lines, or - for isRaw tasks (WAV:DATA?) - an
// IEEE-488.2 definite-length block: '#'<d><length digits><bytes><LF>.
// Set commands finish when fully written, then (unless quiet) a SYST:ERR?
// check is enqueued ahead of everything else so it directly follows its
// command; non-zero codes surface via scpiError() and are drained (up to
// 8) before normal traffic resumes.
//
// Two lanes: UI tasks (command()/query(), or submit(..., ahead))
// always run before pump-lane tasks, so a knob turn never waits behind a
// frame cycle. Tasks may carry a coalesce key: enqueueing a task with a
// key drops same-key tasks still waiting in the same lane, so a burst of
// TIM:SCAL/TIM:OFFS sets collapses to the newest value (Architecture,
// command queue).
//
// submit() is the full-control entry point for same-thread drivers (the
// frame pump): it picks reply style and console echo per task. The
// command()/query() slots stay 2-arg for cross-thread signal wiring.
//
// Pipelining: pump-lane tasks are written ahead without waiting for
// earlier replies, up to m_depth replies outstanding. The meter answers in
// order, so replies are matched to m_inflight front to back. A pump-lane
// set command is done once handed to the port. UI-lane tasks never
// pipeline: one goes only when nothing is in flight, and while UI tasks
// wait no new pump task is written. Depth 1 is the old one-at-a-time flow.
class ScpiClient : public QObject {
    Q_OBJECT
public:
    explicit ScpiClient(QObject *parent = nullptr);
    ~ScpiClient() override;

signals:
    void opened(const QString &port);
    void closed();
    void failed(const QString &what); // link-level failure (link is down after)
    void response(qint64 cookie, const QString &cmd, const QString &answer);
    void rawResponse(qint64 cookie, const QString &cmd, const QByteArray &data);
    void traffic(const QString &line); // "-> cmd" / "<- reply" / ". info" / "! problem"
    void scpiError(const QString &err); // SYST:ERR? code != 0

public slots:
    void openPort(const QString &name);
    void closePort();
    void command(const QString &cmd, qint64 cookie = 0); // set-style, no reply
    void query(const QString &cmd, qint64 cookie = 0);   // '?'-style, reply expected

    // Direct calls only (same thread as this object): reply style + echo
    // per task. quiet suppresses the per-exchange traffic echo (errors and
    // timeouts still surface) and the automatic SYST:ERR? follow-up.
    // ahead = UI lane (runs before pump-lane tasks); coalesceKey tags the
    // task for a later dropQueued(key) call (the caller drops the old
    // group and submits the new one; submit itself never drops, so a
    // multi-task group cannot eat its own head).
    void submit(const QString &cmd, qint64 cookie, bool wantsReply, bool isRaw,
                bool quiet, const QString &coalesceKey = QString(),
                bool ahead = false);
    void dropQueued(const QString &coalesceKey); // remove same-key queued tasks
    void setPipelineDepth(int depth) { m_depth = qMax(1, depth); }
    int pipelineDepth() const { return m_depth; }

private slots:
    void onReadyRead();
    void onBytesWritten(qint64 bytes);
    void onTimeout();
    void afterDtr(); // post-open settle: flush stale input, announce the link
    void onError(QSerialPort::SerialPortError error);

private:
    struct Task {
        QString cmd;
        bool wantsReply = false;
        bool isErrCheck = false;
        bool isRaw = false;  // reply is a binary block (WAV:DATA?)
        bool quiet = false;  // no traffic echo, no auto SYST:ERR? follow-up
        qint64 cookie = 0;
        QString key;         // coalesce key (empty = never replaced)
        QString drainFor;    // error check after a quiet NAN answer: its cmd
        bool fromUi = false; // UI lane (never pipelined)
        qint64 writeEnd = 0; // m_wireQueued after this task's write
    };

    void pump();
    bool writeTask(Task &t); // false: write failed (link problem reported)
    void finishSet(const Task &t); // a set command is done: auto error check
    // Resync after a timeout: late replies to the lost tasks may still
    // arrive and would pair with newer tasks. All queues hold while the
    // client sends "*ESE <tok>" and "*ESE?" twice, drops every input line
    // until two "<tok>" lines in a row, then sends "*ESE 0" and resumes.
    void startResync();
    void desyncResync(const QString &cmd, const char *why); // reply of the wrong shape
    void sendResyncProbe();
    bool scanResync(); // true: the marker came in, the link is clean
    // A quiet (pump) query answered NAN: the meter queued an error for it
    // (-222 out-of-record range, -230 no data yet). Drain it at once, so it
    // is logged as a pump note and never blamed on the next user command.
    void drainAfterQuietNan(const QString &cmd);
    void finishCurrent();
    bool tryReadBlock(); // consume one '#<d><len><data><LF>' for the head task

    QSerialPort m_port;
    QTimer m_timer;    // per-task timeout
    QTimer m_dtrTimer; // 200 ms post-open settle
    QTimer m_resyncTimer; // re-sends the resync probe while no marker comes
    bool m_resync = false;
    int m_resyncTok = 170; // cycles 171..254, unlikely as a stale reply
    int m_resyncHits = 0;  // consecutive "<tok>" lines seen
    std::deque<Task> m_queue;    // UI lane: user commands, error checks
    std::deque<Task> m_pumpQ;    // pump lane: acquisition traffic
    QByteArray m_rx;
    // Written, not finished: front = the task whose reply is read next. A
    // UI set waiting for its bytes to leave also sits here, alone.
    std::deque<Task> m_inflight;
    qint64 m_wireQueued = 0; // bytes handed to the port since open
    qint64 m_wireDone = 0;   // bytes the port reported written
    int m_depth = 8;         // max outstanding pump-lane replies
    int m_timeoutMs = 1500;
    int m_errDrain = 0;
    bool m_autoErrCheck = true;
};
