#include "ScriptEngine.hpp"

#include <lua.hpp>

#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <cctype>
#include <climits>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>
#include <optional>
#include <set>
#include <sstream>

namespace fs = std::filesystem;

namespace {

constexpr double kC0 = 299792458.0;     // m/s
constexpr double kEta0 = 376.730313668; // free-space impedance, ohm
constexpr double kTimeLimitSeconds = 30.0;
constexpr float kEta0f = 376.730313668f;

// =============================================================================
// Small utilities
// =============================================================================
std::string fmt(const char* f, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, f);
    std::vsnprintf(buf, sizeof(buf), f, ap);
    va_end(ap);
    return buf;
}

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

size_t editDistance(const std::string& a, const std::string& b) {
    std::vector<size_t> prev(b.size() + 1), cur(b.size() + 1);
    for (size_t j = 0; j <= b.size(); ++j) prev[j] = j;
    for (size_t i = 1; i <= a.size(); ++i) {
        cur[0] = i;
        for (size_t j = 1; j <= b.size(); ++j)
            cur[j] = std::min({prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + (a[i - 1] == b[j - 1] ? 0 : 1)});
        std::swap(prev, cur);
    }
    return prev[b.size()];
}

std::string suggestion(const std::string& word, const std::vector<std::string>& candidates) {
    std::string best;
    size_t bestD = 3; // only suggest close matches
    for (const std::string& c : candidates) {
        const size_t d = editDistance(lower(word), lower(c));
        if (d < bestD) {
            bestD = d;
            best = c;
        }
    }
    return best.empty() ? std::string() : " (did you mean '" + best + "'?)";
}

std::string where(lua_State* L) {
    luaL_where(L, 1);
    std::string s = lua_tostring(L, -1);
    lua_pop(L, 1);
    return s;
}

// =============================================================================
// Build context (one per ScriptEngine::run)
// =============================================================================
struct MaterialSpec {
    std::string name;
    float epsilonR = 1.0f;
    std::optional<double> sigma;        // normalised conductivity
    std::optional<double> conductivity; // S/m
    std::optional<double> lossTangent;
};

struct SceneSettings {
    std::string name = "Untitled scene";
    std::string description;
    glm::ivec3 size{128, 128, 128};
    float courant = 0.5f;
    int pml = 16;
    float pmlSigmaMax = 1.6f;
    float pmlGrading = 3.0f;
    float cellSize = 1.0e-3f;
    float wavelength = 16.0f;
    std::string outputDir;
};

struct MonopoleCheck {
    bool thin = true;
    glm::ivec3 node{0};   // thin: base lattice node; rod: the cell across the gap from the rod
    int axis = 2;
    float sign = 1.0f;
    std::string where;
};

struct ThinWireSpec {
    glm::ivec3 node{0};  // lattice line (the coordinate along `axis` is ignored)
    int axis = 2;
    int i0 = 0, i1 = 0;  // requested edges [i0, i1) along the axis
    float radius = 0.05f;
    int gapEdge = INT_MIN;
};

struct Context {
    fs::path script;
    std::map<std::string, std::string> overrides;
    SceneSettings settings;
    bool locked = false;
    std::unique_ptr<EnvironmentBuilder> builder;
    std::map<std::string, MaterialSpec> materials; // keyed by lower-case name
    std::vector<ScriptParam> params;
    std::vector<std::string> warnings;
    ViewHints view;
    std::map<std::string, float> usedDielectrics; // name -> eps_r (resolution checks)
    std::chrono::steady_clock::time_point start;
    int sheetSources = 0;
    // Monopole feeds must touch a conductor at their base (checked once the scene is built).
    std::vector<MonopoleCheck> monopoles;
    std::vector<ThinWireSpec> thinWires;
    // Radar calibration: the second run of a radar scene leaves out every target = true object.
    bool withTargets = true;
};

thread_local Context* g_ctx = nullptr;

Context& ctx() { return *g_ctx; }

void warn(lua_State* L, const std::string& msg) { ctx().warnings.push_back(where(L) + msg); } // where() ends in ": "

// Converts C++ exceptions into Lua errors at the binding boundary. Lua raises errors
// with longjmp, so no object with a destructor may be alive when luaL_error runs.
template <int (*Fn)(lua_State*)>
int wrap(lua_State* L) {
    char msg[2048];
    msg[0] = '\0';
    try {
        return Fn(L);
    } catch (const std::exception& e) {
        std::snprintf(msg, sizeof(msg), "%s", e.what());
    } catch (...) {
        std::snprintf(msg, sizeof(msg), "unknown error");
    }
    return luaL_error(L, "%s", msg);
}

// =============================================================================
// Value conversion
// =============================================================================
bool numberField(lua_State* L, int idx, int i, const char* key, double& out) {
    lua_rawgeti(L, idx, i);
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        lua_getfield(L, idx, key);
    }
    const bool ok = lua_type(L, -1) == LUA_TNUMBER;
    if (ok) out = lua_tonumber(L, -1);
    lua_pop(L, 1);
    return ok;
}

// number (splat), {a,b,c}, {x=,y=,z=} or a vec()
bool toVec3(lua_State* L, int idx, glm::vec3& out) {
    idx = lua_absindex(L, idx);
    if (lua_type(L, idx) == LUA_TNUMBER) {
        out = glm::vec3(static_cast<float>(lua_tonumber(L, idx)));
        return std::isfinite(out.x);
    }
    if (!lua_istable(L, idx)) return false;
    double v[3];
    static const char* keys[3] = {"x", "y", "z"};
    for (int i = 0; i < 3; ++i)
        if (!numberField(L, idx, i + 1, keys[i], v[i]) || !std::isfinite(v[i])) return false;
    out = glm::vec3(static_cast<float>(v[0]), static_cast<float>(v[1]), static_cast<float>(v[2]));
    return true;
}

bool toVec2(lua_State* L, int idx, glm::vec2& out) {
    idx = lua_absindex(L, idx);
    if (lua_type(L, idx) == LUA_TNUMBER) {
        out = glm::vec2(static_cast<float>(lua_tonumber(L, idx)));
        return true;
    }
    if (!lua_istable(L, idx)) return false;
    double v[2];
    static const char* keys[2] = {"u", "v"};
    for (int i = 0; i < 2; ++i)
        if (!numberField(L, idx, i + 1, keys[i], v[i]) || !std::isfinite(v[i])) return false;
    out = glm::vec2(static_cast<float>(v[0]), static_cast<float>(v[1]));
    return true;
}

std::optional<int> parseAxis(const std::string& s) {
    std::string a = lower(s);
    if (!a.empty() && (a[0] == '+' || a[0] == '-')) a = a.substr(1);
    if (a == "x") return 0;
    if (a == "y") return 1;
    if (a == "z") return 2;
    return std::nullopt;
}

void pushVec(lua_State* L, const glm::vec3& v) {
    lua_getglobal(L, "vec");
    lua_pushnumber(L, v.x);
    lua_pushnumber(L, v.y);
    lua_pushnumber(L, v.z);
    lua_call(L, 3, 1);
}

// =============================================================================
// Args: validated access to the table passed to a builder function
// =============================================================================
class Args {
public:
    Args(lua_State* L, const char* what, std::initializer_list<const char*> keys, int idx = 1)
        : m_L(L), m_what(what), m_idx(lua_absindex(L, idx)) {
        for (const char* k : keys) m_keys.emplace_back(k);
        if (!lua_istable(L, m_idx)) fail("expects a table argument, e.g. %s{ key = value, ... }", what);
    }

    [[noreturn]] void fail(const char* f, ...) const {
        char buf[1024];
        va_list ap;
        va_start(ap, f);
        std::vsnprintf(buf, sizeof(buf), f, ap);
        va_end(ap);
        throw ScriptError(m_what + std::string(": ") + buf);
    }

    bool has(const char* key) const {
        lua_getfield(m_L, m_idx, key);
        const bool r = !lua_isnil(m_L, -1);
        lua_pop(m_L, 1);
        return r;
    }

    std::optional<double> optNum(const char* key) const {
        lua_getfield(m_L, m_idx, key);
        std::optional<double> r;
        if (!lua_isnil(m_L, -1)) {
            if (lua_type(m_L, -1) != LUA_TNUMBER) { lua_pop(m_L, 1); fail("'%s' must be a number", key); }
            r = lua_tonumber(m_L, -1);
            if (!std::isfinite(*r)) { lua_pop(m_L, 1); fail("'%s' must be finite", key); }
        }
        lua_pop(m_L, 1);
        return r;
    }
    double num(const char* key, double def) const { return optNum(key).value_or(def); }
    double reqNum(const char* key) const {
        auto v = optNum(key);
        if (!v) fail("missing required number '%s'", key);
        return *v;
    }
    double positive(const char* key, double def) const {
        const double v = num(key, def);
        if (!(v > 0.0)) fail("'%s' must be > 0 (got %g)", key, v);
        return v;
    }
    int integer(const char* key, int def) const {
        const double v = num(key, def);
        if (std::floor(v) != v) fail("'%s' must be an integer (got %g)", key, v);
        return static_cast<int>(v);
    }

    std::optional<glm::vec3> optVec(const char* key) const {
        lua_getfield(m_L, m_idx, key);
        std::optional<glm::vec3> r;
        if (!lua_isnil(m_L, -1)) {
            glm::vec3 v;
            if (!toVec3(m_L, -1, v)) { lua_pop(m_L, 1); fail("'%s' must be a 3-vector: {x, y, z}, vec(x, y, z) or a number", key); }
            r = v;
        }
        lua_pop(m_L, 1);
        return r;
    }
    glm::vec3 vec(const char* key, const glm::vec3& def) const { return optVec(key).value_or(def); }
    glm::vec3 reqVec(const char* key) const {
        auto v = optVec(key);
        if (!v) fail("missing required vector '%s'", key);
        return *v;
    }

    std::optional<glm::vec2> optVec2(const char* key) const {
        lua_getfield(m_L, m_idx, key);
        std::optional<glm::vec2> r;
        if (!lua_isnil(m_L, -1)) {
            glm::vec2 v;
            if (!toVec2(m_L, -1, v)) { lua_pop(m_L, 1); fail("'%s' must be a 2-vector {u, v} (tangential coordinates)", key); }
            r = v;
        }
        lua_pop(m_L, 1);
        return r;
    }

    std::optional<std::string> optStr(const char* key) const {
        lua_getfield(m_L, m_idx, key);
        std::optional<std::string> r;
        if (!lua_isnil(m_L, -1)) {
            if (lua_type(m_L, -1) != LUA_TSTRING) { lua_pop(m_L, 1); fail("'%s' must be a string", key); }
            r = lua_tostring(m_L, -1);
        }
        lua_pop(m_L, 1);
        return r;
    }
    std::string str(const char* key, const std::string& def) const { return optStr(key).value_or(def); }

    bool boolean(const char* key, bool def) const {
        lua_getfield(m_L, m_idx, key);
        bool r = def;
        if (!lua_isnil(m_L, -1)) {
            if (!lua_isboolean(m_L, -1)) { lua_pop(m_L, 1); fail("'%s' must be true or false", key); }
            r = lua_toboolean(m_L, -1) != 0;
        }
        lua_pop(m_L, 1);
        return r;
    }

    int axis(const char* key, int def) const {
        lua_getfield(m_L, m_idx, key);
        int r = def;
        if (!lua_isnil(m_L, -1)) {
            std::optional<int> a;
            if (lua_type(m_L, -1) == LUA_TSTRING) a = parseAxis(lua_tostring(m_L, -1));
            else if (lua_type(m_L, -1) == LUA_TNUMBER) {
                const double v = lua_tonumber(m_L, -1);
                if (v == 0.0 || v == 1.0 || v == 2.0) a = static_cast<int>(v);
            }
            lua_pop(m_L, 1);
            if (!a) fail("'%s' must be an axis: \"x\", \"y\" or \"z\"", key);
            return *a;
        }
        lua_pop(m_L, 1);
        return r;
    }

    // "+x" / "-y" / "z" or a vector; returns a unit vector.
    std::optional<glm::vec3> optDirection(const char* key) const {
        lua_getfield(m_L, m_idx, key);
        std::optional<glm::vec3> r;
        if (!lua_isnil(m_L, -1)) {
            if (lua_type(m_L, -1) == LUA_TSTRING) {
                const std::string s = lua_tostring(m_L, -1);
                const auto a = parseAxis(s);
                lua_pop(m_L, 1);
                if (!a) fail("'%s' must be an axis (\"x\", \"-y\", ...) or a vector", key);
                glm::vec3 v(0.0f);
                v[*a] = (!s.empty() && s[0] == '-') ? -1.0f : 1.0f;
                return v;
            }
            glm::vec3 v;
            if (!toVec3(m_L, -1, v)) { lua_pop(m_L, 1); fail("'%s' must be an axis (\"x\", \"-y\", ...) or a vector", key); }
            if (glm::length(v) < 1e-6f) { lua_pop(m_L, 1); fail("'%s' must not be the zero vector", key); }
            r = glm::normalize(v);
        }
        lua_pop(m_L, 1);
        return r;
    }

    // Pushes the field onto the stack (caller pops); returns its Lua type.
    int push(const char* key) const {
        lua_getfield(m_L, m_idx, key);
        return lua_type(m_L, -1);
    }

    // Warns about keys the primitive does not understand (typo detection).
    void checkUnknown() const {
        lua_pushnil(m_L);
        while (lua_next(m_L, m_idx) != 0) {
            if (lua_type(m_L, -2) == LUA_TSTRING) {
                const std::string k = lua_tostring(m_L, -2);
                if (std::find(m_keys.begin(), m_keys.end(), k) == m_keys.end())
                    warn(m_L, fmt("%s: unknown field '%s' ignored%s", m_what.c_str(), k.c_str(),
                                  suggestion(k, m_keys).c_str()));
            } else {
                warn(m_L, fmt("%s: positional (unnamed) values are ignored; use key = value", m_what.c_str()));
            }
            lua_pop(m_L, 1);
        }
    }

