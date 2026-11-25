#include "scpiclient.h"

ScpiClient::ScpiClient(QObject *parent)
    : QObject(parent)
    , m_port(this)
    , m_timer(this)
    , m_dtrTimer(this)
    , m_resyncTimer(this) // parented: it must follow moveToThread
{
    connect(&m_port, &QSerialPort::readyRead, this, &ScpiClient::onReadyRead);
    connect(&m_port, &QSerialPort::bytesWritten, this, &ScpiClient::onBytesWritten);
    connect(&m_port, &QSerialPort::errorOccurred, this, &ScpiClient::onError);

    m_timer.setSingleShot(true);
    connect(&m_timer, &QTimer::timeout, this, &ScpiClient::onTimeout);

    m_dtrTimer.setSingleShot(true);
    connect(&m_dtrTimer, &QTimer::timeout, this, &ScpiClient::afterDtr);

    m_resyncTimer.setSingleShot(true);
    connect(&m_resyncTimer, &QTimer::timeout, this, [this] {
        if (!m_resync || !m_port.isOpen())
            return;
        emit traffic(QStringLiteral("! resync: no marker, probing again"));
        sendResyncProbe();
    });
}

ScpiClient::~ScpiClient()
{
    if (m_port.isOpen())
        m_port.close();
}

void ScpiClient::openPort(const QString &name)
{
    if (m_port.isOpen()) {
        if (m_port.portName() == name)
            return;
        closePort();
    }
    m_port.setPortName(name);
    m_port.setBaudRate(QSerialPort::Baud115200);
    m_port.setDataBits(QSerialPort::Data8);
    m_port.setParity(QSerialPort::NoParity);
    m_port.setStopBits(QSerialPort::OneStop);
    m_port.setFlowControl(QSerialPort::NoFlowControl);
    if (!m_port.open(QSerialPort::ReadWrite)) {
        emit traffic(QStringLiteral("! open %1 failed: %2")
                         .arg(name, m_port.errorString()));
        emit failed(tr("open %1: %2").arg(name, m_port.errorString()));
        return;
    }
    m_wireQueued = 0;
    m_wireDone = 0;
    // CDC link: tell the device the host is present, let it settle, then
    // drop anything it sent in the meantime.
    m_port.setDataTerminalReady(true);
    m_dtrTimer.start(200);
}

void ScpiClient::afterDtr()
{
    if (!m_port.isOpen())
        return;
    m_port.clear(QSerialPort::Input);
    emit traffic(QStringLiteral(". link up %1 115200 8N1 DTR").arg(m_port.portName()));
    emit opened(m_port.portName());
    pump();
}

void ScpiClient::closePort()
{
    const bool wasOpen = m_port.isOpen();
    m_timer.stop();
    m_dtrTimer.stop();
    m_resyncTimer.stop();
    m_resync = false;
    m_queue.clear();
    m_pumpQ.clear();
    m_inflight.clear();
    m_rx.clear();
    if (wasOpen) {
        m_port.clear();
        m_port.close();
        emit traffic(QStringLiteral(". link down"));
    }
    emit closed();
}

void ScpiClient::onError(QSerialPort::SerialPortError error)
{
    if (error == QSerialPort::NoError)
        return;
    if (!m_port.isOpen())
        return; // open() failures are reported by openPort() itself
    emit traffic(QStringLiteral("! port error: %1").arg(m_port.errorString()));
    if (error == QSerialPort::ResourceError) {
        // Device is gone - tear the link down.
        m_timer.stop();
        m_dtrTimer.stop();
        m_resyncTimer.stop();
        m_resync = false;
        m_queue.clear();
        m_pumpQ.clear();
        m_inflight.clear();
        m_rx.clear();
        m_port.close();
        emit failed(m_port.errorString());
        emit closed();
    }
}

// UI lane: alone on the wire. In the pump lane a query the meter never
// answers (such as a typed "FOO BAR?") shifted every later reply by one.
void ScpiClient::command(const QString &cmd, qint64 cookie)
{
    submit(cmd, cookie, false, false, false, QString(), true);
}

void ScpiClient::query(const QString &cmd, qint64 cookie)
{
    submit(cmd, cookie, true, false, false, QString(), true);
}

