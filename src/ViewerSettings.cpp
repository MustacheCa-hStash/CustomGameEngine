#include "ViewerSettings.h"

#include <windows.h>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <fstream>

namespace
{
constexpr auto saveDelay = std::chrono::milliseconds(350);

std::string_view trim(std::string_view value)
{
    constexpr std::string_view whitespace = " \t\r\n";
    const auto start = value.find_first_not_of(whitespace);
    if (start == std::string_view::npos) return {};
    return value.substr(start, value.find_last_not_of(whitespace) - start + 1);
}

std::string identifier(std::string_view name)
{
    constexpr char hex[] = "0123456789ABCDEF";
    std::string result;
    for (const unsigned char c : name)
    {
        if (c <= ' ' || c == 127 || c == '%' || c == '[' || c == ']' || c == '.' || c == '#')
        {
            result += '%';
            result += hex[c >> 4];
            result += hex[c & 15];
        }
        else
        {
            result += static_cast<char>(c);
        }
    }
    return result;
}

template<class T>
std::vector<std::string> namedKeys(const std::vector<T>& items)
{
    std::map<std::string, unsigned> occurrences;
    std::vector<std::string> keys;
    keys.reserve(items.size());
    for (const auto& item : items)
    {
        auto key = identifier(item.name);
        const auto occurrence = ++occurrences[key];
        if (occurrence > 1) key += '#' + std::to_string(occurrence);
        keys.push_back(std::move(key));
    }
    return keys;
}

struct Scope
{
    std::string scene;
    std::string asset;
};

Scope scopeFor(const ViewerScene& scene)
{
    switch (scene.kind)
    {
    case SceneKind::Cube: return {"scene.cube", "asset.cube"};
    case SceneKind::Oak: return {"scene.oak", "asset.oak"};
    case SceneKind::ModeledOak: return {"scene.modeled_oak", "asset.modeled_oak"};
    case SceneKind::ModeledOakWalk: return {"scene.modeled_oak_walk", "asset.modeled_oak"};
    case SceneKind::Grove: return {"scene.grove", "asset.oak"};
    case SceneKind::OakBranch: return {"scene.oak_branch", "asset.oak_branch"};
    case SceneKind::OakBranchGrid: return {"scene.oak_branch_grid", "asset.oak_branch"};
    case SceneKind::OakBranchOrbit: return {"scene.oak_branch_orbit", "asset.oak_branch"};
    case SceneKind::OakBranchTransition: return {"scene.oak_branch_transition", "asset.oak_branch"};
    case SceneKind::Imported:
    {
        std::error_code error;
        const auto canonical = std::filesystem::weakly_canonical(scene.source, error);
        const auto utf8 = (error ? scene.source : canonical).generic_u8string();
        const auto key = identifier({reinterpret_cast<const char*>(utf8.data()), utf8.size()});
        return {"scene.imported." + key, "asset.imported." + key};
    }
    }
    return {};
}

template<class Sections>
std::string_view field(const Sections& sections, const std::string& section, std::string_view key)
{
    if (const auto found = sections.find(section); found != sections.end())
    {
        if (const auto value = found->second.find(key); value != found->second.end()) return value->second;
    }
    return {};
}

template<class T>
T number(std::string_view text, T fallback, T minimum, T maximum)
{
    if (text.empty()) return fallback;
    T value{};
    const auto end = text.data() + text.size();
    const auto [position, error] = std::from_chars(text.data(), end, value);
    if (error != std::errc{} || position != end || !std::isfinite(value) ||
        value < minimum || value > maximum)
    {
        return fallback;
    }
    return value;
}

bool boolean(std::string_view value, bool fallback)
{
    if (value == "true") return true;
    if (value == "false") return false;
    return fallback;
}

template<class T>
std::string formatNumber(T value)
{
    char buffer[64];
    const auto result = std::to_chars(buffer, buffer + sizeof(buffer), value);
    return {buffer, result.ptr};
}

template<class Fields>
void writeShadows(Fields& fields, const MeshShadows& flags)
{
    fields["cast"] = flags.cast ? "true" : "false";
    fields["receive"] = flags.receive ? "true" : "false";
}

template<class Sections>
void readShadows(const Sections& sections, const std::string& section, MeshShadows& flags)
{
    flags.cast = boolean(field(sections, section, "cast"), flags.cast);
    flags.receive = boolean(field(sections, section, "receive"), flags.receive);
}

template<class Fields>
void writeMaterial(Fields& fields, const MaterialData& material)
{
    fields["red"] = formatNumber(material.color.x);
    fields["green"] = formatNumber(material.color.y);
    fields["blue"] = formatNumber(material.color.z);
    if (material.alphaCutoff >= 0.0f)
    {
        fields["alpha_cutoff"] = formatNumber(material.alphaCutoff);
    }
    if (material.hasBacklighting)
    {
        fields["backlighting"] = formatNumber(material.transmission);
    }
}

template<class Sections>
void readMaterial(const Sections& sections, const std::string& section, MaterialData& material)
{
    const auto read = [&](std::string_view key, float fallback) {
        return number(field(sections, section, key), fallback, 0.0f, 1.0f);
    };
    material.color.x = read("red", material.color.x);
    material.color.y = read("green", material.color.y);
    material.color.z = read("blue", material.color.z);
    if (material.alphaCutoff >= 0.0f)
    {
        material.alphaCutoff = read("alpha_cutoff", material.alphaCutoff);
    }
    if (material.hasBacklighting)
    {
        material.transmission = read("backlighting", material.transmission);
    }
}
}

