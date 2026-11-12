#include "Instruments.hpp"
#include "SimulationGrid.hpp"

#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace fs = std::filesystem;

namespace {
constexpr double kEta0 = 376.730313668;
} // namespace

Instruments::Instruments(const fs::path& shaderDir)
    : m_analyzer(shaderDir), m_farField(shaderDir), m_ports(shaderDir), m_radar(shaderDir), m_crpa(shaderDir), m_coax(shaderDir) {}

std::unique_ptr<std::ofstream> Instruments::openLog(const std::string& path, const char* header,
                                                    std::vector<std::string>& warnings) const {
    if (path.empty()) return nullptr;
    fs::path p(path);
    if (p.is_relative()) p = m_outputDir / p;
    std::error_code ec;
    if (p.has_parent_path()) fs::create_directories(p.parent_path(), ec);
    auto f = std::make_unique<std::ofstream>(p, std::ios::trunc);
    if (!*f) {
        warnings.push_back("cannot open log file '" + p.string() + "'");
        return nullptr;
    }
    *f << header << '\n';
    return f;
}

void Instruments::configure(const SimulationGrid& grid, const Environment& env, const fs::path& outputDir,
                            std::vector<std::string>& warnings, RadarMonitor::Start radarStart) {
    m_outputDir = outputDir;
    m_analyzer.discard(); // measurements in flight belong to the previous configuration
    m_averagingTime = 3.0 * std::max(env.referenceWavelength, 2.0f);
    m_unstable = false;

    m_probes.clear();
    for (const ProbeSetup& p : env.probes) {
        Probe probe;
        probe.setup = p;
        probe.setup.position = glm::clamp(p.position, glm::vec3(0.0f), glm::vec3(grid.size() - 1));
        probe.log = openLog(p.logPath, "step,time,Ex,Ey,Ez,Bx,By,Bz,absE,absB,Sx,Sy,Sz", warnings);
        m_probes.push_back(std::move(probe));
    }
    m_selected = 0;

    m_fluxes.clear();
    for (const FluxSetup& f : env.fluxes) {
        Flux flux;
        flux.setup = f;
        flux.log = openLog(f.logPath, "step,time,P,P_avg", warnings);
        m_fluxes.push_back(std::move(flux));
    }

    m_wires = env.wires;
    m_sheets = env.sheets;
    m_ports.disable();
    if (!env.ports.empty()) {
        std::optional<SweepSetup> sweep = env.sweep;
        if (sweep && !sweep->logPath.empty()) {
            fs::path p(sweep->logPath);
            if (p.is_relative()) p = m_outputDir / p;
            std::error_code ec;
            if (p.has_parent_path()) fs::create_directories(p.parent_path(), ec);
            sweep->logPath = p.string();
        }
        m_ports.configure(grid, env.ports, env.farField ? env.farField->averagingPeriods : 4.0f, sweep, env.cellSize);
        for (const PortSetup& p : env.ports)
            if (std::abs(p.wavelength - env.ports.front().wavelength) > 1e-3f) {
                warnings.push_back("ports drive different frequencies; impedances are evaluated at the first "
                                   "port's wavelength");
                break;
            }
    }

    if (env.radar && !env.ports.empty()) {
        RadarSetup r = *env.radar;
        if (!r.logPath.empty()) {
            fs::path p(r.logPath);
            if (p.is_relative()) p = m_outputDir / p;
            std::error_code ec;
            if (p.has_parent_path()) fs::create_directories(p.parent_path(), ec);
            r.logPath = p.string();
        }
        m_radar.configure(grid, r, env.ports, env.sources, env.ports.front().wavelength, env.cellSize, env.targets,
                          radarStart, warnings);
    } else {
        if (env.radar) warnings.push_back("radar: the scene has no antenna feeds to transmit and receive with");
        m_radar.disable();
    }

    if (env.crpa && !env.ports.empty()) {
        CrpaSetup c = *env.crpa;
        if (!c.logPath.empty()) {
            fs::path p(c.logPath);
            if (p.is_relative()) p = m_outputDir / p;
            std::error_code ec;
            if (p.has_parent_path()) fs::create_directories(p.parent_path(), ec);
            c.logPath = p.string();
        }
        m_crpa.configure(grid, c, env.emitters, env.sources, env.ports, env.ports.front().wavelength, env.cellSize,
                         m_sceneKept, warnings);
    } else {
        m_crpa.disable();
    }

    if (!env.coaxes.empty()) m_coax.configure(grid, env.coaxes);
    else m_coax.disable();

    // Every load, including scenes without feeds: otherwise a previous scene's array layout
    // would keep its window open.
    detectArrayLayout();

    m_farField.disable();
    if (env.farField) {
        try {
            FarFieldSetup setup = *env.farField;
            if (!setup.logPath.empty()) {
                fs::path p(setup.logPath);
                if (p.is_relative()) p = m_outputDir / p;
                std::error_code ec;
                if (p.has_parent_path()) fs::create_directories(p.parent_path(), ec);
                if (ec) warnings.push_back("cannot create directory for far-field log '" + p.string() + "'");
                setup.logPath = p.string();
            }
            m_farField.configure(grid, setup);
        } catch (const std::exception& e) {
            warnings.push_back(std::string("far_field disabled: ") + e.what());
        }
    }
}

