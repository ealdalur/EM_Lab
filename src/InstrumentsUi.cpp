// On-screen instrument panels (Dear ImGui + ImPlot): the readouts of probes, flux monitors,
// antenna ports and the far-field monitor in one window, updated in place at the refresh
// interval, plus plot windows for the frequency sweep and the radiation pattern.

#include "Instruments.hpp"
#include "SimulationGrid.hpp"
#include "Ui.hpp"

#include <glm/gtc/constants.hpp>
#include <imgui.h>
#include <implot.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace {
constexpr double kEta0 = 376.730313668;
constexpr double kC0 = 299792458.0;

const ImVec4 kDim{0.62f, 0.66f, 0.74f, 1.0f};
const ImVec4 kGood{0.45f, 0.95f, 0.55f, 1.0f};
const ImVec4 kWarn{1.0f, 0.78f, 0.3f, 1.0f};
const ImVec4 kBad{1.0f, 0.42f, 0.38f, 1.0f};
const ImVec4 kPortColors[4] = {{1.0f, 0.85f, 0.25f, 1.0f}, {0.35f, 0.9f, 1.0f, 1.0f}, {1.0f, 0.45f, 0.8f, 1.0f},
                               {0.55f, 1.0f, 0.45f, 1.0f}};

// Colour for a match quality: green when well matched, amber when usable, red when poor.
ImVec4 vswrColor(double vswr) { return vswr < 1.5 ? kGood : (vswr < 2.0 ? kWarn : kBad); }

void cell(const char* fmt, ...) IM_FMTARGS(1);
void cell(const char* fmt, ...) {
    ImGui::TableNextColumn();
    va_list args;
    va_start(args, fmt);
    ImGui::TextV(fmt, args);
    va_end(args);
}

void cellColored(const ImVec4& c, const char* fmt, ...) IM_FMTARGS(2);
void cellColored(const ImVec4& c, const char* fmt, ...) {
    ImGui::TableNextColumn();
    va_list args;
    va_start(args, fmt);
    ImGui::TextColoredV(c, fmt, args);
    va_end(args);
}

void tooltip(const char* text) {
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) ImGui::SetTooltip("%s", text);
}

// Table header row whose columns carry hover explanations.
void headers(std::initializer_list<std::pair<const char*, const char*>> cols) {
    for (const auto& c : cols) ImGui::TableSetupColumn(c.first);
    ImGui::TableNextRow(ImGuiTableRowFlags_Headers);
    int i = 0;
    for (const auto& c : cols) {
        ImGui::TableSetColumnIndex(i++);
        ImGui::TableHeader(c.first);
        if (c.second && *c.second) tooltip(c.second);
    }
}

constexpr ImGuiTableFlags kTableFlags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit |
                                        ImGuiTableFlags_NoHostExtendX;
} // namespace

void Instruments::takeSnapshot(const SimulationGrid& grid) {
    InstrumentSnapshot& s = m_snap;
    s.valid = true;
    s.time = m_measureTime;
    s.step = m_measureStep;
    s.selected = m_selected;
    s.probes.clear();
    for (const Probe& p : m_probes) s.probes.push_back({p.setup.name, p.sample});
    s.fluxes.clear();
    for (const Flux& f : m_fluxes) s.fluxes.push_back({f.setup.surface.label, f.reading});
    s.ports = m_ports.enabled() ? m_ports.readings() : std::vector<PortReading>{};
    s.portPeriods = m_ports.enabled() ? m_ports.accumulatedPeriods(grid) : 0.0;
    s.accepted = s.available = 0.0;
    for (const PortReading& r : s.ports) {
        s.accepted += r.acceptedPower;
        s.available += r.availablePower;
    }
    s.radiationResistance = s.efficiency = -1.0;
    s.pattern = m_farField.enabled() ? m_farField.pattern() : FarFieldPattern{};
    s.farFieldPeriods = m_farField.enabled() ? m_farField.accumulatedTime(grid) / std::max(m_farField.setup().wavelength, 1.0f) : 0.0;
    if (s.pattern.valid && s.accepted > 0.0 && !m_ports.sweeping()) {
        s.efficiency = s.pattern.radiatedPower / s.accepted;
        if (s.ports.size() == 1 && s.ports[0].valid)
            s.radiationResistance = 2.0 * s.pattern.radiatedPower / std::norm(s.ports[0].I) * kEta0;
    }
}

void Instruments::drawUi(const Ui& ui, float refreshSeconds) {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const float k = ui.scale(), pad = 10.0f * k;
    ImFont* mono = ui.monoFont();

    // ---- readouts -------------------------------------------------------------------------
    const bool cwPorts = m_ports.enabled() && !m_ports.sweeping() && !m_radar.enabled();
    const bool readouts = !m_probes.empty() || !m_fluxes.empty() || cwPorts || m_ports.sweeping() || m_farField.enabled();
    if (readouts) {
        const float w = 600.0f * k;
        ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x - w - pad, vp->WorkPos.y + pad), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(w, 380.0f * k), ImGuiCond_FirstUseEver);
        if (ImGui::Begin("Instruments")) drawReadouts(mono, refreshSeconds);
        ImGui::End();
    }

    // ---- frequency sweep --------------------------------------------------------------------
    if (m_ports.sweeping()) {
        const ImVec2 size(560.0f * k, 360.0f * k);
        ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x - size.x - pad, vp->WorkPos.y + vp->WorkSize.y - size.y - pad),
                                ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(size, ImGuiCond_FirstUseEver);
        if (ImGui::Begin("Frequency sweep")) drawSweepPlot();
        ImGui::End();
    }

    // ---- radar ------------------------------------------------------------------------------
    if (m_radar.enabled()) {
        const ImVec2 size(640.0f * k, std::min(vp->WorkSize.y - 2.0f * pad, 860.0f * k));
        ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x - size.x - pad, vp->WorkPos.y + pad), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(size, ImGuiCond_FirstUseEver);
        if (ImGui::Begin("Radar")) drawRadar(mono);
        ImGui::End();
    }

    // ---- array element map ------------------------------------------------------------------
    if (m_arrayRows > 0 && !m_ports.sweeping() && !m_radar.enabled()) {
        const ImVec2 size(430.0f * k, 440.0f * k);
        ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x - size.x - pad, vp->WorkPos.y + vp->WorkSize.y - size.y - pad),
                                ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(size, ImGuiCond_FirstUseEver);
        if (ImGui::Begin("Array elements")) drawArrayMap(mono);
        ImGui::End();
    }

    // ---- radiation pattern ------------------------------------------------------------------
    if (m_farField.enabled() && m_farField.pattern().valid) {
        const ImVec2 size(480.0f * k, 420.0f * k);
        ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + pad, vp->WorkPos.y + vp->WorkSize.y - size.y - pad), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(size, ImGuiCond_FirstUseEver);
        if (ImGui::Begin("Radiation pattern")) drawPatternPlot();
        ImGui::End();
    }
}

