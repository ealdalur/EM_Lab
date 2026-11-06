#pragma once
// Pulsed phased-array radar: a digital-beamforming receiver on top of the antenna ports.
//
// The scene transmits a short pulse from its antenna feeds (one element for a wide
// illumination, or the whole array). Every element is also a receiver: its port voltage is
// recorded at every time step. Processing works like a modern digital array radar:
//
//  1. Clutter removal. A first run with the radar targets removed records the transmitter's
//     own response (feed ringing, element coupling, reflections from fixed structures). It is
//     subtracted from the run with the targets, leaving only the echoes. Real radars do the
//     same with clutter maps and background subtraction.
//  2. Range timing. A reference point on boresight, recorded during the calibration run,
//     measures when the pulse actually leaves the array (the antennas' group delay included).
//  3. Receiver noise (optional): white noise relative to the transmit pulse at a port.
//  4. Matched filtering: complex baseband at the carrier, Gaussian low-pass matched to the
//     transmitted pulse.
//  5. Imaging by back-projection (delay-and-sum beamforming with the exact geometry of every
//     element, so it also focuses in the array's near field): for each image point the echo
//     samples of all elements at the point's round-trip delay are added coherently.
//  6. Detection: local maxima above the noise floor (and within a dynamic range of the
//     strongest return), reported as range / azimuth / elevation and compared with the true
//     positions of the targets.

#include "Ports.hpp"
#include "SimulationGrid.hpp"

#include <glm/glm.hpp>

#include <complex>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

class SimulationGrid;
struct SourceDesc;

struct RadarSetup {
    float rangeMin = 0.0f;          // image range (cells from the transmit phase centre); 0: 1 wavelength
    float rangeMax = 0.0f;          // 0: to the absorbing layer along boresight
    float fovAzDeg = 40.0f;         // image half-angles
    float fovElDeg = 40.0f;
    float noiseDb = -300.0f;        // receiver noise rms in the radar's band vs the transmit pulse peak at a port (<= -300: none)
    float thresholdDb = 13.0f;      // detection threshold above the image noise floor
    float dynamicRangeDb = 25.0f;   // detections within this of the strongest return
    float referenceDistance = 0.0f; // timing reference on boresight (cells); 0: 1.5 wavelengths
    int taper = 1;                  // receive amplitude taper: 0 uniform, 1 Hamming, 2 Hann
    glm::vec3 phaseCenterOffset{0.0f}; // element phase centre relative to its feed gap (cells)
    std::optional<glm::vec3> txCenter; // transmit phase centre (default: drive-weighted feed centroid)
    std::string logPath;            // CSV of detections, written after every processed measurement
};

// An object marked `target = true` in the script (removed for the calibration run).
struct TargetInfo {
    std::string name;
    glm::vec3 min{0.0f}, max{0.0f}; // lattice bounds
    std::vector<glm::vec3> points;  // centres of (a subsample of) its cells: true surface and direction
};

struct RadarDetection {
    glm::vec3 position{0.0f};       // lattice coordinates
    float range = 0.0f;             // cells from the transmit phase centre
    float azDeg = 0.0f, elDeg = 0.0f;
    float snrDb = 0.0f;             // above the image noise floor
    float relDb = 0.0f;             // relative to the strongest return
    int target = -1;                // nearest target (index into targets), -1 if none
    float targetRange = 0.0f, targetAzDeg = 0.0f, targetElDeg = 0.0f; // the target's point nearest the radar
};

struct RadarImage {
    bool valid = false;
    int nr = 0, naz = 0, nel = 0;
    float r0 = 0.0f, dr = 1.0f;       // range bins (cells)
    float az0 = 0.0f, daz = 1.0f;     // degrees
    float el0 = 0.0f, del = 1.0f;
    std::vector<float> powerDb;       // [r][el][az], 0 dB = strongest return
    float floorDb = -100.0f;          // median image level (noise floor)
    float echoDb = 0.0f;              // strongest echo at an element vs the transmit pulse there
    std::vector<RadarDetection> detections;
    std::vector<float> bscope;        // [r][az]: range-azimuth, maximum over elevation (dB)
    std::vector<float> cscope;        // [el][az]: azimuth-elevation at the strongest return's range
    std::vector<float> ascope;        // [r]: range profile along the strongest return's direction
    float cscopeRange = 0.0f;
    // Received signal at the element nearest the transmit centre (decimated for plotting).
    std::vector<float> traceTime, traceCal, traceMeas, traceEcho;
    float systemDelay = 0.0f;         // two-way antenna group delay at the carrier (time units = cells)
};