// Ports whose feeds share an axis and sit on a full rectangular lattice across the other two
// axes form an array: the UI then shows a per-element map (active VSWR and friends).
void Instruments::detectArrayLayout() {
    m_arrayRows = m_arrayCols = 0;
    m_arrayIndex.clear();
    const auto& ports = m_ports.ports();
    if (ports.size() < 4) return;
    const int ax = ports.front().axis;
    for (const PortSetup& p : ports)
        if (p.axis != ax) return;
    // Lay the elements out as seen from behind the array looking out along its feeds:
    // columns run to the right, rows up (see boresightFrame).
    glm::vec3 n(0.0f), right, up;
    n[ax] = ports.front().sign >= 0.0f ? 1.0f : -1.0f;
    boresightFrame(n, right, up);
    std::vector<int> us, vs;
    for (const PortSetup& p : ports) {
        us.push_back(static_cast<int>(std::lround(glm::dot(glm::vec3(p.texel), right))));
        vs.push_back(static_cast<int>(std::lround(glm::dot(glm::vec3(p.texel), up))));
    }
    auto unique = [](std::vector<int> a) {
        std::sort(a.begin(), a.end());
        a.erase(std::unique(a.begin(), a.end()), a.end());
        return a;
    };
    const std::vector<int> cu = unique(us), cv = unique(vs);
    if (cu.size() < 2 || cv.size() < 2 || cu.size() * cv.size() != ports.size()) return;
    m_arrayCols = static_cast<int>(cu.size());
    m_arrayRows = static_cast<int>(cv.size());
    m_arrayIndex.assign(ports.size(), -1);
    for (size_t i = 0; i < ports.size(); ++i) {
        const int c = static_cast<int>(std::lower_bound(cu.begin(), cu.end(), us[i]) - cu.begin());
        const int r = m_arrayRows - 1 - static_cast<int>(std::lower_bound(cv.begin(), cv.end(), vs[i]) - cv.begin());
        int& slot = m_arrayIndex[static_cast<size_t>(r * m_arrayCols + c)];
        if (slot >= 0) { // two ports on one lattice site: not a simple array
            m_arrayRows = m_arrayCols = 0;
            m_arrayIndex.clear();
            return;
        }
        slot = static_cast<int>(i);
    }
    auto name = [](const glm::vec3& d) {
        int i = 0;
        for (int k = 1; k < 3; ++k)
            if (std::abs(d[k]) > std::abs(d[i])) i = k;
        return std::string(d[i] >= 0.0f ? "+" : "-") + "xyz"[i];
    };
    m_arrayAxes[0] = name(right);
    m_arrayAxes[1] = name(up);
}

void Instruments::onStep(const SimulationGrid& grid) {
    m_farField.accumulate(grid);
    m_ports.accumulate(grid);
    m_radar.accumulate(grid);
    m_crpa.accumulate(grid);
    m_coax.accumulate(grid);
}

void Instruments::evaluatePorts() { m_ports.evaluate(); }

void Instruments::evaluateSweep(const SimulationGrid& grid) { m_ports.evaluateSweep(grid.dt()); }

