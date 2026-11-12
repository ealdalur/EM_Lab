#pragma once
// Controlled-reception-pattern antenna (CRPA): an adaptive GPS antenna array.
//
// A small array of RHCP elements (each a dual-probe patch whose two ports a 90-degree hybrid
// combines into one right-hand-circular channel) receives a GPS satellite and one or more
// jammers. The processor:
//   1. Measures how each emitter's signal arrives at every port, one emitter at a time
//      (full-wave, so mutual coupling, the ground plane and nearby jammers are all included):
//      the emitter's steering vector.
//   2. Builds the covariance the receiver would see: jammers (equal transmit power, the
//      strongest at the chosen jammer-to-noise ratio at an element), the satellite (below the
//      noise, like real GPS) and receiver noise.
//   3. Computes MVDR weights w = R^-1 a / (a^H R^-1 a): unit gain toward the satellite's known
//      direction a (from its azimuth and elevation, or its measured response), minimum total
//      output power, which puts nulls on the jammers. N elements null up to N - 1 jammers.
//   4. Transmits with the weights: by reciprocity the fields then show the adapted receive
//      pattern, with a beam toward the satellite and nulls on the jammers.
//
// Two ways to get the covariance:
//   ideal: steps 1-2 above, the covariance a receiver converges to with unlimited snapshots of
//          independent signals (each emitter's response measured in a pass of its own).
//   blind: what a real receiver does. Every emitter transmits at once, the jammers a few
//          percent off the carrier so they are mutually independent; the receiver
//          downconverts each channel to complex baseband (I/Q), takes snapshots, adds its own
//          thermal noise and estimates R from the snapshots alone. The per-emitter amounts are
//          then separated from the same recording (each at its own frequency) only to score
//          the result, never for the weights.

#include "Ports.hpp"
#include "SimulationGrid.hpp"

#include <glm/glm.hpp>

#include <complex>
#include <filesystem>
#include <string>
#include <vector>

// A transmitter in the scene (GPS satellite or jammer): one or two ideal point dipoles.
struct EmitterSetup {
    std::string name;
    glm::vec3 position{0.0f};
    std::string polarization; // "rhcp", "lhcp" or "linear"
};

struct CrpaElement {
    std::string portX, portY;   // the two probes of a dual-feed patch
    glm::vec3 center{0.0f};     // phase centre (cells)
};

struct CrpaSetup {
    std::string satellite;              // emitter name; every other emitter is a jammer
    std::vector<CrpaElement> elements;
    float jnrDb = 40.0f;                // strongest jammer at one element, vs receiver noise
    float snrDb = -20.0f;               // satellite at one element (GPS is below the noise)
    bool measuredSteering = false;      // steer with the measured satellite response (calibrated array)
    bool mvdr = true;                   // false: plain steering (no nulling), for comparison
    float passPeriods = 36.0f;          // CW periods per measurement pass (blind: settling before the snapshots)
    bool blind = true;                  // sample covariance from snapshots of everything at once
    float snapshotPeriods = 40.0f;      // blind: snapshot window; jammer offsets are multiples of 1 / this
    int snapshotsPerPeriod = 4;         // blind: snapshot rate (per carrier period)
    unsigned noiseSeed = 1;             // blind: receiver noise realization
    glm::vec3 north{0.0f, 1.0f, 0.0f};  // compass reference for azimuths
    std::string logPath;
};

struct CrpaEmitterResult {
    std::string name;
    bool satellite = false;
    float azDeg = 0.0f, elDeg = 0.0f, distance = 0.0f; // from the array centre (compass azimuth)
    double inputDb = 0.0;      // power at one element vs noise (mean over elements)
    double outputDb = 0.0;     // after the weights, vs the output noise
    double steeringOutDb = 0.0; // after plain steering weights (no nulling), for comparison
    double crossPolDb = 0.0;   // LHCP-channel response relative to the RHCP channel
    double settleDb = 0.0;     // ideal: change over the last 4 periods; blind: fit residual (dB vs the signal)
    double offsetPct = 0.0;    // blind: transmit frequency offset from the carrier (%)
};