    lua_State* L() const { return m_L; }
    int index() const { return m_idx; }
    const std::string& what() const { return m_what; }

private:
    lua_State* m_L;
    std::string m_what;
    int m_idx;
    std::vector<std::string> m_keys;
};

// =============================================================================
// Scene settings, units and materials
// =============================================================================
const std::map<std::string, MaterialSpec>& builtinMaterials() {
    static const std::map<std::string, MaterialSpec> lib = [] {
        std::map<std::string, MaterialSpec> m;
        auto add = [&m](const char* name, float eps, std::optional<double> sigmaNorm, std::optional<double> sigmaSI,
                        std::optional<double> tand) {
            MaterialSpec s;
            s.name = name;
            s.epsilonR = eps;
            s.sigma = sigmaNorm;
            s.conductivity = sigmaSI;
            s.lossTangent = tand;
            m[name] = s;
        };
        // name            eps_r  sigma(norm)   sigma(S/m)   tan(delta)
        add("vacuum",      1.0f,  0.0,          {},          {});
        add("air",         1.0f,  0.0,          {},          {});
        add("pec",         1.0f,  1.0e7,        {},          {});   // perfect electric conductor
        add("metal",       1.0f,  {},           5.8e7,       {});   // = copper
        add("copper",      1.0f,  {},           5.8e7,       {});
        add("silver",      1.0f,  {},           6.3e7,       {});
        add("gold",        1.0f,  {},           4.1e7,       {});
        add("aluminum",    1.0f,  {},           3.77e7,      {});
        add("brass",       1.0f,  {},           1.5e7,       {});
        add("steel",       1.0f,  {},           1.45e6,      {});   // stainless
        add("absorber",    1.0f,  0.3,          {},          {});   // generic lossy medium (normalised)
        add("ptfe",        2.1f,  {},           {},          2e-4); // Teflon
        add("teflon",      2.1f,  {},           {},          2e-4);
        add("polyethylene",2.25f, {},           {},          3e-4);
        add("rexolite",    2.53f, {},           {},          5e-4);
        add("polystyrene", 2.6f,  {},           {},          5e-4);
        add("acrylic",     2.6f,  {},           {},          8e-3);
        add("quartz",      3.8f,  {},           {},          1e-4);
        add("fr4",         4.4f,  {},           {},          0.02);
        add("glass",       6.3f,  {},           0.004,       {});   // window glass (ITU-R P.2040)
        add("alumina",     9.8f,  {},           {},          1e-4);
        add("silicon",     11.7f, 0.0,          {},          {});
        add("concrete",    5.24f, {},           0.05,        {});   // ITU-R P.2040 @ ~1 GHz
        add("brick",       3.91f, {},           0.024,       {});
        add("wood",        1.99f, {},           0.005,       {});
        add("drywall",     2.73f, {},           0.0085,      {});
        add("ground_dry",  4.0f,  {},           0.001,       {});
        add("ground_wet",  25.0f, {},           0.02,        {});
        add("water",       78.0f, {},           {},          0.1);
        add("seawater",    74.0f, {},           4.0,         {});
        return m;
    }();
    return lib;
}

Material resolve(const MaterialSpec& s) {
    const SceneSettings& st = ctx().settings;
    double sigma = 0.0;
    if (s.sigma) sigma = *s.sigma;
    else if (s.conductivity) sigma = *s.conductivity * st.cellSize * kEta0; // sigma / (eps0 c) * dx
    else if (s.lossTangent) sigma = *s.lossTangent * (glm::two_pi<double>() / st.wavelength) * s.epsilonR;
    return Material{static_cast<float>(sigma), std::max(s.epsilonR, 1.0f)};
}

MaterialSpec specFromTable(const Args& a, const std::string& name) {
    MaterialSpec s;
    s.name = name;
    s.epsilonR = static_cast<float>(a.num("epsilon_r", 1.0));
    if (s.epsilonR < 1.0f) a.fail("epsilon_r must be >= 1 (got %g)", s.epsilonR);
    s.sigma = a.optNum("sigma");
    s.conductivity = a.optNum("conductivity");
    s.lossTangent = a.optNum("loss_tangent");
    const int n = (s.sigma ? 1 : 0) + (s.conductivity ? 1 : 0) + (s.lossTangent ? 1 : 0);
    if (n > 1) a.fail("give only one of sigma, conductivity (S/m) or loss_tangent");
    if ((s.sigma && *s.sigma < 0) || (s.conductivity && *s.conductivity < 0) || (s.lossTangent && *s.lossTangent < 0))
        a.fail("losses must be >= 0");
    return s;
}

// Reads `material` (name or inline table) from a primitive's arguments.
Material readMaterial(const Args& a, const char* def) {
    lua_State* L = a.L();
    const int t = a.push("material");
    MaterialSpec spec;
    if (t == LUA_TNIL) {
        lua_pop(L, 1);
        spec = ctx().materials.at(def);
    } else if (t == LUA_TSTRING) {
        const std::string name = lua_tostring(L, -1);
        lua_pop(L, 1);
        auto it = ctx().materials.find(lower(name));
        if (it == ctx().materials.end()) {
            std::vector<std::string> names;
            for (const auto& kv : ctx().materials) names.push_back(kv.first);
            a.fail("unknown material '%s'%s. Define it with material{ name = \"%s\", epsilon_r = ..., ... }",
                   name.c_str(), suggestion(name, names).c_str(), name.c_str());
        }
        spec = it->second;
    } else if (t == LUA_TTABLE) {
        Args m(L, "material (inline)", {"epsilon_r", "sigma", "conductivity", "loss_tangent", "name"}, -1);
        spec = specFromTable(m, m.str("name", "inline material"));
        m.checkUnknown();
        lua_pop(L, 1);
    } else {
        lua_pop(L, 1);
        a.fail("'material' must be a material name or a table { epsilon_r = ..., conductivity = ... }");
    }
    if (spec.epsilonR > 1.0f) {
        float& e = ctx().usedDielectrics[spec.name];
        e = std::max(e, spec.epsilonR);
    }
    return resolve(spec);
}

void lock(lua_State* L);

EnvironmentBuilder& builder(lua_State* L) {
    lock(L);
    return *ctx().builder;
}

// Wavelength in cells from `wavelength` (cells) or `frequency` (Hz); scene default otherwise.
float readWavelength(const Args& a) {
    const auto wl = a.optNum("wavelength");
    const auto f = a.optNum("frequency");
    if (wl && f) a.fail("give either wavelength (cells) or frequency (Hz), not both");
    if (f) {
        if (*f <= 0.0) a.fail("frequency must be > 0");
        return static_cast<float>(kC0 / (*f * ctx().settings.cellSize));
    }
    if (wl) {
        if (*wl < 2.0) a.fail("wavelength must be >= 2 cells (got %g)", *wl);
        return static_cast<float>(*wl);
    }
    return ctx().settings.wavelength;
}

// min/max or center/size -> (min, max)
std::pair<glm::vec3, glm::vec3> readBox(const Args& a) {
    const bool mm = a.has("min") || a.has("max");
    const bool cs = a.has("center") || a.has("size");
    if (mm && cs) a.fail("give either min/max or center/size, not both");
    if (mm) {
        const glm::vec3 mn = a.reqVec("min"), mx = a.reqVec("max");
        return {glm::min(mn, mx), glm::max(mn, mx)};
    }
    if (!a.has("size")) a.fail("needs min/max or center/size");
    const glm::vec3 c = a.vec("center", glm::vec3(ctx().settings.size) * 0.5f);
    const glm::vec3 s = glm::abs(a.reqVec("size"));
    return {c - 0.5f * s, c + 0.5f * s};
}

std::optional<glm::mat3> readRotation(const Args& a) {
    auto r = a.optVec("rotate");
    if (!r || glm::length(*r) == 0.0f) return std::nullopt;
    return rotationFromEulerDegrees(*r);
}

void fillReport(lua_State* L, const Args& a, size_t cells) {
    if (cells == 0) warn(L, a.what() + ": shape is empty or lies completely outside the grid");
}

// =============================================================================
// scene / view / material / param
// =============================================================================
void setGridGlobals(lua_State* L) {
    const SceneSettings& s = ctx().settings;
    lua_getglobal(L, "grid");
    const int g = lua_gettop(L);
    const glm::vec3 n(s.size);
    pushVec(L, n); lua_setfield(L, g, "size");
    pushVec(L, 0.5f * n); lua_setfield(L, g, "center");
    pushVec(L, glm::vec3(static_cast<float>(s.pml))); lua_setfield(L, g, "interior_min");
    pushVec(L, n - static_cast<float>(s.pml)); lua_setfield(L, g, "interior_max");
    lua_pushinteger(L, s.size.x); lua_setfield(L, g, "nx");
    lua_pushinteger(L, s.size.y); lua_setfield(L, g, "ny");
    lua_pushinteger(L, s.size.z); lua_setfield(L, g, "nz");
    lua_pushinteger(L, s.pml); lua_setfield(L, g, "pml");
    lua_pushnumber(L, s.wavelength); lua_setfield(L, g, "wavelength");
    lua_pushnumber(L, s.cellSize); lua_setfield(L, g, "cell_size");
    lua_pushnumber(L, s.courant); lua_setfield(L, g, "dt");
    lua_pop(L, 1);
}

void lock(lua_State* L) {
    Context& c = ctx();
    if (c.locked) return;
    c.locked = true;
    GridConfig g;
    g.size = c.settings.size;
    g.courant = c.settings.courant;
    g.pmlThickness = c.settings.pml;
    g.pmlSigmaMax = c.settings.pmlSigmaMax;
    g.pmlGrading = c.settings.pmlGrading;
    c.builder = std::make_unique<EnvironmentBuilder>(c.settings.name, g);
    Environment& env = c.builder->env();
    env.description = c.settings.description;
    env.cellSize = c.settings.cellSize;
    env.referenceWavelength = c.settings.wavelength;
    setGridGlobals(L);
}

int api_scene(lua_State* L) {
    Args a(L, "scene", {"name", "description", "size", "cell_size", "wavelength", "frequency", "courant", "pml",
                        "output_dir"});
    if (ctx().locked)
        a.fail("scene{} must be the first call in the script (before any objects, sources or use of 'grid')");
    SceneSettings& s = ctx().settings;
    s.name = a.str("name", s.name);
    s.description = a.str("description", "");
    s.outputDir = a.str("output_dir", "");
    s.cellSize = static_cast<float>(a.positive("cell_size", s.cellSize));
    s.courant = static_cast<float>(a.positive("courant", s.courant));
    if (s.courant >= 0.577f) a.fail("courant must be < 1/sqrt(3) ~ 0.577 for stability (got %g)", s.courant);

    if (auto v = a.optVec("size")) {
        const glm::vec3 r = glm::round(*v);
        if (glm::any(glm::lessThan(r, glm::vec3(16.0f)))) a.fail("size must be at least 16 cells per axis");
        s.size = glm::ivec3(r);
    }
    const int t = a.push("pml");
    if (t == LUA_TNUMBER) {
        s.pml = static_cast<int>(lua_tonumber(L, -1));
        lua_pop(L, 1);
    } else if (t == LUA_TTABLE) {
        Args p(L, "scene.pml", {"thickness", "sigma_max", "grading"}, -1);
        s.pml = p.integer("thickness", s.pml);
        s.pmlSigmaMax = static_cast<float>(p.num("sigma_max", s.pmlSigmaMax));
        s.pmlGrading = static_cast<float>(p.num("grading", s.pmlGrading));
        p.checkUnknown();
        lua_pop(L, 1);
    } else {
        lua_pop(L, 1);
        if (t != LUA_TNIL) a.fail("'pml' must be a number (thickness) or { thickness, sigma_max, grading }");
    }
    if (s.pml < 0 || s.pml > 64) a.fail("pml thickness must be within 0..64 cells");
    if (glm::any(glm::lessThan(s.size, glm::ivec3(2 * s.pml + 8))))
        a.fail("size must exceed 2 * pml + 8 = %d cells per axis", 2 * s.pml + 8);

    s.wavelength = readWavelength(a);
    const double mem = 48.0 * s.size.x * s.size.y * s.size.z / (1024.0 * 1024.0); // E, B, edge materials
    if (mem > 2048.0) warn(L, fmt("scene: grid needs %.0f MB of GPU memory", mem));
    if (s.wavelength < 10.0f)
        warn(L, fmt("scene: reference wavelength is only %.1f cells; >= 12 cells per wavelength keeps numerical "
                    "dispersion small", s.wavelength));
    a.checkUnknown();
    lock(L);
    return 0;
}

int api_view(lua_State* L) {
    Args a(L, "view", {"mode", "component", "gain", "gain_e", "gain_b", "exposure", "density", "step",
                       "steps_per_frame", "distance", "yaw", "pitch", "target", "overlay"});
    ViewHints& v = ctx().view;
    if (auto m = a.optStr("mode")) {
        static const std::vector<std::string> modes = {"fields", "e", "b", "poynting", "signed"};
        auto it = std::find(modes.begin(), modes.end(), lower(*m));
        if (it == modes.end())
            a.fail("mode must be one of \"fields\", \"e\", \"b\", \"poynting\", \"signed\"%s",
                   suggestion(*m, modes).c_str());
        v.mode = static_cast<int>(it - modes.begin());
    }
    if (a.has("component")) v.signedAxis = a.axis("component", 2);
    if (auto g = a.optNum("gain")) v.gainE = v.gainB = static_cast<float>(*g);
    if (auto g = a.optNum("gain_e")) v.gainE = static_cast<float>(*g);
    if (auto g = a.optNum("gain_b")) v.gainB = static_cast<float>(*g);
    if (auto x = a.optNum("exposure")) v.exposure = static_cast<float>(*x);
    if (auto x = a.optNum("density")) v.density = static_cast<float>(*x);
    if (auto x = a.optNum("step")) v.stepVoxels = static_cast<float>(*x);
    if (a.has("steps_per_frame")) v.stepsPerFrame = std::clamp(a.integer("steps_per_frame", 2), 1, 64);
    v.cameraDistance = static_cast<float>(a.num("distance", v.cameraDistance));
    v.cameraYawDeg = static_cast<float>(a.num("yaw", v.cameraYawDeg));
    v.cameraPitchDeg = static_cast<float>(a.num("pitch", v.cameraPitchDeg));
    if (auto t = a.optVec("target")) v.target = *t;
    if (a.has("overlay")) v.overlay = a.boolean("overlay", true);
    a.checkUnknown();
    return 0;
}

