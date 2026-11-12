#include "Crpa.hpp"

#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <random>

namespace fs = std::filesystem;

namespace {
using cd = std::complex<double>;
constexpr int kMapSize = 61;

double toDb(double p) { return p > 1e-300 ? 10.0 * std::log10(p) : -300.0; }

double meanPower(const std::vector<cd>& v) {
    double s = 0.0;
    for (const cd& x : v) s += std::norm(x);
    return v.empty() ? 0.0 : s / static_cast<double>(v.size());
}

cd dotH(const std::vector<cd>& a, const std::vector<cd>& b) { // a^H b
    cd s(0.0);
    for (size_t i = 0; i < a.size(); ++i) s += std::conj(a[i]) * b[i];
    return s;
}

// Solves A x = b for a small dense complex system (Gaussian elimination, partial pivoting).
std::vector<cd> solve(std::vector<cd> A, std::vector<cd> b, size_t n) {
    for (size_t col = 0; col < n; ++col) {
        size_t piv = col;
        for (size_t r = col + 1; r < n; ++r)
            if (std::abs(A[r * n + col]) > std::abs(A[piv * n + col])) piv = r;
        if (piv != col) {
            for (size_t k = 0; k < n; ++k) std::swap(A[col * n + k], A[piv * n + k]);
            std::swap(b[col], b[piv]);
        }
        const cd d = A[col * n + col];
        if (std::abs(d) < 1e-300) continue;
        for (size_t r = col + 1; r < n; ++r) {
            const cd f = A[r * n + col] / d;
            for (size_t k = col; k < n; ++k) A[r * n + k] -= f * A[col * n + k];
            b[r] -= f * b[col];
        }
    }
    std::vector<cd> x(n);
    for (size_t i = n; i-- > 0;) {
        cd s = b[i];
        for (size_t k = i + 1; k < n; ++k) s -= A[i * n + k] * x[k];
        x[i] = std::abs(A[i * n + i]) > 1e-300 ? s / A[i * n + i] : cd(0.0);
    }
    return x;
}
} // namespace

CrpaMonitor::CrpaMonitor(const fs::path& shaderDir) : m_recorder(shaderDir) {}

void CrpaMonitor::disable() {
    m_phase = Phase::Off;
    m_measured = false;
    m_response.clear();
    m_signature.clear();
    m_result = CrpaResult{};
    m_recorder.disable();
}

glm::vec2 CrpaMonitor::compassAngles(const glm::vec3& p) const {
    const glm::vec3 up(0.0f, 0.0f, 1.0f);
    const glm::vec3 north = m_setup.north, east = glm::cross(north, up);
    const glm::vec3 d = p - m_center;
    const float len = std::max(glm::length(d), 1e-6f);
    float az = glm::degrees(std::atan2(glm::dot(d, east), glm::dot(d, north)));
    if (az < 0.0f) az += 360.0f;
    return {az, glm::degrees(std::asin(std::clamp(glm::dot(d, up) / len, -1.0f, 1.0f)))};
}

