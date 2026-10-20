#include "services/weather.hpp"

#include <cmath>
#include <nlohmann/json.hpp>

#include "core/config.hpp"
#include "core/loop.hpp"
#include "core/util.hpp"
#include "services/images.hpp"

namespace island {

using json = nlohmann::json;

void Weather::describe(int c, bool day, std::string& text, std::string& icon) {
    if (c == 0) { text = "Despejado"; icon = day ? "󰖙" : "󰖔"; }
    else if (c == 1) { text = day ? "Mayormente soleado" : "Mayormente despejado"; icon = day ? "󰖙" : "󰖔"; }
    else if (c == 2) { text = "Parcialmente nublado"; icon = day ? "󰖕" : "󰼱"; }
    else if (c == 3) { text = "Nublado"; icon = "󰖐"; }
    else if (c == 45 || c == 48) { text = "Niebla"; icon = "󰖑"; }
    else if (c >= 51 && c <= 57) { text = "Llovizna"; icon = "󰖗"; }
    else if (c >= 61 && c <= 67) { text = c >= 65 ? "Lluvia fuerte" : "Lluvia"; icon = c >= 65 ? "󰖖" : "󰖗"; }
    else if (c >= 71 && c <= 77) { text = "Nieve"; icon = "󰖘"; }
    else if (c >= 80 && c <= 82) { text = "Chubascos"; icon = "󰖖"; }
    else if (c == 85 || c == 86) { text = "Chubascos de nieve"; icon = "󰼶"; }
    else if (c >= 95) { text = "Tormenta"; icon = "󰙾"; }
    else { text = "Variable"; icon = day ? "󰖕" : "󰼱"; }
}

static std::string hhmm(const std::string& iso) {
    size_t t = iso.find('T');
    return t == std::string::npos ? iso : iso.substr(t + 1, 5);
}

static std::string day_label(const std::string& date_iso, int index) {
    if (index == 0) return "Hoy";
    if (index == 1) return "Mañ";
    int y = 0, m = 0, d = 0;
    if (sscanf(date_iso.c_str(), "%d-%d-%d", &y, &m, &d) != 3) return date_iso;
    struct tm t {};
    t.tm_year = y - 1900;
    t.tm_mon = m - 1;
    t.tm_mday = d;
    t.tm_hour = 12;
    mktime(&t);
    static const char* names[] = {"dom", "lun", "mar", "mié", "jue", "vie", "sáb"};
    return names[t.tm_wday];
}

void Weather::init() {
    reload();
    // Re-check every 10 minutes; noctalia refreshes its cache on its own.
    timer_ = Loop::get().after(600, [this] { reload(); }, 600);
}

void Weather::reload() {
    std::string text;
    WeatherData nd;
    std::string path = expand_home(config().weather_cache);
    bool ok = read_file(path, text) && parse_noctalia(text, nd);
    if (ok) source_ = "noctalia";
    // Our own copy wins when it is newer (noctalia's cache stops updating
    // once noctalia is not running).
    WeatherData own;
    if (read_file(cache_dir() + "/weather.json", text) && parse_openmeteo(text, own) &&
        (!ok || own.fetched_at > nd.fetched_at)) {
        nd = std::move(own);
        ok = true;
        source_ = "open-meteo";
    }
    if (ok) {
        d_ = std::move(nd);
        if (on_change) on_change();
    }
    // Stale or missing: fetch our own copy.
    if (!ok || now_epoch() - d_.fetched_at > 3 * 3600) fetch_fallback();
}

bool Weather::parse_noctalia(const std::string& text, WeatherData& out) try {
    json j = json::parse(text, nullptr, false);
    if (!j.is_object() || !j.contains("snapshot")) return false;
    const json& s = j["snapshot"];
    if (!s.is_object() || !s.contains("current")) return false;
    const json& c = s["current"];
    out.location = s.value("location_name", "");
    out.fetched_at = s.value("fetched_at", (int64_t)0);
    out.temp = static_cast<int>(std::lround(c.value("temperature_c", 0.0)));
    out.feels = static_cast<int>(std::lround(c.value("apparent_temperature_c", c.value("temperature_c", 0.0))));
    out.humidity = c.value("relative_humidity_percent", -1);
    out.wind = static_cast<float>(c.value("wind_speed_kmh", 0.0));
    out.uv = static_cast<float>(c.value("uv_index", -1.0));
    out.day = c.value("is_day", true);
    describe(c.value("weather_code", 0), out.day, out.condition, out.icon);

    std::string now_prefix = fmt_time("%Y-%m-%dT%H");
    if (s.contains("forecast_hours") && s["forecast_hours"].is_array()) {
        const json& hrs = s["forecast_hours"];
        size_t start = 0;
        for (size_t i = 0; i < hrs.size(); ++i)
            if (starts_with(hrs[i].value("time_iso", ""), now_prefix)) {
                start = i;
                break;
            }
        for (size_t i = start + 1; i < hrs.size() && out.hours.size() < 8; ++i) {
            const json& h = hrs[i];
            WeatherHour wh;
            wh.time = hhmm(h.value("time_iso", ""));
            wh.temp = static_cast<int>(std::lround(h.value("temperature_c", 0.0)));
            wh.precip = h.value("precipitation_probability_percent", -1);
            std::string dummy;
            describe(h.value("weather_code", 0), h.value("is_day", true), dummy, wh.icon);
            out.hours.push_back(wh);
        }
    }
    if (s.contains("forecast_days") && s["forecast_days"].is_array()) {
        int i = 0;
        for (auto& d : s["forecast_days"]) {
            if (i == 0) {
                out.sunrise = hhmm(d.value("sunrise_iso", ""));
                out.sunset = hhmm(d.value("sunset_iso", ""));
            }
            WeatherDay wd;
            wd.label = day_label(d.value("date_iso", ""), i);
            wd.min = static_cast<int>(std::lround(d.value("temperature_min_c", 0.0)));
            wd.max = static_cast<int>(std::lround(d.value("temperature_max_c", 0.0)));
            std::string dummy;
            describe(d.value("weather_code", 0), true, dummy, wd.icon);
            out.days.push_back(wd);
            if (++i >= 6) break;
        }
    }
    out.valid = true;
    return true;
} catch (...) {
    return false;
}

bool Weather::parse_openmeteo(const std::string& text, WeatherData& out) try {
    json j = json::parse(text, nullptr, false);
    if (!j.is_object() || !j.contains("current")) return false;
    const json& c = j["current"];
    out.location = j.value("_location", "");
    out.fetched_at = j.value("_fetched_at", (int64_t)0);
    out.temp = static_cast<int>(std::lround(c.value("temperature_2m", 0.0)));
    out.feels = static_cast<int>(std::lround(c.value("apparent_temperature", 0.0)));
    out.humidity = c.value("relative_humidity_2m", -1);
    out.wind = static_cast<float>(c.value("wind_speed_10m", 0.0));
    out.uv = static_cast<float>(c.value("uv_index", -1.0));
    out.day = c.value("is_day", 1) != 0;
    describe(c.value("weather_code", 0), out.day, out.condition, out.icon);
    if (j.contains("hourly")) {
        const json& h = j["hourly"];
        auto times = h.value("time", json::array());
        std::string now_prefix = fmt_time("%Y-%m-%dT%H");
        size_t start = 0;
        for (size_t i = 0; i < times.size(); ++i)
            if (starts_with(times[i].get<std::string>(), now_prefix)) start = i;
        for (size_t i = start + 1; i < times.size() && out.hours.size() < 8; ++i) {
            WeatherHour wh;
            wh.time = hhmm(times[i].get<std::string>());
            wh.temp = static_cast<int>(std::lround(h["temperature_2m"][i].get<double>()));
            if (h.contains("precipitation_probability") && h["precipitation_probability"][i].is_number())
                wh.precip = h["precipitation_probability"][i].get<int>();
            std::string dummy;
            describe(h["weather_code"][i].get<int>(), h["is_day"][i].get<int>() != 0, dummy, wh.icon);
            out.hours.push_back(wh);
        }
    }
    if (j.contains("daily")) {
        const json& d = j["daily"];
        auto times = d.value("time", json::array());
        for (size_t i = 0; i < times.size() && i < 6; ++i) {
            WeatherDay wd;
            wd.label = day_label(times[i].get<std::string>(), static_cast<int>(i));
            wd.min = static_cast<int>(std::lround(d["temperature_2m_min"][i].get<double>()));
            wd.max = static_cast<int>(std::lround(d["temperature_2m_max"][i].get<double>()));
            std::string dummy;
            describe(d["weather_code"][i].get<int>(), true, dummy, wd.icon);
            if (i == 0) {
                out.sunrise = hhmm(d["sunrise"][0].get<std::string>());
                out.sunset = hhmm(d["sunset"][0].get<std::string>());
            }
            out.days.push_back(wd);
        }
    }
    out.valid = true;
    return true;
} catch (...) {
    return false;
}

void Weather::fetch_fallback() {
    if (fetching_) return;
    double lat = config().latitude, lon = config().longitude;
    std::string name = config().location_name;
    if (lat == 0.0 && lon == 0.0) {
        // Borrow noctalia's location if we have none of our own.
        std::string text;
        if (read_file(home_dir() + "/.cache/noctalia/location.json", text)) {
            json j = json::parse(text, nullptr, false);
            if (j.is_object()) {
                lat = j.value("latitude", 0.0);
                lon = j.value("longitude", 0.0);
                if (name.empty()) name = j.value("name", "");
            }
        }
    }
    if (lat == 0.0 && lon == 0.0) return;
    fetching_ = true;
    auto body = std::make_shared<std::string>();
    char url[512];
    snprintf(url, sizeof(url),
             "https://api.open-meteo.com/v1/forecast?latitude=%.4f&longitude=%.4f"
             "&current=temperature_2m,apparent_temperature,relative_humidity_2m,is_day,weather_code,wind_speed_10m,uv_index"
             "&hourly=temperature_2m,weather_code,is_day,precipitation_probability"
             "&daily=weather_code,temperature_2m_max,temperature_2m_min,sunrise,sunset&timezone=auto&forecast_days=6",
             lat, lon);
    std::string u = url;
    Loop::get().async([u, body] { http_get(u, *body, 15); },
                      [this, body, name] {
                          fetching_ = false;
                          json j = json::parse(*body, nullptr, false);
                          if (!j.is_object() || !j.contains("current")) return;
                          j["_location"] = name;
                          j["_fetched_at"] = now_epoch();
                          make_dirs(cache_dir());
                          write_file_atomic(cache_dir() + "/weather.json", j.dump());
                          WeatherData nd;
                          if (parse_openmeteo(j.dump(), nd) && (!d_.valid || nd.fetched_at > d_.fetched_at)) {
                              d_ = std::move(nd);
                              source_ = "open-meteo";
                              if (on_change) on_change();
                          }
                      });
}

}  // namespace island
