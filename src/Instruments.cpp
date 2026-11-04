#include "Instruments.hpp"
#include "SimulationGrid.hpp"

#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace fs = std::filesystem;

Instruments::Instruments(const fs::path& shaderDir) : m_analyzer(shaderDir), m_farField(shaderDir) {}

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
                            std::vector<std::string>& warnings) {
    m_outputDir = outputDir;
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

void Instruments::onStep(const SimulationGrid& grid) { m_farField.accumulate(grid); }

void Instruments::resetHistory(const SimulationGrid& grid) {
    for (Flux& f : m_fluxes) {
        f.reading = {};
        f.lastTime = -1.0;
    }
    if (m_farField.enabled()) m_farField.configure(grid, m_farField.setup());
    m_unstable = false;
}

void Instruments::measure(const SimulationGrid& grid) {
    m_measureTime = grid.time();
    m_measureStep = grid.stepCount();
    bool finite = true;

    for (Probe& p : m_probes) {
        p.sample = m_analyzer.probe(grid, p.setup.position);
        const ProbeSample& s = p.sample;
        finite = finite && std::isfinite(glm::dot(s.E, s.E)) && std::isfinite(glm::dot(s.B, s.B));
        if (p.log)
            *p.log << m_measureStep << ',' << m_measureTime << ',' << s.E.x << ',' << s.E.y << ',' << s.E.z << ','
                   << s.B.x << ',' << s.B.y << ',' << s.B.z << ',' << s.eMagnitude() << ',' << s.bMagnitude() << ','
                   << s.S.x << ',' << s.S.y << ',' << s.S.z << '\n';
    }

    for (Flux& f : m_fluxes) {
        const double P = m_analyzer.measureFlux(grid, f.setup.surface);
        finite = finite && std::isfinite(P);
        FluxReading& r = f.reading;
        if (f.lastTime < 0.0 || m_measureTime < f.lastTime) {
            r = {};
            r.timeAveraged = P;
        } else {
            const double a = 1.0 - std::exp(-(m_measureTime - f.lastTime) / m_averagingTime);
            r.timeAveraged += a * (P - r.timeAveraged);
        }
        r.instantaneous = P;
        r.peakAbs = std::max(r.peakAbs, std::abs(P));
        f.lastTime = m_measureTime;
        if (f.log) *f.log << m_measureStep << ',' << m_measureTime << ',' << P << ',' << r.timeAveraged << '\n';
    }

    if (!finite && !m_unstable) {
        m_unstable = true;
        std::fprintf(stderr, "ERROR: non-finite field values detected at step %llu\n",
                     static_cast<unsigned long long>(m_measureStep));
    }
}

void Instruments::evaluateFarField(const SimulationGrid& grid) {
    if (m_farField.enabled()) m_farField.evaluate(grid);
}

void Instruments::printReport(const SimulationGrid& grid) const {
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

std::string Instruments::titleSummary() const {
    std::string s;
    char buf[160];
    if (!m_probes.empty()) {
        const Probe& p = m_probes[std::min(m_selected, m_probes.size() - 1)];
        std::snprintf(buf, sizeof(buf), " | %s |E|=%.4f |B|=%.4f", p.setup.name.c_str(), p.sample.eMagnitude(),
                      p.sample.bMagnitude());
        s += buf;
    }
    if (!m_fluxes.empty()) {
        std::snprintf(buf, sizeof(buf), " | <P> %s=%+.3e", m_fluxes.front().setup.surface.label.c_str(),
                      m_fluxes.front().reading.timeAveraged);
        s += buf;
    }
    if (m_farField.enabled() && m_farField.pattern().valid) {
        std::snprintf(buf, sizeof(buf), " | D_max=%.2f dBi", m_farField.pattern().maxDirectivityDbi);
        s += buf;
    }
    return s;
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