void ScpiClient::submit(const QString &cmd, qint64 cookie, bool wantsReply,
                        bool isRaw, bool quiet, const QString &coalesceKey,
                        bool ahead)
{
    Task t;
    t.cmd = cmd;
    t.wantsReply = wantsReply;
    t.isRaw = isRaw;
    t.quiet = quiet;
    t.cookie = cookie;
    t.key = coalesceKey;
    if (ahead)
        m_queue.push_back(std::move(t));
    else
        m_pumpQ.push_back(std::move(t));
    pump();
}

void ScpiClient::dropQueued(const QString &key)
{
    auto strip = [&key](std::deque<Task> &q) {
        std::deque<Task> keep;
        for (Task &t : q) {
            if (t.key == key)
                continue;
            keep.push_back(std::move(t));
        }
        q.swap(keep);
    };
    strip(m_queue);
    strip(m_pumpQ);
}

bool ScpiClient::writeTask(Task &t)
{
    if (!t.isErrCheck)
        m_errDrain = 0;
    if (!t.quiet)
        emit traffic(QStringLiteral("-> ") + t.cmd);
    const QByteArray bytes = (t.cmd + QLatin1Char('\n')).toUtf8();
    if (m_port.write(bytes) < 0) {
        emit traffic(QStringLiteral("! write failed: %1").arg(m_port.errorString()));
        return false;
    }
    m_wireQueued += bytes.size();
    t.writeEnd = m_wireQueued;
    return true;
}

void ScpiClient::pump()
{
    if (!m_port.isOpen() || m_dtrTimer.isActive() || m_resync)
        return; // closed, still settling after open, or resyncing

    // UI lane first (user commands never wait behind frame traffic): one
    // task, alone on the wire, once the in-flight pump replies are in.
    if (m_inflight.empty() && !m_queue.empty()) {
        Task t = std::move(m_queue.front());
        m_queue.pop_front();
        t.fromUi = true;
        m_rx.clear();
        if (!writeTask(t)) {
            if (m_port.isOpen() && (!m_queue.empty() || !m_pumpQ.empty()))
                QTimer::singleShot(50, this, &ScpiClient::pump);
            return;
        }
        m_inflight.push_back(std::move(t));
        m_timer.start(m_timeoutMs);
        return;
    }

    // Pump lane: write ahead while no UI task waits.
    while (m_queue.empty() && !m_pumpQ.empty()) {
        if (!m_inflight.empty()
            && (m_inflight.front().fromUi || int(m_inflight.size()) >= m_depth))
            return;
        Task t = std::move(m_pumpQ.front());
        m_pumpQ.pop_front();
        const bool first = m_inflight.empty();
        if (first)
            m_rx.clear();
        if (!writeTask(t)) {
            if (m_port.isOpen() && (!m_queue.empty() || !m_pumpQ.empty()))
                QTimer::singleShot(50, this, &ScpiClient::pump);
            return;
        }
        if (!t.wantsReply) {
            finishSet(t); // handed to the port: done (may queue a UI error check)
            continue;
        }
        m_inflight.push_back(std::move(t));
        if (first)
            m_timer.start(m_timeoutMs);
    }
    // A non-quiet pump set may have queued an error check with nothing in
    // flight: start it now.
    if (m_inflight.empty() && !m_queue.empty())
        pump();
}

void ScpiClient::onBytesWritten(qint64 bytes)
{
    m_wireDone += bytes;
    if (m_inflight.empty())
        return;
    const Task &h = m_inflight.front();
    if (h.fromUi && !h.wantsReply && m_wireDone >= h.writeEnd)
        finishCurrent(); // UI set command: done once it is on the wire
}