int api_material(lua_State* L) {
    Args a(L, "material", {"name", "epsilon_r", "sigma", "conductivity", "loss_tangent"});
    const auto name = a.optStr("name");
    if (!name || name->empty()) a.fail("needs a name, e.g. material{ name = \"foam\", epsilon_r = 1.1 }");
    MaterialSpec s = specFromTable(a, *name);
    const std::string key = lower(*name);
    if (builtinMaterials().count(key)) warn(L, fmt("material: '%s' overrides the built-in material", name->c_str()));
    ctx().materials[key] = s;
    a.checkUnknown();
    lua_pushstring(L, name->c_str());
    return 1;
}

char readKey(const Args& a, lua_State* L, int idx, const char* what) {
    if (lua_type(L, idx) != LUA_TSTRING || std::strlen(lua_tostring(L, idx)) != 1)
        a.fail("%s must be a single-character string", what);
    const char k = static_cast<char>(std::tolower(static_cast<unsigned char>(lua_tostring(L, idx)[0])));
    if (!std::strchr(ScriptEngine::kParamKeys, k))
        a.fail("key '%c' is used by the application; parameter keys must be one of: %s", k, ScriptEngine::kParamKeys);
    for (const ScriptParam& p : ctx().params)
        if (p.keyToggle == k || p.keyDecrease == k || p.keyIncrease == k)
            a.fail("key '%c' is already bound to parameter '%s'", k, p.name.c_str());
    return k;
}

int api_param(lua_State* L) {
    Args a(L, "param", {"name", "label", "default", "min", "max", "step", "key", "keys", "choices", "wrap"});
    ScriptParam p;
    const auto name = a.optStr("name");
    if (!name || name->empty()) a.fail("needs a name");
    for (const ScriptParam& q : ctx().params)
        if (q.name == *name) a.fail("parameter '%s' is declared twice", name->c_str());
    p.name = *name;
    p.label = a.str("label", p.name);

    const auto it = ctx().overrides.find(p.name);
    const std::optional<std::string> override =
        it != ctx().overrides.end() ? std::optional<std::string>(it->second) : std::nullopt;

    if (a.push("choices") == LUA_TTABLE) {
        p.type = ScriptParam::Type::Choice;
        const lua_Integer n = static_cast<lua_Integer>(lua_rawlen(L, -1));
        for (lua_Integer i = 1; i <= n; ++i) {
            lua_rawgeti(L, -1, i);
            if (lua_type(L, -1) != LUA_TSTRING) { lua_pop(L, 2); a.fail("choices must be a list of strings"); }
            p.choices.emplace_back(lua_tostring(L, -1));
            lua_pop(L, 1);
        }
        lua_pop(L, 1);
        if (p.choices.empty()) a.fail("choices must not be empty");
        const std::string def = a.str("default", p.choices.front());
        auto pick = [&p](const std::string& v) { return std::find(p.choices.begin(), p.choices.end(), v); };
        if (pick(def) == p.choices.end()) a.fail("default '%s' is not one of the choices", def.c_str());
        auto chosen = override ? pick(*override) : p.choices.end();
        p.choiceIndex = static_cast<size_t>((chosen != p.choices.end() ? chosen : pick(def)) - p.choices.begin());
    } else {
        lua_pop(L, 1);
        if (a.push("default") == LUA_TBOOLEAN) {
            p.type = ScriptParam::Type::Bool;
            p.defaultValue = lua_toboolean(L, -1) ? 1.0 : 0.0;
            lua_pop(L, 1);
            p.value = p.defaultValue;
            if (override) p.value = (lower(*override) == "true" || *override == "1" || lower(*override) == "on") ? 1.0 : 0.0;
        } else {
            lua_pop(L, 1);
            p.type = ScriptParam::Type::Number;
            p.defaultValue = a.reqNum("default");
            p.min = a.num("min", -1e300);
            p.max = a.num("max", 1e300);
            p.step = a.positive("step", 1.0);
            p.wrap = a.boolean("wrap", false);
            if (p.min > p.max) a.fail("min must be <= max");
            p.value = p.defaultValue;
            if (override) {
                char* end = nullptr;
                const double v = std::strtod(override->c_str(), &end);
                if (end != override->c_str()) p.value = v;
            }
            p.value = std::clamp(p.value, p.min, p.max);
        }
    }

    // Key bindings
    if (a.push("key") != LUA_TNIL) {
        if (p.type == ScriptParam::Type::Number) { lua_pop(L, 1); a.fail("numeric parameters use keys = {\"decrease\", \"increase\"}"); }
        p.keyToggle = readKey(a, L, -1, "key");
    }
    lua_pop(L, 1);
    if (a.push("keys") != LUA_TNIL) {
        if (p.type != ScriptParam::Type::Number || !lua_istable(L, -1) || lua_rawlen(L, -1) != 2) {
            lua_pop(L, 1);
            a.fail("keys = {\"decrease\", \"increase\"} is for numeric parameters");
        }
        lua_rawgeti(L, -1, 1);
        p.keyDecrease = readKey(a, L, -1, "keys[1]");
        lua_pop(L, 1);
        lua_rawgeti(L, -1, 2);
        p.keyIncrease = readKey(a, L, -1, "keys[2]");
        lua_pop(L, 1);
    }
    lua_pop(L, 1);
    a.checkUnknown();

    switch (p.type) {
        case ScriptParam::Type::Bool:   lua_pushboolean(L, p.value != 0.0); break;
        case ScriptParam::Type::Number: lua_pushnumber(L, p.value); break;
        case ScriptParam::Type::Choice: lua_pushstring(L, p.choices[p.choiceIndex].c_str()); break;
    }
    ctx().params.push_back(std::move(p));
    return 1;
}

// =============================================================================
// Geometry
// =============================================================================
// A radar target: its bounds and the centres of its cells (subsampled for big objects), so the
// radar can compare detections with the true nearest surface and direction.
TargetInfo targetInfo(const Shape& shape, const std::string& name) {
    TargetInfo t;
    t.name = name;
    const AABB b = shape.bounds();
    const glm::ivec3 n = ctx().settings.size;
    const glm::ivec3 lo = glm::clamp(glm::ivec3(glm::floor(b.min)), glm::ivec3(0), n - 1);
    const glm::ivec3 hi = glm::clamp(glm::ivec3(glm::ceil(b.max)), glm::ivec3(0), n - 1);
    const glm::vec3 span = glm::vec3(hi - lo + 1);
    const int stride = std::max(1, static_cast<int>(std::ceil(std::cbrt(span.x * span.y * span.z / 200000.0f))));
    glm::vec3 mn(1e30f), mx(-1e30f);
    for (int z = lo.z; z <= hi.z; z += stride)
        for (int y = lo.y; y <= hi.y; y += stride)
            for (int x = lo.x; x <= hi.x; x += stride) {
                const glm::vec3 p = glm::vec3(x, y, z) + 0.5f;
                if (!shape.contains(p)) continue;
                t.points.push_back(p);
                mn = glm::min(mn, p - 0.5f);
                mx = glm::max(mx, p + 0.5f);
            }
    if (t.points.empty()) {
        t.min = b.min;
        t.max = b.max;
    } else {
        t.min = mn;
        t.max = mx;
    }
    return t;
}

size_t fillShape(lua_State* L, const Args& a, ShapePtr local, const glm::vec3& center, const Material& m) {
    ShapePtr shape = local;
    if (auto rot = readRotation(a)) shape = std::make_shared<TransformedShape>(local, center, *rot);
    else shape = std::make_shared<TransformedShape>(local, center, glm::mat3(1.0f));
    // Radar targets are recorded (for the truth comparison) and left out of the calibration run.
    if (a.boolean("target", false)) {
        builder(L).env().targets.push_back(targetInfo(*shape, a.str("name", a.what())));
        if (!ctx().withTargets) return 0;
    }
    const size_t n = builder(L).fill(*shape, m);
    fillReport(L, a, n);
    return n;
}

int api_box(lua_State* L) {
    Args a(L, "box", {"min", "max", "center", "size", "material", "rotate", "thickness", "name", "target"});
    const auto [mn, mx] = readBox(a);
    const Material m = readMaterial(a, "metal");
    const glm::vec3 c = 0.5f * (mn + mx), h = 0.5f * (mx - mn);
    ShapePtr local = std::make_shared<BoxShape>(-h, h);
    if (auto t = a.optNum("thickness")) {
        if (*t <= 0.0) a.fail("thickness must be > 0");
        const glm::vec3 hi = glm::max(h - static_cast<float>(*t), glm::vec3(0.0f));
        local = std::make_shared<DifferenceShape>(local, std::make_shared<BoxShape>(-hi, hi));
    }
    fillShape(L, a, local, c, m);
    a.checkUnknown();
    return 0;
}

int api_sphere(lua_State* L) {
    Args a(L, "sphere", {"center", "radius", "material", "rotate", "thickness", "name", "target"});
    const glm::vec3 c = a.vec("center", glm::vec3(ctx().settings.size) * 0.5f);
    const glm::vec3 r = a.reqVec("radius"); // number -> sphere, vector -> ellipsoid
    if (glm::any(glm::lessThanEqual(r, glm::vec3(0.0f)))) a.fail("radius must be > 0");
    const Material m = readMaterial(a, "metal");
    ShapePtr local = std::make_shared<EllipsoidShape>(glm::vec3(0.0f), r);
    if (auto t = a.optNum("thickness")) {
        if (*t <= 0.0) a.fail("thickness must be > 0");
        const glm::vec3 ri = r - static_cast<float>(*t);
        if (glm::all(glm::greaterThan(ri, glm::vec3(0.0f))))
            local = std::make_shared<DifferenceShape>(local, std::make_shared<EllipsoidShape>(glm::vec3(0.0f), ri));
    }
    fillShape(L, a, local, c, m);
    a.checkUnknown();
    return 0;
}

int api_cylinder(lua_State* L) {
    Args a(L, "cylinder", {"center", "axis", "radius", "length", "material", "rotate", "thickness", "inner_radius",
                           "capped", "name", "target"});
    const glm::vec3 c = a.vec("center", glm::vec3(ctx().settings.size) * 0.5f);
    const int axis = a.axis("axis", 2);
    const float r = static_cast<float>(a.positive("radius", 1.0));
    const float len = static_cast<float>(a.positive("length", 1.0));
    const Material m = readMaterial(a, "metal");
    ShapePtr local = std::make_shared<CylinderShape>(glm::vec3(0.0f), axis, r, 0.5f * len);
    const auto t = a.optNum("thickness");
    const auto ri = a.optNum("inner_radius");
    if (t && ri) a.fail("give thickness or inner_radius, not both");
    if (t || ri) {
        const float inner = ri ? static_cast<float>(*ri) : r - static_cast<float>(*t);
        if (inner <= 0.0f || inner >= r) a.fail("inner radius must be within (0, radius)");
        const bool capped = a.boolean("capped", false);
        const float wall = r - inner;
        const float innerHalf = capped ? 0.5f * len - wall : 0.5f * len + 1.0f; // open tube by default
        local = std::make_shared<DifferenceShape>(
            local, std::make_shared<CylinderShape>(glm::vec3(0.0f), axis, inner, std::max(innerHalf, 0.0f)));
    }
    fillShape(L, a, local, c, m);
    a.checkUnknown();
    return 0;
}

std::vector<WallShape::Aperture> readApertures(const Args& a, int axis) {
    lua_State* L = a.L();
    std::vector<WallShape::Aperture> out;
    const glm::vec3 n(ctx().settings.size);
    const int u = axis == 0 ? 1 : 0, v = axis == 2 ? 1 : 2;
    const glm::vec2 mid(0.5f * n[u], 0.5f * n[v]);

    if (a.push("apertures") == LUA_TTABLE) {
        const lua_Integer count = static_cast<lua_Integer>(lua_rawlen(L, -1));
        for (lua_Integer i = 1; i <= count; ++i) {
            lua_rawgeti(L, -1, i);
            Args ap(L, "wall.apertures[]", {"center", "size", "radius"}, -1);
            WallShape::Aperture w;
            w.center = ap.optVec2("center").value_or(mid);
            if (auto r = ap.optNum("radius")) {
                w.circular = true;
                w.radius = static_cast<float>(*r);
            } else if (auto s = ap.optVec2("size")) {
                w.halfSize = 0.5f * glm::abs(*s);
            } else {
                lua_pop(L, 2);
                a.fail("each aperture needs size = {w, h} or radius = r");
            }
            ap.checkUnknown();
            lua_pop(L, 1);
            out.push_back(w);
        }
    }
    lua_pop(L, 1);

    if (a.push("slits") == LUA_TTABLE) {
        Args s(L, "wall.slits", {"count", "width", "spacing", "length", "direction", "center"}, -1);
        const int count = s.integer("count", 2);
        const float width = static_cast<float>(s.positive("width", 6.0));
        const float spacing = static_cast<float>(s.num("spacing", 4.0 * width));
        const float length = static_cast<float>(s.num("length", 1.0e6));
        const int dir = s.axis("direction", v);
        const glm::vec2 center = s.optVec2("center").value_or(mid);
        s.checkUnknown();
        lua_pop(L, 1);
        if (dir == axis) a.fail("slits.direction must lie in the wall plane");
        if (count < 1) a.fail("slits.count must be >= 1");
        // Slits are long along `dir` and repeat along the other tangential axis.
        const bool longU = dir == u;
        for (int i = 0; i < count; ++i) {
            const float offset = (static_cast<float>(i) - 0.5f * static_cast<float>(count - 1)) * spacing;
            WallShape::Aperture w;
            w.center = center + (longU ? glm::vec2(0.0f, offset) : glm::vec2(offset, 0.0f));
            w.halfSize = longU ? glm::vec2(0.5f * length, 0.5f * width) : glm::vec2(0.5f * width, 0.5f * length);
            out.push_back(w);
        }
    } else {
        lua_pop(L, 1);
    }
    return out;
}