void Instruments::drawReadouts(ImFont* mono, float refreshSeconds) {
    const InstrumentSnapshot& s = m_snap;
    ImGui::TextColored(kDim, "t = %.1f  (step %llu)   updated every %.2g s", s.time, static_cast<unsigned long long>(s.step),
                       static_cast<double>(refreshSeconds));
    tooltip("Simulation time in normalized units (cells / c). Readouts refresh at a fixed interval so the numbers "
            "can be read; plots update whenever a new result is available.");
    if (!s.valid) return;
    if (mono) ImGui::PushFont(mono, 14.0f);

    // Probes
    if (!s.probes.empty() && ImGui::CollapsingHeader("Probes", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (ImGui::BeginTable("probes", 6, kTableFlags)) {
            headers({{"Probe", "Tab selects a probe; arrows / PgUp / PgDn move it (Shift: x5)"},
                     {"Position", "Lattice coordinates (cells)"},
                     {"|E|", "Electric field magnitude at this instant"},
                     {"|B|", "Magnetic field (stored as cB, same units as E)"},
                     {"E (x, y, z)", "Electric field components (hover a row for the Poynting vector and material)"},
                     {"eps_r", "Relative permittivity at the probe"}});
            for (size_t i = 0; i < s.probes.size(); ++i) {
                const ProbeSample& p = s.probes[i].sample;
                ImGui::TableNextRow();
                if (i == s.selected) cellColored(kWarn, "> %s", s.probes[i].name.c_str());
                else cell("  %s", s.probes[i].name.c_str());
                cell("%.0f %.0f %.0f", p.position.x, p.position.y, p.position.z);
                cell("%.3e", p.eMagnitude());
                cell("%.3e", p.bMagnitude());
                cell("%+.2e %+.2e %+.2e", p.E.x, p.E.y, p.E.z);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Poynting vector S = E x B: (%+.3e, %+.3e, %+.3e)\nconductivity sigma = %.3g (normalized)",
                                      p.S.x, p.S.y, p.S.z, p.sigma);
                cell("%.2f", p.epsilonR);
            }
            ImGui::EndTable();
        }
    }

    // Flux monitors
    if (!s.fluxes.empty() && ImGui::CollapsingHeader("Power flow", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (ImGui::BeginTable("flux", 4, kTableFlags)) {
            headers({{"Monitor", ""},
                     {"P(t)", "Instantaneous power through the surface (normalized; see docs/UNITS.md)"},
                     {"<P>", "Time-averaged power (about 3 periods). For a closed box: net power flowing out"},
                     {"peak |P|", "Largest |P| seen since the last reset"}});
            for (const auto& f : s.fluxes) {
                ImGui::TableNextRow();
                cell("%s", f.label.c_str());
                cell("%+.4e", f.reading.instantaneous);
                cell("%+.4e", f.reading.timeAveraged);
                cell("%.4e", f.reading.peakAbs);
            }
            ImGui::EndTable();
        }
    }

    // Antenna ports (continuous wave)
    if (!s.ports.empty() && !m_ports.sweeping() && !m_radar.enabled() &&
        ImGui::CollapsingHeader("Antenna ports", ImGuiTreeNodeFlags_DefaultOpen)) {
        const auto& ports = m_ports.ports();
        if (s.portPeriods < 12.0)
            ImGui::TextColored(kWarn, "averaging: %.0f of ~12 periods, impedances still settling", s.portPeriods);
        // Long port lists (arrays) scroll inside a 10-row table; short ones size to fit.
        const bool scroll = ports.size() > 10;
        const ImVec2 outer(0.0f, scroll ? 11.5f * ImGui::GetFrameHeight() : 0.0f);
        if (ImGui::BeginTable("ports", 7, kTableFlags | (scroll ? ImGuiTableFlags_ScrollY : 0), outer)) {
            ImGui::TableSetupScrollFreeze(0, 1);
            headers({{"Port", "Every antenna feed is a measurement port"},
                     {"Z (ohm)", "Input impedance R + jX at the feed (arrays: active impedance, including coupling)"},
                     {"|Gamma|", "Reflection coefficient magnitude against the feed impedance"},
                     {"S11", "Return loss 20 log10 |Gamma| (dB): -10 dB = 10% reflected, -20 dB = 1%"},
                     {"VSWR", "Voltage standing-wave ratio: 1 = perfect match, 2 = 11% of the power reflected"},
                     {"accepted", "Power accepted by the antenna, as a share of what the source can deliver"},
                     {"P_acc", "Accepted power (normalized)"}});
            for (size_t i = 0; i < ports.size() && i < s.ports.size(); ++i) {
                const PortReading& r = s.ports[i];
                ImGui::TableNextRow();
                cell("%s", ports[i].name.c_str());
                if (!r.valid) {
                    cell("(no current yet)");
                    for (int c = 0; c < 5; ++c) cell(" ");
                    continue;
                }
                cell("%7.1f %+7.1fj", r.zOhm.real(), r.zOhm.imag());
                if (ports[i].voltageFeed) {
                    cell("%.3f", std::abs(r.gamma));
                    cell("%6.1f dB", -r.returnLossDb);
                    cellColored(vswrColor(r.vswr), "%6.2f", r.vswr);
                    cell("%5.1f %%", r.availablePower > 0.0 ? 100.0 * r.acceptedPower / r.availablePower : 0.0);
                } else {
                    cell("-");
                    cell("-");
                    cell("current feed");
                    cell("-");
                }
                cell("%.3e", r.acceptedPower);
            }
            ImGui::EndTable();
        }
        if (s.available > 0.0)
            ImGui::Text("total accepted %.3e of %.3e available (%.1f %%)", s.accepted, s.available, 100.0 * s.accepted / s.available);
        if (s.radiationResistance > 0.0)
            ImGui::Text("radiation resistance 2 P_rad / |I|^2 = %.1f ohm", s.radiationResistance);
        if (s.efficiency > 0.0) {
            ImGui::Text("radiation efficiency P_rad / P_acc = %.1f %%", 100.0 * s.efficiency);
            tooltip("Radiated power (far-field monitor) over the power the feeds deliver. Below 100%: losses in "
                    "lossy materials (substrates, ground).");
        }
    }

    // Frequency sweep summary
    if (m_ports.sweeping() && ImGui::CollapsingHeader("Frequency sweep", ImGuiTreeNodeFlags_DefaultOpen)) {
        const SweepResult& sw = m_ports.sweep();
        if (!sw.valid) {
            ImGui::TextColored(kWarn, "recording the pulse response...");
        } else {
            const bool settled = sw.residual <= 1e-3;
            ImGui::TextColored(settled ? kGood : kWarn, "%zu steps recorded, ring-down %.1e of peak%s", sw.samples, sw.residual,
                               settled ? " (settled)" : (sw.full ? " (buffer full: approximate)" : " (still ringing: settling)"));
            tooltip("The Fourier transform is exact once the antenna has stopped ringing (below 1e-3 of the peak).");
            const double cellSize = m_ports.cellSize();
            auto ghz = [cellSize](double wl) { return kC0 / (wl * cellSize) * 1e-9; };
            if (ImGui::BeginTable("sweep", 5, kTableFlags)) {
                headers({{"Port", ""},
                         {"best match", "Frequency of the lowest S11"},
                         {"S11 / VSWR", "At the best match"},
                         {"Z (ohm)", "Impedance at the best match"},
                         {"-10 dB band", "Where S11 < -10 dB (VSWR < 1.92): the usable bandwidth"}});
                const auto& ports = m_ports.ports();
                for (size_t i = 0; i < sw.ports.size(); ++i) {
                    const PortSweep& ps = sw.ports[i];
                    const SweepPoint& b = ps.points[ps.best];
                    ImGui::TableNextRow();
                    cell("%s", ports[i].name.c_str());
                    cell("%.4f GHz", ghz(ps.bestWavelength));
                    ImGui::TableNextColumn();
                    ImGui::TextColored(vswrColor(b.vswr), "%.1f dB / %.2f", b.s11Db, b.vswr);
                    cell("%.1f %+.1fj", b.zOhm.real(), b.zOhm.imag());
                    if (ps.hasBand) {
                        const double f0 = ghz(ps.bestWavelength), fl = ghz(ps.bandLowWavelength), fh = ghz(ps.bandHighWavelength);
                        cell("%.3f - %.3f GHz (%.1f %%)", fl, fh, 100.0 * (fh - fl) / f0);
                    } else {
                        cellColored(kBad, "never below -10 dB");
                    }
                }
                ImGui::EndTable();
            }
        }
    }

    // Far field
    if (m_farField.enabled() && ImGui::CollapsingHeader("Far field", ImGuiTreeNodeFlags_DefaultOpen)) {
        const FarFieldPattern& pat = s.pattern;
        if (!pat.valid) {
            ImGui::TextColored(kWarn, "accumulating (%.1f periods)...", s.farFieldPeriods);
        } else {
            const glm::vec3 d = pat.maxDirection;
            const float theta = glm::degrees(std::acos(std::clamp(d.z, -1.0f, 1.0f)));
            const float phi = glm::degrees(std::atan2(d.y, d.x));
            const bool settling = s.farFieldPeriods < 3.0 * m_farField.setup().averagingPeriods;
            ImGui::Text("D_max %.2f dBi (%.2f) at theta %.0f, phi %.0f deg", pat.maxDirectivityDbi,
                        std::pow(10.0, pat.maxDirectivityDbi / 10.0), theta, phi);
            tooltip("Peak directivity: radiation intensity in the best direction relative to an isotropic radiator. "
                    "theta is measured from +z, phi from +x toward +y.");
            ImGui::Text("P_rad %.4e", pat.radiatedPower);
            if (settling) {
                ImGui::SameLine();
                ImGui::TextColored(kWarn, "  (still settling: %.0f periods)", s.farFieldPeriods);
            }
            if (ImGui::BeginTable("cuts", 5, kTableFlags)) {
                headers({{"Cut", "xy: azimuth from +x toward +y; xz: from +x toward +z; yz: from +y toward +z"},
                         {"peak", "Highest directivity in this plane"},
                         {"at", "Angle of the peak in this plane"},
                         {"HPBW", "Half-power (-3 dB) beamwidth around the peak"},
                         {"front/back", "Peak over the level in the opposite direction"}});
                for (const PatternCut& c : pat.cuts) {
                    ImGui::TableNextRow();
                    cell("%s", c.name.c_str());
                    cell("%6.2f dBi", c.peakDbi);
                    cell("%6.1f deg", c.peakAngleDeg);
                    if (c.beamwidthDeg >= 359.0f) cell("omni");
                    else cell("%5.1f deg", c.beamwidthDeg);
                    cell("%5.1f dB", c.frontToBackDb);
                }
                ImGui::EndTable();
            }
        }
    }
    if (mono) ImGui::PopFont();
}

void Instruments::drawSweepPlot() {
    const SweepResult& sw = m_ports.sweep();
    if (!sw.valid || sw.ports.empty()) {
        ImGui::TextColored(kWarn, "recording the pulse response...");
        return;
    }
    const double cellSize = m_ports.cellSize();
    auto ghz = [cellSize](double wl) { return kC0 / (wl * cellSize) * 1e-9; };
    const auto& ports = m_ports.ports();
    const double designGhz = ghz(ports.front().wavelength);

    // Per-port curves (frequency ascending).
    const size_t np = std::min<size_t>(sw.ports.size(), 4);
    std::vector<std::vector<double>> f(np), s11(np), vswr(np), r(np), x(np);
    double minS11 = 0.0, maxVswr = 1.0, zMin = 0.0, zMax = 0.0;
    for (size_t p = 0; p < np; ++p)
        for (const SweepPoint& pt : sw.ports[p].points) {
            f[p].push_back(ghz(pt.wavelength));
            s11[p].push_back(pt.s11Db);
            vswr[p].push_back(std::min(pt.vswr, 50.0));
            r[p].push_back(pt.zOhm.real());
            x[p].push_back(pt.zOhm.imag());
            minS11 = std::min(minS11, pt.s11Db);
            maxVswr = std::max(maxVswr, std::min(pt.vswr, 50.0));
            zMin = std::min({zMin, pt.zOhm.real(), pt.zOhm.imag()});
            zMax = std::max({zMax, pt.zOhm.real(), pt.zOhm.imag()});
        }
    const double fLo = f[0].front(), fHi = f[0].back();

    auto bandAndMarkers = [&](bool annotate, double yAnnot) {
        // Design (source) frequency and the -10 dB band of the first port.
        if (designGhz >= fLo && designGhz <= fHi) {
            ImPlotSpec spec;
            spec.LineColor = ImVec4(0.45f, 0.95f, 0.55f, 0.8f);
            spec.Flags = ImPlotItemFlags_NoLegend;
            ImPlot::PlotInfLines("design", &designGhz, 1, spec);
            ImPlot::TagX(designGhz, ImVec4(0.3f, 0.6f, 0.35f, 1.0f), "%.3f", designGhz);
        }
        const PortSweep& ps = sw.ports[0];
        if (ps.hasBand) {
            const double band[2] = {ghz(ps.bandLowWavelength), ghz(ps.bandHighWavelength)};
            ImPlotSpec spec;
            spec.LineColor = ImVec4(0.7f, 0.75f, 0.85f, 0.45f);
            spec.Flags = ImPlotItemFlags_NoLegend;
            ImPlot::PlotInfLines("band", band, 2, spec);
        }
        if (annotate) {
            const SweepPoint& b = ps.points[ps.best];
            ImPlot::Annotation(ghz(ps.bestWavelength), yAnnot, ImVec4(0.15f, 0.17f, 0.22f, 0.9f), ImVec2(12, -12), true,
                               "%.3f GHz\nS11 %.1f dB, VSWR %.2f\nZ %.1f %+.1fj ohm", ghz(ps.bestWavelength), b.s11Db, b.vswr,
                               b.zOhm.real(), b.zOhm.imag());
        }
    };

    if (ImGui::BeginTabBar("sweepTabs")) {
        const ImVec2 plotSize(-1.0f, -1.0f);
        if (ImGui::BeginTabItem("S11")) {
            if (ImPlot::BeginPlot("##s11", plotSize)) {
                ImPlot::SetupAxes("frequency (GHz)", "S11 (dB)");
                ImPlot::SetupAxisLimits(ImAxis_X1, fLo, fHi, ImPlotCond_Always);
                ImPlot::SetupAxisLimits(ImAxis_Y1, std::min(-30.0, std::floor(minS11 / 5.0) * 5.0 - 2.0), 1.0, ImPlotCond_Always);
                ImPlot::SetupLegend(ImPlotLocation_SouthEast);
                const double lines[2] = {-10.0, -20.0};
                ImPlotSpec ref;
                ref.LineColor = ImVec4(0.75f, 0.78f, 0.85f, 0.35f);
                ref.Flags = ImPlotItemFlags_NoLegend | ImPlotInfLinesFlags_Horizontal;
                ImPlot::PlotInfLines("ref", lines, 2, ref);
                ImPlot::TagY(-10.0, ImVec4(0.35f, 0.38f, 0.45f, 1.0f), "-10 dB");
                for (size_t p = 0; p < np; ++p) {
                    ImPlotSpec spec;
                    spec.LineColor = kPortColors[p];
                    spec.LineWeight = 2.0f;
                    ImPlot::PlotLine(ports[p].name.c_str(), f[p].data(), s11[p].data(), static_cast<int>(f[p].size()), spec);
                }
                bandAndMarkers(true, sw.ports[0].points[sw.ports[0].best].s11Db);
                ImPlot::EndPlot();
            }
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("VSWR")) {
            if (ImPlot::BeginPlot("##vswr", plotSize)) {
                ImPlot::SetupAxes("frequency (GHz)", "VSWR");
                ImPlot::SetupAxisLimits(ImAxis_X1, fLo, fHi, ImPlotCond_Always);
                ImPlot::SetupAxisLimits(ImAxis_Y1, 1.0, std::min(std::max(maxVswr, 3.0), 10.0), ImPlotCond_Always);
                ImPlot::SetupLegend(ImPlotLocation_NorthEast);
                const double two = 2.0;
                ImPlotSpec ref;
                ref.LineColor = ImVec4(0.75f, 0.78f, 0.85f, 0.35f);
                ref.Flags = ImPlotItemFlags_NoLegend | ImPlotInfLinesFlags_Horizontal;
                ImPlot::PlotInfLines("ref", &two, 1, ref);
                for (size_t p = 0; p < np; ++p) {
                    ImPlotSpec spec;
                    spec.LineColor = kPortColors[p];
                    spec.LineWeight = 2.0f;
                    ImPlot::PlotLine(ports[p].name.c_str(), f[p].data(), vswr[p].data(), static_cast<int>(f[p].size()), spec);
                }
                bandAndMarkers(false, 0.0);
                ImPlot::EndPlot();
            }
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Impedance")) {
            if (ImPlot::BeginPlot("##z", plotSize)) {
                ImPlot::SetupAxes("frequency (GHz)", "ohm");
                ImPlot::SetupAxisLimits(ImAxis_X1, fLo, fHi, ImPlotCond_Always);
                const double lo = std::max(zMin, -400.0), hi = std::min(zMax, 600.0);
                ImPlot::SetupAxisLimits(ImAxis_Y1, lo - 0.05 * (hi - lo), hi + 0.05 * (hi - lo), ImPlotCond_Always);
                ImPlot::SetupLegend(ImPlotLocation_NorthWest);
                const double refs[2] = {0.0, static_cast<double>(ports.front().impedanceOhm)};
                ImPlotSpec ref;
                ref.LineColor = ImVec4(0.75f, 0.78f, 0.85f, 0.35f);
                ref.Flags = ImPlotItemFlags_NoLegend | ImPlotInfLinesFlags_Horizontal;
                ImPlot::PlotInfLines("ref", refs, 2, ref);
                for (size_t p = 0; p < np; ++p) {
                    ImPlotSpec spec;
                    spec.LineColor = kPortColors[p];
                    spec.LineWeight = 2.0f;
                    const std::string rn = "R " + ports[p].name, xn = "X " + ports[p].name;
                    ImPlot::PlotLine(rn.c_str(), f[p].data(), r[p].data(), static_cast<int>(f[p].size()), spec);
                    spec.LineColor.w = 0.55f;
                    ImPlot::PlotLine(xn.c_str(), f[p].data(), x[p].data(), static_cast<int>(f[p].size()), spec);
                }
                bandAndMarkers(false, 0.0);
                ImPlot::EndPlot();
            }
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
}

void Instruments::drawArrayMap(ImFont* mono) {
    const InstrumentSnapshot& s = m_snap;
    const auto& ports = m_ports.ports();
    if (s.ports.size() != ports.size()) return;
    const char* metrics[5] = {"active VSWR", "active S11 (dB)", "accepted power (%)", "R (ohm)", "X (ohm)"};
    ImGui::SetNextItemWidth(180.0f);
    ImGui::Combo("show", &m_arrayMetric, metrics, 5);
    ImGui::SameLine();
    ImGui::TextColored(kDim, "(?)");
    tooltip("Each cell is one element's feed, laid out as seen from behind the array looking out along its boresight "
            "(as the azimuth and elevation angles are defined). In an array the impedance of an element "
            "depends on what its neighbours do (mutual coupling), so it changes with the steering: this is the active "
            "(scan) impedance. A well-designed array keeps it matched over its scan range.");

    const int rows = m_arrayRows, cols = m_arrayCols;
    std::vector<double> values(static_cast<size_t>(rows * cols), 0.0);
    double worst = 1.0, sum = 0.0;
    int n = 0;
    for (size_t cellIdx = 0; cellIdx < values.size(); ++cellIdx) {
        const int pi = m_arrayIndex[cellIdx];
        if (pi < 0) continue;
        const PortReading& r = s.ports[static_cast<size_t>(pi)];
        if (!r.valid) continue;
        double v = 0.0;
        switch (m_arrayMetric) {
            case 0: v = std::min(r.vswr, 99.0); break;
            case 1: v = -r.returnLossDb; break;
            case 2: v = r.availablePower > 0.0 ? 100.0 * r.acceptedPower / r.availablePower : 0.0; break;
            case 3: v = r.zOhm.real(); break;
            default: v = r.zOhm.imag(); break;
        }
        values[cellIdx] = v;
        worst = std::max(worst, std::min(r.vswr, 99.0));
        sum += std::min(r.vswr, 99.0);
        ++n;
    }
    if (n == 0) {
        ImGui::TextColored(kWarn, "waiting for the feed currents...");
        return;
    }
    ImGui::Text("%d x %d elements   mean VSWR %.2f, worst %.2f", cols, rows, sum / n, worst);
    if (s.available > 0.0) ImGui::Text("array accepts %.1f %% of the available power", 100.0 * s.accepted / s.available);

    double lo = 0.0, hi = 1.0;
    ImPlotColormap cmap = ImPlotColormap_Plasma;
    switch (m_arrayMetric) {
        case 0: lo = 1.0; hi = 3.0; break;
        case 1: lo = -30.0; hi = 0.0; break;
        case 2: lo = 50.0; hi = 100.0; cmap = ImPlotColormap_Viridis; break;
        default: {
            lo = *std::min_element(values.begin(), values.end());
            hi = *std::max_element(values.begin(), values.end());
            if (hi - lo < 1.0) { lo -= 0.5; hi += 0.5; }
            cmap = ImPlotColormap_Viridis;
        }
    }
    ImPlot::PushColormap(cmap);
    const float scaleWidth = 70.0f;
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const float side = std::max(std::min(avail.x - scaleWidth, avail.y), 120.0f);
    if (ImPlot::BeginPlot("##array", ImVec2(side, side), ImPlotFlags_NoLegend)) {
        char xl[32], yl[32];
        std::snprintf(xl, sizeof(xl), "column (toward %s)", m_arrayAxes[0].c_str());
        std::snprintf(yl, sizeof(yl), "row (toward %s)", m_arrayAxes[1].c_str());
        ImPlot::SetupAxes(xl, yl, ImPlotAxisFlags_NoGridLines, ImPlotAxisFlags_NoGridLines);
        ImPlot::SetupAxisLimits(ImAxis_X1, 0.0, cols, ImPlotCond_Always);
        ImPlot::SetupAxisLimits(ImAxis_Y1, 0.0, rows, ImPlotCond_Always);
        if (mono) ImGui::PushFont(mono, 13.0f);
        const char* fmt = m_arrayMetric == 0 ? "%.2f" : "%.0f";
        ImPlot::PlotHeatmap("elements", values.data(), rows, cols, lo, hi, rows * cols <= 100 ? fmt : nullptr,
                            ImPlotPoint(0, 0), ImPlotPoint(cols, rows));
        if (mono) ImGui::PopFont();
        if (ImPlot::IsPlotHovered()) {
            const ImPlotPoint m = ImPlot::GetPlotMousePos();
            const int c = static_cast<int>(m.x), r = rows - 1 - static_cast<int>(m.y);
            if (c >= 0 && c < cols && r >= 0 && r < rows) {
                const int pi = m_arrayIndex[static_cast<size_t>(r * cols + c)];
                if (pi >= 0) {
                    const PortReading& pr = s.ports[static_cast<size_t>(pi)];
                    ImGui::SetTooltip("%s\nZ = %.1f %+.1fj ohm\nS11 %.1f dB, VSWR %.2f\naccepted %.1f %%",
                                      ports[static_cast<size_t>(pi)].name.c_str(), pr.zOhm.real(), pr.zOhm.imag(),
                                      -pr.returnLossDb, pr.vswr,
                                      pr.availablePower > 0.0 ? 100.0 * pr.acceptedPower / pr.availablePower : 0.0);
                }
            }
        }
        ImPlot::EndPlot();
    }
    ImGui::SameLine();
    ImPlot::ColormapScale("##scale", lo, hi, ImVec2(scaleWidth - 8.0f, side), m_arrayMetric == 0 ? "%.1f" : "%.0f");
    ImPlot::PopColormap();
}

void Instruments::drawRadar(ImFont* mono) {
    const RadarMonitor& radar = m_radar;
    const RadarImage& img = radar.image();
    const double cellMm = radar.cellSize() * 1e3;
    const double lambda = radar.wavelength();
    switch (radar.phase()) {
        case RadarMonitor::Phase::Calibrating:
            ImGui::TextColored(kWarn, "Calibrating: transmitting into the scene without its targets");
            tooltip("This run records everything that is not a target: the transmit pulse ringing in the feeds, "
                    "coupling between elements, reflections from fixed objects. It is subtracted from the next run, "
                    "leaving only the target echoes (clutter cancellation). It is reused while only the targets move.");
            ImGui::ProgressBar(static_cast<float>(radar.progress()), ImVec2(-1.0f, 0.0f));
            if (radar.windowMeasured())
                ImGui::TextColored(kDim, "listening window set from the antennas' measured ring-down: %zu steps",
                                   radar.recordSteps());
            else
                ImGui::TextColored(kDim, "measuring the antennas' ring-down to set the listening window...");
            break;
        case RadarMonitor::Phase::Measuring: {
            const double r = img.valid ? img.completeRange * cellMm : 0.0;
            const double rmax = radar.setup().rangeMax * cellMm;
            if (radar.recordedSteps() >= radar.recordSteps())
                ImGui::TextColored(kWarn, "Echoes recorded: processing the final image...");
            else
                ImGui::TextColored(kWarn, "Measuring: image final out to %.0f mm of %.0f mm", r, rmax);
            tooltip("The image fills in from near to far as the echoes arrive. An echo from range r has fully "
                    "arrived once the recording reaches 2 r + the array's size + the radar's response length: "
                    "everything nearer than the line is final and will not change; beyond it the image is "
                    "still forming (dimmed). Detections appear once they are in the final part.");
            ImGui::ProgressBar(static_cast<float>(rmax > 0.0 ? r / rmax : 0.0), ImVec2(-1.0f, 0.0f));
            break;
        }
        default:
            ImGui::TextColored(kGood, "Image ready: %zu detection(s)", img.detections.size());
            ImGui::SameLine();
            ImGui::TextColored(kDim, "(press R to transmit again)");
            break;
    }
    if (!img.valid) {
        ImGui::TextColored(kDim, "%zu elements receive; %zu time steps per pass.", radar.elementCount(), radar.recordSteps());
        return;
    }
    ImGui::Text("echo %.1f dB below the transmit pulse at an element; image floor %.1f dB", -img.echoDb, img.floorDb);
    tooltip("The echo at a single element is tiny compared with the transmitted pulse. Adding all elements "
            "coherently (beamforming) lifts it out of the noise: the floor is the median image level.");

    const RadarSetup& rs = radar.setup();
    const float floorLine = img.floorDb + rs.thresholdDb;
    auto axisLabel = [](const glm::vec3& v, const char* what) {
        int i = 0;
        for (int k = 1; k < 3; ++k)
            if (std::abs(v[k]) > std::abs(v[i])) i = k;
        static char buf[2][48];
        static int slot = 0;
        slot ^= 1;
        std::snprintf(buf[slot], sizeof(buf[slot]), "%s (deg, toward %c%c)", what, v[i] >= 0.0f ? '+' : '-', "xyz"[i]);
        return buf[slot];
    };
    const char* azLabel = axisLabel(radar.direction(90.0f, 0.0f), "azimuth");
    const char* elLabel = axisLabel(radar.direction(0.0f, 90.0f), "elevation");
    if (ImGui::BeginTabBar("radarTabs")) {
        const float plotH = ImGui::GetContentRegionAvail().y - 9.5f * ImGui::GetFrameHeight();
        const ImVec2 plotSize(-1.0f, std::max(plotH, 160.0f));
        const double lo = -35.0;
        if (ImGui::BeginTabItem("Range-azimuth")) {
            // Rows from the far range (top) to the near range (bottom).
            std::vector<double> v(static_cast<size_t>(img.nr) * img.naz);
            for (int r = 0; r < img.nr; ++r)
                for (int a = 0; a < img.naz; ++a)
                    v[static_cast<size_t>(img.nr - 1 - r) * img.naz + a] =
                        std::max<double>(img.bscope[static_cast<size_t>(r) * img.naz + a], lo);
            ImPlot::PushColormap(ImPlotColormap_Viridis);
            if (ImPlot::BeginPlot("##bscope", plotSize, ImPlotFlags_NoLegend)) {
                ImPlot::SetupAxes(azLabel, "range (mm)");
                const double a0 = img.az0 - 0.5 * img.daz, a1 = img.az0 + (img.naz - 0.5) * img.daz;
                const double r0 = (img.r0 - 0.5 * img.dr) * cellMm, r1 = (img.r0 + (img.nr - 0.5) * img.dr) * cellMm;
                ImPlot::SetupAxesLimits(a0, a1, r0, r1, ImPlotCond_Always);
                ImPlot::PlotHeatmap("B", v.data(), img.nr, img.naz, lo, 0.0, nullptr, ImPlotPoint(a0, r0), ImPlotPoint(a1, r1));
                if (!img.complete) {
                    // Not yet final beyond this range: dim it and mark the boundary.
                    const double rc = img.completeRange * cellMm;
                    const double xs[2] = {a0, a1}, top[2] = {r1, r1}, bottom[2] = {std::max(rc, r0), std::max(rc, r0)};
                    ImPlotSpec shade;
                    shade.FillColor = ImVec4(0.05f, 0.06f, 0.09f, 1.0f);
                    shade.FillAlpha = 0.65f;
                    shade.Flags = ImPlotItemFlags_NoLegend;
                    ImPlot::PlotShaded("##pending", xs, bottom, top, 2, shade);
                    ImPlotSpec edge;
                    edge.LineColor = ImVec4(1.0f, 0.78f, 0.3f, 0.9f);
                    edge.Flags = ImPlotItemFlags_NoLegend | ImPlotInfLinesFlags_Horizontal;
                    ImPlot::PlotInfLines("##final", &rc, 1, edge);
                    ImPlot::TagY(rc, ImVec4(0.55f, 0.42f, 0.15f, 1.0f), "final");
                }
                for (const RadarDetection& d : img.detections) {
                    const double x = d.azDeg, y = d.range * cellMm;
                    ImPlotSpec spec;
                    spec.Marker = ImPlotMarker_Cross;
                    spec.MarkerSize = 7.0f;
                    spec.MarkerLineColor = ImVec4(1.0f, 0.3f, 0.3f, 1.0f);
                    spec.Flags = ImPlotItemFlags_NoLegend;
                    ImPlot::PlotScatter("##det", &x, &y, 1, spec);
                    if (d.target >= 0) {
                        const double tx = d.targetAzDeg, ty = d.targetRange * cellMm;
                        ImPlotSpec ts;
                        ts.Marker = ImPlotMarker_Circle;
                        ts.MarkerSize = 6.0f;
                        ts.MarkerFillColor = ImVec4(0, 0, 0, 0);
                        ts.MarkerLineColor = ImVec4(1.0f, 1.0f, 1.0f, 0.9f);
                        ts.Flags = ImPlotItemFlags_NoLegend;
                        ImPlot::PlotScatter("##truth", &tx, &ty, 1, ts);
                    }
                }
                ImPlot::EndPlot();
            }
            ImPlot::PopColormap();
            ImGui::TextColored(kDim, "Strongest return over elevation. Red +: detections; white o: the target's point nearest the radar.");
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Azimuth-elevation")) {
            std::vector<double> v(static_cast<size_t>(img.nel) * img.naz);
            for (int e = 0; e < img.nel; ++e)
                for (int a = 0; a < img.naz; ++a)
                    v[static_cast<size_t>(img.nel - 1 - e) * img.naz + a] =
                        std::max<double>(img.cscope[static_cast<size_t>(e) * img.naz + a], lo);
            ImPlot::PushColormap(ImPlotColormap_Viridis);
            if (ImPlot::BeginPlot("##cscope", plotSize, ImPlotFlags_NoLegend | ImPlotFlags_Equal)) {
                ImPlot::SetupAxes(azLabel, elLabel);
                const double a0 = img.az0 - 0.5 * img.daz, a1 = img.az0 + (img.naz - 0.5) * img.daz;
                const double e0 = img.el0 - 0.5 * img.del, e1 = img.el0 + (img.nel - 0.5) * img.del;
                ImPlot::SetupAxesLimits(a0, a1, e0, e1, ImPlotCond_Always);
                ImPlot::PlotHeatmap("C", v.data(), img.nel, img.naz, lo, 0.0, nullptr, ImPlotPoint(a0, e0), ImPlotPoint(a1, e1));
                for (const RadarDetection& d : img.detections) {
                    if (std::abs(d.range - img.cscopeRange) > 3.0f * img.dr) continue;
                    const double x = d.azDeg, y = d.elDeg;
                    ImPlotSpec spec;
                    spec.Marker = ImPlotMarker_Cross;
                    spec.MarkerSize = 7.0f;
                    spec.MarkerLineColor = ImVec4(1.0f, 0.3f, 0.3f, 1.0f);
                    spec.Flags = ImPlotItemFlags_NoLegend;
                    ImPlot::PlotScatter("##det", &x, &y, 1, spec);
                }
                for (const glm::vec3& q : radar.targetNearest()) {
                    const glm::vec2 ang = radar.anglesOf(q);
                    const double x = ang.x, y = ang.y;
                    ImPlotSpec ts;
                    ts.Marker = ImPlotMarker_Circle;
                    ts.MarkerSize = 6.0f;
                    ts.MarkerFillColor = ImVec4(0, 0, 0, 0);
                    ts.MarkerLineColor = ImVec4(1.0f, 1.0f, 1.0f, 0.9f);
                    ts.Flags = ImPlotItemFlags_NoLegend;
                    ImPlot::PlotScatter("##truth", &x, &y, 1, ts);
                }
                ImPlot::EndPlot();
            }
            ImPlot::PopColormap();
            ImGui::TextColored(kDim, "Angle map at %.0f mm (the strongest return's range): what a seeker's monopulse tracks.",
                               img.cscopeRange * cellMm);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Range profile")) {
            if (ImPlot::BeginPlot("##ascope", plotSize)) {
                ImPlot::SetupAxes("range (mm)", "level (dB)");
                ImPlot::SetupAxisLimits(ImAxis_Y1, std::min<double>(img.floorDb - 10.0, -40.0), 3.0, ImPlotCond_Always);
                ImPlot::SetupLegend(ImPlotLocation_NorthEast);
                std::vector<double> x(img.ascope.size()), y(img.ascope.size());
                for (size_t i = 0; i < x.size(); ++i) {
                    x[i] = (img.r0 + static_cast<float>(i) * img.dr) * cellMm;
                    y[i] = std::max(img.ascope[i], -200.0f);
                }
                ImPlotSpec spec;
                spec.LineColor = kPortColors[0];
                spec.LineWeight = 2.0f;
                ImPlot::PlotLine("along the strongest return", x.data(), y.data(), static_cast<int>(x.size()), spec);
                const double lines[2] = {static_cast<double>(img.floorDb), static_cast<double>(floorLine)};
                ImPlotSpec ref;
                ref.LineColor = ImVec4(0.75f, 0.78f, 0.85f, 0.4f);
                ref.Flags = ImPlotItemFlags_NoLegend | ImPlotInfLinesFlags_Horizontal;
                ImPlot::PlotInfLines("ref", lines, 2, ref);
                ImPlot::TagY(img.floorDb, ImVec4(0.3f, 0.32f, 0.4f, 1.0f), "floor");
                ImPlot::TagY(floorLine, ImVec4(0.55f, 0.35f, 0.2f, 1.0f), "threshold");
                if (!img.complete) {
                    const double rc = img.completeRange * cellMm;
                    ImPlotSpec edge;
                    edge.LineColor = ImVec4(1.0f, 0.78f, 0.3f, 0.9f);
                    edge.Flags = ImPlotItemFlags_NoLegend;
                    ImPlot::PlotInfLines("##final", &rc, 1, edge);
                    ImPlot::TagX(rc, ImVec4(0.55f, 0.42f, 0.15f, 1.0f), "final");
                }
                ImPlot::EndPlot();
            }
            ImGui::TextColored(kDim, "Range resolution is set by the bandwidth: c / 2B = %.0f mm for this patch array's ~7%% band.",
                               0.5 * lambda * cellMm / 0.07);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Element signal")) {
            if (ImPlot::BeginPlot("##trace", plotSize)) {
                ImPlot::SetupAxes("time (periods)", "voltage / transmit peak");
                ImPlot::SetupAxis(ImAxis_Y2, "echo", ImPlotAxisFlags_AuxDefault | ImPlotAxisFlags_AutoFit);
                ImPlot::SetupLegend(ImPlotLocation_NorthEast);
                const int n = static_cast<int>(img.traceTime.size());
                ImPlotSpec a;
                a.LineColor = ImVec4(0.6f, 0.65f, 0.75f, 0.8f);
                ImPlot::PlotLine("calibration (no targets)", img.traceTime.data(), img.traceCal.data(), n, a);
                ImPlotSpec b;
                b.LineColor = ImVec4(0.35f, 0.9f, 1.0f, 0.6f);
                ImPlot::PlotLine("with targets", img.traceTime.data(), img.traceMeas.data(), n, b);
                ImPlot::SetAxes(ImAxis_X1, ImAxis_Y2);
                ImPlotSpec c;
                c.LineColor = kPortColors[0];
                c.LineWeight = 1.5f;
                ImPlot::PlotLine("difference = echo (right axis)", img.traceTime.data(), img.traceEcho.data(), n, c);
                ImPlot::EndPlot();
            }
            ImGui::TextColored(kDim, "Centre element. The two runs are indistinguishable by eye; their difference is the echo.");
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }

    if (mono) ImGui::PushFont(mono, 14.0f);
    if (ImGui::BeginTable("detections", 6, kTableFlags)) {
        headers({{"#", "Detections, strongest first"},
                 {"range", "Distance from the array's transmit centre"},
                 {"az / el", "Azimuth and elevation from boresight (directions as on the image axes)"},
                 {"level", "Relative to the strongest return / above the image floor"},
                 {"target", "Nearest object marked target = true"},
                 {"true nearest point", "Range and direction of the target's point nearest the radar: where a convex "
                                         "object's mirror-like (specular) return comes from"}});
        for (size_t i = 0; i < img.detections.size(); ++i) {
            const RadarDetection& d = img.detections[i];
            ImGui::TableNextRow();
            cell("%zu", i + 1);
            cell("%.1f mm (%.2f lam)", d.range * cellMm, d.range / lambda);
            cell("%+.1f / %+.1f", d.azDeg, d.elDeg);
            cell("%+.1f / %.1f dB", d.relDb, d.snrDb);
            if (d.target >= 0) {
                cell("%s", radar.targets()[static_cast<size_t>(d.target)].name.c_str());
                cell("%.1f mm, %+.1f / %+.1f", d.targetRange * cellMm, d.targetAzDeg, d.targetElDeg);
            } else {
                cell("-");
                cell("-");
            }
        }
        ImGui::EndTable();
    }
    if (mono) ImGui::PopFont();
}

void Instruments::drawPatternPlot() {
    const FarFieldPattern& pat = m_farField.pattern();
    if (!pat.valid) return;
    if (!ImGui::BeginTabBar("patternTabs")) return;
    auto axisName = [](const glm::vec3& v) {
        static const char* names[6] = {"+x", "+y", "+z", "-x", "-y", "-z"};
        int i = 2;
        for (int k = 0; k < 3; ++k)
            if (std::abs(v[k]) > std::abs(v[i])) i = k;
        return names[i + (v[i] < 0.0f ? 3 : 0)];
    };
    char front[48], back[48];
    std::snprintf(front, sizeof(front), "Beam map (%s)###front", axisName(pat.mapNormal));
    std::snprintf(back, sizeof(back), "Beam map (%s)###back", axisName(-pat.mapNormal));
    if (ImGui::BeginTabItem(front)) {
        drawBeamMap(pat, true);
        ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem(back)) {
        drawBeamMap(pat, false);
        ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem("Cuts")) {
        drawPatternCuts(pat);
        ImGui::EndTabItem();
    }
    ImGui::EndTabBar();
}

void Instruments::drawBeamMap(const FarFieldPattern& pat, bool upper) {
    const auto& map = upper ? pat.mapUpperDbi : pat.mapLowerDbi;
    if (pat.mapSize < 2 || map.empty()) return;
    auto axisChar = [](const glm::vec3& v) {
        int i = 0;
        for (int k = 1; k < 3; ++k)
            if (std::abs(v[k]) > std::abs(v[i])) i = k;
        return "xyz"[i];
    };
    const glm::vec3 n = upper ? pat.mapNormal : -pat.mapNormal;
    const char nc = axisChar(n), uc = axisChar(pat.mapU), vc = axisChar(pat.mapV);
    ImGui::TextColored(kDim, "Hemisphere around %c%c: u, v = direction components along +%c, +%c; rings 30, 60, 90 deg off the %c axis.",
                       n[0] + n[1] + n[2] > 0.0f ? '+' : '-', nc, uc, vc, nc);
    const double peak = pat.maxDirectivityDbi;
    const double lo = peak - 30.0;
    std::vector<double> vals(map.size());
    for (size_t i = 0; i < map.size(); ++i) vals[i] = std::max(static_cast<double>(map[i]), lo);
    ImPlot::PushColormap(ImPlotColormap_Viridis);
    const float scaleWidth = 80.0f;
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const float side = std::max(std::min(avail.x - scaleWidth, avail.y), 120.0f);
    if (ImPlot::BeginPlot("##beam", ImVec2(side, side), ImPlotFlags_NoLegend)) {
        char ul[24], vl[24];
        std::snprintf(ul, sizeof(ul), "u (toward +%c)", uc);
        std::snprintf(vl, sizeof(vl), "v (toward +%c)", vc);
        ImPlot::SetupAxes(ul, vl);
        ImPlot::SetupAxesLimits(-1.05, 1.05, -1.05, 1.05, ImPlotCond_Always);
        ImPlot::PlotHeatmap("D", vals.data(), pat.mapSize, pat.mapSize, lo, peak, nullptr, ImPlotPoint(-1, -1), ImPlotPoint(1, 1));
        for (double th : {30.0, 60.0, 90.0}) {
            const double r = std::sin(glm::radians(th));
            double xs[97], ys[97];
            for (int i = 0; i <= 96; ++i) {
                const double a = glm::two_pi<double>() * i / 96.0;
                xs[i] = r * std::cos(a);
                ys[i] = r * std::sin(a);
            }
            ImPlotSpec spec;
            spec.LineColor = ImVec4(1.0f, 1.0f, 1.0f, th == 90.0 ? 0.6f : 0.25f);
            spec.Flags = ImPlotItemFlags_NoLegend;
            ImPlot::PlotLine("##ring", xs, ys, 97, spec);
        }
        if (upper == (glm::dot(pat.maxDirection, pat.mapNormal) >= 0.0f)) {
            const double mx = glm::dot(pat.maxDirection, pat.mapU), my = glm::dot(pat.maxDirection, pat.mapV);
            ImPlotSpec spec;
            spec.Marker = ImPlotMarker_Cross;
            spec.MarkerSize = 8.0f;
            spec.MarkerLineColor = ImVec4(1.0f, 0.3f, 0.3f, 1.0f);
            spec.Flags = ImPlotItemFlags_NoLegend;
            ImPlot::PlotScatter("##peak", &mx, &my, 1, spec);
            const float off = glm::degrees(std::acos(std::clamp(std::abs(glm::dot(pat.maxDirection, n)), 0.0f, 1.0f)));
            const float around = glm::degrees(std::atan2(static_cast<float>(my), static_cast<float>(mx)));
            ImPlot::Annotation(mx, my, ImVec4(0.15f, 0.17f, 0.22f, 0.9f), ImVec2(14, -14), true,
                               "peak %.1f dBi\n%.0f deg off %c%c, toward %.0f deg", peak, off,
                               n[0] + n[1] + n[2] > 0.0f ? '+' : '-', nc, around);
        }
        ImPlot::EndPlot();
    }
    ImGui::SameLine();
    ImPlot::ColormapScale("dBi", lo, peak, ImVec2(scaleWidth - 8.0f, side), "%.0f");
    ImPlot::PopColormap();
}

void Instruments::drawPatternCuts(const FarFieldPattern& pat) {
    const char* names[3] = {"xy  (azimuth, from +x to +y)", "xz  (from +x to +z)", "yz  (from +y to +z)"};
    const ImVec4 colors[3] = {{1.0f, 0.6f, 0.15f, 1.0f}, {0.3f, 0.9f, 1.0f, 1.0f}, {0.55f, 1.0f, 0.4f, 1.0f}};
    ImGui::TextColored(kDim, "Directivity vs angle in the three principal planes (also drawn as polar curves in 3D).");
    if (ImPlot::BeginPlot("##pattern", ImVec2(-1.0f, -1.0f))) {
        ImPlot::SetupAxes("angle (deg)", "directivity (dBi)");
        ImPlot::SetupAxisLimits(ImAxis_X1, 0.0, 360.0, ImPlotCond_Always);
        const double peak = pat.maxDirectivityDbi;
        ImPlot::SetupAxisLimits(ImAxis_Y1, std::floor((peak - 40.0) / 5.0) * 5.0, peak + 3.0, ImPlotCond_Always);
        const double ticks[9] = {0, 45, 90, 135, 180, 225, 270, 315, 360};
        ImPlot::SetupAxisTicks(ImAxis_X1, ticks, 9);
        ImPlot::SetupLegend(ImPlotLocation_North, ImPlotLegendFlags_Horizontal | ImPlotLegendFlags_Outside);
        const double ref = peak - 3.0;
        ImPlotSpec refSpec;
        refSpec.LineColor = ImVec4(0.75f, 0.78f, 0.85f, 0.35f);
        refSpec.Flags = ImPlotItemFlags_NoLegend | ImPlotInfLinesFlags_Horizontal;
        ImPlot::PlotInfLines("-3 dB", &ref, 1, refSpec);
        ImPlot::TagY(peak, ImVec4(0.35f, 0.38f, 0.45f, 1.0f), "%.1f dBi", peak);
        for (size_t c = 0; c < pat.cuts.size() && c < 3; ++c) {
            const PatternCut& cut = pat.cuts[c];
            std::vector<double> a, d;
            a.reserve(cut.angleDeg.size() + 1);
            for (size_t i = 0; i < cut.angleDeg.size(); ++i) {
                float ang = std::fmod(cut.angleDeg[i], 360.0f);
                if (ang < 0.0f) ang += 360.0f;
                a.push_back(ang);
                d.push_back(cut.directivityDbi[i]);
            }
            // Sort by angle so the curve does not jump across 0/360.
            std::vector<size_t> idx(a.size());
            for (size_t i = 0; i < idx.size(); ++i) idx[i] = i;
            std::sort(idx.begin(), idx.end(), [&a](size_t l, size_t r) { return a[l] < a[r]; });
            std::vector<double> as, ds;
            for (size_t i : idx) {
                as.push_back(a[i]);
                ds.push_back(d[i]);
            }
            if (!as.empty() && as.front() == 0.0) {
                as.push_back(360.0);
                ds.push_back(ds.front());
            }
            ImPlotSpec spec;
            spec.LineColor = colors[c];
            spec.LineWeight = 2.0f;
            ImPlot::PlotLine(names[c], as.data(), ds.data(), static_cast<int>(as.size()), spec);
        }
        ImPlot::EndPlot();
    }
}