void ScpiClient::onReadyRead()
{
    const QByteArray in = m_port.readAll();
    m_rx += in;
    if (m_resync) {
        if (scanResync()) {
            m_resync = false;
            m_resyncTimer.stop();
            m_rx.clear();
            m_port.write("*ESE 0\n");
            m_wireQueued += 7;
            emit traffic(QStringLiteral(". resync done, link clean"));
            pump();
        }
        return;
    }
    // Progress-based timeout: a long block that keeps arriving never times
    // out, only a silent link does.
    if (!in.isEmpty() && !m_inflight.empty() && m_timer.isActive())
        m_timer.start(m_timeoutMs);
    for (;;) {
        if (!m_inflight.empty() && m_inflight.front().isRaw) {
            if (!tryReadBlock())
                return; // block header/body still arriving
            continue;    // block consumed, the head moved on
        }
        // A block where a text line is due: the replies are shifted (a lost
        // or extra line upstream). Resync rather than split the block into
        // garbage lines for the tasks behind.
        if (!m_inflight.empty() && m_rx.size() >= 2 && m_rx.at(0) == '#'
            && m_rx.at(1) >= '1' && m_rx.at(1) <= '9') {
            desyncResync(m_inflight.front().cmd, "block for a text query");
            return;
        }
        const int nl = m_rx.indexOf('\n');
        if (nl < 0)
            return;
        QByteArray raw = m_rx.left(nl);
        m_rx.remove(0, nl + 1);
        if (raw.endsWith('\r'))
            raw.chop(1);
        const QString line = QString::fromUtf8(raw).trimmed();
        if (line.isEmpty())
            continue;

        if (m_inflight.empty()) {
            emit traffic(QStringLiteral("<- ") + line + QStringLiteral("  (unsolicited)"));
            continue;
        }
        // A copy: a response slot may submit, drop or even close the link.
        const Task cur = m_inflight.front();
        if (cur.isErrCheck && !cur.drainFor.isEmpty()) {
            const int code = line.section(QLatin1Char(','), 0, 0).toInt();
            if (code != 0) {
                emit traffic(QStringLiteral(". %1 NAN, pump-side SYST:ERR %2")
                                 .arg(cur.drainFor, line));
                if (++m_errDrain < 8)
                    drainAfterQuietNan(cur.drainFor);
            }
            finishCurrent();
        } else if (cur.isErrCheck) {
            const int code = line.section(QLatin1Char(','), 0, 0).toInt();
            if (code != 0) {
                emit traffic(QStringLiteral("! SYST:ERR ") + line);
                emit scpiError(line);
                if (++m_errDrain < 8) {
                    Task e;
                    e.cmd = QStringLiteral("SYST:ERR?");
                    e.wantsReply = true;
                    e.isErrCheck = true;
                    m_queue.push_front(std::move(e));
                }
            }
            finishCurrent();
        } else if (cur.wantsReply) {
            if (!cur.quiet)
                emit traffic(QStringLiteral("<- ") + line);
            emit response(cur.cookie, cur.cmd, line);
            if (cur.quiet && line == QLatin1String("NAN"))
                drainAfterQuietNan(cur.cmd);
            finishCurrent();
        } else {
            emit traffic(QStringLiteral("<- ") + line + QStringLiteral("  (unexpected)"));
        }
    }
}

void ScpiClient::onTimeout()
{
    if (m_inflight.empty())
        return;
    const Task head = m_inflight.front();
    QString partial;
    if (!m_rx.isEmpty()) {
        partial = head.isRaw ? QStringLiteral("  (partial: %1 B)").arg(m_rx.size())
                             : QStringLiteral("  (partial: %1)")
                                   .arg(QString::fromUtf8(m_rx.trimmed()));
    }
    const int more = int(m_inflight.size()) - 1;
    emit traffic(QStringLiteral("! timeout %1 ms: %2%3%4")
                     .arg(m_timeoutMs)
                     .arg(head.cmd, partial,
                          more > 0 ? QStringLiteral("  (%1 more in flight dropped)").arg(more)
                                   : QString()));
    // Replies behind a lost one cannot be matched any more: drop them all,
    // then resync so their late answers never pair with newer tasks.
    m_inflight.clear();
    m_rx.clear();
    finishSet(head);
    startResync();
}

void ScpiClient::desyncResync(const QString &cmd, const char *why)
{
    emit traffic(QStringLiteral("! desync on %1 (%2): %3 in flight dropped")
                     .arg(cmd, QLatin1String(why))
                     .arg(m_inflight.size()));
    m_timer.stop();
    m_inflight.clear();
    m_rx.clear();
    startResync();
}

void ScpiClient::startResync()
{
    m_resync = true;
    emit traffic(QStringLiteral(". resync: holding the queues until the link is clean"));
    sendResyncProbe();
}

void ScpiClient::sendResyncProbe()
{
    m_resyncTok = m_resyncTok >= 254 ? 171 : m_resyncTok + 1;
    m_resyncHits = 0;
    const QByteArray probe = QStringLiteral("*ESE %1\n*ESE?\n*ESE?\n")
                                 .arg(m_resyncTok)
                                 .toLatin1();
    if (m_port.write(probe) < 0) {
        emit traffic(QStringLiteral("! write failed: %1").arg(m_port.errorString()));
    } else {
        m_wireQueued += probe.size();
    }
    m_resyncTimer.start(3000);
}

