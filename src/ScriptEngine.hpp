#pragma once
// Lua scene scripting.
//
// A scene script is a Lua 5.4+ file that calls declarative builder functions:
//
//     scene { name = "Dipole", size = {128, 128, 128}, wavelength = 16 }
//     dipole { center = grid.center, orientation = "z", amplitude = 7 }
//     far_field {}
//
// Because it is a real language, loops, math and functions can generate geometry
// procedurally. Scripts run in a sandbox (no io/os/C modules) with a time limit,
// and every call is validated with file:line diagnostics. See docs/SCRIPTING.md.
//
// `param{}` declares interactive parameters bound to keys; the application re-runs
// the script with new values (keeping the fields when only sources changed).

#include "Environment.hpp"

#include <filesystem>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

struct ScriptParam {
    enum class Type { Bool, Number, Choice };

    std::string name;
    std::string label;
    Type type = Type::Number;
    double value = 0.0;           // Bool: 0/1, Number: value
    double defaultValue = 0.0;
    double min = -1e300, max = 1e300, step = 1.0;
    bool wrap = false;
    std::vector<std::string> choices;
    size_t choiceIndex = 0;
    char keyToggle = 0;           // Bool: toggle, Choice: cycle
    char keyDecrease = 0;         // Number
    char keyIncrease = 0;

    [[nodiscard]] std::string valueString() const;    // human readable
    [[nodiscard]] std::string overrideString() const; // round-trips through ScriptEngine::run overrides
    // Applies a key press; returns true if the value changed.
    bool handleKey(char key);
};

struct ScriptResult {
    Environment env;
    std::vector<ScriptParam> params;
    std::vector<std::string> warnings;
    std::filesystem::path outputDir;  // where relative log paths are written
};

class ScriptError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class ScriptEngine {
public:
    // Keys a script may bind to parameters (all other keys belong to the application).
    static constexpr const char* kParamKeys = "bfgijtuxyz,.;'/\\`";

    // Runs `script`. `overrides` maps parameter names to values (as produced by
    // ScriptParam::overrideString or the --set command-line option).
    // Throws ScriptError with a readable, location-tagged message on failure.
    static ScriptResult run(const std::filesystem::path& script,
                            const std::map<std::string, std::string>& overrides = {});
};