int api_wall(lua_State* L) {
    Args a(L, "wall", {"axis", "position", "thickness", "material", "apertures", "slits", "name"});
    const int axis = a.axis("axis", 0);
    const float pos = static_cast<float>(a.reqNum("position"));
    const float t = static_cast<float>(a.positive("thickness", 2.0));
    const Material m = readMaterial(a, "metal");
    auto apertures = readApertures(a, axis);
    lock(L);
    const WallShape wall(axis, pos, pos + t, std::move(apertures), ctx().settings.size);
    fillReport(L, a, builder(L).fill(wall, m));
    a.checkUnknown();
    return 0;
}

int api_grating(lua_State* L) {
    Args a(L, "grating", {"axis", "position", "thickness", "period", "width", "direction", "min", "max", "material",
                          "name"});
    const int axis = a.axis("axis", 0);
    const float x0 = static_cast<float>(a.reqNum("position"));
    const float t = static_cast<float>(a.positive("thickness", 1.0));
    const float period = static_cast<float>(a.positive("period", 4.0));
    const float width = static_cast<float>(a.positive("width", 1.0));
    if (width >= period) a.fail("width must be smaller than period");
    const int u = axis == 0 ? 1 : 0, v = axis == 2 ? 1 : 2;
    const int dir = a.axis("direction", v); // bar long axis
    if (dir == axis) a.fail("direction (bar axis) must lie in the grating plane");
    const int across = dir == u ? v : u;
    const glm::vec3 n(ctx().settings.size);
    const glm::vec2 lo = a.optVec2("min").value_or(glm::vec2(0.0f));
    const glm::vec2 hi = a.optVec2("max").value_or(glm::vec2(n[u], n[v]));
    const Material m = readMaterial(a, "metal");

    AABB b;
    b.min = glm::vec3(0.0f);
    b.max = n;
    b.min[axis] = x0;
    b.max[axis] = x0 + t;
    b.min[u] = lo.x; b.max[u] = hi.x;
    b.min[v] = lo.y; b.max[v] = hi.y;
    const float start = b.min[across];
    const LambdaShape shape(b, [=](const glm::vec3& p) {
        if (glm::any(glm::lessThan(p, b.min)) || glm::any(glm::greaterThanEqual(p, b.max))) return false;
        const float q = std::fmod(p[across] - start, period);
        return q >= 0.0f && q < width;
    });
    fillReport(L, a, builder(L).fill(shape, m));
    a.checkUnknown();
    return 0;
}

int api_cage(lua_State* L) {
    Args a(L, "cage", {"min", "max", "center", "size", "spacing", "bar", "material", "name"});
    const auto [mn, mx] = readBox(a);
    const int spacing = a.integer("spacing", 4);
    const int bar = a.integer("bar", 2);
    if (bar < 1 || spacing <= bar) a.fail("need bar >= 1 and spacing > bar");
    const Material m = readMaterial(a, "metal");
    fillReport(L, a, builder(L).meshCage(glm::ivec3(glm::round(mn)), glm::ivec3(glm::round(mx)) - 1, spacing, bar, m));
    a.checkUnknown();
    return 0;
}

int api_frame(lua_State* L) {
    Args a(L, "frame", {"min", "max", "center", "size", "bar", "material", "name"});
    const auto [mn, mx] = readBox(a);
    const float bar = static_cast<float>(a.positive("bar", 2.0));
    const Material m = readMaterial(a, "metal");
    fillReport(L, a, builder(L).wireframeBox(mn, mx, bar, m));
    a.checkUnknown();
    return 0;
}

int api_shape(lua_State* L) {
    Args a(L, "shape", {"min", "max", "center", "size", "inside", "material", "name", "target"});
    const auto [mn, mx] = readBox(a);
    if (a.push("inside") != LUA_TFUNCTION) {
        lua_pop(L, 1);
        a.fail("needs inside = function(x, y, z) return <boolean> end");
    }
    const int ref = luaL_ref(L, LUA_REGISTRYINDEX); // pops the function
    const Material m = readMaterial(a, "metal");
    const LambdaShape shape({mn, mx}, [L, ref, mn, mx](const glm::vec3& p) {
        if (glm::any(glm::lessThan(p, mn)) || glm::any(glm::greaterThanEqual(p, mx))) return false;
        lua_rawgeti(L, LUA_REGISTRYINDEX, ref);
        lua_pushnumber(L, p.x);
        lua_pushnumber(L, p.y);
        lua_pushnumber(L, p.z);
        if (lua_pcall(L, 3, 1, 0) != LUA_OK) {
            std::string err = lua_tostring(L, -1) ? lua_tostring(L, -1) : "error";
            lua_pop(L, 1);
            throw ScriptError("shape.inside(): " + err);
        }
        const bool in = lua_toboolean(L, -1) != 0;
        lua_pop(L, 1);
        return in;
    });
    if (a.boolean("target", false)) {
        builder(L).env().targets.push_back(targetInfo(shape, a.str("name", a.what())));
        if (!ctx().withTargets) {
            luaL_unref(L, LUA_REGISTRYINDEX, ref);
            a.checkUnknown();
            return 0;
        }
    }
    size_t n = 0;
    try {
        n = builder(L).fill(shape, m);
    } catch (...) {
        luaL_unref(L, LUA_REGISTRYINDEX, ref);
        throw;
    }
    luaL_unref(L, LUA_REGISTRYINDEX, ref);
    fillReport(L, a, n);
    a.checkUnknown();
    return 0;
}

// =============================================================================
// Sources
// =============================================================================
#define WAVEFORM_KEYS "wavelength", "frequency", "phase", "waveform", "ramp", "pulse_width", "interval", "delay", "enabled"

void readWaveform(const Args& a, SourceDesc& s) {
    s.wavelength = readWavelength(a);
    s.phase = static_cast<float>(glm::radians(a.num("phase", 0.0)));
    const std::string wf = lower(a.str("waveform", "cw"));
    if (wf == "cw" || wf == "continuous" || wf == "sine") {
        s.waveform = Waveform::ContinuousWave;
        s.rampPeriods = static_cast<float>(a.num("ramp", 3.0));
        if (a.has("pulse_width")) warn(a.L(), a.what() + ": pulse_width is ignored for waveform = \"cw\"");
    } else if (wf == "pulse" || wf == "gaussian") {
        s.waveform = Waveform::GaussianPulse;
        s.rampPeriods = static_cast<float>(a.positive("pulse_width", 1.0));
        if (a.has("ramp")) warn(a.L(), a.what() + ": ramp is ignored for waveform = \"pulse\" (use pulse_width)");
    } else {
        a.fail("waveform must be \"cw\" or \"pulse\" (got \"%s\")", wf.c_str());
    }
    s.repeatPeriods = static_cast<float>(a.num("interval", 0.0)); // pulse repetition interval
    s.delayPeriods = static_cast<float>(a.num("delay", 0.0));
    s.enabled = a.boolean("enabled", true);
    if (s.wavelength < 8.0f)
        warn(a.L(), fmt("%s: wavelength %.1f cells is under-resolved (< 8 cells)", a.what().c_str(), s.wavelength));
}

void addSource(lua_State* L, const SourceDesc& s) {
    if (s.enabled) builder(L).env().sources.push_back(s);
}

// Axis-aligned orientation required for extended wires (footprints are axis-aligned boxes).
int wireAxis(const Args& a, const glm::vec3& dir) {
    const glm::vec3 d = glm::abs(dir);
    const int ax = (d.x >= d.y && d.x >= d.z) ? 0 : (d.y >= d.z ? 1 : 2);
    if (d[ax] < 0.9999f) a.fail("extended (length > 0) wires must be aligned with x, y or z; use length = 0 for "
                                "an arbitrarily oriented point dipole");
    return ax;
}

// -----------------------------------------------------------------------------
// Antennas: conductors + feeds.
//
// Conductor models:
//   "thin"    (default) thin wire of a given radius on a line of Yee edges, using the
//             Umashankar-Taflove sub-cell model: the wire's edges are perfect conductors
//             and the four B components circling each edge get the factor 2 / ln(dx / a).
//   "rod"     solid 1 x 1-cell metal conductor (voxels), electrically a fat wire.
//   "current" ideal impressed current (no metal), for theory checks.
// Feeds (thin / rod): a resistive voltage source (default) or an ideal current source in
// a one-edge (thin) or one-cell (rod) gap. Every feed is also a measurement port.
// -----------------------------------------------------------------------------
enum class Conductor { Thin, Rod, Current };

Conductor readConductor(const Args& a) {
    const std::string m = lower(a.str("model", "thin"));
    if (m == "thin" || m == "wire") return Conductor::Thin;
    if (m == "rod") return Conductor::Rod;
    if (m == "current" || m == "ideal") return Conductor::Current;
    a.fail("model must be \"thin\" (thin wire, default), \"rod\" (1x1-cell metal conductor) or \"current\" "
           "(ideal uniform current)");
}

struct FeedSpec {
    bool voltage = true;
    float impedanceOhm = 50.0f;
    float drive = 1.0f; // peak EMF (voltage) or peak current (current), normalized
};

FeedSpec readFeed(const Args& a) {
    FeedSpec f;
    const std::string kind = lower(a.str("feed", "voltage"));
    if (kind == "voltage") f.voltage = true;
    else if (kind == "current") f.voltage = false;
    else a.fail("feed must be \"voltage\" (resistive voltage source, default) or \"current\" (ideal current source)");
    f.impedanceOhm = static_cast<float>(a.positive("impedance", 50.0));
    if (!f.voltage && a.has("impedance")) warn(a.L(), a.what() + ": 'impedance' only applies to feed = \"voltage\"");
    f.drive = static_cast<float>(a.num("amplitude", 1.0));
    return f;
}

// Thin-wire radius in cells (default lambda / 400, a typical thin antenna wire).
float readRadius(const Args& a, float wavelength) {
    const float r = static_cast<float>(a.num("radius", wavelength / 400.0));
    if (!(r > 0.0f)) a.fail("radius must be > 0");
    if (r > 0.4f)
        a.fail("radius %.2f cells is too thick for the thin-wire model (max 0.4 cells); use model = \"rod\" for "
               "a 1x1-cell conductor or refine the grid", r);
    return r;
}

glm::ivec3 cellOf(const glm::vec3& p) { return glm::ivec3(glm::floor(p)); }
glm::ivec3 nodeOf(const glm::vec3& p) { return glm::ivec3(glm::round(p)); }

// 1x1-cell rod through `cell` (transverse position) covering cells [from, to) along `axis`.
size_t fillRod(lua_State* L, const glm::ivec3& cell, int axis, int from, int to, const Material& m) {
    if (to <= from) return 0;
    glm::vec3 lo(cell), hi = glm::vec3(cell) + 1.0f;
    lo[axis] = static_cast<float>(from);
    hi[axis] = static_cast<float>(to);
    return builder(L).box(lo, hi, m);
}

// Thin wire along `axis` on the lattice line through `node` (its other two coordinates),
// covering the edges [i0, i1). Edge `gapEdge` is left for a feed. Recorded here and emitted
// by emitThinWires() once the whole scene is known (free ends get the end correction).
void addThinWire(lua_State* L, glm::ivec3 node, int axis, int i0, int i1, float radius, int gapEdge = INT_MIN) {
    if (i1 <= i0) return;
    ThinWireSpec w;
    w.node = node;
    w.axis = axis;
    w.i0 = i0;
    w.i1 = i1;
    w.radius = radius;
    w.gapEdge = gapEdge;
    ctx().thinWires.push_back(w);

    WireSegment seg; // drawn at the requested (electrical) length
    seg.a = glm::vec3(node);
    seg.b = glm::vec3(node);
    seg.a[axis] = static_cast<float>(i0);
    seg.b[axis] = static_cast<float>(i1);
    seg.radius = radius;
    builder(L).env().wires.push_back(seg);
}

// Feed + port. Thin wires: the single gap edge owned by `texel`. Rods: the four corner
// edges of gap cell `texel`, which carry the rod's current.
void addFeed(lua_State* L, const SourceDesc& wave, const glm::ivec3& texel, int axis, float sign, int edges,
             const FeedSpec& feed, const std::string& name) {
    Environment& env = builder(L).env();
    const int u = (axis + 1) % 3, v = (axis + 2) % 3;
    glm::ivec3 eu(0), ev(0);
    eu[u] = 1;
    ev[v] = 1;

    SourceDesc s = wave;
    s.softness = glm::vec3(0.25f);
    s.polarization = glm::vec3(0.0f);
    s.polarization[axis] = sign;
    if (edges == 4) {
        s.center = glm::vec3(texel) + 0.5f;
        s.halfExtent = glm::vec3(0.5f);
        s.halfExtent[axis] = 0.0f;
    } else {
        s.center = glm::vec3(texel);
        s.center[axis] += 0.5f;
        s.halfExtent = glm::vec3(0.0f);
    }

    if (feed.voltage) {
        // Resistive voltage source (EMF V_s behind R_s) as its Norton equivalent: each of the
        // `edges` parallel gap edges gets conductance 1 / (edges R_s) and an impressed
        // current -V_s / (edges R_s), so the open-circuit gap voltage is V_s.
        const float sigma = kEta0f / (static_cast<float>(edges) * feed.impedanceOhm);
        const glm::ivec3 offs[4] = {glm::ivec3(0), eu, ev, eu + ev};
        for (int i = 0; i < edges; ++i) env.subcell.edges.push_back({texel + offs[i], axis, sigma});
        s.amplitude = -sigma * feed.drive;
    } else {
        s.amplitude = feed.drive / static_cast<float>(edges);
    }
    addSource(L, s);

    PortSetup p;
    p.name = name;
    p.texel = texel;
    p.axis = axis;
    p.sign = sign;
    p.edges = edges;
    p.wavelength = wave.wavelength;
    p.voltageFeed = feed.voltage;
    p.impedanceOhm = feed.impedanceOhm;
    p.drive = feed.drive;
    env.ports.push_back(p);
}