void Instruments::resetHistory(const SimulationGrid& grid) {
    for (Flux& f : m_fluxes) {
        f.reading = {};
        f.lastTime = -1.0;
    }
    if (m_farField.enabled()) m_farField.configure(grid, m_farField.setup());
    if (m_ports.enabled()) {
        const std::vector<PortSetup> ports = m_ports.ports();
        const std::optional<SweepSetup> sweep = m_ports.sweepSetup();
        m_ports.configure(grid, ports, m_farField.enabled() ? m_farField.setup().averagingPeriods : 4.0f, sweep,
                          m_ports.cellSize());
    }
    m_radar.restart(grid);
    m_crpa.restart(grid);
    if (m_coax.enabled()) m_coax.restart(grid);
    m_unstable = false;
}

void Instruments::measure(const SimulationGrid& grid, bool wait) {
    if (m_probes.empty() && m_fluxes.empty()) {
        // Nothing to read back: the readouts' time stamp is simply now.
        m_measureTime = grid.time();
        m_measureStep = grid.stepCount();
        return;
    }
    MeasureRequest req;
    req.time = grid.time();
    req.step = grid.stepCount();
    for (const Probe& p : m_probes) req.probes.push_back(p.setup.position);
    for (const Flux& f : m_fluxes) req.fluxes.push_back(f.setup.surface);
    m_analyzer.submit(grid, req);
    MeasureResult r;
    while (m_analyzer.collect(r, wait)) applyMeasurement(r);
}

void Instruments::applyMeasurement(const MeasureResult& r) {
    if (r.probes.size() != m_probes.size() || r.fluxes.size() != m_fluxes.size()) return; // scene changed meanwhile
    m_measureTime = r.time;
    m_measureStep = r.step;
    bool finite = true;

    for (size_t i = 0; i < m_probes.size(); ++i) {
        Probe& p = m_probes[i];
        p.sample = r.probes[i];
        const ProbeSample& s = p.sample;
        finite = finite && std::isfinite(glm::dot(s.E, s.E)) && std::isfinite(glm::dot(s.B, s.B));
        if (p.log)
            *p.log << m_measureStep << ',' << m_measureTime << ',' << s.E.x << ',' << s.E.y << ',' << s.E.z << ','
                   << s.B.x << ',' << s.B.y << ',' << s.B.z << ',' << s.eMagnitude() << ',' << s.bMagnitude() << ','
                   << s.S.x << ',' << s.S.y << ',' << s.S.z << '\n';
    }

    for (size_t i = 0; i < m_fluxes.size(); ++i) {
        Flux& f = m_fluxes[i];
        const double P = r.fluxes[i];
        finite = finite && std::isfinite(P);
        FluxReading& rd = f.reading;
        if (f.lastTime < 0.0 || m_measureTime < f.lastTime) {
            rd = {};
            rd.timeAveraged = P;
        } else {
            const double a = 1.0 - std::exp(-(m_measureTime - f.lastTime) / m_averagingTime);
            rd.timeAveraged += a * (P - rd.timeAveraged);
        }
        rd.instantaneous = P;
        rd.peakAbs = std::max(rd.peakAbs, std::abs(P));
        f.lastTime = m_measureTime;
        if (f.log) *f.log << m_measureStep << ',' << m_measureTime << ',' << P << ',' << rd.timeAveraged << '\n';
    }

    if (!finite && !m_unstable) {
        m_unstable = true;
        std::fprintf(stderr, "ERROR: non-finite field values detected at step %llu\n",
                     static_cast<unsigned long long>(m_measureStep));
    }
}

void Instruments::evaluateFarField(const SimulationGrid& grid) {
    if (m_farField.enabled()) m_farField.evaluate(grid);
    m_ports.evaluate();
}

void Instruments::beginFarFieldEvaluation() {
    if (m_farField.enabled()) m_farField.beginEvaluation();
    m_ports.beginEvaluate();
}

void Instruments::printAllPorts(const SimulationGrid& grid) const { printPorts(grid, m_ports.ports().size()); }