void CrpaMonitor::configure(const SimulationGrid& grid, const CrpaSetup& setup, const std::vector<EmitterSetup>& emitters,
                            const std::vector<SourceDesc>& sources, const std::vector<PortSetup>& ports, float wavelength,
                            float cellSize, bool sameScene, std::vector<std::string>& warnings) {
    m_setup = setup;
    m_emitters = emitters;
    m_sources = sources;
    m_ports = ports;
    m_wavelength = std::max(wavelength, 2.0f);
    m_cellSize = cellSize;
    m_dt = grid.dt();
    if (m_emitters.empty()) {
        warnings.push_back("crpa: the scene has no emitter{} transmitters");
        disable();
        return;
    }
    m_elementPorts.clear();
    m_center = glm::vec3(0.0f);
    for (const CrpaElement& el : setup.elements) {
        int ix = -1, iy = -1;
        for (size_t p = 0; p < ports.size(); ++p) {
            if (ports[p].name == el.portX) ix = static_cast<int>(p);
            if (ports[p].name == el.portY) iy = static_cast<int>(p);
        }
        m_elementPorts.emplace_back(ix, iy);
        m_center += el.center;
    }
    m_center /= static_cast<float>(std::max<size_t>(setup.elements.size(), 1));
    m_satellite = 0;
    for (size_t e = 0; e < m_emitters.size(); ++e)
        if (m_emitters[e].name == setup.satellite) m_satellite = static_cast<int>(e);
    m_passSteps = static_cast<size_t>(std::ceil(setup.passPeriods * m_wavelength / m_dt));
    // Blind run: settling, the snapshot window, and 2 periods either side for the baseband filter.
    m_blindSteps = static_cast<size_t>(std::ceil((setup.passPeriods + setup.snapshotPeriods + 4.5) * m_wavelength / m_dt));

    // Blind run: jammers equal power; the satellite set for about snrDb when the strongest
    // jammer is at jnrDb (free-space estimate: two dipoles, full RHCP vs a linear jammer's -3 dB).
    m_emitterWavelength.assign(m_emitters.size(), m_wavelength);
    for (const SourceDesc& s : sources)
        if (s.emitter >= 0 && s.emitter < static_cast<int>(m_emitters.size()))
            m_emitterWavelength[static_cast<size_t>(s.emitter)] = s.wavelength / (1.0 + emitterOffset(s.emitter));
    double nearestJammer = 0.0;
    for (size_t e = 0; e < m_emitters.size(); ++e)
        if (static_cast<int>(e) != m_satellite) {
            const double r = glm::length(m_emitters[e].position - m_center);
            nearestJammer = nearestJammer > 0.0 ? std::min(nearestJammer, r) : r;
        }
    const double rSat = glm::length(m_emitters[static_cast<size_t>(m_satellite)].position - m_center);
    m_satAmplitude = nearestJammer > 0.0
                         ? 0.5 * std::pow(10.0, (setup.snrDb - setup.jnrDb) / 20.0) * rSat / nearestJammer
                         : 1.0;

    // What the measurements depend on: the grid, the emitters' sources, the ports, the pass length.
    std::vector<double> sig = {static_cast<double>(grid.size().x), static_cast<double>(grid.size().y),
                               static_cast<double>(grid.size().z), setup.passPeriods, m_wavelength,
                               setup.blind ? 1.0 : 0.0};
    if (setup.blind) // the transmit powers are part of the physical run
        sig.insert(sig.end(), {setup.snapshotPeriods, setup.jnrDb, setup.snrDb});
    for (const SourceDesc& s : sources)
        if (s.emitter >= 0)
            sig.insert(sig.end(), {static_cast<double>(s.emitter), s.center.x, s.center.y, s.center.z, s.polarization.x,
                                   s.polarization.y, s.polarization.z, s.amplitude, s.phase, s.wavelength});
    for (const PortSetup& p : ports)
        sig.insert(sig.end(), {static_cast<double>(p.texel.x), static_cast<double>(p.texel.y), static_cast<double>(p.texel.z)});
    const bool reuse = sameScene && m_measured && sig == m_signature && m_response.size() == m_emitters.size() &&
                       (!setup.blind || !m_baseband.empty());
    m_signature = sig;
    if (reuse) { // only the processing changed: new weights, transmit them again
        computeWeights();
        m_phase = Phase::Adapted;
        m_needsApply = true;
        writeLog();
        return;
    }
    m_measured = false;
    m_response.assign(m_emitters.size(), {});
    m_baseband.clear();
    m_result = CrpaResult{};
    m_pass = 0;
    m_phase = setup.blind ? Phase::Listening : Phase::Measuring;
    m_needsApply = true;
}

double CrpaMonitor::passProgress() const noexcept {
    const size_t steps = m_phase == Phase::Measuring ? m_passSteps : m_phase == Phase::Listening ? m_blindSteps : 0;
    if (steps == 0) return 1.0;
    return std::min(1.0, static_cast<double>(m_recorder.recordedSteps()) / static_cast<double>(steps));
}

// Blind run: the jammers transmit a few percent off the carrier (+1, -1, +2, -2, ... times
// 1 / snapshotPeriods), so over the snapshot window they are exactly uncorrelated with the
// satellite and with each other, as independent real jammers are statistically.
double CrpaMonitor::emitterOffset(int emitter) const {
    if (!m_setup.blind || emitter == m_satellite) return 0.0;
    const int rank = emitter < m_satellite ? emitter : emitter - 1;
    const int n = (rank / 2 + 1) * (rank % 2 == 0 ? 1 : -1);
    return static_cast<double>(n) / std::max(m_setup.snapshotPeriods, 1.0f);
}