// Arms for a requested total length (2 * arm + 1 gap); warns when rounding is visible.
int dipoleArm(const Args& a, float length) {
    if (length < 3.0f)
        a.fail("a dipole needs length >= 3 cells (two arms plus the feed gap); use model = \"current\" for shorter "
               "ideal elements, or length = 0 for an ideal point dipole");
    if (length < 5.0f && lower(a.str("model", "thin")) != "rod")
        a.fail("a thin-wire dipole needs length >= 5 cells (each free end is built one cell short to correct the "
               "grid's end effect); refine the grid, or use model = \"rod\"");
    const int arm = std::max(1, static_cast<int>(std::lround((length - 1.0f) * 0.5f)));
    const int realized = 2 * arm + 1;
    if (std::abs(static_cast<float>(realized) - length) >= 1.0f)
        warn(a.L(), fmt("%s: length %.1f realised as %d cells (two %d-cell arms + 1-cell feed gap)",
                        a.what().c_str(), length, realized, arm));
    return arm;
}

// Centre-fed dipole along `axis` around point `c` (feed gap at floor(c) along the axis).
void buildDipole(lua_State* L, const Args& a, Conductor model, const SourceDesc& wave, const glm::vec3& c,
                 int axis, float sign, int arm, float radius, const Material& metal, const FeedSpec& feed,
                 const std::string& name) {
    if (model == Conductor::Thin) {
        glm::ivec3 node = nodeOf(c);
        node[axis] = static_cast<int>(std::floor(c[axis]));
        const int g = node[axis];
        addThinWire(L, node, axis, g - arm, g + 1 + arm, radius, g);
        addFeed(L, wave, node, axis, sign, 1, feed, name);
    } else {
        const glm::ivec3 g = cellOf(c);
        fillRod(L, g, axis, g[axis] - arm, g[axis], metal);
        fillRod(L, g, axis, g[axis] + 1, g[axis] + 1 + arm, metal);
        addFeed(L, wave, g, axis, sign, 4, feed, name);
    }
    (void)a;
}

void copyTiming(SourceDesc& d, const SourceDesc& s) {
    d.wavelength = s.wavelength; d.phase = s.phase; d.waveform = s.waveform; d.rampPeriods = s.rampPeriods;
    d.repeatPeriods = s.repeatPeriods; d.delayPeriods = s.delayPeriods; d.enabled = s.enabled;
}

#define ANTENNA_KEYS "model", "radius", "feed", "impedance", "material"

int api_dipole(lua_State* L) {
    Args a(L, "dipole", {"center", "orientation", "length", "amplitude", "name", ANTENNA_KEYS, WAVEFORM_KEYS});
    lock(L);
    SourceDesc s;
    readWaveform(a, s);
    const glm::vec3 c = a.vec("center", glm::vec3(ctx().settings.size) * 0.5f);
    const glm::vec3 o = a.optDirection("orientation").value_or(glm::vec3(0, 0, 1));
    const float len = static_cast<float>(a.num("length", 0.0));
    if (len < 0.0f) a.fail("length must be >= 0");
    const Conductor model = len > 0.0f ? readConductor(a) : Conductor::Current;

    if (model != Conductor::Current) {
        const int ax = wireAxis(a, o);
        const float radius = model == Conductor::Thin ? readRadius(a, s.wavelength) : 0.0f;
        const Material metal = model == Conductor::Rod ? readMaterial(a, "metal") : Material::vacuum();
        if (model == Conductor::Thin && a.has("material")) warn(L, "dipole: thin wires are perfect conductors; 'material' applies to model = \"rod\"");
        buildDipole(L, a, model, s, c, ax, o[ax] >= 0.0f ? 1.0f : -1.0f, dipoleArm(a, len), radius, metal,
                    readFeed(a), a.str("name", "dipole"));
    } else {
        // Ideal current element: uniform current along `length`, or a point (Hertzian) dipole.
        if (len > 0.0f) wireAxis(a, o);
        for (const char* k : {"radius", "feed", "impedance", "material"})
            if (a.has(k)) warn(L, fmt("dipole: '%s' does not apply to an ideal current element", k));
        SourceDesc d = SourceDesc::dipole(c, o, s.wavelength, static_cast<float>(a.num("amplitude", 1.0)), len);
        copyTiming(d, s);
        addSource(L, d);
    }
    a.checkUnknown();
    return 0;
}

int api_monopole(lua_State* L) {
    Args a(L, "monopole", {"base", "orientation", "length", "amplitude", "name", ANTENNA_KEYS, WAVEFORM_KEYS});
    lock(L);
    SourceDesc s;
    readWaveform(a, s);
    const glm::vec3 base = a.reqVec("base");
    const glm::vec3 o = a.optDirection("orientation").value_or(glm::vec3(0, 0, 1));
    const int ax = wireAxis(a, o);
    const float sign = o[ax] >= 0.0f ? 1.0f : -1.0f;
    const float len = static_cast<float>(a.positive("length", 0.25 * s.wavelength));
    if (len < 2.0f) a.fail("length must be >= 2 cells (feed gap plus rod)");
    // A 2-cell thin monopole is fine when its top joins a conductor (a patch probe); a free top
    // end would be shortened to nothing by the end correction, which emitThinWires() reports.
    const int rod = std::max(1, static_cast<int>(std::lround(len)) - 1);
    if (std::abs(static_cast<float>(rod + 1) - len) >= 1.0f)
        warn(L, fmt("monopole: length %.1f realised as %d cells (1-cell feed gap + %d-cell conductor)", len, rod + 1, rod));
    const Conductor model = readConductor(a);
    if (model == Conductor::Current) a.fail("a monopole needs a conductor: use model = \"thin\" or \"rod\"");
    const FeedSpec feed = readFeed(a);
    const std::string name = a.str("name", "monopole");

    // `base` is a lattice point on the conducting surface; the gap starts there.
    const int surface = static_cast<int>(std::lround(base[ax]));
    const int g = sign > 0.0f ? surface : surface - 1;
    MonopoleCheck check;
    check.axis = ax;
    check.sign = sign;
    check.where = where(L);
    if (model == Conductor::Thin) {
        glm::ivec3 node = nodeOf(base);
        node[ax] = g;
        if (sign > 0.0f) addThinWire(L, node, ax, g, g + 1 + rod, readRadius(a, s.wavelength), g);
        else addThinWire(L, node, ax, g - rod, g + 1, readRadius(a, s.wavelength), g);
        addFeed(L, s, node, ax, sign, 1, feed, name);
        check.thin = true;
        check.node = node;
        check.node[ax] = surface;
    } else {
        glm::ivec3 cell = cellOf(base);
        cell[ax] = g;
        const Material m = readMaterial(a, "metal");
        if (sign > 0.0f) fillRod(L, cell, ax, g + 1, g + 1 + rod, m);
        else fillRod(L, cell, ax, g - rod, g, m);
        addFeed(L, s, cell, ax, sign, 4, feed, name);
        check.thin = false;
        check.node = cell;
        check.node[ax] -= static_cast<int>(sign); // the cell across the gap from the rod
    }
    ctx().monopoles.push_back(check);
    a.checkUnknown();
    return 0;
}

int api_wire(lua_State* L) {
    Args a(L, "wire", {"from", "to", "radius", "name"});
    lock(L);
    const glm::ivec3 p0 = nodeOf(a.reqVec("from")), p1 = nodeOf(a.reqVec("to"));
    const glm::ivec3 d = p1 - p0;
    const int nonzero = (d.x != 0) + (d.y != 0) + (d.z != 0);
    if (nonzero != 1)
        a.fail("from and to must differ along exactly one axis (thin wires run along x, y or z); build bent or "
               "slanted conductors from several wires or from metal shapes");
    const int ax = d.x != 0 ? 0 : (d.y != 0 ? 1 : 2);
    addThinWire(L, p0, ax, std::min(p0[ax], p1[ax]), std::max(p0[ax], p1[ax]),
                readRadius(a, ctx().settings.wavelength));
    a.checkUnknown();
    return 0;
}

// Zero-thickness perfectly conducting rectangle in a lattice plane (patch, microstrip,
// ground foil): every E edge lying in the rectangle is a perfect conductor, no cells filled.
int api_sheet(lua_State* L) {
    Args a(L, "sheet", {"min", "max", "center", "size", "name"});
    lock(L);
    const auto [mn, mx] = readBox(a);
    const glm::ivec3 lo = nodeOf(mn), hi = nodeOf(mx);
    int normal = -1;
    for (int i = 0; i < 3; ++i)
        if (lo[i] == hi[i]) {
            if (normal >= 0) a.fail("a sheet needs a non-zero extent along two axes (use wire{} for a line)");
            normal = i;
        }
    if (normal < 0)
        a.fail("a sheet has zero thickness: give min and max equal along its normal axis (e.g. min.z == max.z), "
               "or a size with one zero component; use box{} for solid metal");
    const int u = (normal + 1) % 3, v = (normal + 2) % 3;
    Environment& env = builder(L).env();
    for (int iu = lo[u]; iu <= hi[u]; ++iu)
        for (int iv = lo[v]; iv <= hi[v]; ++iv) {
            glm::ivec3 q = lo;
            q[u] = iu;
            q[v] = iv;
            if (iu < hi[u]) env.subcell.edges.push_back({q, u, kPerfectEdgeSigma});
            if (iv < hi[v]) env.subcell.edges.push_back({q, v, kPerfectEdgeSigma});
        }
    env.sheets.push_back({glm::vec3(lo), glm::vec3(hi)});
    a.checkUnknown();
    return 0;
}

// Frequency sweep of every antenna port. Drive the antennas with waveform = "pulse"; the
// impedance, S11 and VSWR are computed at `points` frequencies between the band edges.
int api_sweep(lua_State* L) {
    Args a(L, "sweep", {"wavelengths", "frequencies", "points", "log", "name"});
    lock(L);
    const bool hasW = a.has("wavelengths"), hasF = a.has("frequencies");
    if (hasW == hasF) a.fail("give the band as wavelengths = {lo, hi} (cells) or frequencies = {lo, hi} (Hz)");
    const auto band = a.optVec2(hasW ? "wavelengths" : "frequencies");
    if (!band || !(band->x > 0.0f) || !(band->y > 0.0f) || band->x == band->y)
        a.fail("the band needs two different positive values");
    SweepSetup s;
    if (hasW) {
        s.wavelengthMin = std::min(band->x, band->y);
        s.wavelengthMax = std::max(band->x, band->y);
    } else {
        const double cell = ctx().settings.cellSize;
        s.wavelengthMin = static_cast<float>(kC0 / (std::max(band->x, band->y) * cell));
        s.wavelengthMax = static_cast<float>(kC0 / (std::min(band->x, band->y) * cell));
    }
    if (s.wavelengthMin < 6.0f)
        warn(L, fmt("sweep: the band reaches %.1f cells per wavelength; below ~10 the results are inaccurate",
                    s.wavelengthMin));
    s.points = std::clamp(a.integer("points", 81), 2, 2001);
    s.logPath = a.str("log", "");
    builder(L).env().sweep = s;
    a.checkUnknown();
    return 0;
}

int api_loop(lua_State* L) {
    Args a(L, "loop", {"center", "normal", "size", "amplitude", "name", ANTENNA_KEYS, WAVEFORM_KEYS});
    lock(L);
    SourceDesc base;
    readWaveform(a, base);
    const glm::vec3 c = a.vec("center", glm::vec3(ctx().settings.size) * 0.5f);
    const int n = a.axis("normal", 2);
    const float side = static_cast<float>(a.positive("size", 4.0));
    const int u = (n + 1) % 3, v = (n + 2) % 3; // u x v = n (right-handed)
    const Conductor model = readConductor(a);
    const std::string name = a.str("name", "loop");

    if (model == Conductor::Thin) {
        // Square thin-wire loop, `s` cells per side, fed across one edge in the middle of the
        // -v side (current counter-clockwise about +normal).
        const int s = static_cast<int>(std::lround(side));
        if (s < 2) a.fail("a thin-wire loop needs size >= 2 cells");
        const float radius = readRadius(a, base.wavelength);
        const glm::ivec3 n0 = nodeOf(c);
        const int u0 = n0[u] - s / 2, v0 = n0[v] - s / 2, ug = u0 + s / 2;
        glm::ivec3 bottom = n0, top = n0, left = n0, right = n0;
        bottom[v] = v0; top[v] = v0 + s; left[u] = u0; right[u] = u0 + s;
        addThinWire(L, bottom, u, u0, u0 + s, radius, ug);
        addThinWire(L, top, u, u0, u0 + s, radius);
        addThinWire(L, left, v, v0, v0 + s, radius);
        addThinWire(L, right, v, v0, v0 + s, radius);
        glm::ivec3 gap = bottom;
        gap[u] = ug;
        addFeed(L, base, gap, u, 1.0f, 1, readFeed(a), name);
        a.checkUnknown();
        return 0;
    }
    if (model == Conductor::Rod) {
        // Square ring of 1x1-cell metal rods, outer size `s` cells, fed in the middle of the -v side.
        const int s = static_cast<int>(std::lround(side));
        if (s < 3) a.fail("a rod loop needs size >= 3 cells");
        const Material m = readMaterial(a, "metal");
        const glm::ivec3 g0 = cellOf(c);
        const int u0 = g0[u] - s / 2, v0 = g0[v] - s / 2, ug = u0 + s / 2;
        glm::ivec3 bottom = g0, top = g0, left = g0, right = g0;
        bottom[v] = v0; top[v] = v0 + s - 1; left[u] = u0; right[u] = u0 + s - 1;
        fillRod(L, bottom, u, u0, ug, m);
        fillRod(L, bottom, u, ug + 1, u0 + s, m);
        fillRod(L, top, u, u0, u0 + s, m);
        fillRod(L, left, v, v0 + 1, v0 + s - 1, m);
        fillRod(L, right, v, v0 + 1, v0 + s - 1, m);
        glm::ivec3 gap = bottom;
        gap[u] = ug;
        addFeed(L, base, gap, u, 1.0f, 4, readFeed(a), name);
        a.checkUnknown();
        return 0;
    }
    // Ideal: four straight current segments, counter-clockwise about +normal.
    const float I = static_cast<float>(a.num("amplitude", 1.0));
    const struct { int along; int at; float atSign; float dirSign; } seg[4] = {
        {u, v, -1.0f, +1.0f}, {v, u, +1.0f, +1.0f}, {u, v, +1.0f, -1.0f}, {v, u, -1.0f, -1.0f}};
    for (const auto& sg : seg) {
        glm::vec3 center = c, axisVec(0.0f);
        center[sg.at] += sg.atSign * 0.5f * side;
        axisVec[sg.along] = sg.dirSign;
        SourceDesc d = SourceDesc::dipole(center, axisVec, base.wavelength, I, side);
        copyTiming(d, base);
        addSource(L, d);
    }
    a.checkUnknown();
    return 0;
}