void Instruments::printPorts(const SimulationGrid& grid, size_t maxLines) const {
    if (!m_ports.enabled()) return;
    const auto& ports = m_ports.ports();
    const auto& rd = m_ports.readings();
    const double periods = m_ports.accumulatedPeriods(grid);
    double accepted = 0.0, available = 0.0;
    for (size_t i = 0; i < ports.size(); ++i) {
        accepted += rd[i].acceptedPower;
        available += rd[i].availablePower;
    }
    // A frequency sweep drives the ports with a pulse: the single-frequency (continuous-wave)
    // readings are meaningless then, so only the sweep summary below is reported.
    const size_t cwLines = (m_ports.sweeping() || m_radar.enabled() || m_crpa.enabled()) ? 0 : maxLines;
    if (periods < 12.0 && cwLines > 0)
        std::printf("    ports: averaging (%.0f of ~12 periods), impedances still settling\n", periods);
    for (size_t i = 0; i < ports.size() && i < cwLines; ++i) {
        const PortSetup& p = ports[i];
        const PortReading& r = rd[i];
        if (!r.valid) {
            std::printf("    port  %-14s (no current yet)\n", p.name.c_str());
            continue;
        }
        if (p.voltageFeed)
            std::printf("    port  %-14s Z = %7.1f %+7.1fj ohm | Gamma %.3f @ %+5.0f deg (S11 %6.1f dB), VSWR %5.2f vs %.0f ohm "
                        "| P_acc %.3e of %.3e avail (%.0f%%)\n",
                        p.name.c_str(), r.zOhm.real(), r.zOhm.imag(), std::abs(r.gamma),
                        std::arg(r.gamma) * 57.29577951308232, -r.returnLossDb, r.vswr, p.impedanceOhm, r.acceptedPower,
                        r.availablePower, r.availablePower > 0.0 ? 100.0 * r.acceptedPower / r.availablePower : 0.0);
        else
            std::printf("    port  %-14s Z = %7.1f %+7.1fj ohm | current feed %.3g | P_acc %.3e\n", p.name.c_str(),
                        r.zOhm.real(), r.zOhm.imag(), static_cast<double>(p.drive), r.acceptedPower);
    }
    if (cwLines > 0 && ports.size() > maxLines)
        std::printf("    ... %zu more ports (press P for all) | total P_acc %.3e of %.3e available\n",
                    ports.size() - maxLines, accepted, available);

    // Frequency sweep summary.
    if (m_ports.sweeping()) {
        const SweepResult& sw = m_ports.sweep();
        const double cell = m_ports.cellSize();
        auto ghz = [cell](double wl) { return 299792458.0 / (wl * cell) * 1e-9; };
        if (!sw.valid) {
            std::printf("    sweep: recording the pulse response...\n");
        } else {
            std::printf("    sweep: %zu steps recorded, ring-down %.1e of peak%s\n", sw.samples, sw.residual,
                        sw.residual > 1e-3 ? (sw.full ? " (buffer full: results approximate)" : " (still ringing: results settling)") : " (settled)");
            for (size_t i = 0; i < sw.ports.size() && i < maxLines; ++i) {
                const PortSweep& ps = sw.ports[i];
                const SweepPoint& b = ps.points[ps.best];
                std::printf("    sweep %-12s best match %.4f GHz (lambda %.2f cells): S11 %.1f dB, VSWR %.2f, Z = %.1f %+.1fj ohm",
                            ports[i].name.c_str(), ghz(ps.bestWavelength), ps.bestWavelength, b.s11Db, b.vswr,
                            b.zOhm.real(), b.zOhm.imag());
                if (ps.hasBand) {
                    const double f0 = ghz(ps.bestWavelength), fl = ghz(ps.bandLowWavelength), fh = ghz(ps.bandHighWavelength);
                    std::printf(" | -10 dB band %.4f-%.4f GHz (%.1f%%)", fl, fh, 100.0 * (fh - fl) / f0);
                } else {
                    std::printf(" | never below -10 dB in the sweep");
                }
                std::printf("\n");
            }
        }
    }

    // Radiation resistance and efficiency from the far-field power.
    if (m_farField.enabled() && m_farField.pattern().valid && accepted > 0.0) {
        const double prad = m_farField.pattern().radiatedPower;
        if (ports.size() == 1 && rd[0].valid) {
            const double i2 = std::norm(rd[0].I);
            std::printf("    radiation resistance R_rad = 2 P_rad / |I|^2 = %.1f ohm, radiation efficiency P_rad / P_acc "
                        "= %.0f%%\n", 2.0 * prad / i2 * kEta0, 100.0 * prad / accepted);
        } else {
            std::printf("    radiation efficiency P_rad / total P_acc = %.0f%%\n", 100.0 * prad / accepted);
        }
    }
}