void CrpaMonitor::restart(const SimulationGrid& /*grid*/) {
    if (m_phase != Phase::Off) m_needsApply = true; // repeat the current pass
}

void CrpaMonitor::accumulate(const SimulationGrid& grid) {
    if (m_phase == Phase::Measuring || m_phase == Phase::Listening) m_recorder.accumulate(grid);
}

// Measurement pass: only one emitter transmits, the array listens (feeds are 50-ohm loads).
// Adapted pass: the emitters are silent and the array transmits with the receive weights.
void CrpaMonitor::startPass(SimulationGrid& grid) {
    std::vector<SourceDesc> active;
    if (m_phase == Phase::Measuring) {
        for (const SourceDesc& s : m_sources)
            if (s.emitter == m_pass) active.push_back(s);
    } else if (m_phase == Phase::Listening) {
        for (const SourceDesc& s : m_sources) {
            if (s.emitter < 0) continue;
            SourceDesc t = s;
            t.wavelength = static_cast<float>(s.wavelength / (1.0 + emitterOffset(s.emitter)));
            if (s.emitter == m_satellite) t.amplitude = static_cast<float>(s.amplitude * m_satAmplitude);
            active.push_back(t);
        }
    } else {
        // Receive output y = sum_i conj(w_i) (X_i - j Y_i) / sqrt 2. By reciprocity, driving every
        // port with the same coefficients radiates the receive pattern.
        std::vector<cd> coef(m_ports.size(), cd(0.0));
        const double r2 = 1.0 / std::sqrt(2.0);
        for (size_t i = 0; i < m_elementPorts.size() && i < m_result.weights.size(); ++i) {
            const cd cw = std::conj(m_result.weights[i]);
            if (m_elementPorts[i].first >= 0) coef[static_cast<size_t>(m_elementPorts[i].first)] = cw * r2;
            if (m_elementPorts[i].second >= 0) coef[static_cast<size_t>(m_elementPorts[i].second)] = cw * cd(0.0, -1.0) * r2;
        }
        double cmax = 0.0;
        for (const cd& c : coef) cmax = std::max(cmax, std::abs(c));
        for (const SourceDesc& s : m_sources) {
            if (s.port < 0 || s.port >= static_cast<int>(coef.size()) || cmax <= 0.0) continue;
            const cd c = coef[static_cast<size_t>(s.port)] / cmax;
            if (std::abs(c) < 1e-9) continue;
            SourceDesc t = s;
            t.amplitude = static_cast<float>(s.amplitude * std::abs(c));
            t.phase = static_cast<float>(s.phase + std::arg(c));
            active.push_back(t);
        }
    }
    grid.setSources(active);
    grid.resetFields();
    m_recorder.configure(grid, m_ports, 4.0f, std::nullopt, m_cellSize,
                         m_phase == Phase::Measuring ? m_passSteps : m_phase == Phase::Listening ? m_blindSteps : 0);
}

void CrpaMonitor::update(SimulationGrid& grid) {
    if (m_phase == Phase::Off) return;
    if (m_needsApply) {
        m_needsApply = false;
        startPass(grid);
        return;
    }
    if (m_phase == Phase::Measuring && m_recorder.recordedSteps() >= m_passSteps) {
        finishPass();
        ++m_pass;
        if (m_pass >= static_cast<int>(m_emitters.size())) {
            m_measured = true;
            computeWeights();
            m_phase = Phase::Adapted;
            writeLog();
        }
        startPass(grid);
    } else if (m_phase == Phase::Listening && m_recorder.recordedSteps() >= m_blindSteps) {
        finishBlind();
        m_measured = true;
        computeWeights();
        m_phase = Phase::Adapted;
        writeLog();
        startPass(grid);
    }
}

