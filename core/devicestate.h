#pragma once

#include <QMetaType>

// Device state model. The frame pump owns it: a full read at connect,
// then one poll query per frame gap (round-robin), so knob turns on the
// meter show in the app without pausing frames. The UI binds to it through
// FramePump::stateChanged().
struct ChannelState {
    bool disp = false;
    double scal = 0.0; // V/div (probe factor included)
    double offs = 0.0; // V (CHAN:OFFS, +-4 div in 1/20 div steps)
    bool ac = false;   // coupling AC (else DC; the firmware has no GND)
    double prob = 1.0; // probe factor
    int known = 0;     // bit per Field below: answered at least once
};

// Edge trigger. Answers per src/scpi/scope.cpp (upstream 0.16.1 tree).
struct TriggerState {
    int source = 1;      // TRIG:EDGE:SOUR CHAN1|CHAN2
    bool rising = true;  // TRIG:EDGE:SLOP POS|NEG
    double level = 0.0;  // TRIG:EDGE:LEV, V (NAN when the channel is uncalibrated)
    int sweep = 0;       // TRIG:SWE: 0 AUTO, 1 NORM, 2 SING
    bool nrej = false;   // TRIG:EDGE:NREJ 0|1
    int known = 0;       // bit per TrigField
};

// Read-only telemetry for the status bar.
struct Telemetry {
    double srat = 0.0;   // ACQ:SRAT?, Hz
    int mdep = 0;        // ACQ:MDEP?, points
    double coreHz = 0.0; // SYST:PERF? field 2; USB data needs 200 MHz
    bool roll = true;    // ACQ:ROLL?: roll allowed at >= 200 ms/div in Auto
    // Hardware counter on the trigger channel. MEAS:FCNT? Hz and
    // MEAS:FCNT:QUAL? "CLEAN|FILTERED|UNVERIFIED|UNCERTAIN|CHANGING,<filter>".
    double fcnt = 0.0;
    int fqual = -1;      // index into the names above, -1 unknown
    int known = 0;       // bit per TeleField
};

struct DeviceState {
    enum Field { Disp = 0, Scal, Offs, Coup, Prob, FieldCount };
    enum TrigField { TSour = 0, TSlop, TLev, TSwe, TNrej, TrigFieldCount };
    enum TeleField { YSrat = 0, YMdep, YPerf, YRoll, YFcnt, YFqual, TeleFieldCount };
    // Poll index layout: channels 0-9 (ch * FieldCount + field), trigger
    // 10-14, telemetry 15-20.
    static constexpr int kTrigBase = 2 * FieldCount;
    static constexpr int kTeleBase = kTrigBase + TrigFieldCount;
    static constexpr int kPollCount = kTeleBase + TeleFieldCount;
    ChannelState ch[2];
    TriggerState trig;
    Telemetry tele;
};
Q_DECLARE_METATYPE(DeviceState)

// Capability probe at connect. A missing query answers NAN (and -113).
struct DeviceCaps {
    bool poin = false;  // WAV:POIN (screen-window decimation)
    bool stall = false; // SYST:STALl? (fletto2 diagnostics)
    bool boot = false;  // SYST:BOOT?
    bool cros = false;  // WAV:CROSsing?
    bool peak = false;  // WAV:POIN:MODE PEAK (min/max decimation, 0.18.0)
    bool frame = false; // WAV:FRAMe? (batch 11 item 1)
    bool env = false;   // WAV:ENVelope? (whole-record min/max envelope)
    bool meas = false;  // WAV:MEASure? (firmware readouts for decimated windows)
    bool done = false;
};
Q_DECLARE_METATYPE(DeviceCaps)