ViewerSettings::ViewerSettings(std::filesystem::path path) : path_(std::move(path))
{
    std::ifstream input(path_, std::ios::binary);
    std::string section;
    std::string line;
    bool firstLine = true;
    while (std::getline(input, line))
    {
        if (firstLine && line.starts_with("\xEF\xBB\xBF")) line.erase(0, 3);
        firstLine = false;
        const auto text = trim(line);
        if (text.empty() || text.starts_with(';') || text.starts_with('#')) continue;
        if (text.starts_with('['))
        {
            section = text.ends_with(']') ? trim(text.substr(1, text.size() - 2)) : std::string_view{};
            continue;
        }
        const auto equals = text.find('=');
        if (section.empty() || equals == std::string_view::npos) continue;
        const auto key = trim(text.substr(0, equals));
        if (!key.empty()) sections_[section][std::string(key)] = trim(text.substr(equals + 1));
    }
    const auto version = field(sections_, "format", "version");
    if (input.bad() || (!version.empty() && version != "1")) sections_.clear();
}

bool ViewerSettings::msaaEnabled() const
{
    return boolean(field(sections_, "viewer", "msaa"), true);
}

void ViewerSettings::apply(ViewerScene& scene) const
{
    const auto scope = scopeFor(scene);
    const auto get = [&](std::string_view key) { return field(sections_, scope.scene, key); };
    scene.sunAzimuth = number(get("sun_azimuth"), scene.sunAzimuth, -180.0f, 180.0f);
    scene.sunElevation = number(get("sun_elevation"), scene.sunElevation, 25.0f, 85.0f);
    scene.windStrength = number(get("wind_strength"), scene.windStrength, 0.0f, maxWindStrength);
    scene.windPaused = boolean(get("wind_paused"), scene.windPaused);
    const auto count = number(get("tree_count"), scene.treeCount, groveCounts.front(), groveCounts.back());
    if (std::ranges::find(groveCounts, count) != groveCounts.end()) scene.treeCount = count;

    auto& bias = scene.shadowBias;
    bias.constant = number(get("constant_bias"), bias.constant, 0, ShadowBiasSettings::maxConstant);
    bias.slopeScale = number(get("slope_bias"), bias.slopeScale, 0.0f, ShadowBiasSettings::maxSlopeScale);
    bias.samplingTexels = number(get("sampling_bias"), bias.samplingTexels, 0.0f, ShadowBiasSettings::maxReceiverTexels);
    bias.normalTexels = number(get("normal_bias"), bias.normalTexels, 0.0f, ShadowBiasSettings::maxReceiverTexels);

    readShadows(sections_, scope.scene + ".ground", scene.groundShadows);
    readMaterial(sections_, scope.scene + ".ground", scene.groundMaterial);
    const auto meshKeys = namedKeys(scene.asset->meshes);
    for (std::size_t i = 0; i < meshKeys.size(); ++i)
    {
        readShadows(sections_, scope.scene + ".mesh." + meshKeys[i], scene.meshShadows[i]);
    }
    const auto materialKeys = namedKeys(scene.asset->materials);
    for (std::size_t i = 0; i < materialKeys.size(); ++i)
    {
        readMaterial(sections_, scope.asset + ".material." + materialKeys[i], scene.asset->materials[i]);
    }
}

