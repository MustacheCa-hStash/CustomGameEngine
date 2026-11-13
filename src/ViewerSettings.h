#pragma once

#include "ViewerScene.h"

#include <chrono>
#include <map>
#include <string_view>

class ViewerSettings
{
public:
    explicit ViewerSettings(std::filesystem::path path);

    bool msaaEnabled() const;
    void apply(ViewerScene& scene) const;
    void recordMsaa(bool enabled);
    void recordScene(const ViewerScene& scene);
    void reset();
    void saveIfDue();
    bool save();
    const std::string& error() const { return error_; }

private:
    using Fields = std::map<std::string, std::string, std::less<>>;
    using Sections = std::map<std::string, Fields, std::less<>>;

    void scheduleSave();

    std::filesystem::path path_;
    Sections sections_;
    std::optional<std::chrono::steady_clock::time_point> saveAt_;
    std::string error_;
};