// Shared by plane_wave and gaussian_beam: one sheet, or two sheets a quarter
// wavelength apart (the second delayed by a quarter period) radiating one way.
void addSheet(lua_State* L, const Args& a, int axis, float position, const glm::vec3& center,
              const glm::vec3& halfExtent, const glm::vec3& softness) {
    lock(L);
    SourceDesc base;
    readWaveform(a, base);
    glm::vec3 pol = a.optDirection("polarization").value_or([&] {
        glm::vec3 p(0.0f);
        p[axis == 2 ? 0 : 2] = 1.0f; // z, or x for sheets normal to z
        return p;
    }());
    if (std::abs(pol[axis]) > 1e-4f) {
        warn(L, fmt("%s: polarization has a component along the propagation axis; it was removed", a.what().c_str()));
        pol[axis] = 0.0f;
        if (glm::length(pol) < 1e-6f) a.fail("polarization must be perpendicular to the propagation axis");
        pol = glm::normalize(pol);
    }
    const std::string dir = lower(a.str("direction", "+"));
    if (dir != "+" && dir != "-" && dir != "both") a.fail("direction must be \"+\", \"-\" or \"both\"");
    const float E0 = static_cast<float>(a.num("amplitude", 1.0));
    const bool oneWay = dir != "both";

    SourceDesc s = base;
    s.center = center;
    s.center[axis] = position;
    s.halfExtent = halfExtent;
    s.softness = softness;
    s.polarization = pol;
    s.amplitude = SourceDesc::sheetCurrentForField(oneWay ? 0.5f * E0 : E0, s.wavelength, softness[axis]);
    addSource(L, s);
    if (oneWay) {
        SourceDesc s2 = s;
        s2.center[axis] += (dir == "+" ? 1.0f : -1.0f) * 0.25f * s.wavelength;
        s2.delayPeriods += 0.25f;
        addSource(L, s2);
    }
    ++ctx().sheetSources;
}

int api_plane_wave(lua_State* L) {
    Args a(L, "plane_wave", {"axis", "position", "polarization", "amplitude", "direction", "name", WAVEFORM_KEYS});
    const int axis = a.axis("axis", 0);
    const float pos = static_cast<float>(a.num("position", ctx().settings.pml + 8.0));
    glm::vec3 half(1.0e5f), soft(SourceDesc::kSheetSoftness);
    half[axis] = 0.0f;
    addSheet(L, a, axis, pos, glm::vec3(0.0f), half, soft);
    a.checkUnknown();
    return 0;
}

int api_gaussian_beam(lua_State* L) {
    Args a(L, "gaussian_beam", {"axis", "position", "center", "waist", "polarization", "amplitude", "direction",
                                "name", WAVEFORM_KEYS});
    const int axis = a.axis("axis", 0);
    const float pos = static_cast<float>(a.num("position", ctx().settings.pml + 8.0));
    const glm::vec3 c = a.vec("center", glm::vec3(ctx().settings.size) * 0.5f);
    const float waist = static_cast<float>(a.positive("waist", 2.0 * ctx().settings.wavelength));
    glm::vec3 soft(waist);
    soft[axis] = SourceDesc::kSheetSoftness;
    addSheet(L, a, axis, pos, c, glm::vec3(0.0f), soft);
    a.checkUnknown();
    return 0;
}

std::vector<float> taperWeights(const Args& a, lua_State* L, int n) {
    std::vector<float> w(static_cast<size_t>(n), 1.0f);
    std::string kind = "uniform";
    {
        const int t = a.push("taper");
        if (t == LUA_TSTRING) kind = lower(lua_tostring(L, -1));
        else if (t == LUA_TTABLE) {
            if (static_cast<int>(lua_rawlen(L, -1)) != n) { lua_pop(L, 1); a.fail("taper weights list needs %d entries", n); }
            for (int i = 0; i < n; ++i) {
                lua_rawgeti(L, -1, i + 1);
                w[static_cast<size_t>(i)] = static_cast<float>(lua_tonumber(L, -1));
                lua_pop(L, 1);
            }
            lua_pop(L, 1);
            return w;
        } else if (t != LUA_TNIL) { lua_pop(L, 1); a.fail("taper must be a name or a list of weights"); }
        lua_pop(L, 1);
    }
    for (int i = 0; i < n; ++i) {
        const double x = static_cast<double>(i);
        double v = 1.0;
        if (kind == "uniform") v = 1.0;
        else if (kind == "binomial") {
            v = 1.0; // C(n-1, i)
            for (int k = 1; k <= i; ++k) v = v * (n - k) / k;
        } else if (kind == "hann") v = std::pow(std::sin(glm::pi<double>() * (x + 1.0) / (n + 1.0)), 2.0);
        else if (kind == "cosine") v = std::sin(glm::pi<double>() * (x + 0.5) / n);
        else if (kind == "triangular") v = 1.0 - std::abs((x - 0.5 * (n - 1)) / (0.5 * (n + 1)));
        else a.fail("taper must be \"uniform\", \"binomial\", \"hann\", \"cosine\", \"triangular\" or a weights list");
        w[static_cast<size_t>(i)] = static_cast<float>(v);
    }
    const float mx = *std::max_element(w.begin(), w.end());
    if (mx > 0.0f) for (float& v : w) v /= mx;
    return w;
}

int api_array(lua_State* L) {
    Args a(L, "array", {"center", "elements", "axis", "spacing", "orientation", "length", "amplitude", "phase_step",
                        "steer", "taper", "name", ANTENNA_KEYS, WAVEFORM_KEYS});
    lock(L);
    SourceDesc base;
    readWaveform(a, base);
    const glm::vec3 c = a.vec("center", glm::vec3(ctx().settings.size) * 0.5f);

    // elements / axis / spacing / phase_step / steer: number|string for 1D, 2-lists for 2D.
    int n[2] = {1, 1};
    int ax[2] = {1, 2};
    float d[2] = {0.5f * base.wavelength, 0.5f * base.wavelength};
    float dphi[2] = {0.0f, 0.0f};
    int dims = 1;
    auto read2 = [&](const char* key, auto&& one) {
        const int t = a.push(key);
        if (t == LUA_TTABLE) {
            if (lua_rawlen(L, -1) != 2) { lua_pop(L, 1); a.fail("'%s' must have two entries for a 2-D array", key); }
            for (int i = 0; i < 2; ++i) {
                lua_rawgeti(L, -1, i + 1);
                one(i, -1);
                lua_pop(L, 1);
            }
            lua_pop(L, 1);
            return 2;
        }
        if (t != LUA_TNIL) one(0, -1);
        lua_pop(L, 1);
        return t == LUA_TNIL ? 0 : 1;
    };
    auto asInt = [&](int idx, const char* what) {
        if (lua_type(L, idx) != LUA_TNUMBER) a.fail("'%s' entries must be numbers", what);
        return static_cast<int>(lua_tonumber(L, idx));
    };
    dims = read2("elements", [&](int i, int idx) { n[i] = asInt(idx, "elements"); });
    if (dims == 0) { dims = 1; n[0] = 4; }
    const int axisDims = read2("axis", [&](int i, int idx) {
        std::optional<int> v;
        if (lua_type(L, idx) == LUA_TSTRING) v = parseAxis(lua_tostring(L, idx));
        if (!v) a.fail("'axis' entries must be \"x\", \"y\" or \"z\"");
        ax[i] = *v;
    });
    if (axisDims != 0 && axisDims != dims) a.fail("'axis' needs %d entr%s to match 'elements'", dims, dims == 1 ? "y" : "ies");
    if (dims == 2 && axisDims == 0) { ax[0] = 1; ax[1] = 0; }
    if (dims == 2 && ax[0] == ax[1]) a.fail("2-D array axes must differ");
    read2("spacing", [&](int i, int idx) {
        if (lua_type(L, idx) != LUA_TNUMBER) a.fail("'spacing' must be a number");
        d[i] = static_cast<float>(lua_tonumber(L, idx));
        if (dims == 2 && i == 0) d[1] = d[0];
    });
    const bool hasSteer = a.has("steer"), hasStep = a.has("phase_step");
    if (hasSteer && hasStep) a.fail("give steer (degrees from broadside) or phase_step (degrees), not both");
    read2(hasSteer ? "steer" : "phase_step", [&](int i, int idx) {
        if (lua_type(L, idx) != LUA_TNUMBER) a.fail("steer / phase_step must be numbers (degrees)");
        const double deg = lua_tonumber(L, idx);
        if (hasSteer) {
            // Main beam at angle theta from broadside toward +axis:  dphi = -k d sin(theta)
            const double k = glm::two_pi<double>() / base.wavelength;
            dphi[i] = static_cast<float>(-k * d[i] * std::sin(glm::radians(deg)));
        } else {
            dphi[i] = static_cast<float>(glm::radians(deg));
        }
    });
    for (int i = 0; i < dims; ++i) {
        if (n[i] < 1 || n[i] > 64) a.fail("elements must be within 1..64 per axis");
        if (d[i] <= 0.0f) a.fail("spacing must be > 0");
        if (d[i] > base.wavelength)
            warn(L, fmt("array: spacing %.1f > wavelength %.1f produces grating lobes", d[i], base.wavelength));
    }
    if (n[0] * n[1] > SimulationGrid::kMaxSources) a.fail("too many elements (max %d)", SimulationGrid::kMaxSources);

    const glm::vec3 o = a.optDirection("orientation").value_or(glm::vec3(0, 0, 1));
    const float len = static_cast<float>(a.num("length", 0.45 * base.wavelength));
    if (len < 0.0f) a.fail("length must be >= 0");
    const int elemAxis = len > 0.0f ? wireAxis(a, o) : 2;
    const Conductor model = len > 0.0f ? readConductor(a) : Conductor::Current;
    const bool wire = model != Conductor::Current;
    const int arm = wire ? dipoleArm(a, len) : 0;
    const Material metal = model == Conductor::Rod ? readMaterial(a, "metal") : Material::vacuum();
    const float radius = model == Conductor::Thin ? readRadius(a, base.wavelength) : 0.0f;
    const FeedSpec feed = wire ? readFeed(a) : FeedSpec{};
    const std::string baseName = a.str("name", "array");
    const float I = static_cast<float>(a.num("amplitude", 1.0));
    const std::vector<float> w0 = taperWeights(a, L, n[0]);
    const std::vector<float> w1 = dims == 2 ? taperWeights(a, L, n[1]) : std::vector<float>{1.0f};

    for (int j = 0; j < n[1]; ++j)
        for (int i = 0; i < n[0]; ++i) {
            glm::vec3 p = c;
            p[ax[0]] += (static_cast<float>(i) - 0.5f * static_cast<float>(n[0] - 1)) * d[0];
            if (dims == 2) p[ax[1]] += (static_cast<float>(j) - 0.5f * static_cast<float>(n[1] - 1)) * d[1];
            const float current = I * w0[static_cast<size_t>(i)] * w1[static_cast<size_t>(dims == 2 ? j : 0)];
            SourceDesc timing = base;
            timing.phase = base.phase + static_cast<float>(i) * dphi[0] + static_cast<float>(j) * dphi[1];
            if (wire) {
                // Elements stay in place even when undriven (tapered to 0): they still couple.
                FeedSpec ef = feed;
                ef.drive = current;
                const std::string ename = dims == 2 ? fmt("%s[%d,%d]", baseName.c_str(), i, j)
                                                    : fmt("%s[%d]", baseName.c_str(), i);
                buildDipole(L, a, model, timing, p, elemAxis, o[elemAxis] >= 0.0f ? 1.0f : -1.0f, arm, radius, metal,
                            ef, ename);
            } else if (current != 0.0f) {
                SourceDesc e = SourceDesc::dipole(p, o, base.wavelength, current, len);
                copyTiming(e, timing);
                addSource(L, e);
            }
        }
    a.checkUnknown();
    return 0;
}

int api_current_source(lua_State* L) {
    Args a(L, "current_source", {"min", "max", "center", "size", "polarization", "amplitude", "softness", "name",
                                 WAVEFORM_KEYS});
    lock(L);
    SourceDesc s;
    readWaveform(a, s);
    const auto [mn, mx] = readBox(a);
    s.center = 0.5f * (mn + mx);
    s.halfExtent = 0.5f * (mx - mn);
    s.softness = a.vec("softness", glm::vec3(SourceDesc::kDipoleSoftness));
    s.polarization = a.optDirection("polarization").value_or(glm::vec3(0, 0, 1));
    s.amplitude = static_cast<float>(a.num("amplitude", 1.0));
    addSource(L, s);
    a.checkUnknown();
    return 0;
}