void ViewerSettings::recordMsaa(bool enabled)
{
    sections_["viewer"]["msaa"] = enabled ? "true" : "false";
    scheduleSave();
}

void ViewerSettings::recordScene(const ViewerScene& scene)
{
    const auto scope = scopeFor(scene);
    auto& fields = sections_[scope.scene];
    fields["sun_azimuth"] = formatNumber(scene.sunAzimuth);
    fields["sun_elevation"] = formatNumber(scene.sunElevation);
    fields["wind_strength"] = formatNumber(scene.windStrength);
    fields["wind_paused"] = scene.windPaused ? "true" : "false";
    if (scene.kind == SceneKind::Grove) fields["tree_count"] = formatNumber(scene.treeCount);
    fields["constant_bias"] = formatNumber(scene.shadowBias.constant);
    fields["slope_bias"] = formatNumber(scene.shadowBias.slopeScale);
    fields["sampling_bias"] = formatNumber(scene.shadowBias.samplingTexels);
    fields["normal_bias"] = formatNumber(scene.shadowBias.normalTexels);

    auto& ground = sections_[scope.scene + ".ground"];
    writeShadows(ground, scene.groundShadows);
    writeMaterial(ground, scene.groundMaterial);
    const auto meshKeys = namedKeys(scene.asset->meshes);
    for (std::size_t i = 0; i < meshKeys.size(); ++i)
    {
        writeShadows(sections_[scope.scene + ".mesh." + meshKeys[i]], scene.meshShadows[i]);
    }
    const auto materialKeys = namedKeys(scene.asset->materials);
    for (std::size_t i = 0; i < materialKeys.size(); ++i)
    {
        writeMaterial(sections_[scope.asset + ".material." + materialKeys[i]], scene.asset->materials[i]);
    }
    scheduleSave();
}

void ViewerSettings::reset()
{
    sections_.clear();
    scheduleSave();
}

void ViewerSettings::scheduleSave()
{
    saveAt_ = std::chrono::steady_clock::now() + saveDelay;
}

void ViewerSettings::saveIfDue()
{
    if (saveAt_ && std::chrono::steady_clock::now() >= *saveAt_) save();
}

bool ViewerSettings::save()
{
    if (!saveAt_) return true;

    auto temporary = path_;
    temporary += L"." + std::to_wstring(GetCurrentProcessId()) + L".tmp";
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    output << "; Custom Game Engine viewer settings\n[format]\nversion = 1\n";
    for (const auto& [section, fields] : sections_)
    {
        if (section == "format") continue;
        output << "\n[" << section << "]\n";
        for (const auto& [key, value] : fields) output << key << " = " << value << '\n';
    }
    output.close();
    if (output && MoveFileExW(temporary.c_str(), path_.c_str(),
                             MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
    {
        saveAt_.reset();
        error_.clear();
        return true;
    }

    std::error_code ignored;
    std::filesystem::remove(temporary, ignored);
    error_ = "Could not save viewer-settings.ini in the project folder.";
    saveAt_ = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    return false;
}