void Instruments::printCrpa() const {
    if (!m_crpa.enabled()) return;
    if (m_crpa.phase() == CrpaMonitor::Phase::Measuring) {
        const auto& em = m_crpa.emitters();
        const int p = m_crpa.pass();
        std::printf("    crpa: measuring %s at the array (pass %d of %zu, %.0f%%)\n",
                    p < static_cast<int>(em.size()) ? em[static_cast<size_t>(p)].name.c_str() : "?", p + 1, em.size(),
                    100.0 * m_crpa.passProgress());
        return;
    }
    if (m_crpa.phase() == CrpaMonitor::Phase::Listening) {
        std::printf("    crpa: listening, all %zu emitters on (%.0f%%)\n", m_crpa.emitters().size(),
                    100.0 * m_crpa.passProgress());
        return;
    }
    const CrpaResult& r = m_crpa.result();
    if (!r.valid) return;
    if (r.blind)
        std::printf("    crpa: blind, covariance from %zu snapshots (%.3f us)\n", r.snapshots, r.snapshotUs);
    std::printf("    crpa (%s weights): SINR single element %.1f dB, steering %.1f dB, applied %.1f dB\n",
                m_crpa.setup().mvdr ? "MVDR" : "steering", r.sinrSingleDb, r.sinrSteeringDb, r.sinrMvdrDb);
    for (const CrpaEmitterResult& e : r.emitters)
        std::printf("      %-12s az %5.1f el %5.1f  f %+4.1f%%  in %+6.1f dB  out %+6.1f dB  (steering only %+6.1f dB)  cross-pol %+.1f dB  settled %.0f dB\n",
                    e.name.c_str(), e.azDeg, e.elDeg, e.offsetPct, e.inputDb, e.outputDb, e.steeringOutDb, e.crossPolDb,
                    e.settleDb);
}

void Instruments::printCoax(const SimulationGrid& grid) const {
    if (!m_coax.enabled()) return;
    const double periods = m_coax.periods(grid);
    for (size_t i = 0; i < m_coax.lines().size(); ++i) {
        const CoaxSetup& c = m_coax.lines()[i];
        const CoaxReading& r = m_coax.readings()[i];
        std::printf("    %s: Z0 %.1f ohm (eps_r %.2f, velocity factor %.2f), %s end, %.0f periods\n", c.name.c_str(),
                    c.z0Ohm, c.epsilonR, 1.0 / std::sqrt(c.epsilonR), c.termination.c_str(), periods);
        if (!r.valid) continue;
        std::printf("      waves: Z0 measured %.1f ohm, end Gamma %.3f @ %+.0f deg (RL %.1f dB), VSWR %.2f, Z_end %.1f %+.1fj ohm\n",
                    r.z0Measured, std::abs(r.gammaEnd), glm::degrees(std::arg(r.gammaEnd)), r.returnLossDb, r.vswr,
                    r.zEnd.real(), r.zEnd.imag());
        std::printf("      forward %.0f%% of available, delivered %.0f%% of forward; shield outside current %.1f dB "
                    "vs line current; fit residual %.1e\n",
                    100.0 * r.forwardFraction, 100.0 * (1.0 - std::norm(r.gammaEnd)), std::max(r.commonModeDb, -200.0),
                    r.fitResidual);
    }
}

