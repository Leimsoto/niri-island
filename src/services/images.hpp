#pragma once

#include <cairo.h>
#include <functional>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include "core/util.hpp"

namespace island {

// Worker-safe helpers -------------------------------------------------------

// Decodes an image (file path, file:// URI or http(s) URL) scaled to fit
// max_px, into a cairo surface. http(s) is downloaded to the cache first.
cairo_surface_t* load_image_blocking(const std::string& src, int max_px);
// Decodes at the file's own size (screenshots).
cairo_surface_t* load_image_full(const std::string& path);
// Raw pixels from a notification's image-data hint.
cairo_surface_t* surface_from_rgba(int w, int h, int stride, bool alpha, int channels, const uint8_t* data,
                                   size_t len, int max_px);
// Resolves an icon name through the user's icon theme (Tela-circle, hicolor…).
std::string find_icon_blocking(const std::string& name, int size);
// Average colour of a surface, saturation-boosted: the art's mood.
Color dominant_color(cairo_surface_t* s);
// Downloads a URL to a file (libcurl). Returns true on success.
bool http_get(const std::string& url, std::string& body, long timeout_s = 15);
bool http_download(const std::string& url, const std::string& dest, long timeout_s = 20);

// Main-thread cache --------------------------------------------------------

class ImageCache {
public:
    static ImageCache& get();
    ~ImageCache();

    // Returns the surface when ready; otherwise starts loading it and calls
    // on_ready (once per key) when done. `src` may be an icon name: prefix it
    // with "icon:" to go through the theme.
    cairo_surface_t* request(const std::string& src, int max_px);
    Color color_of(const std::string& src, int max_px, Color fallback);
    std::function<void()> on_ready;
    void clear();

private:
    struct Entry {
        cairo_surface_t* surface = nullptr;
        Color color{};
        bool failed = false;
        double used = 0;
        size_t bytes = 0;
    };
    std::unordered_map<std::string, Entry> entries_;
    size_t bytes_ = 0;  // decoded pixels held, kept under a budget
    std::unordered_set<std::string> loading_;
    void trim();
};

}  // namespace island
