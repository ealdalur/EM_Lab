#include "Radar.hpp"
#include "FarField.hpp"
#include "SimulationGrid.hpp"

#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <future>
#include <random>
#include <thread>

namespace fs = std::filesystem;

namespace {
constexpr int kReferenceChannels = 3; // Ex, Ey, Ez at the timing reference point
constexpr size_t kMaxDetections = 8;

float toDb(double p) { return p > 1e-300 ? static_cast<float>(10.0 * std::log10(p)) : -3000.0f; }

// Offset of a parabola's vertex through (-1, a), (0, b), (1, c), within [-0.5, 0.5].
float vertexOffset(float a, float b, float c) {
    const float den = a - 2.0f * b + c;
    if (std::abs(den) < 1e-12f) return 0.0f;
    return std::clamp(0.5f * (a - c) / den, -0.5f, 0.5f);
}

float distanceToBox(const glm::vec3& p, const glm::vec3& mn, const glm::vec3& mx) {
    return glm::length(p - glm::clamp(p, mn, mx));
}

float distanceToTarget(const glm::vec3& p, const TargetInfo& t) {
    if (t.points.empty()) return distanceToBox(p, t.min, t.max);
    float d = 1e30f;
    for (const glm::vec3& q : t.points) d = std::min(d, glm::length(q - p));
    return d;
}

// In-place radix-2 FFT (size a power of two). Forward: X(k) = sum x(n) exp(-2 pi j k n / N).
void fft(std::vector<std::complex<double>>& a, bool inverse) {
    const size_t n = a.size();
    for (size_t i = 1, j = 0; i < n; ++i) {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    for (size_t len = 2; len <= n; len <<= 1) {
        const double ang = glm::two_pi<double>() / static_cast<double>(len) * (inverse ? 1.0 : -1.0);
        const std::complex<double> wl(std::cos(ang), std::sin(ang));
        for (size_t i = 0; i < n; i += len) {
            std::complex<double> wk(1.0);
            for (size_t j = 0; j < len / 2; ++j) {
                const std::complex<double> u = a[i + j], v = a[i + j + len / 2] * wk;
                a[i + j] = u + v;
                a[i + j + len / 2] = u - v;
                wk *= wl;
            }
        }
    }
    if (inverse)
        for (auto& x : a) x /= static_cast<double>(n);
}
} // namespace

RadarMonitor::RadarMonitor(const fs::path& shaderDir) : m_recorder(shaderDir) {}

glm::vec3 RadarMonitor::direction(float azDeg, float elDeg) const {
    const float a = glm::radians(azDeg), e = glm::radians(elDeg);
    return std::sin(a) * std::cos(e) * m_axes[0] + std::sin(e) * m_axes[1] + std::cos(a) * std::cos(e) * m_axes[2];
}

// Each transmitting element contributes amplitude a_j g_j / d_j, with g_j = cos(angle off
// boresight) for a patch over a ground plane (no radiation along the board).
std::complex<double> RadarMonitor::txArrayFactor(const glm::vec3& p, double omega) const {
    std::complex<double> a(0.0);
    for (size_t j = 0; j < m_txPos.size(); ++j) {
        const glm::vec3 v = p - m_txPos[j];
        const double d = std::max(static_cast<double>(glm::length(v)), 0.5);
        const double g = std::max(static_cast<double>(glm::dot(v, m_axes[2])) / d, 0.0);
        a += (m_txAmp[j] * g / d) * std::polar(1.0, -omega * d);
    }
    return a;
}

double RadarMonitor::txDelay(const glm::vec3& p) const {
    const double w = glm::two_pi<double>() / m_wavelength;
    std::complex<double> a(0.0), ad(0.0);
    double mag = 0.0;
    for (size_t j = 0; j < m_txPos.size(); ++j) {
        const glm::vec3 v = p - m_txPos[j];
        const double d = std::max(static_cast<double>(glm::length(v)), 0.5);
        const double g = std::max(static_cast<double>(glm::dot(v, m_axes[2])) / d, 0.0);
        const std::complex<double> term = (m_txAmp[j] * g / d) * std::polar(1.0, -w * d);
        a += term;
        ad += d * term;
        mag += m_txAmp[j] * g / d;
    }
    // In a deep null of the transmit pattern the group delay is undefined: use the centre.
    if (std::abs(a) < 1e-3 * mag) return static_cast<double>(glm::length(p - m_tx));
    return std::real(ad / a);
}

glm::vec2 RadarMonitor::anglesOf(const glm::vec3& p) const {
    const glm::vec3 u = glm::normalize(p - m_tx);
    return {glm::degrees(std::atan2(glm::dot(u, m_axes[0]), glm::dot(u, m_axes[2]))),
            glm::degrees(std::asin(std::clamp(glm::dot(u, m_axes[1]), -1.0f, 1.0f)))};
}

void RadarMonitor::disable() {
    waitJob();
    m_phase = Phase::Off;
    m_haveCalibration = m_haveMeasurement = false;
    m_cal.clear();
    m_meas.clear();
    m_image = RadarImage{};
    m_recorder.disable();
}

void RadarMonitor::configure(const SimulationGrid& grid, const RadarSetup& setup, const std::vector<PortSetup>& elements,
                             const std::vector<SourceDesc>& sources, float wavelength, float cellSize,
                             const std::vector<TargetInfo>& targets, Start start, std::vector<std::string>& warnings) {
    waitJob(); // the background processing reads this monitor's settings
    const Phase oldPhase = m_phase;
    m_setup = setup;
    m_targets = targets;
    m_wavelength = std::max(wavelength, 2.0f);
    m_cellSize = cellSize;
    m_dt = grid.dt();
    m_elements = elements;
    if (m_elements.empty()) {
        warnings.push_back("radar: the scene has no antenna feeds to transmit and receive with");
        disable();
        return;
    }

    // Array frame: boresight along the feeds' direction; seen from behind the array looking out,
    // azimuth is positive to the right and elevation positive up (see boresightFrame).
    const PortSetup& p0 = m_elements.front();
    glm::vec3 n(0.0f), right, up;
    n[p0.axis] = p0.sign >= 0.0f ? 1.0f : -1.0f;
    boresightFrame(n, right, up);
    m_axes[0] = right;
    m_axes[1] = up;
    m_axes[2] = n;
    m_elementPos.clear();
    for (const PortSetup& p : m_elements) {
        glm::vec3 c = glm::vec3(p.texel);
        c[p.axis] += 0.5f; // centre of the gap edge
        m_elementPos.push_back(c + setup.phaseCenterOffset);
    }

    // Receive taper: a separable window over the element rows and columns. Uniform weighting
    // gives the sharpest beam but -13 dB sidelobes, which show up as false targets next to a
    // strong one; Hamming (-43 dB) and Hann (-31 dB) trade a wider beam for clean sidelobes.
    m_weights.assign(m_elementPos.size(), 1.0);
    if (setup.taper != 0) {
        const double a = setup.taper == 1 ? 0.54 : 0.5;
        for (int axisIndex = 0; axisIndex < 2; ++axisIndex) {
            std::vector<float> coords;
            for (const glm::vec3& p : m_elementPos) coords.push_back(std::round(glm::dot(p, m_axes[axisIndex]) * 4.0f) / 4.0f);
            std::vector<float> uniq = coords;
            std::sort(uniq.begin(), uniq.end());
            uniq.erase(std::unique(uniq.begin(), uniq.end()), uniq.end());
            const double count = static_cast<double>(uniq.size());
            if (uniq.size() < 2) continue;
            for (size_t i = 0; i < coords.size(); ++i) {
                const double idx = static_cast<double>(std::lower_bound(uniq.begin(), uniq.end(), coords[i]) - uniq.begin());
                // Window over count + 2 points with the zero end points dropped.
                m_weights[i] *= a - (1.0 - a) * std::cos(glm::two_pi<double>() * (idx + 1.0) / (count + 1.0));
            }
        }
    }

    // Transmit phase centre and pulse timing from the pulsed feeds (drive-weighted).
    double wsum = 0.0, tsum = 0.0, tauSum = 0.0, tEnd = 0.0;
    glm::dvec3 csum(0.0);
    for (const SourceDesc& s : sources) {
        if (!s.enabled || s.waveform != Waveform::GaussianPulse) continue;
        const double w = std::abs(static_cast<double>(s.amplitude));
        if (w <= 0.0) continue;
        const double period = std::max(static_cast<double>(s.wavelength), 1e-3);
        const double tau = std::max(static_cast<double>(s.rampPeriods), 0.1) * period;
        const double t0 = std::max(static_cast<double>(s.delayPeriods), 0.0) * period + 3.0 * tau;
        csum += w * glm::dvec3(s.center);
        tsum += w * t0;
        tauSum += w * tau;
        wsum += w;
        tEnd = std::max(tEnd, t0 + 3.0 * tau);
    }
    if (wsum <= 0.0) {
        warnings.push_back("radar: no pulsed transmitter; drive the antennas with waveform = \"pulse\"");
        disable();
        return;
    }
    m_tx = setup.txCenter ? *setup.txCenter : glm::vec3(csum / wsum) + setup.phaseCenterOffset;
    m_txSources.clear();
    for (const SourceDesc& s : sources)
        if (s.enabled && s.waveform == Waveform::GaussianPulse && s.amplitude != 0.0f) m_txSources.push_back(s);
    m_txWeight = wsum;
    m_txPos.clear();
    m_txAmp.clear();
    if (setup.txCenter) {
        m_txPos.push_back(*setup.txCenter);
        m_txAmp.push_back(1.0);
    } else {
        for (const SourceDesc& s : m_txSources) {
            m_txPos.push_back(s.center + setup.phaseCenterOffset);
            m_txAmp.push_back(std::abs(static_cast<double>(s.amplitude)));
        }
    }
    m_txTime = tsum / wsum;
    m_targetNearest.clear();
    for (const TargetInfo& t : m_targets) {
        glm::vec3 best = glm::clamp(m_tx, t.min, t.max);
        float bestD = 1e30f;
        for (const glm::vec3& q : t.points) {
            const float dist = glm::length(q - m_tx);
            if (dist < bestD) {
                bestD = dist;
                best = q;
            }
        }
        m_targetNearest.push_back(best);
    }
    m_pulseWidth = tauSum / wsum;

    // Image ranges: by default from one wavelength out to the absorbing layer along boresight.
    const glm::ivec3 gs = grid.size();
    const float L = static_cast<float>(grid.config().pmlThickness);
    m_interiorMin = glm::vec3(L);
    m_interiorMax = glm::vec3(gs) - L;
    const int ax = p0.axis;
    const float toEdge = n[ax] > 0.0f ? static_cast<float>(gs[ax]) - L - 2.0f - m_tx[ax] : m_tx[ax] - L - 1.0f;
    m_setup.rangeMax = setup.rangeMax > 0.0f ? setup.rangeMax : std::max(toEdge, 2.0f * m_wavelength);
    m_setup.rangeMin = setup.rangeMin > 0.0f ? setup.rangeMin : m_wavelength;
    if (m_setup.rangeMax <= m_setup.rangeMin) m_setup.rangeMax = m_setup.rangeMin + m_wavelength;
    if (m_setup.rangeMax > toEdge + 1.0f)
        warnings.push_back("radar: the image range reaches into the absorbing layer along boresight");
    m_referenceDistance = setup.referenceDistance > 0.0f ? setup.referenceDistance : 1.5f * m_wavelength;
    m_referenceDistance = std::min(m_referenceDistance, 0.8f * toEdge);
    m_reference = glm::round(m_tx + n * m_referenceDistance);
    m_referenceDistance = glm::length(m_reference - m_tx);

    // Listening window: the pulse, its round trip to the farthest range plus the array's extent,
    // and the antennas' ring-down. The ring-down is measured during calibration (measureWindow);
    // until then a generous provisional margin is used.
    m_extent = 0.0;
    for (const glm::vec3& p : m_elementPos) m_extent = std::max(m_extent, static_cast<double>(glm::length(p - m_tx)));
    m_pulseEnd = tEnd;
    const double provisional = tEnd + 2.0 * (m_setup.rangeMax + m_extent) + 24.0 * m_wavelength;
    m_provisionalSteps = static_cast<size_t>(std::ceil(provisional / m_dt));
    // The transmitted drive waveform (drive-weighted mean of the pulsed feeds), at the times the
    // solver applies source currents: (n + 1/2) dt.
    m_drive.assign(m_provisionalSteps, 0.0);
    for (size_t k = 0; k < m_provisionalSteps; ++k) {
        double v = 0.0;
        for (const SourceDesc& s : m_txSources) v += static_cast<double>(s.amplitude) * s.signal((static_cast<double>(k) + 0.5) * m_dt);
        m_drive[k] = v / m_txWeight;
    }

    if (start == Start::Reprocess && oldPhase != Phase::Off) {
        // Only processing settings changed: re-process a finished measurement, or let the
        // current recording pass (calibration or measurement) continue with the new settings.
        if (m_haveMeasurement && m_phase == Phase::Done) {
            m_image = computeImage(m_meas, m_meas.size() / (m_elements.size() + kReferenceChannels), true);
            writeLog();
        }
        return;
    }
    if (start != Start::Calibrate && m_haveCalibration && !m_cal.empty()) {
        m_haveMeasurement = false;
        m_phase = Phase::Measuring;
        m_steps = m_calSteps;
        m_image = RadarImage{};
        m_lastPartial = std::chrono::steady_clock::now();
        startRecording(grid);
        return;
    }
    m_haveCalibration = m_haveMeasurement = false;
    m_cal.clear();
    m_meas.clear();
    m_image = RadarImage{};
    m_phase = Phase::Calibrating;
    m_steps = m_provisionalSteps;
    m_windowMeasured = false;
    m_lastWindowCheck = 0;
    startRecording(grid);
}

void RadarMonitor::startRecording(const SimulationGrid& grid) {
    std::vector<PortSetup> channels = m_elements;
    for (int k = 0; k < kReferenceChannels; ++k) {
        PortSetup r;
        r.name = "reference";
        r.texel = glm::ivec3(m_reference);
        r.axis = k;
        r.edges = 1;
        r.sign = 1.0f;
        r.wavelength = m_wavelength;
        r.voltageFeed = false;
        r.drive = 0.0f;
        channels.push_back(r);
    }
    m_recorder.configure(grid, channels, 4.0f, std::nullopt, m_cellSize, m_steps);
}

double RadarMonitor::progress() const noexcept {
    if (m_phase == Phase::Done) return 1.0;
    if (m_phase == Phase::Off || m_steps == 0) return 0.0;
    return std::min(1.0, static_cast<double>(m_recorder.recordedSteps()) / static_cast<double>(m_steps));
}

void RadarMonitor::accumulate(const SimulationGrid& grid) {
    if (m_phase == Phase::Calibrating || m_phase == Phase::Measuring) m_recorder.accumulate(grid);
}

bool RadarMonitor::calibrationComplete() const noexcept { return m_phase == Phase::Calibrating && m_haveCalibration; }

void RadarMonitor::beginMeasurement(const SimulationGrid& grid) {
    if (!m_haveCalibration) return;
    waitJob();
    m_haveMeasurement = false;
    m_phase = Phase::Measuring;
    m_steps = m_calSteps;
    m_image = RadarImage{};
    m_lastPartial = std::chrono::steady_clock::now();
    startRecording(grid);
}

void RadarMonitor::restart(const SimulationGrid& grid) {
    if (m_phase == Phase::Off) return;
    waitJob();
    if (m_phase == Phase::Calibrating) {
        m_haveCalibration = false;
        m_steps = m_provisionalSteps;
        m_windowMeasured = false;
        m_lastWindowCheck = 0;
    } else {
        m_phase = Phase::Measuring;
        m_haveMeasurement = false;
        m_steps = m_calSteps;
        m_image = RadarImage{};
        m_lastPartial = std::chrono::steady_clock::now();
    }
    startRecording(grid);
}

void RadarMonitor::readSeries(std::vector<float>& out) const {
    std::vector<glm::vec2> vi;
    m_recorder.readSeries(vi);
    out.resize(vi.size());
    for (size_t i = 0; i < vi.size(); ++i) out[i] = vi[i].x;
}

void RadarMonitor::waitJob() {
    if (!m_job.valid()) return;
    m_job.wait();
    (void)m_job.get(); // discard: whatever changes next makes it stale
}

void RadarMonitor::launch(bool final) {
    const size_t CH = m_elements.size() + kReferenceChannels;
    m_jobFinal = final;
    m_job = std::async(std::launch::async, [this, meas = m_meas, n = m_meas.size() / CH, final]() {
        return computeImage(meas, n, final);
    });
}

void RadarMonitor::update() {
    using namespace std::chrono;
    // Collect a finished background image.
    if (m_job.valid() && m_job.wait_for(seconds(0)) == std::future_status::ready) {
        m_image = m_job.get();
        if (m_jobFinal) {
            m_phase = Phase::Done;
            writeLog();
        }
    }
    const size_t CH = m_elements.size() + kReferenceChannels;
    const size_t recorded = m_recorder.recordedSteps();
    if (m_phase == Phase::Calibrating && !m_haveCalibration) {
        const size_t period = std::max<size_t>(16, static_cast<size_t>(m_wavelength / m_dt));
        if (!m_windowMeasured && recorded >= m_lastWindowCheck + period) {
            m_lastWindowCheck = recorded;
            measureWindow();
        }
        if (recorded >= m_steps) {
            readSeries(m_cal);
            m_cal.resize(std::min(m_cal.size(), m_steps * CH));
            m_calSteps = m_cal.size() / CH;
            buildTemplate();
            m_haveCalibration = true; // the application now swaps in the targets (beginMeasurement)
        }
    } else if (m_phase == Phase::Measuring && !m_job.valid()) {
        if (recorded >= m_steps) {
            readSeries(m_meas);
            m_meas.resize(std::min(m_meas.size(), m_steps * CH));
            m_haveMeasurement = true;
            launch(true);
        } else if (recorded > 0 && steady_clock::now() - m_lastPartial > milliseconds(500)) {
            readSeries(m_meas); // the record so far: the image fills in from near to far
            m_lastPartial = steady_clock::now();
            launch(false);
        }
    }
}

// Fidelity level of the record: echo contributions this far below a return's peak are
// dropped. 10 dB below the detection dynamic range, so they cannot create, remove or move a
// detection.
double RadarMonitor::fidelityLevel() const { return std::pow(10.0, -(m_setup.dynamicRangeDb + 10.0) / 20.0); }

// During calibration: once the timing reference (one-way: drive -> antennas -> 1.5 wavelengths
// of space) has rung down below the fidelity level, the radar's two-way response length is
// measured from the pulse-compression template built from the record so far. Both passes then
// stop as soon as the echo from the farthest range has fully arrived.
void RadarMonitor::measureWindow() {
    std::vector<float> rec;
    readSeries(rec);
    const size_t C = m_elements.size(), CH = C + kReferenceChannels;
    const size_t n = rec.size() / CH;
    const size_t period = std::max<size_t>(1, static_cast<size_t>(std::lround(m_wavelength / m_dt)));
    const size_t blocks = n / period;
    if (blocks < 6) return;
    const int best = referenceChannel(rec, n);
    double peak = 0.0;
    std::vector<double> blockMax(blocks, 0.0);
    for (size_t b = 0; b < blocks; ++b)
        for (size_t i = b * period; i < (b + 1) * period; ++i) {
            blockMax[b] = std::max(blockMax[b], static_cast<double>(std::abs(rec[i * CH + C + static_cast<size_t>(best)])));
            peak = std::max(peak, blockMax[b]);
        }
    if (!(peak > 0.0)) return;
    // Rung down: the last three periods are all below the fidelity level.
    for (size_t b = blocks - 3; b < blocks; ++b)
        if (blockMax[b] >= fidelityLevel() * peak) return;
    size_t F = 1;
    while (F < 2 * n) F <<= 1;
    std::vector<std::complex<double>> templ;
    buildTemplateSpectrum(rec, n, F, templ);
    const double response = responseLength(templ);
    const double window = 2.0 * m_setup.rangeMax + m_extent + response + m_wavelength;
    m_steps = std::clamp(static_cast<size_t>(std::ceil(window / m_dt)), n, m_provisionalSteps);
    m_windowMeasured = true;
}

int RadarMonitor::referenceChannel(const std::vector<float>& rec, size_t n) const {
    const size_t C = m_elements.size(), CH = C + kReferenceChannels;
    int best = 0;
    double bestPeak = -1.0;
    for (int k = 0; k < kReferenceChannels; ++k) {
        double pk = 0.0;
        for (size_t i = 0; i < n; ++i) pk = std::max(pk, static_cast<double>(std::abs(rec[i * CH + C + k])));
        if (pk > bestPeak) {
            bestPeak = pk;
            best = k;
        }
    }
    return best;
}

// Two-way response length: where the template's envelope stays below the fidelity level.
double RadarMonitor::responseLength(const std::vector<std::complex<double>>& templ) const {
    std::vector<std::complex<double>> t = templ;
    fft(t, true);
    const size_t half = t.size() / 2;
    double tmax = 0.0;
    for (size_t i = 0; i < half; ++i) tmax = std::max(tmax, std::abs(t[i]));
    size_t last = 0;
    for (size_t i = 0; i < half; ++i)
        if (std::abs(t[i]) > fidelityLevel() * tmax) last = i;
    return static_cast<double>(last + 1) * m_dt;
}

// After calibration: the pulse-compression filter. The calibration run's reference field on
// boresight is REF = S H exp(-jw R_ref): the drive spectrum S through the transmitting
// antennas H. A point target's echo is S H H exp(-jw path), since receiving has the same
// response as transmitting (reciprocity). So T = REF^2 / S, with the propagation to the
// reference removed, is the radar's complete two-way response; correlating an echo with T
// peaks at exactly the path delay, whatever the antennas' group delay. The band window keeps
// the positive frequencies around the carrier (an analytic signal) and rejects out-of-band noise.
// The template spectrum T = REF^2 / S (with the propagation to the reference removed) from the
// first n samples of a calibration record, on an F-point frequency grid.
void RadarMonitor::buildTemplateSpectrum(const std::vector<float>& rec, size_t n, size_t F,
                                         std::vector<std::complex<double>>& templ) const {
    const size_t C = m_elements.size(), CH = C + kReferenceChannels;
    const double dt = m_dt, w = glm::two_pi<double>() / m_wavelength;
    const double dw = glm::two_pi<double>() / (static_cast<double>(F) * dt);
    const int best = referenceChannel(rec, n);
    std::vector<std::complex<double>> ref(F), drive(F);
    for (size_t i = 0; i < n; ++i) {
        ref[i] = rec[i * CH + C + static_cast<size_t>(best)];
        if (i < m_drive.size()) drive[i] = m_drive[i];
    }
    fft(ref, false);
    fft(drive, false);
    double driveMax = 0.0;
    for (const auto& v : drive) driveMax = std::max(driveMax, std::abs(v));
    templ.assign(F, 0.0);
    // Propagation from the transmitting elements to the reference point (its near-field array
    // factor) is divided out, leaving the drive through one element's response. Within a big
    // aperture's Fresnel zone that factor dips at some frequencies: regularize the division.
    std::vector<std::complex<double>> refAf(F / 2, 0.0);
    double afMax = 0.0;
    for (size_t k = 1; k < F / 2; ++k) {
        const double om = static_cast<double>(k) * dw;
        if (std::abs(om - w) > 0.6 * w) continue;
        refAf[k] = txArrayFactor(m_reference, om);
        afMax = std::max(afMax, std::abs(refAf[k]));
    }
    for (size_t k = 1; k < F / 2; ++k) {
        const std::complex<double> af = refAf[k];
        if (std::abs(af) < 1e-12) continue;
        const std::complex<double> r = ref[k] * std::conj(af) / (std::norm(af) + 0.05 * afMax * afMax);
        templ[k] = r * r * std::conj(drive[k]) / (std::norm(drive[k]) + 1e-8 * driveMax * driveMax);
    }
}

// After calibration: the pulse-compression filter. The calibration run's reference field on
// boresight is REF = S H exp(-jw R_ref): the drive spectrum S through the transmitting
// antennas H. A point target's echo is S H H exp(-jw path), since receiving has the same
// response as transmitting (reciprocity). So the template T = REF^2 / S is the radar's
// complete two-way response; correlating an echo with T peaks at exactly the path delay,
// whatever the antennas' group delay. The band window keeps the positive frequencies around
// the carrier (an analytic signal) and rejects out-of-band noise.
void RadarMonitor::buildTemplate() {
    const size_t C = m_elements.size(), CH = C + kReferenceChannels;
    const size_t N = m_calSteps;
    const double dt = m_dt, w = glm::two_pi<double>() / m_wavelength;
    m_peakTx = 0.0;
    for (size_t n = 0; n < N; ++n)
        for (size_t i = 0; i < C; ++i) m_peakTx = std::max(m_peakTx, static_cast<double>(std::abs(m_cal[n * CH + i])));
    size_t F = 1;
    while (F < 2 * N) F <<= 1;
    m_fftSize = F;
    const double dw = glm::two_pi<double>() / (static_cast<double>(F) * dt);
    std::vector<std::complex<double>> templ;
    buildTemplateSpectrum(m_cal, N, F, templ);
    m_filter.assign(F, 0.0);
    for (size_t k = 1; k < F / 2; ++k) {
        const double om = static_cast<double>(k) * dw;
        const double band = std::exp(-std::pow((om - w) / (0.3 * w), 2.0));
        m_filter[k] = std::conj(templ[k]) * band;
    }
    {
        // Two-way antenna group delay at the carrier (for display): template delay minus pulse centre.
        const size_t k0 = std::clamp<size_t>(static_cast<size_t>(std::lround(w / dw)), 2, F / 2 - 2);
        const double slope = std::arg(templ[k0 + 1] * std::conj(templ[k0 - 1])) / (2.0 * dw);
        m_systemDelay = static_cast<float>(-slope - m_txTime);
    }
    m_templateEnd = responseLength(templ); // for the progressive display's "final" range
}

RadarImage RadarMonitor::computeImage(const std::vector<float>& meas, size_t nMeas, bool final) const {
    RadarImage img;
    const size_t C = m_elements.size(), CH = C + kReferenceChannels;
    const size_t N = std::min({nMeas, m_calSteps, meas.size() / CH});
    if (N < 32 || m_fftSize < 2 * N || !(m_peakTx > 0.0)) return img;
    const double dt = m_dt, w = glm::two_pi<double>() / m_wavelength;
    auto timeOf = [dt](size_t n) { return static_cast<double>(n + 1) * dt; }; // E after step n + 1
    const double peakTx = m_peakTx;
    img.systemDelay = m_systemDelay;
    img.recordedTime = static_cast<double>(N) * dt;
    img.complete = final;
    // Echoes out to this range have fully arrived (round trip + array extent + response length).
    img.completeRange = final ? m_setup.rangeMax
                              : static_cast<float>(std::clamp((img.recordedTime - m_templateEnd - m_extent) / 2.0, 0.0,
                                                              static_cast<double>(m_setup.rangeMax)));

    double peakEcho = 0.0;
    for (size_t n = 0; n < N; ++n)
        for (size_t i = 0; i < C; ++i)
            peakEcho = std::max(peakEcho, static_cast<double>(std::abs(meas[n * CH + i] - m_cal[n * CH + i])));
    img.echoDb = toDb((peakEcho / peakTx) * (peakEcho / peakTx));

    // Echoes (target run minus calibration run) plus receiver noise, compressed and decimated to
    // complex baseband: z(m) at path delay m D dt - dt / 2.
    // Receiver noise: white, scaled so that its rms within the radar's band (the noise bandwidth
    // of the band window, 0.376 f0) is noiseDb below the transmit pulse peak at a port. Each
    // element has its own seeded generator, so a longer record extends the same noise.
    const double inBand = m_setup.noiseDb > -299.0f ? peakTx * std::pow(10.0, m_setup.noiseDb / 20.0) : 0.0;
    const double sigma = inBand * std::sqrt((0.5 / dt) / (0.376 / m_wavelength));
    const size_t F = m_fftSize;
    const size_t D = std::max<size_t>(1, static_cast<size_t>(std::lround(0.25 * m_wavelength / dt)));
    const size_t M = (N + D - 1) / D;
    std::vector<std::complex<float>> z(M * C);
    std::vector<std::complex<double>> buf(F);
    for (size_t i = 0; i < C; ++i) {
        std::mt19937 rng(20251105u + static_cast<unsigned>(i));
        std::normal_distribution<double> gauss(0.0, 1.0);
        std::fill(buf.begin(), buf.end(), std::complex<double>(0.0));
        for (size_t n = 0; n < N; ++n)
            buf[n] = static_cast<double>(meas[n * CH + i]) - static_cast<double>(m_cal[n * CH + i]) +
                     (sigma > 0.0 ? sigma * gauss(rng) : 0.0);
        fft(buf, false);
        for (size_t k = 0; k < F; ++k) buf[k] *= m_filter[k];
        fft(buf, true);
        for (size_t m = 0; m < M; ++m) {
            const double lag = static_cast<double>(m * D) * dt - 0.5 * dt;
            z[m * C + i] = std::complex<float>(buf[m * D] * std::polar(1.0, -w * lag));
        }
    }

    // Back-projection image over range x azimuth x elevation.
    const float dr = 0.125f * m_wavelength, dAng = 1.5f;
    img.r0 = m_setup.rangeMin;
    img.dr = dr;
    img.nr = std::max(2, static_cast<int>((m_setup.rangeMax - m_setup.rangeMin) / dr) + 1);
    img.naz = 2 * static_cast<int>(std::round(m_setup.fovAzDeg / dAng)) + 1;
    img.nel = 2 * static_cast<int>(std::round(m_setup.fovElDeg / dAng)) + 1;
    img.daz = img.del = dAng;
    img.az0 = -dAng * static_cast<float>(img.naz / 2);
    img.el0 = -dAng * static_cast<float>(img.nel / 2);
    const size_t total = static_cast<size_t>(img.nr) * img.nel * img.naz;
    std::vector<double> power(total, 0.0);
    const double sampleDt = static_cast<double>(D) * dt;
    // Elevation rows are independent: spread them over the CPU cores.
    auto rows = [&](int ieBegin, int ieEnd) {
    for (int ie = ieBegin; ie < ieEnd; ++ie)
        for (int ia = 0; ia < img.naz; ++ia) {
            const glm::vec3 dir = direction(img.az0 + ia * img.daz, img.el0 + ie * img.del);
            for (int ir = 0; ir < img.nr; ++ir) {
                const float r = img.r0 + ir * dr;
                const glm::vec3 P = m_tx + r * dir;
                // The image only covers the simulated volume: outside it nothing can scatter, and
                // the absorbing layer's small reflections would show up as false returns.
                if (glm::any(glm::lessThan(P, m_interiorMin)) || glm::any(glm::greaterThan(P, m_interiorMax))) {
                    power[(static_cast<size_t>(ir) * img.nel + ie) * img.naz + ia] = -1.0;
                    continue;
                }
                const double outbound = txDelay(P);
                std::complex<double> sum(0.0);
                for (size_t i = 0; i < C; ++i) {
                    const double t = outbound + glm::length(P - m_elementPos[i]); // path delay
                    const double s = (t + 0.5 * dt) / sampleDt;              // decimated sample index
                    if (s < 0.0 || s >= static_cast<double>(M - 1)) continue;
                    const size_t m0 = static_cast<size_t>(s);
                    const float f = static_cast<float>(s - static_cast<double>(m0));
                    const std::complex<float> v = z[m0 * C + i] * (1.0f - f) + z[(m0 + 1) * C + i] * f;
                    sum += m_weights[i] * std::complex<double>(v) * std::polar(1.0, w * t);
                }
                power[(static_cast<size_t>(ir) * img.nel + ie) * img.naz + ia] = std::norm(sum);
            }
        }
    };
    const int workers = std::clamp(static_cast<int>(std::thread::hardware_concurrency()), 1, img.nel);
    std::vector<std::thread> pool;
    for (int t = 0; t < workers; ++t) {
        const int b = img.nel * t / workers, e = img.nel * (t + 1) / workers;
        if (t + 1 < workers) pool.emplace_back(rows, b, e);
        else rows(b, e);
    }
    for (std::thread& th : pool) th.join();

    // Levels relative to the strongest return in the final part of the image; the median of the
    // final part is the floor. Points outside the simulated volume are marked -1.
    const int finalBins = final ? img.nr
                                : std::clamp(static_cast<int>(std::floor((img.completeRange - img.r0) / dr)) + 1, 0, img.nr);
    double pmax = 0.0, pmaxAll = 0.0;
    for (size_t k = 0; k < total; ++k) {
        pmaxAll = std::max(pmaxAll, power[k]);
        if (static_cast<int>(k / (static_cast<size_t>(img.nel) * img.naz)) < finalBins) pmax = std::max(pmax, power[k]);
    }
    if (!(pmax > 0.0)) pmax = pmaxAll;
    if (!(pmax > 0.0)) return img;
    img.powerDb.resize(total);
    std::vector<float> inside;
    for (size_t k = 0; k < total; ++k) {
        img.powerDb[k] = power[k] < 0.0 ? -3000.0f : toDb(power[k] / pmax);
        if (power[k] >= 0.0 && static_cast<int>(k / (static_cast<size_t>(img.nel) * img.naz)) < finalBins)
            inside.push_back(img.powerDb[k]);
    }
    if (inside.size() >= 16) {
        std::nth_element(inside.begin(), inside.begin() + inside.size() / 2, inside.end());
        img.floorDb = inside[inside.size() / 2];
    }

    // Detections in the final part: local maxima above the floor + threshold and within the
    // dynamic range of the strongest return.
    auto at = [&](int ir, int ie, int ia) {
        ir = std::clamp(ir, 0, img.nr - 1);
        ie = std::clamp(ie, 0, img.nel - 1);
        ia = std::clamp(ia, 0, img.naz - 1);
        return img.powerDb[(static_cast<size_t>(ir) * img.nel + ie) * img.naz + ia];
    };
    const float minLevel = std::max(img.floorDb + m_setup.thresholdDb, -m_setup.dynamicRangeDb);
    const int lastBin = std::min(img.nr - 1, finalBins - 1); // the last final bin is an edge too
    for (int ir = 1; ir < lastBin && inside.size() >= 16; ++ir)
        for (int ie = 0; ie < img.nel; ++ie)
            for (int ia = 0; ia < img.naz; ++ia) {
                const float v = at(ir, ie, ia);
                if (v < minLevel) continue;
                bool peak = true;
                for (int a = -1; a <= 1 && peak; ++a)
                    for (int b = -1; b <= 1 && peak; ++b)
                        for (int c = -1; c <= 1 && peak; ++c) {
                            if (!a && !b && !c) continue;
                            const int jr = ir + a, je = ie + b, ja = ia + c;
                            if (jr < 0 || je < 0 || ja < 0 || jr >= img.nr || je >= img.nel || ja >= img.naz) continue;
                            const float u = at(jr, je, ja);
                            // Ties go to the first index so a flat top yields one detection.
                            if (u > v || (u == v && (a < 0 || (a == 0 && (b < 0 || (b == 0 && c < 0)))))) peak = false;
                        }
                if (!peak) continue;
                RadarDetection d;
                const float fr = vertexOffset(at(ir - 1, ie, ia), v, at(ir + 1, ie, ia));
                const float fe = vertexOffset(at(ir, ie - 1, ia), v, at(ir, ie + 1, ia));
                const float fa = vertexOffset(at(ir, ie, ia - 1), v, at(ir, ie, ia + 1));
                d.range = img.r0 + (static_cast<float>(ir) + fr) * img.dr;
                d.elDeg = img.el0 + (static_cast<float>(ie) + fe) * img.del;
                d.azDeg = img.az0 + (static_cast<float>(ia) + fa) * img.daz;
                d.position = m_tx + d.range * direction(d.azDeg, d.elDeg);
                d.relDb = v;
                d.snrDb = v - img.floorDb;
                img.detections.push_back(d);
            }
    std::sort(img.detections.begin(), img.detections.end(),
              [](const RadarDetection& a, const RadarDetection& b) { return a.relDb > b.relDb; });
    if (img.detections.size() > kMaxDetections) img.detections.resize(kMaxDetections);

    // Truth: the nearest marked target and its point nearest the radar.
    for (RadarDetection& d : img.detections) {
        float nearest = 1e30f;
        for (size_t t = 0; t < m_targets.size(); ++t) {
            const float dist = distanceToTarget(d.position, m_targets[t]);
            if (dist < nearest) {
                nearest = dist;
                d.target = static_cast<int>(t);
            }
        }
        if (d.target < 0) continue;
        const glm::vec3 q = m_targetNearest[static_cast<size_t>(d.target)];
        d.targetRange = glm::length(q - m_tx);
        const glm::vec2 ang = anglesOf(q);
        d.targetAzDeg = ang.x;
        d.targetElDeg = ang.y;
    }

    // Display slices.
    img.bscope.assign(static_cast<size_t>(img.nr) * img.naz, -3000.0f);
    for (int ir = 0; ir < img.nr; ++ir)
        for (int ie = 0; ie < img.nel; ++ie)
            for (int ia = 0; ia < img.naz; ++ia) {
                float& b = img.bscope[static_cast<size_t>(ir) * img.naz + ia];
                b = std::max(b, at(ir, ie, ia));
            }
    int br = 0, be = img.nel / 2, ba = img.naz / 2;
    if (!img.detections.empty()) {
        const RadarDetection& d = img.detections.front();
        br = std::clamp(static_cast<int>(std::lround((d.range - img.r0) / img.dr)), 0, img.nr - 1);
        be = std::clamp(static_cast<int>(std::lround((d.elDeg - img.el0) / img.del)), 0, img.nel - 1);
        ba = std::clamp(static_cast<int>(std::lround((d.azDeg - img.az0) / img.daz)), 0, img.naz - 1);
    }
    img.cscopeRange = img.r0 + static_cast<float>(br) * img.dr;
    img.cscope.assign(static_cast<size_t>(img.nel) * img.naz, -3000.0f);
    for (int ir = std::max(br - 2, 0); ir <= std::min(br + 2, img.nr - 1); ++ir)
        for (int ie = 0; ie < img.nel; ++ie)
            for (int ia = 0; ia < img.naz; ++ia) {
                float& c = img.cscope[static_cast<size_t>(ie) * img.naz + ia];
                c = std::max(c, at(ir, ie, ia));
            }
    img.ascope.resize(static_cast<size_t>(img.nr));
    for (int ir = 0; ir < img.nr; ++ir) img.ascope[static_cast<size_t>(ir)] = at(ir, be, ba);

    // Raw signals at the element nearest the transmit centre, relative to its transmit peak.
    size_t ce = 0;
    for (size_t i = 1; i < C; ++i)
        if (glm::length(m_elementPos[i] - m_tx) < glm::length(m_elementPos[ce] - m_tx)) ce = i;
    const size_t stride = std::max<size_t>(1, N / 1500);
    for (size_t n = 0; n < N; n += stride) {
        const double cal = m_cal[n * CH + ce], mv = meas[n * CH + ce];
        img.traceTime.push_back(static_cast<float>(timeOf(n) / m_wavelength));
        img.traceCal.push_back(static_cast<float>(cal / peakTx));
        img.traceMeas.push_back(static_cast<float>(mv / peakTx));
        img.traceEcho.push_back(static_cast<float>((mv - cal) / peakTx));
    }
    img.valid = true;
    return img;
}

void RadarMonitor::writeLog() const {
    if (m_setup.logPath.empty() || !m_image.valid) return;
    std::ofstream f(m_setup.logPath, std::ios::trunc);
    if (!f) return;
    f << "detection,range_cells,range_m,azimuth_deg,elevation_deg,above_floor_db,relative_db,target,"
         "target_range_cells,target_azimuth_deg,target_elevation_deg\n";
    for (size_t k = 0; k < m_image.detections.size(); ++k) {
        const RadarDetection& d = m_image.detections[k];
        f << k + 1 << ',' << d.range << ',' << d.range * m_cellSize << ',' << d.azDeg << ',' << d.elDeg << ','
          << d.snrDb << ',' << d.relDb << ',' << (d.target >= 0 ? m_targets[static_cast<size_t>(d.target)].name : "")
          << ',' << d.targetRange << ',' << d.targetAzDeg << ',' << d.targetElDeg << '\n';
    }
}