class RadarMonitor {
public:
    enum class Phase { Off, Calibrating, Measuring, Done };
    // What a (re)configuration needs: a new calibration (the fixed scene or the transmitter
    // changed), only a new measurement (the targets changed), or only new processing (noise,
    // threshold, field of view).
    enum class Start { Calibrate, Measure, Reprocess };

    explicit RadarMonitor(const std::filesystem::path& shaderDir);

    void configure(const SimulationGrid& grid, const RadarSetup& setup, const std::vector<PortSetup>& elements,
                   const std::vector<SourceDesc>& sources, float wavelength, float cellSize,
                   const std::vector<TargetInfo>& targets, Start start, std::vector<std::string>& warnings);
    void disable();
    [[nodiscard]] bool enabled() const noexcept { return m_phase != Phase::Off; }
    [[nodiscard]] Phase phase() const noexcept { return m_phase; }
    [[nodiscard]] double progress() const noexcept;
    [[nodiscard]] size_t recordSteps() const noexcept { return m_steps; }

    void accumulate(const SimulationGrid& grid);        // after every completed step
    [[nodiscard]] bool calibrationComplete() const noexcept;
    void beginMeasurement(const SimulationGrid& grid);  // targets uploaded and fields reset
    void update();                                      // processes a completed measurement
    void restart(const SimulationGrid& grid);           // fields were reset: record the current phase again

    [[nodiscard]] const RadarImage& image() const noexcept { return m_image; }
    [[nodiscard]] const RadarSetup& setup() const noexcept { return m_setup; }
    [[nodiscard]] const std::vector<TargetInfo>& targets() const noexcept { return m_targets; }
    // Each target's point nearest the transmit centre: where its glint (specular return) is.
    [[nodiscard]] const std::vector<glm::vec3>& targetNearest() const noexcept { return m_targetNearest; }
    // Azimuth / elevation (degrees) of a lattice point as seen from the transmit centre.
    [[nodiscard]] glm::vec2 anglesOf(const glm::vec3& p) const;
    [[nodiscard]] glm::vec3 txCenter() const noexcept { return m_tx; }
    [[nodiscard]] glm::vec3 boresight() const noexcept { return m_axes[2]; }
    [[nodiscard]] glm::vec3 direction(float azDeg, float elDeg) const;
    [[nodiscard]] float wavelength() const noexcept { return m_wavelength; }
    [[nodiscard]] float cellSize() const noexcept { return m_cellSize; }
    [[nodiscard]] size_t elementCount() const noexcept { return m_elements.size(); }

private:
    void startRecording(const SimulationGrid& grid);
    void readSeries(std::vector<float>& out) const; // V per [step][channel]
    void process();
    void writeLog() const;
    // Transmit delay to a point: the group delay at the carrier of the near-field array factor
    // A(w) = sum_j (a_j / d_j) exp(-j w d_j) of the transmitting elements. In the near field of a
    // large aperture the pulse does not come from one point; stationary phase picks the paths
    // that add in phase (the plane-wave arrival on boresight).
    [[nodiscard]] double txDelay(const glm::vec3& p) const;
    [[nodiscard]] std::complex<double> txArrayFactor(const glm::vec3& p, double omega) const;

    PortMonitor m_recorder;              // elements + 3 reference channels (Ex, Ey, Ez)
    RadarSetup m_setup;
    std::vector<PortSetup> m_elements;
    std::vector<glm::vec3> m_elementPos; // phase centres (cells)
    std::vector<double> m_weights;       // receive taper per element
    std::vector<TargetInfo> m_targets;
    std::vector<glm::vec3> m_targetNearest;
    glm::vec3 m_tx{0.0f};
    glm::vec3 m_axes[3]{{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}; // azimuth axis, elevation axis, boresight
    glm::vec3 m_reference{0.0f};
    float m_referenceDistance = 0.0f;
    float m_wavelength = 16.0f;
    float m_cellSize = 1e-3f;
    double m_dt = 0.5;
    double m_txTime = 0.0;               // drive-weighted centre of the transmitted pulses
    double m_pulseWidth = 8.0;           // 1/e half-width of the pulse envelope (time units)
    size_t m_steps = 0;                  // recording length per pass
    Phase m_phase = Phase::Off;
    bool m_haveCalibration = false;
    bool m_haveMeasurement = false;
    std::vector<float> m_cal, m_meas;    // [step][channel] voltages
    std::vector<SourceDesc> m_txSources; // pulsed feeds that transmit
    std::vector<glm::vec3> m_txPos;      // their phase centres
    std::vector<double> m_txAmp;
    double m_txWeight = 1.0;
    std::vector<double> m_drive;         // transmitted drive waveform, for the pulse-compression template
    RadarImage m_image;
};