bool ScpiClient::scanResync()
{
    const QByteArray tok = QByteArray::number(m_resyncTok);
    for (;;) {
        const int nl = m_rx.indexOf('\n');
        if (nl < 0)
            return false; // a partial line stays for the next read
        const QByteArray line = m_rx.left(nl).trimmed();
        m_rx.remove(0, nl + 1);
        if (line.isEmpty())
            continue;
        if (line == tok) {
            if (++m_resyncHits >= 2)
                return true;
        } else {
            m_resyncHits = 0;
        }
    }
}

// Consume one IEEE-488.2 definite-length block for the head raw task:
// '#' <d> <d decimal length digits> <length bytes> <LF>. Returns false
// while the block is still arriving (bytes kept in m_rx); on completion
// the task is finished and true is returned.
bool ScpiClient::tryReadBlock()
{
    const Task cur = m_inflight.front(); // a copy, see onReadyRead
    if (m_rx.isEmpty())
        return false;
    if (m_rx.at(0) != '#') {
        // A text answer instead of a block, such as NAN when a channel
        // switch released the lease mid-cycle.
        // Wait for its LF, consume exactly that line and hand the caller an
        // empty block, so the cycle goes on (empty = frame skipped) and the
        // tail of the line cannot leak into the next task.
        const int lf = m_rx.indexOf('\n');
        if (lf < 0)
            return false;
        const QString line = QString::fromUtf8(m_rx.left(lf)).trimmed();
        if (line != QLatin1String("NAN")) {
            // Only NAN is a real text answer to WAV:DATA?. Anything else is
            // the reply of another query: the stream is shifted.
            desyncResync(cur.cmd, "text for a block query");
            return false;
        }
        m_rx.remove(0, lf + 1);
        emit traffic(QStringLiteral(". %1 answered %2, no block").arg(cur.cmd, line));
        emit rawResponse(cur.cookie, cur.cmd, QByteArray());
        if (cur.quiet && line == QLatin1String("NAN"))
            drainAfterQuietNan(cur.cmd);
        finishCurrent();
        return true;
    }
    if (m_rx.size() < 2)
        return false;
    const int digits = m_rx.at(1) - '0';
    if (digits < 1 || digits > 9) {
        desyncResync(cur.cmd, "bad block digit count");
        return false;
    }
    if (m_rx.size() < 2 + digits)
        return false;
    bool ok = false;
    const qint64 len = m_rx.mid(2, digits).toLongLong(&ok);
    if (!ok || len < 0 || len > 4000000) {
        desyncResync(cur.cmd, "bad block length");
        return false;
    }
    if (m_rx.size() < 2 + digits + len + 1)
        return false; // data (plus trailing LF) still in flight
    const QByteArray data = m_rx.mid(2 + digits, int(len));
    m_rx.remove(0, 2 + digits + int(len) + 1);
    if (!cur.quiet)
        emit traffic(QStringLiteral("<- #%1 bytes").arg(len));
    emit rawResponse(cur.cookie, cur.cmd, data);
    finishCurrent();
    return true;
}

void ScpiClient::drainAfterQuietNan(const QString &cmd)
{
    Task e;
    e.cmd = QStringLiteral("SYST:ERR?");
    e.wantsReply = true;
    e.isErrCheck = true;
    e.quiet = true;
    e.drainFor = cmd;
    m_queue.push_front(std::move(e));
}

void ScpiClient::finishSet(const Task &done)
{
    // Follow plain set commands with an error check, ahead of anything the
    // UI queued meanwhile, so a non-zero SYST:ERR? is attributable. Quiet
    // (pump) tasks opt out - they run several times per second.
    if (m_autoErrCheck && !done.quiet && !done.isErrCheck
        && !done.cmd.contains(QLatin1Char('?'))) {
        Task e;
        e.cmd = QStringLiteral("SYST:ERR?");
        e.wantsReply = true;
        e.isErrCheck = true;
        m_queue.push_front(std::move(e));
    }
}

void ScpiClient::finishCurrent()
{
    m_timer.stop();
    if (!m_inflight.empty()) {
        const Task done = std::move(m_inflight.front());
        m_inflight.pop_front();
        finishSet(done);
        if (!m_inflight.empty())
            m_timer.start(m_timeoutMs); // the next reply's own timeout
    }
    pump();
}