// The end of the blind run. Two separate things happen to the same recording:
//  - The receiver: each port is mixed down to complex baseband with the carrier,
//    x(t) = 2 LPF{ v(t) e^{-j w0 t} } (I/Q demodulation; a 4-period Blackman-Harris low-pass
//    removes the 2 w0 image), sampled snapshotsPerPeriod times per period over the window.
//    The 90-degree hybrid then forms each element's RHCP channel. Receiver noise is added
//    later, in computeWeights.
//  - The scorekeeper: a joint least-squares fit of every emitter's tone (each at its own
//    frequency) separates the emitters' contributions, for the SINR and null-depth figures
//    and the "measured" (calibrated) steering vector. The weights never see it.
void CrpaMonitor::finishBlind() {
    std::vector<glm::vec2> vi;
    m_recorder.readSeries(vi);
    const size_t P = m_ports.size(), E = m_emitters.size();
    const size_t N = P > 0 ? vi.size() / P : 0;
    const double lam0 = m_wavelength, w0 = glm::two_pi<double>() / lam0;
    const double ta = (m_setup.passPeriods + 2.0) * lam0, tb = ta + m_setup.snapshotPeriods * lam0;
    auto tAt = [&](size_t n) { return static_cast<double>(n + 1) * m_dt; };

    // Truth: v_p(t) = sum_e Re{V_pe e^{j w_e t}} over [ta, tb).
    const size_t B = 2 * E;
    std::vector<cd> G(B * B, cd(0.0));
    std::vector<std::vector<cd>> rhs(P, std::vector<cd>(B, cd(0.0)));
    std::vector<double> basis(B);
    auto fillBasis = [&](double t) {
        for (size_t e = 0; e < E; ++e) {
            const double w = glm::two_pi<double>() / m_emitterWavelength[e];
            basis[2 * e] = std::cos(w * t);
            basis[2 * e + 1] = std::sin(w * t);
        }
    };
    for (size_t n = 0; n < N; ++n) {
        const double t = tAt(n);
        if (t < ta || t >= tb) continue;
        fillBasis(t);
        for (size_t i = 0; i < B; ++i)
            for (size_t k = 0; k < B; ++k) G[i * B + k] += basis[i] * basis[k];
        for (size_t p = 0; p < P; ++p)
            for (size_t i = 0; i < B; ++i) rhs[p][i] += static_cast<double>(vi[n * P + p].x) * basis[i];
    }
    m_response.assign(E, std::vector<cd>(P, cd(0.0)));
    std::vector<std::vector<cd>> coef(P);
    for (size_t p = 0; p < P; ++p) {
        coef[p] = solve(G, rhs[p], B);
        for (size_t e = 0; e < E; ++e) m_response[e][p] = cd(coef[p][2 * e].real(), -coef[p][2 * e + 1].real());
    }
    // Fit residual (transients still ringing, numerical noise), relative to each emitter.
    double resid = 0.0;
    size_t count = 0;
    for (size_t n = 0; n < N; ++n) {
        const double t = tAt(n);
        if (t < ta || t >= tb) continue;
        fillBasis(t);
        for (size_t p = 0; p < P; ++p) {
            double f = 0.0;
            for (size_t i = 0; i < B; ++i) f += coef[p][i].real() * basis[i];
            const double r = static_cast<double>(vi[n * P + p].x) - f;
            resid += r * r;
        }
        ++count;
    }
    const double residPower = count > 0 && P > 0 ? 2.0 * resid / static_cast<double>(count * P) : 0.0; // as |phasor|^2
    m_settle.assign(E, 1.0);
    for (size_t e = 0; e < E; ++e) {
        const double pe = meanPower(m_response[e]);
        m_settle[e] = pe > 0.0 ? std::sqrt(residPower / pe) : 1.0;
    }

    // The receiver: I/Q downconversion and snapshots.
    const int S = std::max(m_setup.snapshotsPerPeriod, 1);
    const size_t K = static_cast<size_t>(std::lround(m_setup.snapshotPeriods * S));
    const double half = 2.0 * lam0;
    m_baseband.clear();
    m_baseband.reserve(K);
    std::vector<cd> x(P);
    for (size_t k = 0; k < K; ++k) {
        const double tk = ta + static_cast<double>(k) * lam0 / S;
        std::fill(x.begin(), x.end(), cd(0.0));
        double wsum = 0.0;
        const double n0 = std::max(0.0, std::ceil((tk - half) / m_dt - 1.0));
        for (size_t n = static_cast<size_t>(n0); n < N; ++n) {
            const double t = tAt(n);
            if (t > tk + half) break;
            const double u = (t - tk) / (2.0 * half) + 0.5, a = glm::two_pi<double>() * u;
            const double w = 0.35875 - 0.48829 * std::cos(a) + 0.14128 * std::cos(2.0 * a) - 0.01168 * std::cos(3.0 * a);
            const cd lo = std::polar(1.0, -w0 * t);
            for (size_t p = 0; p < P; ++p) x[p] += w * static_cast<double>(vi[n * P + p].x) * lo;
            wsum += w;
        }
        if (wsum > 0.0)
            for (cd& v : x) v *= 2.0 / wsum;
        m_baseband.push_back(channels(x));
    }
}

