#include "Radar.hpp"
#include "SimulationGrid.hpp"

#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <random>

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

    // Array frame: boresight along the feeds' direction; azimuth toward the next axis.
    const PortSetup& p0 = m_elements.front();
    glm::vec3 n(0.0f), e1(0.0f);
    n[p0.axis] = p0.sign >= 0.0f ? 1.0f : -1.0f;
    e1[(p0.axis + 1) % 3] = 1.0f;
    m_axes[0] = e1;
    m_axes[1] = glm::cross(n, e1);
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

    // Record the pulse, its round trip to the farthest range (plus the array's own extent), and
    // a dozen periods for the antennas to ring down.
    float extent = 0.0f;
    for (const glm::vec3& p : m_elementPos) extent = std::max(extent, glm::length(p - m_tx));
    const double T = tEnd + 2.0 * (m_setup.rangeMax + extent) + 12.0 * m_wavelength;
    m_steps = static_cast<size_t>(std::ceil(T / m_dt));
    // The transmitted drive waveform (drive-weighted mean of the pulsed feeds), at the times the
    // solver applies source currents: (n + 1/2) dt.
    m_drive.assign(m_steps, 0.0);
    for (size_t k = 0; k < m_steps; ++k) {
        double v = 0.0;
        for (const SourceDesc& s : m_txSources) v += static_cast<double>(s.amplitude) * s.signal((static_cast<double>(k) + 0.5) * m_dt);
        m_drive[k] = v / m_txWeight;
    }

    if (start == Start::Reprocess && oldPhase != Phase::Off) {
        // Only processing settings changed: re-process a finished measurement, or let the
        // current recording pass (calibration or measurement) continue.
        if (m_haveMeasurement) {
            process();
            m_phase = Phase::Done;
            writeLog();
        }
        return;
    }
    if (start != Start::Calibrate && m_haveCalibration && !m_cal.empty()) {
        m_haveMeasurement = false;
        m_phase = Phase::Measuring;
        startRecording(grid);
        return;
    }
    m_haveCalibration = m_haveMeasurement = false;
    m_cal.clear();
    m_meas.clear();
    m_image = RadarImage{};
    m_phase = Phase::Calibrating;
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
    m_haveMeasurement = false;
    m_phase = Phase::Measuring;
    startRecording(grid);
}

void RadarMonitor::restart(const SimulationGrid& grid) {
    if (m_phase == Phase::Off) return;
    if (m_phase == Phase::Calibrating) {
        m_haveCalibration = false;
    } else {
        m_phase = Phase::Measuring;
        m_haveMeasurement = false;
    }
    startRecording(grid);
}

void RadarMonitor::readSeries(std::vector<float>& out) const {
    std::vector<glm::vec2> vi;
    m_recorder.readSeries(vi);
    out.resize(vi.size());
    for (size_t i = 0; i < vi.size(); ++i) out[i] = vi[i].x;
}

void RadarMonitor::update() {
    if (m_phase == Phase::Calibrating && !m_haveCalibration && m_recorder.recordedSteps() >= m_steps) {
        readSeries(m_cal);
        m_haveCalibration = true; // the application now swaps in the targets (beginMeasurement)
    } else if (m_phase == Phase::Measuring && m_recorder.recordedSteps() >= m_steps) {
        readSeries(m_meas);
        m_haveMeasurement = true;
        process();
        m_phase = Phase::Done;
        writeLog();
    }
}