void Instruments::printRadar() const {
    if (!m_radar.enabled()) return;
    const RadarImage& img = m_radar.image();
    switch (m_radar.phase()) {
        case RadarMonitor::Phase::Calibrating:
            std::printf("    radar: calibrating without the targets (%.0f%% of %zu steps)\n", 100.0 * m_radar.progress(),
                        m_radar.recordSteps());
            return;
        case RadarMonitor::Phase::Measuring:
            std::printf("    radar: measuring (%.0f%% of %zu steps), image final out to %.1f mm\n", 100.0 * m_radar.progress(),
                        m_radar.recordSteps(), img.valid ? img.completeRange * m_radar.cellSize() * 1e3 : 0.0);
            if (!img.valid) return;
            break;
        default: break;
    }
    if (!img.valid) {
        std::printf("    radar: no image\n");
        return;
    }
    const double cell = m_radar.cellSize() * 1e3;
    std::printf("    radar: strongest echo %.1f dB below the transmit pulse at an element, image floor %.1f dB, "
                "%zu detection(s)\n", -img.echoDb, img.floorDb, img.detections.size());
    for (size_t k = 0; k < img.detections.size(); ++k) {
        const RadarDetection& d = img.detections[k];
        std::printf("      #%zu range %6.1f mm (%5.2f lambda)  az %+5.1f  el %+5.1f deg  %+6.1f dB (%.1f dB above floor)",
                    k + 1, d.range * cell, d.range / m_radar.wavelength(), d.azDeg, d.elDeg, d.relDb, d.snrDb);
        if (d.target >= 0)
            std::printf(" | %s: front at %.1f mm, az %+.1f, el %+.1f", m_radar.targets()[static_cast<size_t>(d.target)].name.c_str(),
                        d.targetRange * cell, d.targetAzDeg, d.targetElDeg);
        std::printf("\n");
    }
}

void Instruments::printReport(const SimulationGrid& grid) const {
    if (empty()) {
        std::printf("[t=%8.1f step %7llu] %s\n", grid.time(), static_cast<unsigned long long>(grid.stepCount()),
                    kNoInstrumentsHint);
        return;
    }
    std::printf("[t=%8.1f step %7llu]\n", m_measureTime, static_cast<unsigned long long>(m_measureStep));
    for (size_t i = 0; i < m_probes.size(); ++i) {
        const ProbeSample& p = m_probes[i].sample;
        std::printf("  %c probe %-14s (%6.1f %6.1f %6.1f): |E|=%.5f E=(%+.4f %+.4f %+.4f) |B|=%.5f "
                    "S=(%+.2e %+.2e %+.2e) sigma=%.3g eps_r=%.2f\n",
                    i == m_selected ? '>' : ' ', m_probes[i].setup.name.c_str(), p.position.x, p.position.y,
                    p.position.z, p.eMagnitude(), p.E.x, p.E.y, p.E.z, p.bMagnitude(), p.S.x, p.S.y, p.S.z, p.sigma,
                    p.epsilonR);
    }
    for (const Flux& f : m_fluxes)
        std::printf("    flux  %-28s P=%+.4e  <P>=%+.4e  peak|P|=%.4e\n", f.setup.surface.label.c_str(),
                    f.reading.instantaneous, f.reading.timeAveraged, f.reading.peakAbs);

    printPorts(grid, 4);
    printRadar();
    printCrpa();
    printCoax(grid);

    if (m_farField.enabled()) {
        const FarFieldPattern& pat = m_farField.pattern();
        const double settle = m_farField.accumulatedTime(grid) / std::max(m_farField.setup().wavelength, 1.0f);
        if (!pat.valid) {
            std::printf("    far field: accumulating (%.1f periods)...\n", settle);
        } else {
            const glm::vec3 d = pat.maxDirection;
            const float theta = glm::degrees(std::acos(std::clamp(d.z, -1.0f, 1.0f)));
            const float phi = glm::degrees(std::atan2(d.y, d.x));
            std::printf("    far field [lambda=%.1f cells, %.0f periods averaged%s]: P_rad=%.4e  D_max=%.2f dBi "
                        "(%.2f) @ theta=%.0f phi=%.0f\n",
                        m_farField.setup().wavelength, settle, settle < 3.0 * m_farField.setup().averagingPeriods
                                                                  ? ", still settling" : "",
                        pat.radiatedPower, pat.maxDirectivityDbi, std::pow(10.0, pat.maxDirectivityDbi / 10.0), theta,
                        phi);
            for (const PatternCut& c : pat.cuts) {
                char bw[32];
                if (c.beamwidthDeg >= 359.0f) std::snprintf(bw, sizeof(bw), "omni");
                else std::snprintf(bw, sizeof(bw), "%.1f deg", c.beamwidthDeg);
                std::printf("      %s-cut: peak %6.2f dBi @ %5.1f deg, HPBW %s, front/back %.1f dB\n", c.name.c_str(),
                            c.peakDbi, c.peakAngleDeg, bw, c.frontToBackDb);
            }
        }
    }
}