// The steady-state phasor of every port: a least-squares fit of A cos(wt) + B sin(wt) over the
// last four periods of the pass (exact for a pure tone, whatever the window length).
void CrpaMonitor::finishPass() {
    std::vector<glm::vec2> vi;
    m_recorder.readSeries(vi);
    const size_t P = m_ports.size();
    const size_t N = P > 0 ? vi.size() / P : 0;
    const double w = glm::two_pi<double>() / m_wavelength;
    const size_t W = std::min(N / 2, static_cast<size_t>(std::lround(4.0 * m_wavelength / m_dt)));
    auto fit = [&](size_t p, size_t end) {
        double scc = 0, sss = 0, scs = 0, svc = 0, svs = 0;
        for (size_t n = end - W; n < end; ++n) {
            const double t = static_cast<double>(n + 1) * m_dt;
            const double c = std::cos(w * t), s = std::sin(w * t), v = vi[n * P + p].x;
            scc += c * c;
            sss += s * s;
            scs += c * s;
            svc += v * c;
            svs += v * s;
        }
        const double det = scc * sss - scs * scs;
        if (std::abs(det) < 1e-30) return cd(0.0);
        const double A = (svc * sss - svs * scs) / det, B = (svs * scc - svc * scs) / det;
        return cd(A, -B); // v = Re{V e^{jwt}} with V = A - jB
    };
    std::vector<cd> phasors(P, cd(0.0));
    double change = 0.0, level = 0.0;
    for (size_t p = 0; p < P && W > 8; ++p) {
        phasors[p] = fit(p, N);
        const cd earlier = fit(p, N - W);
        change = std::max(change, std::abs(phasors[p] - earlier));
        level = std::max(level, std::abs(phasors[p]));
    }
    if (m_pass >= 0 && m_pass < static_cast<int>(m_response.size())) {
        m_response[static_cast<size_t>(m_pass)] = phasors;
        if (m_settle.size() != m_response.size()) m_settle.assign(m_response.size(), 0.0);
        m_settle[static_cast<size_t>(m_pass)] = level > 0.0 ? change / level : 1.0;
    }
}

// RHCP (or LHCP) channel of every element: the 90-degree hybrid, (X -/+ jY) / sqrt 2.
std::vector<cd> CrpaMonitor::channels(const std::vector<cd>& ports, bool lhcp) const {
    std::vector<cd> out;
    const cd j(0.0, lhcp ? 1.0 : -1.0);
    for (const auto& ep : m_elementPorts) {
        const cd x = ep.first >= 0 && static_cast<size_t>(ep.first) < ports.size() ? ports[static_cast<size_t>(ep.first)] : cd(0.0);
        const cd y = ep.second >= 0 && static_cast<size_t>(ep.second) < ports.size() ? ports[static_cast<size_t>(ep.second)] : cd(0.0);
        out.push_back((x + j * y) / std::sqrt(2.0));
    }
    return out;
}