// =============================================================================
// Instruments
// =============================================================================
int api_probe(lua_State* L) {
    Args a(L, "probe", {"position", "name", "log"});
    lock(L);
    ProbeSetup p;
    p.position = a.reqVec("position");
    auto& probes = ctx().builder->env().probes;
    p.name = a.str("name", "probe" + std::to_string(probes.size() + 1));
    p.logPath = a.str("log", "");
    probes.push_back(p);
    a.checkUnknown();
    return 0;
}

int api_flux_plane(lua_State* L) {
    Args a(L, "flux_plane", {"axis", "position", "min", "max", "name", "log"});
    lock(L);
    const int axis = a.axis("axis", 0);
    const glm::ivec3 n = ctx().settings.size;
    const int u = axis == 0 ? 1 : 0, v = axis == 2 ? 1 : 2;
    const int L0 = ctx().settings.pml;
    const glm::vec2 lo = a.optVec2("min").value_or(glm::vec2(static_cast<float>(L0)));
    const glm::vec2 hi = a.optVec2("max").value_or(glm::vec2(n[u] - L0, n[v] - L0));
    FluxSetup f;
    auto& fluxes = ctx().builder->env().fluxes;
    f.surface = FluxSurface::plane(axis, static_cast<int>(std::lround(a.reqNum("position"))), glm::ivec2(glm::round(lo)),
                                   glm::ivec2(glm::round(hi)), a.str("name", "flux" + std::to_string(fluxes.size() + 1)));
    f.logPath = a.str("log", "");
    fluxes.push_back(f);
    a.checkUnknown();
    return 0;
}

int api_flux_box(lua_State* L) {
    Args a(L, "flux_box", {"min", "max", "center", "size", "name", "log"});
    lock(L);
    const auto [mn, mx] = readBox(a);
    FluxSetup f;
    auto& fluxes = ctx().builder->env().fluxes;
    f.surface = FluxSurface::box(glm::ivec3(glm::round(mn)), glm::ivec3(glm::round(mx)),
                                 a.str("name", "flux" + std::to_string(fluxes.size() + 1)));
    f.logPath = a.str("log", "");
    fluxes.push_back(f);
    a.checkUnknown();
    return 0;
}

// Pulsed radar receiver on the antenna ports (digital beamforming, clutter calibration,
// detection). Objects the radar should see are marked target = true.
int api_radar(lua_State* L) {
    Args a(L, "radar", {"range", "fov", "noise", "threshold", "dynamic_range", "reference", "phase_center", "tx", "taper",
                        "log", "name"});
    lock(L);
    RadarSetup r;
    if (auto rg = a.optVec2("range")) {
        if (!(rg->x >= 0.0f) || !(rg->y > rg->x)) a.fail("range = {min, max} needs 0 <= min < max (cells)");
        r.rangeMin = rg->x;
        r.rangeMax = rg->y;
    }
    if (a.has("fov")) {
        if (auto f = a.optNum("fov")) {
            r.fovAzDeg = r.fovElDeg = static_cast<float>(*f);
        } else if (auto f2 = a.optVec2("fov")) {
            r.fovAzDeg = f2->x;
            r.fovElDeg = f2->y;
        }
        if (!(r.fovAzDeg > 0.0f && r.fovAzDeg <= 80.0f && r.fovElDeg > 0.0f && r.fovElDeg <= 80.0f))
            a.fail("fov = half-angle in degrees (0..80), or {azimuth, elevation}");
    }
    if (auto n = a.optNum("noise")) {
        if (*n > 0.0) a.fail("noise is in dB relative to the transmit pulse at a port, so it must be negative (e.g. -70)");
        r.noiseDb = static_cast<float>(*n);
    }
    r.thresholdDb = static_cast<float>(a.num("threshold", 13.0));
    r.dynamicRangeDb = static_cast<float>(a.positive("dynamic_range", 25.0));
    r.referenceDistance = static_cast<float>(a.num("reference", 0.0));
    r.phaseCenterOffset = a.vec("phase_center", glm::vec3(0.0f));
    r.txCenter = a.optVec("tx");
    const std::string taper = lower(a.str("taper", "hamming"));
    if (taper == "uniform" || taper == "none") r.taper = 0;
    else if (taper == "hamming") r.taper = 1;
    else if (taper == "hann" || taper == "hanning") r.taper = 2;
    else a.fail("taper must be \"uniform\", \"hamming\" or \"hann\" (got \"%s\")", taper.c_str());
    r.logPath = a.str("log", "");
    if (builder(L).env().radar) warn(L, "radar: only one radar per scene; replacing the previous one");
    builder(L).env().radar = r;
    a.checkUnknown();
    return 0;
}

// warning("text"): a message shown with the scene's warnings (Scene panel and console).
int api_warning(lua_State* L) {
    const char* msg = luaL_checkstring(L, 1);
    warn(L, msg);
    return 0;
}

int api_far_field(lua_State* L) {
    Args a(L, "far_field", {"min", "max", "center", "size", "margin", "wavelength", "frequency", "averaging", "name",
                            "log", "axis"});
    lock(L);
    FarFieldSetup f;
    if (a.has("min") || a.has("max") || a.has("size")) {
        const auto [mn, mx] = readBox(a);
        f.boxMin = glm::ivec3(glm::round(mn));
        f.boxMax = glm::ivec3(glm::round(mx));
    } else {
        const int margin = a.integer("margin", 4);
        f.boxMin = glm::ivec3(ctx().settings.pml + 1 + margin);
        f.boxMax = ctx().settings.size - ctx().settings.pml - 2 - margin;
    }
    f.wavelength = readWavelength(a);
    f.averagingPeriods = static_cast<float>(a.positive("averaging", 4.0));
    f.name = a.str("name", "far field");
    f.logPath = a.str("log", "");
    if (auto ax = a.optDirection("axis")) {
        const glm::vec3 d = glm::abs(*ax);
        const int i = (d.x >= d.y && d.x >= d.z) ? 0 : (d.y >= d.z ? 1 : 2);
        if (d[i] < 0.9999f) a.fail("axis must be along x, y or z (\"+x\", \"-z\", ...)");
        f.mapAxis = glm::vec3(0.0f);
        f.mapAxis[i] = (*ax)[i] > 0.0f ? 1.0f : -1.0f;
    }
    if (ctx().builder->env().farField) warn(L, "far_field: only one far-field monitor is supported; replacing the previous one");
    ctx().builder->env().farField = f;
    a.checkUnknown();
    return 0;
}

// =============================================================================
// Lua runtime
// =============================================================================
const char* kPrelude = R"LUA(
-- ---------------------------------------------------------------- vectors
local V = {}
V.__index = function(t, k)
  if k == "x" then return rawget(t, 1) elseif k == "y" then return rawget(t, 2)
  elseif k == "z" then return rawget(t, 3) end
  return V[k]
end
function vec(x, y, z)
  if type(x) == "table" then
    return setmetatable({x[1] or x.x or 0, x[2] or x.y or 0, x[3] or x.z or 0}, V)
  end
  if y == nil and z == nil then x = x or 0; return setmetatable({x, x, x}, V) end
  return setmetatable({x or 0, y or 0, z or 0}, V)
end
local function lift(a) return vec(a) end
V.__add = function(a, b) a, b = lift(a), lift(b); return vec(a[1] + b[1], a[2] + b[2], a[3] + b[3]) end
V.__sub = function(a, b) a, b = lift(a), lift(b); return vec(a[1] - b[1], a[2] - b[2], a[3] - b[3]) end
V.__mul = function(a, b) a, b = lift(a), lift(b); return vec(a[1] * b[1], a[2] * b[2], a[3] * b[3]) end
V.__div = function(a, b) a, b = lift(a), lift(b); return vec(a[1] / b[1], a[2] / b[2], a[3] / b[3]) end
V.__unm = function(a) return vec(-a[1], -a[2], -a[3]) end
V.__eq = function(a, b) return a[1] == b[1] and a[2] == b[2] and a[3] == b[3] end
V.__tostring = function(a) return string.format("vec(%g, %g, %g)", a[1], a[2], a[3]) end
function V.dot(a, b) b = lift(b); return a[1] * b[1] + a[2] * b[2] + a[3] * b[3] end
function V.length(a) return math.sqrt(V.dot(a, a)) end
function V.normalize(a) local l = V.length(a); if l == 0 then return vec(0, 0, 0) end; return a / l end
function V.cross(a, b) b = lift(b)
  return vec(a[2] * b[3] - a[3] * b[2], a[3] * b[1] - a[1] * b[3], a[1] * b[2] - a[2] * b[1]) end
function V.with(a, axis, value) local i = ({x = 1, y = 2, z = 3})[axis] or axis; local r = vec(a); r[i] = value; return r end
X, Y, Z = vec(1, 0, 0), vec(0, 1, 0), vec(0, 0, 1)

-- ---------------------------------------------------------------- helpers
c0 = 299792458
pi = math.pi
function deg(x) return x * math.pi / 180 end
function wavelengths(n) return n * grid.wavelength end      -- n reference wavelengths in cells
function meters(x) return x / grid.cell_size end           -- metres -> cells
function mm(x) return x * 1e-3 / grid.cell_size end        -- millimetres -> cells
function lerp(a, b, t) return a + (b - a) * t end
function clamp(x, lo, hi) return math.max(lo, math.min(hi, x)) end
function range(a, b, step)                                 -- inclusive numeric iterator
  step = step or 1
  local i = a - step
  return function() i = i + step; if (step > 0 and i <= b + 1e-9) or (step < 0 and i >= b - 1e-9) then return i end end
end

-- Text-only chunk loading (binary bytecode could bypass the VM's safety checks).
local _load = load
load = function(chunk, name, mode, env) return _load(chunk, name, "t", env) end
dofile, loadfile = nil, nil
)LUA";

int traceback(lua_State* L) {
    const char* msg = lua_tostring(L, 1);
    luaL_traceback(L, L, msg ? msg : "(error object is not a string)", 1);
    return 1;
}

// Keeps traceback lines that point into script files (drops [C] / internal frames).
std::string cleanTraceback(const std::string& tb) {
    std::istringstream in(tb);
    std::string line, out;
    bool first = true;
    while (std::getline(in, line)) {
        if (first) { out = line; first = false; continue; }
        if (line.find("[C]") != std::string::npos || line.find("(...tail calls...)") != std::string::npos ||
            line.find("[string") != std::string::npos || line.find("stack traceback") != std::string::npos)
            continue;
        out += "\n  " + line.substr(line.find_first_not_of(" \t") == std::string::npos ? 0 : line.find_first_not_of(" \t"));
    }
    return out;
}

void timeoutHook(lua_State* L, lua_Debug*) {
    const double elapsed =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - ctx().start).count();
    if (elapsed > kTimeLimitSeconds) luaL_error(L, "script exceeded the %d s time limit (infinite loop?)",
                                                static_cast<int>(kTimeLimitSeconds));
}

int gridIndex(lua_State* L) {
    // grid.<field> before scene{}: lock in the defaults, then answer from the filled table.
    lock(L);
    lua_rawget(L, 1); // grid[key]
    return 1;
}

struct LuaState {
    lua_State* L = luaL_newstate();
    ~LuaState() { if (L) lua_close(L); }
};

void registerApi(lua_State* L) {
    const luaL_Reg api[] = {
        {"scene", wrap<api_scene>},         {"view", wrap<api_view>},
        {"material", wrap<api_material>},   {"param", wrap<api_param>},
        {"box", wrap<api_box>},             {"sphere", wrap<api_sphere>},
        {"cylinder", wrap<api_cylinder>},   {"wall", wrap<api_wall>},
        {"grating", wrap<api_grating>},     {"cage", wrap<api_cage>},
        {"frame", wrap<api_frame>},         {"shape", wrap<api_shape>},
        {"dipole", wrap<api_dipole>},       {"loop", wrap<api_loop>},
        {"monopole", wrap<api_monopole>},  {"wire", wrap<api_wire>},
        {"sheet", wrap<api_sheet>},        {"sweep", wrap<api_sweep>},
        {"plane_wave", wrap<api_plane_wave>}, {"gaussian_beam", wrap<api_gaussian_beam>},
        {"array", wrap<api_array>},         {"current_source", wrap<api_current_source>},
        {"probe", wrap<api_probe>},         {"flux_plane", wrap<api_flux_plane>},
        {"flux_box", wrap<api_flux_box>},   {"far_field", wrap<api_far_field>},
        {"radar", wrap<api_radar>},         {"warning", wrap<api_warning>},
    };
    for (const luaL_Reg& r : api) lua_register(L, r.name, r.func);

    // grid: filled at lock time; reading it earlier locks the default settings.
    lua_newtable(L);
    lua_newtable(L);
    lua_pushcfunction(L, wrap<gridIndex>);
    lua_setfield(L, -2, "__index");
    lua_setmetatable(L, -2);
    lua_setglobal(L, "grid");
}

void openSandboxedLibs(lua_State* L, const fs::path& scriptDir) {
    const luaL_Reg libs[] = {{LUA_GNAME, luaopen_base},       {LUA_LOADLIBNAME, luaopen_package},
                             {LUA_TABLIBNAME, luaopen_table}, {LUA_STRLIBNAME, luaopen_string},
                             {LUA_MATHLIBNAME, luaopen_math}, {LUA_UTF8LIBNAME, luaopen_utf8}};
    for (const luaL_Reg& lib : libs) {
        luaL_requiref(L, lib.name, lib.func, 1);
        lua_pop(L, 1);
    }
    // require() only finds Lua modules next to the script (or in ./lib); no C modules.
    lua_getglobal(L, "package");
    const std::string dir = scriptDir.generic_string();
    lua_pushstring(L, (dir + "/?.lua;" + dir + "/lib/?.lua").c_str());
    lua_setfield(L, -2, "path");
    lua_pushstring(L, "");
    lua_setfield(L, -2, "cpath");
    lua_getfield(L, -1, "searchers");
    const lua_Integer n = static_cast<lua_Integer>(lua_rawlen(L, -1));
    for (lua_Integer i = n; i >= 3; --i) { // drop the C and all-in-one loaders
        lua_pushnil(L);
        lua_rawseti(L, -2, i);
    }
    lua_pop(L, 2);
}

