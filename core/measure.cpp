#include "measure.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

// Line for line with the firmware (upstream cbbcf51, 0.16.1) where it
// matters for the numbers: thresholds, integer comparisons, the period
// consensus and its reservoir sampler. The lane burst rule is left out
// (the Screen basis runs the scanner with burst 1).
namespace meas {
namespace {

struct Band {
    int low = 0, high = 0;
    bool valid = false;
};

// BandEstimate: the span of the bulk, at most Outliers units left out per side.
class BandEstimate {
public:
    static constexpr unsigned Outliers = 2;
    void add(int low, int high)
    {
        if (!units_++) {
            low_ = low;
            high_ = high;
        }
        low_ = std::min(low_, low);
        high_ = std::max(high_, high);
        keep(lows_, low, [](int a, int b) { return a < b; });
        keep(highs_, high, [](int a, int b) { return a > b; });
        if (kept_ < Outliers + 1)
            ++kept_;
    }
    Band band() const
    {
        Band b;
        if (!units_)
            return b;
        b.valid = true;
        if (kept_ <= Outliers) {
            b.low = low_;
            b.high = high_;
            return b;
        }
        b.low = lows_[Outliers];
        b.high = highs_[Outliers];
        if (b.high - b.low < 4 && high_ - low_ >= 4) {
            b.low = low_;
            b.high = high_;
        }
        return b;
    }

private:
    template <class Before> void keep(int (&values)[Outliers + 1], int value, Before before)
    {
        unsigned at = kept_ < Outliers + 1 ? kept_ : kept_ - 1;
        if (at == kept_ - 1 && !before(value, values[at]))
            return;
        for (; at && before(value, values[at - 1]); --at)
            values[at] = values[at - 1];
        values[at] = value;
    }
    int lows_[Outliers + 1] = {}, highs_[Outliers + 1] = {};
    unsigned units_ = 0, kept_ = 0;
    int low_ = 0, high_ = 0;
};

// StateLevels: edge thresholds from the two histogram plateaus.
class StateLevels {
public:
    static constexpr unsigned MinimumCodes = 8;
    static constexpr double NoiseMargin = 4;
    void add(int code)
    {
        uint16_t &c = counts_[uint8_t(code + 128)];
        if (c != UINT16_MAX)
            ++c;
    }
    Band thresholds(const Band &band) const
    {
        const int low = band.low, high = band.high, span = high - low, twice = low + high;
        if (!band.valid || span < 4)
            return band;
        const int w = std::max(1, span / 16);
        uint32_t centre = 0;
        for (int c = low; c <= high; ++c)
            if (std::abs(2 * c - twice) <= 2 * w)
                centre += counts_[c + 128];
        const int lowTop = (twice + 1 + 256) / 2 - 128 - 1, highBottom = (twice + 256) / 2 - 128 + 1;
        const Plateau a = plateau(low, lowTop, w, centre, true);
        const Plateau b = plateau(highBottom, high, w, centre, false);
        Band levels = band;
        if (a.found)
            levels.low = std::max(low, a.level - a.margin);
        if (b.found)
            levels.high = std::min(high, b.level + b.margin);
        return levels.high - levels.low < 4 ? band : levels;
    }

private:
    struct Plateau {
        int level, margin;
        bool found;
    };
    Plateau plateau(int from, int to, int w, uint32_t centre, bool upward) const
    {
        const auto count = [&](int code) { return uint32_t(counts_[code + 128]); };
        uint32_t side = 0;
        for (int c = from; c <= to; ++c)
            side += count(c);
        uint32_t best = 0;
        int bestAt = 0;
        for (int k = 0; k <= to - from; ++k) {
            const int m = upward ? from + k : to - k;
            uint32_t n = 0;
            for (int c = std::max(from, m - w); c <= std::min(to, m + w); ++c)
                n += count(c);
            if (n > best) {
                best = n;
                bestAt = m;
            }
        }
        if (best < MinimumCodes || 3 * best < side || best < 2 * centre)
            return {0, 0, false};
        int64_t sum = 0, squares = 0;
        for (int c = std::max(from, bestAt - w); c <= std::min(to, bestAt + w); ++c) {
            sum += int64_t(c) * count(c);
            squares += int64_t(c) * c * count(c);
        }
        const double mean = double(sum) / best, variance = double(squares) / best - mean * mean;
        return {int(std::lround(mean)),
                int(std::lround(NoiseMargin * std::sqrt(std::max(0.0, variance)))), true};
    }
    uint16_t counts_[256] = {};
};

struct Summary {
    int minimum = 0, maximum = 0, robustMinimum = 0, robustMaximum = 0;
    bool hasRobust = false;
    double mean = 0, rms = 0, frequencyHz = 0, dutyPercent = 0, frequencySpan = 0;
    uint32_t pulses = 0;
    bool hasPulses = true, valid = false, hasFrequency = false, hasDuty = false;
};

// EdgeScanner: 30/70 % hysteresis, 50 % crossings, period consensus.
class EdgeScanner {
public:
    static constexpr unsigned PeriodLog = 32, ConsensusPrefix = 8;
    void begin(int minimum, int maximum)
    {
        *this = EdgeScanner();
        if (maximum - minimum < 4)
            return;
        middle_ = (maximum + minimum) * .5;
        const double rearm = minimum + (maximum - minimum) * .3;
        const double upper = minimum + (maximum - minimum) * .7;
        rearmFloor_ = int(std::floor(rearm));
        upperCeil_ = int(std::ceil(upper));
        middleFloor_ = int(std::floor(middle_));
        middleCeil_ = int(std::ceil(middle_));
        active_ = true;
    }
    void feed(int value)
    {
        if (!active_)
            return;
        if (!started_) {
            started_ = true;
            armed_ = value <= rearmFloor_;
            fallingArmed_ = value >= upperCeil_;
            previous_ = value;
            index_ = 1;
            return;
        }
        if (value <= rearmFloor_)
            armed_ = true;
        if (value >= upperCeil_)
            fallingArmed_ = true;
        if (fallingArmed_ && previous_ >= middleCeil_ && value < middleCeil_) {
            fallingArmed_ = false;
            count(false, index_ - 1 + (previous_ - middle_) / (previous_ - value));
        }
        if (armed_ && previous_ <= middleFloor_ && value > middleFloor_) {
            armed_ = false;
            count(true, index_ - 1 + (middle_ - previous_) / (value - previous_));
        }
        previous_ = value;
        ++index_;
    }
    bool active() const { return active_; }
    double retryCandidate() const
    {
        const uint32_t periods = edges_ ? edges_ - 1 : 0;
        if (!active_ || !periods || 4 * settled().agreeing >= 3 * periods)
            return 0;
        double period = 0, sum = 0;
        unsigned agreeing = 0;
        return dominant(period, sum, agreeing) ? period : 0;
    }
    void confirm(double candidate)
    {
        const int rearm = rearmFloor_, upper = upperCeil_, mf = middleFloor_, mc = middleCeil_;
        const double middle = middle_;
        *this = EdgeScanner();
        rearmFloor_ = rearm;
        upperCeil_ = upper;
        middleFloor_ = mf;
        middleCeil_ = mc;
        middle_ = middle;
        active_ = true;
        consensus_.candidate = candidate;
        prefixed_ = ConsensusPrefix;
    }
    void finish(double samplesPerSecond, Summary &result) const
    {
        if (!active_)
            return;
        result.pulses = edges_;
        const uint32_t periods = edges_ ? edges_ - 1 : 0;
        if (!periods || last_ <= first_)
            return;
        const Consensus c = settled();
        if (4 * c.agreeing < 3 * periods)
            return;
        if (c.agreeing == periods) {
            result.frequencyHz = periods * samplesPerSecond / (last_ - first_);
            if (periodSamples_ > 0)
                result.dutyPercent = 100 * highSamples_ / periodSamples_;
        } else {
            result.frequencyHz = c.agreeing * samplesPerSecond / c.sum;
            if (c.highPeriods > 0)
                result.dutyPercent = 100 * c.highSum / c.highPeriods;
        }
        result.hasFrequency = std::isfinite(result.frequencyHz) && result.frequencyHz > 0;
        result.hasDuty = result.hasFrequency
                         && (c.agreeing == periods ? periodSamples_ > 0 : c.highPeriods > 0)
                         && std::isfinite(result.dutyPercent);
        if (result.hasFrequency)
            result.frequencySpan = last_ - first_;
        if (!result.hasFrequency)
            result.frequencyHz = 0;
        if (!result.hasDuty)
            result.dutyPercent = 0;
    }

private:
    struct Consensus {
        double candidate = 0, sum = 0, highSum = 0, highPeriods = 0;
        uint32_t agreeing = 0;
        void count(double period, double high)
        {
            if (std::abs(period - candidate) > .2 * candidate)
                return;
            ++agreeing;
            sum += period;
            if (high >= 0) {
                highSum += high;
                highPeriods += period;
            }
        }
    };
    static double medianOf(const double *values, unsigned count)
    {
        double sorted[ConsensusPrefix];
        std::copy(values, values + count, sorted);
        std::nth_element(sorted, sorted + count / 2, sorted + count);
        return sorted[count / 2];
    }
    void count(bool rising, double position)
    {
        if (!rising) {
            falling_ = position;
            haveFalling_ = edges_ && falling_ > last_;
            return;
        }
        if (!edges_) {
            first_ = position;
        } else {
            const double period = position - last_;
            const bool high = haveFalling_ && falling_ < position;
            if (high) {
                highSamples_ += falling_ - last_;
                periodSamples_ += period;
            }
            log(period);
            consensus(period, high ? falling_ - last_ : -1);
        }
        last_ = position;
        haveFalling_ = false;
        ++edges_;
    }
    void log(double period)
    {
        const uint32_t index = edges_ - 1;
        if (index < PeriodLog) {
            periods_[logged_++] = float(period);
            return;
        }
        sampler_ = sampler_ * 1664525u + 1013904223u;
        const uint32_t slot = uint32_t(uint64_t(sampler_) * (index + 1) >> 32);
        if (slot < PeriodLog)
            periods_[slot] = float(period);
    }
    void consensus(double period, double high)
    {
        if (prefixed_ < ConsensusPrefix) {
            prefix_[prefixed_] = period;
            prefixHigh_[prefixed_] = high;
            if (++prefixed_ < ConsensusPrefix)
                return;
            consensus_.candidate = medianOf(prefix_, prefixed_);
            for (unsigned i = 0; i < prefixed_; ++i)
                consensus_.count(prefix_[i], prefixHigh_[i]);
            return;
        }
        consensus_.count(period, high);
    }
    Consensus settled() const
    {
        Consensus c = consensus_;
        if (prefixed_ < ConsensusPrefix && prefixed_) {
            c.candidate = medianOf(prefix_, prefixed_);
            for (unsigned i = 0; i < prefixed_; ++i)
                c.count(prefix_[i], prefixHigh_[i]);
        }
        return c;
    }
    bool dominant(double &period, double &sum, unsigned &agreeing) const
    {
        const unsigned count = logged_;
        if (count < 2)
            return false;
        float sorted[PeriodLog];
        std::copy(periods_, periods_ + count, sorted);
        std::nth_element(sorted, sorted + count / 2, sorted + count);
        const double median = sorted[count / 2];
        sum = 0;
        agreeing = 0;
        for (unsigned i = 0; i < count; ++i)
            if (std::abs(periods_[i] - median) <= median * .2) {
                sum += periods_[i];
                ++agreeing;
            }
        period = median;
        return median > 0 && 4 * agreeing >= 3 * count;
    }