struct CrpaResult {
    bool valid = false;
    std::vector<CrpaEmitterResult> emitters;
    std::vector<std::complex<double>> weights; // per element
    double sinrSingleDb = 0.0, sinrSteeringDb = 0.0, sinrMvdrDb = 0.0;
    double satelliteGainDb = 0.0;              // satellite output SNR vs one element
    bool blind = false;
    size_t snapshots = 0;                      // blind: snapshots in the covariance estimate
    double snapshotUs = 0.0;                   // blind: their span in microseconds
    int mapSize = 0;
    std::vector<float> mapDb;                  // model sky map of the applied weights (north up, east right)
    std::vector<float> steeringMapDb;          // the same with plain steering weights
};

class CrpaMonitor {
public:
    // Measuring: ideal mode, one emitter at a time. Listening: blind mode, all at once.
    enum class Phase { Off, Measuring, Listening, Adapted };

    explicit CrpaMonitor(const std::filesystem::path& shaderDir);

    // `sameScene`: the geometry and the emitters are unchanged since the last configuration, so
    // stored measurements can be reused (only weights / processing settings changed).
    void configure(const SimulationGrid& grid, const CrpaSetup& setup, const std::vector<EmitterSetup>& emitters,
                   const std::vector<SourceDesc>& sources, const std::vector<PortSetup>& ports, float wavelength,
                   float cellSize, bool sameScene, std::vector<std::string>& warnings);
    void disable();
    [[nodiscard]] bool enabled() const noexcept { return m_phase != Phase::Off; }
    [[nodiscard]] Phase phase() const noexcept { return m_phase; }

    // Once per frame, before the simulation steps: switches the active sources between passes.
    void update(SimulationGrid& grid);
    void accumulate(const SimulationGrid& grid); // after every step
    void restart(const SimulationGrid& grid);    // fields were reset (R)

    [[nodiscard]] const CrpaResult& result() const noexcept { return m_result; }
    [[nodiscard]] const CrpaSetup& setup() const noexcept { return m_setup; }
    [[nodiscard]] const std::vector<EmitterSetup>& emitters() const noexcept { return m_emitters; }
    [[nodiscard]] int pass() const noexcept { return m_pass; }
    [[nodiscard]] double passProgress() const noexcept;
    [[nodiscard]] glm::vec3 arrayCenter() const noexcept { return m_center; }
    [[nodiscard]] size_t elementCount() const noexcept { return m_elementPorts.size(); }

private:
    void startPass(SimulationGrid& grid);
    void finishPass();
    void finishBlind();
    [[nodiscard]] double emitterOffset(int emitter) const; // blind: fractional frequency offset
    void computeWeights();
    void writeLog() const;
    [[nodiscard]] std::vector<std::complex<double>> channels(const std::vector<std::complex<double>>& ports,
                                                             bool lhcp = false) const;
    [[nodiscard]] glm::vec2 compassAngles(const glm::vec3& p) const; // azimuth, elevation (deg)

    PortMonitor m_recorder;
    CrpaSetup m_setup;
    std::vector<EmitterSetup> m_emitters;
    std::vector<SourceDesc> m_sources;
    std::vector<PortSetup> m_ports;
    std::vector<std::pair<int, int>> m_elementPorts; // (X, Y) port indices per element
    std::vector<int> m_emitterSources;               // emitter index per source (-1: none)
    int m_satellite = 0;
    glm::vec3 m_center{0.0f};
    float m_wavelength = 16.0f, m_cellSize = 1e-3f;
    double m_dt = 0.5;
    size_t m_passSteps = 0;
    size_t m_blindSteps = 0;
    std::vector<double> m_emitterWavelength;                    // blind: as transmitted
    std::vector<std::vector<std::complex<double>>> m_baseband;  // blind: [snapshot][element] RHCP, noise-free
    double m_satAmplitude = 1.0;                                // blind: satellite transmit amplitude
    Phase m_phase = Phase::Off;
    int m_pass = 0;               // emitter being measured
    bool m_needsApply = false;    // switch sources at the next update
    bool m_measured = false;
    std::vector<std::vector<std::complex<double>>> m_response; // [emitter][port]
    std::vector<double> m_settle;                               // [emitter] relative change, last 4 periods
    std::vector<double> m_signature;
    CrpaResult m_result;
};