void RadarMonitor::process() {
    RadarImage img;
    const size_t C = m_elements.size(), CH = C + kReferenceChannels;
    const size_t N = std::min(m_cal.size(), m_meas.size()) / CH;
    if (N < 32) {
        m_image = img;
        return;
    }
    const double dt = m_dt, w = glm::two_pi<double>() / m_wavelength;
    auto timeOf = [dt](size_t n) { return static_cast<double>(n + 1) * dt; }; // E after step n + 1

    // Transmit pulse at the elements, and the strongest echo (before noise).
    double peakTx = 0.0, peakEcho = 0.0;
    for (size_t n = 0; n < N; ++n)
        for (size_t i = 0; i < C; ++i) {
            peakTx = std::max(peakTx, static_cast<double>(std::abs(m_cal[n * CH + i])));
            peakEcho = std::max(peakEcho, static_cast<double>(std::abs(m_meas[n * CH + i] - m_cal[n * CH + i])));
        }
    if (!(peakTx > 0.0)) {
        m_image = img;
        return;
    }
    img.echoDb = toDb((peakEcho / peakTx) * (peakEcho / peakTx));

    // Pulse compression with a measured template. The calibration run's reference field on
    // boresight is REF = S H exp(-jw R_ref): the drive spectrum S through the transmitting
    // antennas H. A point target's echo is S H H exp(-jw path), since receiving has the same
    // response as transmitting (reciprocity). So T = REF^2 / S, with R_ref removed, is the
    // radar's complete two-way response, and correlating an echo with T peaks at exactly the
    // path delay, whatever the antennas' group delay. The band window keeps the positive
    // frequencies around the carrier (an analytic signal) and rejects out-of-band noise.
    size_t F = 1;
    while (F < 2 * N) F <<= 1;
    const double dw = glm::two_pi<double>() / (static_cast<double>(F) * dt);
    int best = 0;
    {
        double bestPeak = -1.0;
        for (int k = 0; k < kReferenceChannels; ++k) {
            double pk = 0.0;
            for (size_t n = 0; n < N; ++n) pk = std::max(pk, static_cast<double>(std::abs(m_cal[n * CH + C + k])));
            if (pk > bestPeak) {
                bestPeak = pk;
                best = k;
            }
        }
    }
    std::vector<std::complex<double>> ref(F), drive(F);
    for (size_t n = 0; n < N; ++n) {
        ref[n] = m_cal[n * CH + C + static_cast<size_t>(best)];
        if (n < m_drive.size()) drive[n] = m_drive[n];
    }
    fft(ref, false);
    fft(drive, false);
    double driveMax = 0.0;
    for (const auto& v : drive) driveMax = std::max(driveMax, std::abs(v));
    std::vector<std::complex<double>> filter(F, 0.0); // conj(T) x band window
    std::vector<std::complex<double>> templ(F, 0.0);
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
        const double om = static_cast<double>(k) * dw;
        const std::complex<double> af = refAf[k];
        if (std::abs(af) < 1e-12) continue;
        const std::complex<double> r = ref[k] * std::conj(af) / (std::norm(af) + 0.05 * afMax * afMax);
        templ[k] = r * r * std::conj(drive[k]) / (std::norm(drive[k]) + 1e-8 * driveMax * driveMax);
        const double band = std::exp(-std::pow((om - w) / (0.3 * w), 2.0));
        filter[k] = std::conj(templ[k]) * band;
    }
    {
        // Two-way antenna group delay at the carrier (for display): the template's delay minus the
        // pulse centre.
        const size_t k0 = std::clamp<size_t>(static_cast<size_t>(std::lround(w / dw)), 2, F / 2 - 2);
        const double slope = std::arg(templ[k0 + 1] * std::conj(templ[k0 - 1])) / (2.0 * dw);
        img.systemDelay = static_cast<float>(-slope - m_txTime);
    }

    // Echoes (target run minus calibration run) plus receiver noise, compressed and decimated to
    // complex baseband: z(m) at path delay m D dt - dt / 2.
    // Receiver noise: white, scaled so that its rms within the radar's band (the noise bandwidth
    // of the band window, 0.376 f0) is noiseDb below the transmit pulse peak at a port.
    const double inBand = m_setup.noiseDb > -299.0f ? peakTx * std::pow(10.0, m_setup.noiseDb / 20.0) : 0.0;
    const double sigma = inBand * std::sqrt((0.5 / dt) / (0.376 / m_wavelength));
    std::mt19937 rng(20251105u);
    std::normal_distribution<double> gauss(0.0, 1.0);
    const size_t D = std::max<size_t>(1, static_cast<size_t>(std::lround(0.25 * m_wavelength / dt)));
    const size_t M = (N + D - 1) / D;
    std::vector<std::complex<float>> z(M * C);
    std::vector<std::complex<double>> buf(F);
    for (size_t i = 0; i < C; ++i) {
        std::fill(buf.begin(), buf.end(), std::complex<double>(0.0));
        for (size_t n = 0; n < N; ++n)
            buf[n] = static_cast<double>(m_meas[n * CH + i]) - static_cast<double>(m_cal[n * CH + i]) +
                     (sigma > 0.0 ? sigma * gauss(rng) : 0.0);
        fft(buf, false);
        for (size_t k = 0; k < F; ++k) buf[k] *= filter[k];
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
    for (int ie = 0; ie < img.nel; ++ie)
        for (int ia = 0; ia < img.naz; ++ia) {
            const glm::vec3 dir = direction(img.az0 + ia * img.daz, img.el0 + ie * img.del);
            for (int ir = 0; ir < img.nr; ++ir) {
                const float r = img.r0 + ir * dr;
                const glm::vec3 P = m_tx + r * dir;
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

    // Levels relative to the strongest return; the median is the floor.
    const double pmax = *std::max_element(power.begin(), power.end());
    if (!(pmax > 0.0)) {
        m_image = img;
        return;
    }
    img.powerDb.resize(total);
    for (size_t k = 0; k < total; ++k) img.powerDb[k] = toDb(power[k] / pmax);
    {
        std::vector<float> sorted = img.powerDb;
        std::nth_element(sorted.begin(), sorted.begin() + sorted.size() / 2, sorted.end());
        img.floorDb = sorted[sorted.size() / 2];
    }

    // Detections: local maxima above the floor + threshold and within the dynamic range.
    auto at = [&](int ir, int ie, int ia) {
        ir = std::clamp(ir, 0, img.nr - 1);
        ie = std::clamp(ie, 0, img.nel - 1);
        ia = std::clamp(ia, 0, img.naz - 1);
        return img.powerDb[(static_cast<size_t>(ir) * img.nel + ie) * img.naz + ia];
    };
    const float minLevel = std::max(img.floorDb + m_setup.thresholdDb, -m_setup.dynamicRangeDb);
    for (int ir = 0; ir < img.nr; ++ir)
        for (int ie = 0; ie < img.nel; ++ie)
            for (int ia = 0; ia < img.naz; ++ia) {
                // Maxima on the first or last range bin are the edges of returns outside the image.
                if (ir == 0 || ir == img.nr - 1) continue;
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

    // Truth: the nearest marked target, its front-surface range and its centre's direction.
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
        const double cal = m_cal[n * CH + ce], meas = m_meas[n * CH + ce];
        img.traceTime.push_back(static_cast<float>(timeOf(n) / m_wavelength));
        img.traceCal.push_back(static_cast<float>(cal / peakTx));
        img.traceMeas.push_back(static_cast<float>(meas / peakTx));
        img.traceEcho.push_back(static_cast<float>((meas - cal) / peakTx));
    }
    img.valid = true;
    m_image = std::move(img);
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