    double middle_ = 0, first_ = 0, last_ = 0;
    double falling_ = 0, highSamples_ = 0, periodSamples_ = 0;
    float periods_[PeriodLog] = {};
    uint32_t logged_ = 0, sampler_ = 0x2545F491u;
    Consensus consensus_;
    double prefix_[ConsensusPrefix] = {}, prefixHigh_[ConsensusPrefix] = {};
    unsigned prefixed_ = 0;
    int rearmFloor_ = 0, upperCeil_ = 0, middleFloor_ = 0, middleCeil_ = 0;
    uint32_t edges_ = 0, index_ = 0;
    int previous_ = 0;
    bool active_ = false, started_ = false, armed_ = false, fallingArmed_ = false,
         haveFalling_ = false;
};

// Analyzer::summarize over a short series (codes version).
Summary summarize(const int8_t *codes, int count, double samplesPerSecond)
{
    Summary result;
    if (!codes || count <= 0)
        return result;
    int lo = codes[0], hi = codes[0];
    int64_t sum = 0, squares = 0;
    for (int i = 0; i < count; ++i) {
        lo = std::min<int>(lo, codes[i]);
        hi = std::max<int>(hi, codes[i]);
        sum += codes[i];
        squares += int64_t(codes[i]) * codes[i];
    }
    result.minimum = lo;
    result.maximum = hi;
    result.mean = double(sum) / count;
    result.rms = std::sqrt(double(squares) / count);
    result.valid = true;
    BandEstimate estimate;
    StateLevels levels;
    for (int i = 0; i < count; ++i) {
        estimate.add(codes[i], codes[i]);
        levels.add(codes[i]);
    }
    const Band band = estimate.band();
    result.hasRobust = band.valid;
    if (band.valid) {
        result.robustMinimum = band.low;
        result.robustMaximum = band.high;
    }
    EdgeScanner edges;
    const Band t = levels.thresholds(band);
    edges.begin(t.low, t.high);
    for (int i = 0; i < count; ++i)
        edges.feed(codes[i]);
    if (const double candidate = edges.retryCandidate()) {
        edges.confirm(candidate);
        for (int i = 0; i < count; ++i)
            edges.feed(codes[i]);
    }
    edges.finish(samplesPerSecond, result);
    return result;
}

const char *const kTags[Count] = {"f", "Vpp", "RMS", "T", "Avg", "Min", "Max", "AC",
                                  "Duty", "n", "", "FC", "Amp", "Top", "Bot"};
const char *const kNames[Count] = {"Frequency", "Peak to peak", "RMS", "Period", "Mean",
                                   "Minimum", "Maximum", "AC RMS", "Duty",
                                   "Pulses (rising edges)", "Off", "Hardware counter",
                                   "Amplitude (Top - Bottom)", "Top (robust high)",
                                   "Bottom (robust low)"};
const char *const kUnits[Count] = {"Hz", "V", "V", "s", "V", "V", "V", "V", "%", "", "", "Hz",
                                   "V", "V", "V"};

// Measurements::supported(): digits the bound allows (3 without one).
int supported(const Result &r, Kind k)
{
    const double u = r.bound[k], v = r.v[k];
    if (!r.has(k) || !(u > 0) || !(std::abs(v) > 0))
        return 3;
    return int(std::floor(std::log10(std::abs(v)))) - int(std::floor(std::log10(u))) + 1;
}

} // namespace

Result screenBasis(const int8_t *codes, int n, double rate, double zero, double factor)
{
    Result r;
    if (!codes || n <= 0 || !(rate > 0) || !(factor > 0))
        return r;
    // ScreenSeries: one sample per column (its first point) once the window
    // has a point per column, else every point (at most 320).
    const bool aggregated = n >= kColumns;
    int8_t series[320];
    uint8_t spans[kColumns];
    int count = 0, lo = 127, hi = -128;
    BandEstimate bands;
    double seriesRate = rate;
    if (aggregated) {
        seriesRate = rate * kColumns / double(n);
        for (int x = 0; x < kColumns; ++x) {
            const int first = int(int64_t(n) * x / kColumns);
            const int last = int(int64_t(n) * (x + 1) / kColumns);
            if (first >= last)
                continue;
            int clo = codes[first], chi = codes[first];
            for (int i = first + 1; i < last; ++i) {
                clo = std::min<int>(clo, codes[i]);
                chi = std::max<int>(chi, codes[i]);
            }
            series[count] = codes[first];
            spans[count] = uint8_t(chi - clo);
            ++count;
            lo = std::min(lo, clo);
            hi = std::max(hi, chi);
            bands.add(clo, chi);
        }
    } else {
        for (int i = 0; i < n && count < 320; ++i) {
            series[count++] = codes[i];
            lo = std::min<int>(lo, codes[i]);
            hi = std::max<int>(hi, codes[i]);
            bands.add(codes[i], codes[i]);
        }
    }
    Summary s = summarize(series, count, seriesRate);
    if (!s.valid)
        return r;
    // ScreenReadouts::resolution().
    bool edgesOk = true, countOk = true;
    if (aggregated && hi - lo >= 4) {
        const int span = hi - lo, twiceMiddle = hi + lo;
        for (int i = 0; i + 1 < count; ++i)
            if (2 * spans[i] > span
                && (2 * series[i] > twiceMiddle) == (2 * series[i + 1] > twiceMiddle)) {
                edgesOk = countOk = false;
                break;
            }
        if (edgesOk)
            countOk = !(count && 2 * spans[count - 1] > span);
    }
    if (!edgesOk || (s.hasFrequency && s.frequencyHz * 4 > seriesRate)) {
        s.hasFrequency = s.hasDuty = s.hasPulses = false;
        s.frequencyHz = s.dutyPercent = s.frequencySpan = 0;
    }
    if (!countOk)
        s.hasPulses = false;
    s.minimum = lo;
    s.maximum = hi;
    const Band band = bands.band();
    s.hasRobust = band.valid;
    s.robustMinimum = band.low;
    s.robustMaximum = band.high;

    // Measurements::calculate().
    r.valid = true;
    const auto set = [&r](Kind k, double v) {
        r.v[k] = v;
        if (std::isfinite(v))
            r.mask |= 1u << k;
    };
    if (s.hasPulses)
        set(Pulses, s.pulses);
    if (s.hasDuty && std::isfinite(s.dutyPercent) && s.dutyPercent >= 0 && s.dutyPercent <= 100)
        set(Duty, s.dutyPercent);
    if (s.hasFrequency && std::isfinite(s.frequencyHz) && s.frequencyHz > 0) {
        set(Freq, s.frequencyHz);
        set(Period, 1 / s.frequencyHz);
        r.span = s.frequencySpan;
    }
    r.clipLow = (s.hasRobust ? s.robustMinimum : s.minimum) <= -128;
    r.clipHigh = (s.hasRobust ? s.robustMaximum : s.maximum) >= 127;
    set(Vpp, (s.maximum - s.minimum) * factor);
    set(Mean, (s.mean - zero) * factor);
    set(Min, (s.minimum - zero) * factor);
    set(Max, (s.maximum - zero) * factor);
    set(Rms, std::sqrt(std::max(0.0, s.rms * s.rms - 2 * zero * s.mean + zero * zero)) * factor);
    set(AcRms, std::sqrt(std::max(0.0, s.rms * s.rms - s.mean * s.mean)) * factor);
    if (s.hasRobust) {
        set(Amp, (s.robustMaximum - s.robustMinimum) * factor);
        set(Top, (s.robustMaximum - zero) * factor);
        set(Bot, (s.robustMinimum - zero) * factor);
    }
    // ScreenReadouts::bound().
    if (r.has(Freq) && seriesRate > 0) {
        const double hz = r.v[Freq];
        if (r.span > 1) {
            r.bound[Freq] = float(hz / (r.span - 1));
            r.bound[Period] = float(1 / (hz * (r.span - 1)));
        }
        r.bound[Duty] = float(100 * hz / seriesRate);
    }
    return r;
}

const char *tag(Kind k) { return k >= 0 && k < Count ? kTags[k] : ""; }
const char *name(Kind k) { return k >= 0 && k < Count ? kNames[k] : ""; }

QString text(const Result &r, Kind k, QString *mark)
{
    if (mark)
        mark->clear();
    if (!r.has(k))
        return QStringLiteral("--");
    // Marks (ReadoutText::presentation): "~" when the bound allows fewer
    // than two digits, a clip mark over it.
    const int sup = supported(r, k);
    QString m = sup < 2 ? QStringLiteral("~") : QString();
    const bool clip = r.clipLow || r.clipHigh;
    switch (k) {
    case Vpp: case Amp: if (clip) m = QStringLiteral(">"); break;
    case Max: case Top: if (r.clipHigh) m = QStringLiteral(">"); break;
    case Min: case Bot: if (r.clipLow) m = QStringLiteral("<"); break;
    case Mean: case Rms: case AcRms: if (clip) m = QStringLiteral("?"); break;
    default: break;
    }
    if (mark)
        *mark = m;
    const double v = r.v[k];
    if (k == Pulses)
        return QString::number(qint64(std::llround(v)));
    const int digits = std::clamp(sup, 2, 3);
    const QString unit = QString::fromLatin1(kUnits[k]);
    if (k == Duty)
        return QString::number(v, 'g', digits) + QStringLiteral(" %");
    if (v == 0)
        return QStringLiteral("0 ") + unit;
    static const char *const prefixes[] = {"p", "n", "u", "m", "", "k", "M", "G"};
    int e3 = int(std::floor(std::log10(std::abs(v)) / 3));
    e3 = std::clamp(e3, -4, 3);
    double mant = v / std::pow(1000.0, e3);
    // Rounding to `digits` may carry to 1000: step up a prefix.
    const double scale = std::pow(10.0, digits - 1 - int(std::floor(std::log10(std::abs(mant)))));
    mant = std::round(mant * scale) / scale;
    if (std::abs(mant) >= 1000 && e3 < 3) {
        ++e3;
        mant /= 1000;
    }
    const int intDigits = int(std::floor(std::log10(std::max(1.0, std::abs(mant))))) + 1;
    const int decimals = std::max(0, digits - intDigits);
    return QString::number(mant, 'f', decimals) + QLatin1Char(' ')
           + QLatin1String(prefixes[e3 + 4]) + unit;
}

} // namespace meas
