#pragma once

#include <cstdint>

#include <functional>
#include <string>
#include <vector>

namespace island {

struct WeatherHour {
    std::string time;  // "14:00"
    std::string icon;
    int temp = 0;
    int precip = -1;   // %
};

struct WeatherDay {
    std::string label;  // "Hoy", "mar"…
    std::string icon;
    int min = 0, max = 0;
};

struct WeatherData {
    bool valid = false;
    std::string location;
    std::string condition, icon;
    int temp = 0, feels = 0, humidity = -1;
    float wind = 0, uv = -1;
    bool day = true;
    std::string sunrise, sunset;
    int64_t fetched_at = 0;
    std::vector<WeatherHour> hours;
    std::vector<WeatherDay> days;
};

class Weather {
public:
    void init();
    const WeatherData& data() const { return d_; }
    void reload();      // re-read the cache
    std::string source() const { return source_; }
    std::function<void()> on_change;

    static void describe(int wmo, bool day, std::string& text, std::string& icon);

private:
    WeatherData d_;
    std::string source_;
    bool fetching_ = false;
    uint64_t timer_ = 0;
    void fetch_fallback();
    bool parse_noctalia(const std::string& text, WeatherData& out);
    bool parse_openmeteo(const std::string& text, WeatherData& out);
};

}  // namespace island
