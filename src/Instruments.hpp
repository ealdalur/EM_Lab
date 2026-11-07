#pragma once
// Measurement instruments of the active scene: any number of virtual probes and
// Poynting-flux monitors plus an optional far-field (NTFF) antenna monitor.
// Handles time averaging, CSV logging, console/HUD reporting and overlay graphics.

#include "Environment.hpp"
#include "FarField.hpp"
#include "FieldAnalyzer.hpp"
#include "Ports.hpp"
#include "Radar.hpp"
#include "VolumeRenderer.hpp"

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

class SimulationGrid;
class Ui;
struct ImFont;

// Copy of the instrument readouts taken at the display refresh interval, so on-screen
// numbers stay still long enough to read (the instruments themselves sample faster).
struct InstrumentSnapshot {
    bool valid = false;
    double time = 0.0;
    std::uint64_t step = 0;
    struct ProbeRow {
        std::string name;
        ProbeSample sample;
    };
    struct FluxRow {
        std::string label;
        FluxReading reading;
    };
    std::vector<ProbeRow> probes;
    size_t selected = 0;
    std::vector<FluxRow> fluxes;
    std::vector<PortReading> ports;
    double portPeriods = 0.0;
    double accepted = 0.0, available = 0.0;
    double radiationResistance = -1.0; // single-port scenes with a far-field monitor
    double efficiency = -1.0;
    FarFieldPattern pattern;
    double farFieldPeriods = 0.0;
};

class Instruments {
public:
    explicit Instruments(const std::filesystem::path& shaderDir);

    // Applies the scene's instrument definitions. Relative log paths are resolved
    // against `outputDir`. Problems are reported through `warnings`.
    void configure(const SimulationGrid& grid, const Environment& env, const std::filesystem::path& outputDir,
                   std::vector<std::string>& warnings,
                   RadarMonitor::Start radarStart = RadarMonitor::Start::Calibrate);

    void onStep(const SimulationGrid& grid);            // per time step (far-field DFT)
    // Probes + flux monitors (+ CSV rows). Non-blocking by default: this call queues a new GPU
    // measurement and applies the previous one once it has finished. wait = true applies the
    // current state immediately (reports, key presses, the end of a --frames run).
    void measure(const SimulationGrid& grid, bool wait = false);
    void evaluateFarField(const SimulationGrid& grid);  // radiation pattern (+ CSV), immediately
    // Interactive use: start an incremental far-field evaluation (ports are evaluated now) and
    // advance it once per frame, so a large monitor never stalls a frame.
    void beginFarFieldEvaluation();
    void advanceFarFieldEvaluation() {
        m_farField.advanceEvaluation();
        m_ports.pollEvaluate();
    }
    void resetHistory(const SimulationGrid& grid);      // after a field reset

    void printReport(const SimulationGrid& grid) const;
    [[nodiscard]] std::string titleSummary() const;
    void appendOverlay(std::vector<LineVertex>& out, const glm::ivec3& gridSize) const;
    // On-screen panels (Dear ImGui): readouts from the last snapshot plus live plots.
    void takeSnapshot(const SimulationGrid& grid);
    void drawUi(const Ui& ui, float refreshSeconds);
    void evaluatePorts();
    void evaluateSweep(const SimulationGrid& grid);
    [[nodiscard]] bool hasSweep() const noexcept { return m_ports.sweeping(); }

    void selectNextProbe();
    void moveSelectedProbe(const glm::vec3& delta, const glm::ivec3& gridSize);
    [[nodiscard]] bool unstable() const noexcept { return m_unstable; }
    [[nodiscard]] bool hasFarField() const noexcept { return m_farField.enabled(); }
    [[nodiscard]] bool hasPorts() const noexcept { return m_ports.enabled(); }
    // Radar: the application swaps between the calibration scene (targets removed) and the
    // full scene when radarCalibrationComplete() turns true, then calls radarBeginMeasurement().
    [[nodiscard]] bool hasRadar() const noexcept { return m_radar.enabled(); }
    [[nodiscard]] bool radarCalibrationComplete() const noexcept { return m_radar.calibrationComplete(); }
    void radarBeginMeasurement(const SimulationGrid& grid) { m_radar.beginMeasurement(grid); }
    void radarUpdate() { m_radar.update(); }
    [[nodiscard]] const RadarMonitor& radar() const noexcept { return m_radar; }
    // Far field and ports sample every time step.
    [[nodiscard]] bool needsEveryStep() const noexcept { return hasFarField() || hasPorts() || hasRadar(); }
    // True when the scene defines no probes, flux monitors, far-field monitor or feed ports.
    [[nodiscard]] bool empty() const noexcept {
        return m_probes.empty() && m_fluxes.empty() && !m_farField.enabled() && !m_ports.enabled() && !m_radar.enabled();
    }
    // Detailed port listing for every port (the periodic report shows the first few).
    void printAllPorts(const SimulationGrid& grid) const;
    static constexpr const char* kNoInstrumentsHint =
        "No instruments in this scene, so there are no readings to report. Add probe{}, flux_plane{}, "
        "flux_box{} or far_field{} to the script, or feed an antenna, to measure fields, power, impedance or "
        "radiation patterns.";
    [[nodiscard]] const FarFieldMonitor& farField() const noexcept { return m_farField; }

private:
    struct Probe {
        ProbeSetup setup;
        ProbeSample sample;
        std::unique_ptr<std::ofstream> log;
    };
    struct Flux {
        FluxSetup setup;
        FluxReading reading;
        double lastTime = -1.0;
        std::unique_ptr<std::ofstream> log;
    };

    std::unique_ptr<std::ofstream> openLog(const std::string& path, const char* header,
                                           std::vector<std::string>& warnings) const;

    void printPorts(const SimulationGrid& grid, size_t maxLines) const;
    void applyMeasurement(const MeasureResult& r);
    void drawReadouts(ImFont* mono, float refreshSeconds);
    void drawSweepPlot();
    void drawPatternPlot();
    void drawArrayMap(ImFont* mono);
    void drawRadar(ImFont* mono);
    void printRadar() const;
    void drawBeamMap(const FarFieldPattern& pat, bool upper);
    void drawPatternCuts(const FarFieldPattern& pat);
    void detectArrayLayout();

    FieldAnalyzer m_analyzer;
    FarFieldMonitor m_farField;
    PortMonitor m_ports;
    RadarMonitor m_radar;
    std::vector<WireSegment> m_wires;
    std::vector<SheetRect> m_sheets;
    std::vector<Probe> m_probes;
    std::vector<Flux> m_fluxes;
    size_t m_selected = 0;
    bool m_unstable = false;
    double m_averagingTime = 48.0;    // flux EMA time constant (3 reference periods)
    double m_measureTime = 0.0;
    std::uint64_t m_measureStep = 0;
    std::filesystem::path m_outputDir;
    InstrumentSnapshot m_snap;
    // Ports arranged on a regular 2-D lattice (phased arrays): element map for the UI.
    int m_arrayRows = 0, m_arrayCols = 0;
    std::vector<int> m_arrayIndex;    // row-major (row 0 = largest v), port index per element
    std::string m_arrayAxes[2] = {"+x", "+y"}; // world directions of the columns (right) and rows (up)
    int m_arrayMetric = 0;            // 0 VSWR, 1 S11 dB, 2 accepted %, 3 R, 4 X
};