void Instruments::selectNextProbe() {
    if (!m_probes.empty()) m_selected = (m_selected + 1) % m_probes.size();
}

void Instruments::moveSelectedProbe(const glm::vec3& delta, const glm::ivec3& gridSize) {
    if (m_probes.empty()) return;
    glm::vec3& p = m_probes[m_selected].setup.position;
    p = glm::clamp(p + delta, glm::vec3(0.0f), glm::vec3(gridSize - 1));
}

void Instruments::appendOverlay(std::vector<LineVertex>& out, const glm::ivec3& g) const {
    auto W = [&g](const glm::vec3& p) { return VolumeRenderer::latticeToWorld(g, p); };
    const float voxel = VolumeRenderer::worldScale(g);

    // Thin wires are sub-cell conductors (not voxels): draw them as bright metal lines.
    const glm::vec4 wireColor{0.85f, 0.88f, 0.95f, 0.95f};
    for (const WireSegment& w : m_wires) {
        out.push_back({W(w.a), wireColor});
        out.push_back({W(w.b), wireColor});
    }
    // Zero-thickness sheets: outline plus a light hatch so the surface reads as metal.
    const glm::vec4 sheetColor{0.85f, 0.75f, 0.45f, 0.9f}, hatchColor{0.85f, 0.75f, 0.45f, 0.35f};
    for (const SheetRect& s : m_sheets) {
        int n = 0;
        for (int i = 0; i < 3; ++i)
            if (s.a[i] == s.b[i]) n = i;
        const int u = (n + 1) % 3, v = (n + 2) % 3;
        auto corner = [&](float cu, float cv) { glm::vec3 p = s.a; p[u] = cu; p[v] = cv; return W(p); };
        const glm::vec3 q[4] = {corner(s.a[u], s.a[v]), corner(s.b[u], s.a[v]), corner(s.b[u], s.b[v]), corner(s.a[u], s.b[v])};
        for (int i = 0; i < 4; ++i) {
            out.push_back({q[i], sheetColor});
            out.push_back({q[(i + 1) % 4], sheetColor});
        }
        const float spanU = s.b[u] - s.a[u];
        const int hatches = std::max(1, static_cast<int>(spanU / 3.0f));
        for (int h = 1; h < hatches; ++h) {
            const float cu = s.a[u] + spanU * static_cast<float>(h) / static_cast<float>(hatches);
            out.push_back({corner(cu, s.a[v]), hatchColor});
            out.push_back({corner(cu, s.b[v]), hatchColor});
        }
    }

    for (size_t i = 0; i < m_probes.size(); ++i) {
        const bool sel = i == m_selected;
        VolumeRenderer::appendCross(out, W(m_probes[i].setup.position), (sel ? 5.0f : 3.0f) * voxel,
                                    sel ? glm::vec4(1.0f, 0.95f, 0.2f, 0.95f) : glm::vec4(1.0f, 0.75f, 0.2f, 0.5f));
    }

    const glm::vec4 fluxColor{0.25f, 1.0f, 0.45f, 0.55f};
    for (const Flux& f : m_fluxes) {
        const FluxSurface& s = f.setup.surface;
        if (s.kind == FluxSurface::Kind::Box) {
            VolumeRenderer::appendBox(out, W(glm::vec3(s.boxMin)), W(glm::vec3(s.boxMax)), fluxColor);
            continue;
        }
        const int u = s.axis == 0 ? 1 : 0;
        const int v = s.axis == 2 ? 1 : 2;
        const glm::vec2 c[4] = {{s.rectMin.x, s.rectMin.y}, {s.rectMax.x, s.rectMin.y},
                                {s.rectMax.x, s.rectMax.y}, {s.rectMin.x, s.rectMax.y}};
        glm::vec3 q[4];
        for (int i = 0; i < 4; ++i) {
            glm::vec3 p(0.0f);
            p[s.axis] = static_cast<float>(s.index);
            p[u] = c[i].x;
            p[v] = c[i].y;
            q[i] = W(p);
        }
        for (int i = 0; i < 4; ++i) {
            out.push_back({q[i], fluxColor});
            out.push_back({q[(i + 1) % 4], fluxColor});
        }
    }

    if (m_crpa.enabled()) {
        const glm::vec3 c = m_crpa.arrayCenter();
        for (size_t e = 0; e < m_crpa.emitters().size(); ++e) {
            const EmitterSetup& em = m_crpa.emitters()[e];
            const bool sat = em.name == m_crpa.setup().satellite;
            const bool active = (m_crpa.phase() == CrpaMonitor::Phase::Measuring && static_cast<int>(e) == m_crpa.pass()) ||
                                m_crpa.phase() == CrpaMonitor::Phase::Listening;
            const glm::vec4 col = sat ? glm::vec4(0.4f, 1.0f, 0.5f, active ? 1.0f : 0.8f) : glm::vec4(1.0f, 0.3f, 0.3f, active ? 1.0f : 0.8f);
            VolumeRenderer::appendCross(out, W(em.position), (active ? 9.0f : 6.0f) * voxel, col);
            out.push_back({W(c), glm::vec4(col.r, col.g, col.b, 0.3f)});
            out.push_back({W(em.position), glm::vec4(col.r, col.g, col.b, 0.3f)});
        }
    }
    if (m_radar.enabled()) {
        const glm::vec4 targetColor{1.0f, 0.55f, 0.2f, 0.45f}, hitColor{1.0f, 0.25f, 0.25f, 0.95f};
        for (const TargetInfo& t : m_radar.targets()) VolumeRenderer::appendBox(out, W(t.min), W(t.max), targetColor);
        const glm::vec3 tx = m_radar.txCenter();
        VolumeRenderer::appendCross(out, W(tx), 3.0f * voxel, {0.5f, 1.0f, 0.5f, 0.9f});
        const RadarImage& img = m_radar.image();
        if (img.valid) // detections appear as soon as they are in the final part of the image
            for (const RadarDetection& d : img.detections) {
                VolumeRenderer::appendCross(out, W(d.position), 6.0f * voxel, hitColor);
                out.push_back({W(tx), {1.0f, 0.25f, 0.25f, 0.35f}});
                out.push_back({W(d.position), {1.0f, 0.25f, 0.25f, 0.35f}});
            }
    }

    if (!m_farField.enabled()) return;
    const FarFieldSetup& ff = m_farField.setup();
    VolumeRenderer::appendBox(out, W(glm::vec3(ff.boxMin)), W(glm::vec3(ff.boxMax)), {0.75f, 0.45f, 1.0f, 0.35f});

    const FarFieldPattern& pat = m_farField.pattern();
    if (!pat.valid) return;
    // Polar plots of the three principal cuts on a 30 dB scale around the box centre.
    const glm::vec3 center = 0.5f * glm::vec3(ff.boxMin + ff.boxMax);
    const glm::vec3 half = 0.5f * glm::vec3(ff.boxMax - ff.boxMin);
    const float radius = 0.9f * std::min({half.x, half.y, half.z});
    const glm::vec4 colors[3] = {{1.0f, 0.6f, 0.15f, 0.9f}, {0.3f, 0.9f, 1.0f, 0.9f}, {0.55f, 1.0f, 0.4f, 0.9f}};
    for (size_t c = 0; c < pat.cuts.size() && c < 3; ++c) {
        const PatternCut& cut = pat.cuts[c];
        const size_t n = cut.angleDeg.size();
        auto point = [&](size_t i) {
            const float a = glm::radians(cut.angleDeg[i]);
            const float r = std::clamp((cut.directivityDbi[i] - pat.maxDirectivityDbi + 30.0f) / 30.0f, 0.01f, 1.0f);
            return W(center + radius * r * (std::cos(a) * cut.axisA + std::sin(a) * cut.axisB));
        };
        for (size_t i = 0; i < n; ++i) {
            out.push_back({point(i), colors[c]});
            out.push_back({point((i + 1) % n), colors[c]});
        }
    }
}