void CrpaMonitor::computeWeights() {
    CrpaResult r;
    const size_t N = m_elementPorts.size(), E = m_emitters.size();
    if (N == 0 || m_response.size() != E) {
        m_result = r;
        return;
    }
    const std::vector<cd> sRaw = channels(m_response[static_cast<size_t>(m_satellite)]);
    std::vector<std::vector<cd>> jams;
    std::vector<size_t> jamIndex;
    for (size_t e = 0; e < E; ++e)
        if (static_cast<int>(e) != m_satellite) {
            jams.push_back(channels(m_response[e]));
            jamIndex.push_back(e);
        }
    // Powers: unit receiver noise per channel; the satellite at snrDb, the jammers at equal
    // transmit power with the strongest at jnrDb (the others weaker by their measured coupling).
    double strongest = 0.0;
    for (const auto& j : jams) strongest = std::max(strongest, meanPower(j));
    const double ps = meanPower(sRaw);
    double gj = strongest > 0.0 ? std::pow(10.0, m_setup.jnrDb / 10.0) / strongest : 0.0;
    double gs = ps > 0.0 ? std::pow(10.0, m_setup.snrDb / 10.0) / ps : 0.0;
    const bool blind = m_setup.blind && !m_baseband.empty();
    if (blind) {
        // Everything was received together at its transmitted power: one scale, the receiver
        // noise, set so the strongest jammer is at jnrDb (the satellite then lands near snrDb).
        const double sigma2 = strongest > 0.0 ? strongest / std::pow(10.0, m_setup.jnrDb / 10.0)
                                              : ps / std::pow(10.0, m_setup.snrDb / 10.0);
        gj = gs = sigma2 > 0.0 ? 1.0 / sigma2 : 0.0;
    }

    std::vector<cd> Rin(N * N, cd(0.0));
    for (size_t i = 0; i < N; ++i) Rin[i * N + i] = 1.0;
    for (const auto& j : jams)
        for (size_t a = 0; a < N; ++a)
            for (size_t b = 0; b < N; ++b) Rin[a * N + b] += gj * j[a] * std::conj(j[b]);
    std::vector<cd> R = Rin;
    for (size_t a = 0; a < N; ++a)
        for (size_t b = 0; b < N; ++b) R[a * N + b] += gs * sRaw[a] * std::conj(sRaw[b]);
    if (blind) {
        // The receiver's estimate: R = (1/K) sum x_k x_k^H over its snapshots, each with its own
        // thermal noise (complex Gaussian, unit power per channel).
        std::mt19937 rng(m_setup.noiseSeed);
        std::normal_distribution<double> gauss(0.0, std::sqrt(0.5));
        std::fill(R.begin(), R.end(), cd(0.0));
        const double amp = std::sqrt(gs);
        std::vector<cd> xk(N);
        for (const auto& snap : m_baseband) {
            for (size_t i = 0; i < N; ++i) xk[i] = amp * snap[i] + cd(gauss(rng), gauss(rng));
            for (size_t a = 0; a < N; ++a)
                for (size_t b = 0; b < N; ++b) R[a * N + b] += xk[a] * std::conj(xk[b]);
        }
        for (cd& v : R) v /= static_cast<double>(m_baseband.size());
        r.blind = true;
        r.snapshots = m_baseband.size();
        r.snapshotUs = m_setup.snapshotPeriods * m_wavelength * m_cellSize / 299792458.0 * 1e6;
    }

    // Steering vector toward the satellite: from its known direction (plane-wave model of the
    // array), or its measured response (a calibrated array).
    std::vector<cd> a(N);
    const glm::vec3 u = glm::normalize(m_emitters[static_cast<size_t>(m_satellite)].position - m_center);
    const double k = glm::two_pi<double>() / m_wavelength;
    for (size_t i = 0; i < N; ++i)
        a[i] = m_setup.measuredSteering ? sRaw[i]
                                        : std::polar(1.0, k * glm::dot(m_setup.elements[i].center - m_center, u));

    const std::vector<cd> Ria = solve(R, a, N);
    const cd denom = dotH(a, Ria);
    std::vector<cd> wMvdr(N), wSteer(N);
    const double aa = std::real(dotH(a, a));
    for (size_t i = 0; i < N; ++i) {
        wMvdr[i] = std::abs(denom) > 1e-300 ? Ria[i] / denom : cd(0.0);
        wSteer[i] = a[i] / aa;
    }
    const std::vector<cd>& w = m_setup.mvdr ? wMvdr : wSteer;
    r.weights = w;

    auto quad = [&](const std::vector<cd>& x, const std::vector<cd>& M) { // x^H M x
        cd s(0.0);
        for (size_t p = 0; p < N; ++p)
            for (size_t q = 0; q < N; ++q) s += std::conj(x[p]) * M[p * N + q] * x[q];
        return std::real(s);
    };
    auto sinr = [&](const std::vector<cd>& ww) { return gs * std::norm(dotH(ww, sRaw)) / std::max(quad(ww, Rin), 1e-300); };
    double single = 0.0;
    for (size_t i = 0; i < N; ++i) {
        double interf = 1.0;
        for (const auto& j : jams) interf += gj * std::norm(j[i]);
        single += gs * std::norm(sRaw[i]) / interf;
    }
    r.sinrSingleDb = toDb(single / static_cast<double>(N));
    r.sinrSteeringDb = toDb(sinr(wSteer));
    r.sinrMvdrDb = toDb(sinr(w));
    const double wn = std::real(dotH(w, w)), wsn = std::real(dotH(wSteer, wSteer));
    r.satelliteGainDb = toDb(gs * std::norm(dotH(w, sRaw)) / wn) - toDb(gs * ps);

    for (size_t e = 0; e < E; ++e) {
        CrpaEmitterResult er;
        er.name = m_emitters[e].name;
        er.satellite = static_cast<int>(e) == m_satellite;
        const glm::vec2 ang = compassAngles(m_emitters[e].position);
        er.azDeg = ang.x;
        er.elDeg = ang.y;
        er.distance = glm::length(m_emitters[e].position - m_center);
        const std::vector<cd> v = channels(m_response[e]);
        const double g = er.satellite ? gs : gj;
        er.inputDb = toDb(g * meanPower(v));
        er.outputDb = toDb(g * std::norm(dotH(w, v)) / wn);
        er.steeringOutDb = toDb(g * std::norm(dotH(wSteer, v)) / wsn);
        er.crossPolDb = toDb(meanPower(channels(m_response[e], true))) - toDb(meanPower(v));
        er.settleDb = e < m_settle.size() ? 20.0 * std::log10(std::max(m_settle[e], 1e-12)) : 0.0;
        er.offsetPct = 100.0 * emitterOffset(static_cast<int>(e));
        r.emitters.push_back(er);
    }

    // Model sky maps (what the processor's array model predicts): ideal elements with a cos
    // power pattern, north up and east right, zenith in the centre.
    r.mapSize = kMapSize;
    r.mapDb.assign(static_cast<size_t>(kMapSize * kMapSize), -300.0f);
    r.steeringMapDb = r.mapDb;
    const glm::vec3 up(0.0f, 0.0f, 1.0f), north = m_setup.north, east = glm::cross(north, up);
    std::vector<double> pa(r.mapDb.size(), -1.0), ps2(r.mapDb.size(), -1.0);
    double peak = 0.0;
    for (int row = 0; row < kMapSize; ++row)
        for (int col = 0; col < kMapSize; ++col) {
            const double ue = -1.0 + 2.0 * col / (kMapSize - 1), vn = 1.0 - 2.0 * row / (kMapSize - 1);
            const double rho2 = ue * ue + vn * vn;
            if (rho2 > 1.0) continue;
            const double wz = std::sqrt(1.0 - rho2);
            const glm::vec3 d = static_cast<float>(ue) * east + static_cast<float>(vn) * north + static_cast<float>(wz) * up;
            std::vector<cd> m(N);
            for (size_t i = 0; i < N; ++i) m[i] = std::polar(1.0, k * glm::dot(m_setup.elements[i].center - m_center, d));
            const size_t idx = static_cast<size_t>(row * kMapSize + col);
            pa[idx] = std::norm(dotH(w, m)) / wn * wz;
            ps2[idx] = std::norm(dotH(wSteer, m)) / wsn * wz;
            peak = std::max(peak, ps2[idx]);
        }
    for (size_t i = 0; i < pa.size(); ++i) {
        if (pa[i] < 0.0) continue;
        r.mapDb[i] = static_cast<float>(toDb(pa[i] / peak));
        r.steeringMapDb[i] = static_cast<float>(toDb(ps2[i] / peak));
    }
    r.valid = true;
    m_result = std::move(r);
}

void CrpaMonitor::writeLog() const {
    if (m_setup.logPath.empty() || !m_result.valid) return;
    std::ofstream f(m_setup.logPath, std::ios::trunc);
    if (!f) return;
    f << "emitter,kind,azimuth_deg,elevation_deg,distance_cells,input_db,output_db,steering_output_db,crosspol_db\n";
    for (const CrpaEmitterResult& e : m_result.emitters)
        f << e.name << ',' << (e.satellite ? "satellite" : "jammer") << ',' << e.azDeg << ',' << e.elDeg << ','
          << e.distance << ',' << e.inputDb << ',' << e.outputDb << ',' << e.steeringOutDb << ',' << e.crossPolDb << '\n';
    f << "\nelement,weight_mag,weight_phase_deg\n";
    for (size_t i = 0; i < m_result.weights.size(); ++i)
        f << i + 1 << ',' << std::abs(m_result.weights[i]) << ',' << glm::degrees(std::arg(m_result.weights[i])) << '\n';
    f << "\nsinr_single_db," << m_result.sinrSingleDb << "\nsinr_steering_db," << m_result.sinrSteeringDb
      << "\nsinr_mvdr_db," << m_result.sinrMvdrDb << '\n';
}
