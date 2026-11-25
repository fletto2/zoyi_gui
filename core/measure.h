#pragma once

#include <QString>
#include <cstdint>

// Measurement engine. A port of the meter's Screen-basis readouts
// (firmware src/ui/scope/screen_readouts.cpp, projection.cpp screenSeries,
// acquisition/analysis.cpp Analyzer::summarize over codes, signal_band.cpp,
// measurements.cpp), so the values match what the meter itself shows.
// Input is every raw point of the screen window as signed ADC codes.
namespace meas {

// Same order as the firmware's Measurement enum.
enum Kind { Freq, Vpp, Rms, Period, Mean, Min, Max, AcRms, Duty, Pulses, Off, Fc, Amp,
            Top, Bot, Count };

// The meter's plot width in columns (plot_frame.hpp PlotWidth).
constexpr int kColumns = 300;

struct Result {
    bool valid = false;
    double v[Count] = {};
    float bound[Count] = {};
    unsigned mask = 0;
    bool clipLow = false, clipHigh = false;
    bool spectrum = false; // frequency source (never set on the Screen basis)
    double span = 0;       // samples between the first and last rising crossing
    bool has(Kind k) const { return valid && (mask & (1u << k)); }
};

// codes: n raw points of the screen window. rate: samples per second.
// Volts = (code - zero) * factor.
Result screenBasis(const int8_t *codes, int n, double rate, double zero, double factor);

const char *tag(Kind k);   // readout name, such as "Vpp"
const char *name(Kind k);  // menu name, such as "Peak to peak"
// Value text with the meter's digit rule and SI prefix, such as "1.25 kHz",
// or "--" when not valid. mark gets "~", ">", "<", "?" or "".
QString text(const Result &r, Kind k, QString *mark = nullptr);

} // namespace meas