// Emits the thin wires into the sub-cell model. End correction: the lattice node at a free
// wire end holds more charge than the tip of a real thin wire, which makes the wire act
// about one cell longer per free end (measured: impedance and radiation resistance of
// dipoles match thin-wire theory when 2 cells are added). Each free end is therefore
// built one cell short, so the electrical length equals the requested length. Ends that
// touch another conductor (another wire, metal cells) are not free.
void emitThinWires(Context& c) {
    Environment& env = c.builder->env();
    auto key = [](const glm::ivec3& p) {
        return (static_cast<long long>(p.x + 4096) << 26) | (static_cast<long long>(p.y + 4096) << 13) |
               static_cast<long long>(p.z + 4096);
    };
    // Lattice nodes on each wire, to detect wires touching other wires.
    std::vector<std::set<long long>> nodes(c.thinWires.size());
    for (size_t w = 0; w < c.thinWires.size(); ++w) {
        const ThinWireSpec& t = c.thinWires[w];
        for (int k = t.i0; k <= t.i1; ++k) {
            glm::ivec3 p = t.node;
            p[t.axis] = k;
            nodes[w].insert(key(p));
        }
    }
    // Nodes on perfectly conducting edges that exist before the wires (metal sheets).
    std::set<long long> sheetNodes;
    for (const EdgeOverride& e : env.subcell.edges) {
        if (e.sigma < 0.5f * kPerfectEdgeSigma) continue;
        glm::ivec3 end = e.texel;
        end[e.axis] += 1;
        sheetNodes.insert(key(e.texel));
        sheetNodes.insert(key(end));
    }
    auto touchesConductor = [&](size_t self, const glm::ivec3& p) {
        if (sheetNodes.count(key(p))) return true;
        for (size_t w = 0; w < nodes.size(); ++w)
            if (w != self && nodes[w].count(key(p))) return true;
        for (int dz = -1; dz <= 0; ++dz)
            for (int dy = -1; dy <= 0; ++dy)
                for (int dx = -1; dx <= 0; ++dx)
                    if (env.materials.get(p + glm::ivec3(dx, dy, dz)).sigma >= 1.0f) return true;
        return false;
    };

    for (size_t w = 0; w < c.thinWires.size(); ++w) {
        ThinWireSpec t = c.thinWires[w];
        glm::ivec3 a = t.node, b = t.node;
        a[t.axis] = t.i0;
        b[t.axis] = t.i1;
        const bool freeA = !touchesConductor(w, a) && t.gapEdge != t.i0;
        const bool freeB = !touchesConductor(w, b) && t.gapEdge != t.i1 - 1;
        // The end correction must leave at least one conductor edge besides the feed gap.
        const int keep = (t.gapEdge >= t.i0 && t.gapEdge < t.i1) ? 2 : 1;
        if ((t.i1 - t.i0) - (freeA ? 1 : 0) - (freeB ? 1 : 0) >= keep) {
            if (freeA) ++t.i0;
            if (freeB) --t.i1;
        } else if (freeA || freeB) {
            c.warnings.push_back(fmt("thin wire at (%d, %d, %d) is too short for the end correction: its free end "
                                     "acts about one cell longer than built (make it longer, or join its end to a "
                                     "conductor)", a.x, a.y, a.z));
        }

        const float f = 2.0f / std::log(1.0f / t.radius);
        const int u = (t.axis + 1) % 3, v = (t.axis + 2) % 3;
        glm::ivec3 eu(0), ev(0);
        eu[u] = 1;
        ev[v] = 1;
        // In the update of B component c, curl_c E = term0 - term1 with term0 = d_{c+1} E_{c+2}:
        // the term holding the wire's own (axial) E component gets the thin-wire factor.
        const int ax = t.axis;
        auto term = [ax](int comp) { return (comp + 2) % 3 == ax ? 0 : 1; };
        for (int k = t.i0; k < t.i1; ++k) {
            glm::ivec3 q = t.node;
            q[t.axis] = k;
            if (k != t.gapEdge) env.subcell.edges.push_back({q, t.axis, kPerfectEdgeSigma});
            env.subcell.wireTerms.push_back({q, u, term(u), f});       // B_u at +1/2 v
            env.subcell.wireTerms.push_back({q - ev, u, term(u), f});  // B_u at -1/2 v
            env.subcell.wireTerms.push_back({q, v, term(v), f});       // B_v at +1/2 u
            env.subcell.wireTerms.push_back({q - eu, v, term(v), f});  // B_v at -1/2 u
        }
    }
}

void finalChecks(Context& c) {
    Environment& env = c.builder->env();
    const glm::vec3 n(env.grid.size);
    const float L = static_cast<float>(env.grid.pmlThickness);
    auto inAbsorber = [&](const glm::vec3& p) {
        return glm::any(glm::lessThan(p, glm::vec3(L))) || glm::any(glm::greaterThanEqual(p, n - L));
    };
    if (env.sources.empty()) c.warnings.push_back("scene has no (enabled) sources: nothing will radiate");
    for (const MonopoleCheck& m : c.monopoles) {
        bool grounded = false;
        if (!m.thin) {
            grounded = env.materials.get(m.node).sigma >= 1.0f;
        } else {
            // A metal cell touching the base node on the ground side, or a thin wire ending there.
            const int u = (m.axis + 1) % 3, v = (m.axis + 2) % 3;
            for (int du = -1; du <= 0 && !grounded; ++du)
                for (int dv = -1; dv <= 0 && !grounded; ++dv) {
                    glm::ivec3 cell = m.node;
                    cell[u] += du;
                    cell[v] += dv;
                    cell[m.axis] = m.sign > 0.0f ? m.node[m.axis] - 1 : m.node[m.axis];
                    grounded = env.materials.get(cell).sigma >= 1.0f;
                }
            for (const EdgeOverride& e : env.subcell.edges) {
                if (grounded) break;
                if (e.sigma < 0.5f * kPerfectEdgeSigma) continue;
                glm::ivec3 end = e.texel;
                end[e.axis] += 1;
                grounded = e.texel == m.node || end == m.node;
            }
        }
        if (!grounded)
            c.warnings.push_back(m.where + "monopole: no conductor touches the base of the feed gap, so the feed has "
                                           "nothing to drive against. Add a ground plane, radials or wall at the base.");
    }
    for (const auto& [name, eps] : c.usedDielectrics) {
        const float lambdaIn = env.referenceWavelength / std::sqrt(eps);
        if (lambdaIn < 8.0f)
            c.warnings.push_back(fmt("material '%s' (eps_r = %g) shortens the wavelength to %.1f cells; below ~8 cells "
                                     "per wavelength the results are inaccurate (increase scene.wavelength)",
                                     name.c_str(), eps, lambdaIn));
    }
    for (const ProbeSetup& p : env.probes)
        if (inAbsorber(p.position)) c.warnings.push_back("probe '" + p.name + "' lies inside the absorbing boundary layer");
    for (const SourceDesc& s : env.sources)
        if (glm::length(s.halfExtent) < 1e3f && inAbsorber(s.center))
            c.warnings.push_back("a source lies inside the absorbing boundary layer (it will be damped)");
    if (env.sweep) {
        bool pulsed = false;
        for (const SourceDesc& s : env.sources) pulsed = pulsed || s.waveform == Waveform::GaussianPulse;
        if (env.ports.empty()) c.warnings.push_back("sweep: the scene has no antenna feeds (ports) to sweep");
        else if (!pulsed)
            c.warnings.push_back("sweep: the antennas are driven with a continuous wave, which only contains one "
                                 "frequency; use waveform = \"pulse\" for a frequency sweep");
    }
    if (env.farField) {
        if (c.sheetSources > 0)
            c.warnings.push_back("far_field: plane-wave / beam sources cross the Huygens box; the pattern is only "
                                 "meaningful for localized sources (antennas)");
        const FarFieldSetup& f = *env.farField;
        for (const SourceDesc& s : env.sources) {
            const glm::vec3 lo = s.center - s.halfExtent - 2.0f, hi = s.center + s.halfExtent + 2.0f;
            if (glm::any(glm::lessThan(lo, glm::vec3(f.boxMin))) || glm::any(glm::greaterThan(hi, glm::vec3(f.boxMax)))) {
                if (c.sheetSources == 0)
                    c.warnings.push_back("far_field: a source is not fully inside the Huygens box");
                break;
            }
        }
        // Materials on the box surface break the vacuum Green's function assumption.
        const MaterialVolume& m = env.materials;
        size_t hits = 0;
        for (int z = f.boxMin.z; z <= f.boxMax.z && !hits; ++z)
            for (int y = f.boxMin.y; y <= f.boxMax.y && !hits; ++y)
                for (int x = f.boxMin.x; x <= f.boxMax.x; ++x) {
                    const bool onSurface = x == f.boxMin.x || x == f.boxMax.x || y == f.boxMin.y || y == f.boxMax.y ||
                                           z == f.boxMin.z || z == f.boxMax.z;
                    if (!onSurface) {
                        x = f.boxMax.x - 1;
                        continue;
                    }
                    const Material mat = m.get({x, y, z});
                    if (mat.sigma > 0.0f || mat.epsilonR != 1.0f) { ++hits; break; }
                }
        if (hits) c.warnings.push_back("far_field: the Huygens box intersects material; enclose all objects in vacuum");
    }
}

} // namespace

// =============================================================================
// ScriptParam
// =============================================================================
std::string ScriptParam::valueString() const {
    switch (type) {
        case Type::Bool: return value != 0.0 ? "on" : "off";
        case Type::Choice: return choices.empty() ? "" : choices[choiceIndex];
        default: {
            char buf[64];
            std::snprintf(buf, sizeof(buf), "%g", value);
            return buf;
        }
    }
}

std::string ScriptParam::overrideString() const {
    switch (type) {
        case Type::Bool: return value != 0.0 ? "true" : "false";
        case Type::Choice: return choices.empty() ? "" : choices[choiceIndex];
        default: {
            char buf[64];
            std::snprintf(buf, sizeof(buf), "%.17g", value);
            return buf;
        }
    }
}

bool ScriptParam::handleKey(char key) {
    switch (type) {
        case Type::Bool:
            if (key != keyToggle) return false;
            value = value != 0.0 ? 0.0 : 1.0;
            return true;
        case Type::Choice:
            if (key != keyToggle || choices.size() < 2) return false;
            choiceIndex = (choiceIndex + 1) % choices.size();
            return true;
        case Type::Number: {
            if (key != keyDecrease && key != keyIncrease) return false;
            double v = value + (key == keyIncrease ? step : -step);
            if (wrap && min > -1e299 && max < 1e299) {
                const double span = max - min;
                if (v > max + 1e-9) v -= span;
                if (v < min - 1e-9) v += span;
            }
            v = std::clamp(v, min, max);
            if (v == value) return false;
            value = v;
            return true;
        }
    }
    return false;
}

// =============================================================================
// ScriptEngine
// =============================================================================
namespace {
ScriptResult runOnce(const fs::path& script, const std::map<std::string, std::string>& overrides, bool withTargets);
} // namespace

ScriptResult ScriptEngine::run(const fs::path& script, const std::map<std::string, std::string>& overrides) {
    ScriptResult r = runOnce(script, overrides, true);
    if (r.env.radar) {
        // The radar's calibration scene: everything except the targets.
        ScriptResult bg = runOnce(script, overrides, false);
        r.backgroundMaterials = std::move(bg.env.materials);
        r.backgroundSubcell = std::move(bg.env.subcell);
        if (r.env.targets.empty())
            r.warnings.push_back("radar: no object is marked target = true, so the calibration (which removes the "
                                 "targets) sees the same scene and every echo cancels; mark what the radar should "
                                 "detect with target = true");
    }
    return r;
}

namespace {
ScriptResult runOnce(const fs::path& script, const std::map<std::string, std::string>& overrides, bool withTargets) {
    std::ifstream in(script, std::ios::binary);
    if (!in) {
        std::error_code ec;
        throw ScriptError("cannot open script '" + fs::absolute(script, ec).string() + "'");
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    const std::string source = ss.str();

    Context c;
    c.script = script;
    c.overrides = overrides;
    c.withTargets = withTargets;
    c.start = std::chrono::steady_clock::now();
    for (const auto& kv : builtinMaterials()) c.materials[kv.first] = kv.second;
    g_ctx = &c;
    struct Reset { ~Reset() { g_ctx = nullptr; } } reset;

    LuaState state;
    lua_State* L = state.L;
    if (!L) throw ScriptError("cannot create Lua state");
    openSandboxedLibs(L, fs::absolute(script).parent_path());
    if (luaL_dostring(L, kPrelude) != LUA_OK)
        throw ScriptError(std::string("internal prelude error: ") + lua_tostring(L, -1));
    registerApi(L);
    lua_sethook(L, timeoutHook, LUA_MASKCOUNT, 100000);

    lua_pushcfunction(L, traceback);
    const int handler = lua_gettop(L);
    const std::string chunkName = "@" + script.filename().string();
    if (luaL_loadbufferx(L, source.data(), source.size(), chunkName.c_str(), "t") != LUA_OK)
        throw ScriptError(lua_tostring(L, -1));
    if (lua_pcall(L, 0, 0, handler) != LUA_OK) throw ScriptError(cleanTraceback(lua_tostring(L, -1)));

    lock(L); // a script without scene{} or objects still yields a valid (empty) scene
    emitThinWires(c);
    finalChecks(c);
    c.builder->env().view = c.view;

    ScriptResult r;
    r.env = c.builder->build();
    r.params = std::move(c.params);
    r.warnings = std::move(c.warnings);
    const fs::path base = fs::absolute(script).parent_path();
    r.outputDir = c.settings.outputDir.empty() ? base / "output" / script.stem()
                                               : (fs::path(c.settings.outputDir).is_absolute()
                                                      ? fs::path(c.settings.outputDir)
                                                      : base / c.settings.outputDir);
    return r;
}
} // namespace
