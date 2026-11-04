#pragma once
// Measurement instruments of the active scene: any number of virtual probes and
// Poynting-flux monitors plus an optional far-field (NTFF) antenna monitor.
// Handles time averaging, CSV logging, console/HUD reporting and overlay graphics.

#include "Environment.hpp"
#include "FarField.hpp"
#include "FieldAnalyzer.hpp"
#include "VolumeRenderer.hpp"

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

class SimulationGrid;

class Instruments {
public:
    explicit Instruments(const std::filesystem::path& shaderDir);

    // Applies the scene's instrument definitions. Relative log paths are resolved
    // against `outputDir`. Problems are reported through `warnings`.
    void configure(const SimulationGrid& grid, const Environment& env, const std::filesystem::path& outputDir,
                   std::vector<std::string>& warnings);

    void onStep(const SimulationGrid& grid);            // per time step (far-field DFT)
    void measure(const SimulationGrid& grid);           // probes + flux monitors (+ CSV rows)
    void evaluateFarField(const SimulationGrid& grid);  // radiation pattern (+ CSV)
    void resetHistory(const SimulationGrid& grid);      // after a field reset

    void printReport(const SimulationGrid& grid) const;
    [[nodiscard]] std::string titleSummary() const;
    void appendOverlay(std::vector<LineVertex>& out, const glm::ivec3& gridSize) const;

    void selectNextProbe();
    void moveSelectedProbe(const glm::vec3& delta, const glm::ivec3& gridSize);
    [[nodiscard]] bool unstable() const noexcept { return m_unstable; }
    [[nodiscard]] bool hasFarField() const noexcept { return m_farField.enabled(); }
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

    FieldAnalyzer m_analyzer;
    FarFieldMonitor m_farField;
    std::vector<Probe> m_probes;
    std::vector<Flux> m_fluxes;
    size_t m_selected = 0;
    bool m_unstable = false;
    double m_averagingTime = 48.0;    // flux EMA time constant (3 reference periods)
    double m_measureTime = 0.0;
    std::uint64_t m_measureStep = 0;
    std::filesystem::path m_outputDir;
};
