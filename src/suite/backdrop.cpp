#include "suite/backdrop.hpp"

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <csignal>
#include <dirent.h>
#include <malloc.h>
#include <iostream>
#include <nlohmann/json.hpp>
#include <sys/stat.h>
#include <unistd.h>

#include "core/config.hpp"
#include "core/loop.hpp"
#include "services/images.hpp"
#include "services/engine.hpp"
#include "services/workshop.hpp"
#include "ui/paint.hpp"

namespace island {

using json = nlohmann::json;

namespace {

std::string ext_of(const std::string& p) {
    size_t dot = p.rfind('.');
    return dot == std::string::npos ? "" : lower(p.substr(dot + 1));
}
bool is_video(const std::string& p) {
    std::string e = ext_of(p);
    return e == "mp4" || e == "webm" || e == "mkv" || e == "mov";
}
bool is_wallpaper(const std::string& p) {
    std::string e = ext_of(p);
    return is_video(p) || e == "jpg" || e == "jpeg" || e == "png" || e == "webp" || e == "bmp" || e == "gif";
}

void collect(const std::string& dir, std::vector<std::string>& out, int depth = 0) {
    DIR* d = opendir(dir.c_str());
    if (!d) return;
    while (dirent* e = readdir(d)) {
        std::string name = e->d_name;
        if (name.empty() || name[0] == '.') continue;
        std::string full = dir + "/" + name;
        struct stat st {};
        if (stat(full.c_str(), &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            if (depth < 2) collect(full, out, depth + 1);
        } else if (is_wallpaper(full)) {
            out.push_back(full);
        }
    }
    closedir(d);
}

std::string hex(Color c) {
    char b[10];
    snprintf(b, sizeof(b), "#%02x%02x%02x", static_cast<int>(clamp01(c.r) * 255), static_cast<int>(clamp01(c.g) * 255),
             static_cast<int>(clamp01(c.b) * 255));
    return b;
}

}  // namespace

std::string Backdrop::state_path() { return state_dir() + "/wallpaper.json"; }
std::string Backdrop::palette_path() { return cache_dir() + "/colors.json"; }

Backdrop::~Backdrop() {
    stop_video();
    if (img_) cairo_surface_destroy(img_);
}

void Backdrop::init() {
    // A previous run may have left the engine drawing: whatever comes next
    // retires it (a cheap no-op when nothing runs).
    engine_on_ = true;
    std::string text, path;
    if (read_file(state_path(), text)) {
        json j = json::parse(text, nullptr, false);
        if (j.is_object()) path = j.value("path", "");
    }
    if (path.empty() || !file_exists(path)) {
        // First run: take over from whatever skwd-wall had.
        std::string old = home_dir() + "/.cache/skwd-wall-v2/last-wallpaper.json";
        if (read_file(old, text)) {
            json j = json::parse(text, nullptr, false);
            if (j.is_object()) path = j.value("path", "");
        }
    }
    if (!path.empty() && file_exists(path)) set(path);
}

void Backdrop::ensure_surface() {
    // every screen there is (or, none known yet, wherever the compositor says)
    std::vector<wl_output*> want = wl_.outputs();
    if (want.empty()) want.push_back(nullptr);
    for (wl_output* out : want) {
        bool have = false;
        for (auto& o : outs_) have |= o.output == out;
        if (have) continue;
        LayerSpec spec;
        spec.layer = ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND;
        spec.anchor = ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP | ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM |
                      ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT | ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT;
        spec.exclusive_zone = -1;
        spec.keyboard = KeyboardMode::None;
        spec.name = "niri-island-wallpaper";
        spec.output = out;
        Out o;
        o.output = out;
        o.surf = wl_.create_layer(spec);
        LayerSurface* raw = o.surf.get();
        raw->on_configure = [this, raw] {
            for (auto& x : outs_)
                if (x.surf.get() == raw) x.dirty = true;
        };
        raw->on_closed = [this, raw] {
            // that screen went away or changed: drop it, rebuild shortly
            Loop::get().post([this, raw] {
                outs_.erase(std::remove_if(outs_.begin(), outs_.end(), [raw](const Out& x) { return x.surf.get() == raw; }),
                            outs_.end());
                Loop::get().after(0.5, [this] {
                    if (!is_video(path_) && img_) ensure_surface();
                });
            });
        };
        outs_.push_back(std::move(o));
    }
}

void Backdrop::outputs_changed() {
    // surfaces on screens that are gone; new screens get one
    const auto& now = wl_.outputs();
    outs_.erase(std::remove_if(outs_.begin(), outs_.end(),
                               [&](const Out& x) {
                                   return x.output && std::find(now.begin(), now.end(), x.output) == now.end();
                               }),
                outs_.end());
    if (!is_video(path_) && img_ && fitted_) {
        // the copy was made for the screens there were: a new one may be bigger
        set(path_);
        return;
    }
    if (!is_video(path_) && img_) {
        // the one made before any screen was known is replaced by real ones
        if (!now.empty())
            outs_.erase(std::remove_if(outs_.begin(), outs_.end(), [](const Out& x) { return !x.output; }), outs_.end());
        ensure_surface();
        mark_dirty();
    }
}

void Backdrop::drop_surface() { outs_.clear(); }

void Backdrop::stop_video() {
    if (video_.pid > 0) {
        kill(video_.pid, SIGTERM);
        proc::reap(video_);
    }
}

// mpvpaper's mpv listens here, so the video can be paused and resumed.
static std::string mpv_socket() {
    const char* rt = getenv("XDG_RUNTIME_DIR");
    return std::string(rt ? rt : "/tmp") + "/niri-island-mpv.sock";
}

void Backdrop::send_mpv(const std::string& cmd) {
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0) return;
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::string path = mpv_socket();
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path.c_str());
    if (connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0) {
        std::string line = cmd + "\n";
        ssize_t r = write(fd, line.data(), line.size());
        (void)r;
    }
    close(fd);
}

void Backdrop::set_paused(bool paused) {
    if (paused == paused_) return;
    paused_ = paused;
    if (video_.pid > 0) send_mpv(std::string(R"({"command":["set_property","pause",)") + (paused ? "true" : "false") + "]}");
    if (engine_on_) Loop::get().async([paused] { engine::set_paused(paused); });
    if (web_ && web_->running()) web_->set_paused(paused);
}

void Backdrop::set_audio(bool on) {
    if (drawer_ == Drawer::Engine && workshop::is_item(path_)) Loop::get().async([on] { engine::set_muted(!on); });
    if (drawer_ == Drawer::Web && web_) web_->set_muted(!on);
}

// Whoever drew before, except `keep`, stops (our surface is on top by now,
// or the new drawer is up).
void Backdrop::retire(Drawer keep) {
    if (keep != Drawer::Mpv) stop_video();
    if (keep != Drawer::Engine && engine_on_) {
        engine_on_ = false;
        Loop::get().async([] { engine::stop(); });
    }
    if (keep != Drawer::Web && web_ && web_->running()) web_->stop();
}

// Someone else draws `path` now: our surface and the previous drawer go.
void Backdrop::handed_over(Drawer to, uint64_t gen, const std::string& path, double settle) {
    drawer_ = to;
    Loop::get().after(settle, [this, gen, path, to] {
        if (gen != gen_) return;
        drop_surface();
        retire(to);
        if (on_shown) on_shown(path);
    });
}

std::string Backdrop::still() const {
    if (is_video(path_)) return cache_dir() + "/wallpaper-frame.jpg";
    if (workshop::is_item(path_)) {
        workshop::Item it;
        return workshop::read_item(path_, it) ? it.preview : "";
    }
    return path_;
}

void Backdrop::set(const std::string& path) {
    if (path.empty()) return;
    path_ = path;
    save_state();
    uint64_t gen = ++gen_;
    paused_ = false;
    bool item = workshop::is_item(path);
    workshop::Item project;
    if (item) workshop::read_item(path, project);

    std::string still_src = path;
    // Wallpaper Engine scenes and videos, and plain videos: our engine.
    if ((item && engine::renders(project.type)) || (!item && is_video(path) && engine::installed())) {
        bool muted = !item || !config().wallpaper_engine_audio;
        auto err = std::make_shared<std::string>();
        engine::Kind kind = item ? engine::Kind::Engine : engine::Kind::Video;
        Loop::get().async([path, kind, muted, err] { engine::apply(path, kind, muted, err.get()); },
                          [this, gen, path, err] {
                              if (!err->empty()) {
                                  if (gen == gen_ && on_error) on_error(*err);
                                  return;
                              }
                              engine_on_ = true;
                              if (gen != gen_) {  // something newer came meanwhile
                                  if (drawer_ != Drawer::Engine) retire(drawer_);
                                  return;
                              }
                              handed_over(Drawer::Engine, gen, path, 0.3);
                          });
        if (item && !project.preview.empty()) make_palette(project.preview, false);
        else if (!item) make_palette(path, true);
        return;
    }
    if (item && project.type == "web") {
        // Web: waywallen's CEF renderer, its frames on our own surfaces.
        if (!WebWall::available()) {
            if (on_error) on_error("Para los fondos web falta su renderizador");
            return;
        }
        if (!web_) web_ = std::make_unique<WebWall>(wl_);
        web_->on_shown = [this, gen, path] {
            if (gen == gen_) handed_over(Drawer::Web, gen, path, 0.1);
        };
        web_->on_error = [this, gen](const std::string& m) {
            if (gen == gen_ && on_error) on_error(m);
        };
        if (!web_->start(path, !config().wallpaper_engine_audio) && on_error) on_error("El fondo web no pudo empezar");
        if (!project.preview.empty()) make_palette(project.preview, false);
        return;
    }
    if (item) {
        if (on_error) on_error("Este tipo de fondo de Wallpaper Engine no se puede mostrar");
        return;
    }
    if (is_video(path)) {
        // no engine built: mpvpaper paints videos on the background layer by itself
        stop_video();
        if (!proc::has("mpvpaper")) {
            if (on_error) on_error("Para fondos de vídeo hace falta niri-island-engine o mpvpaper");
            return;
        }
        video_ = proc::start({"mpvpaper", "-l", "background", "-o", "no-audio --loop-file=inf --panscan=1.0 --hwdec=auto --input-ipc-server=" + mpv_socket(),
                              "*", path},
                             cache_dir() + "/mpvpaper.log");
        handed_over(Drawer::Mpv, gen, path, 0.6);
        make_palette(path, true);
        return;
    }
    auto img = std::make_shared<cairo_surface_t*>(nullptr);
    Loop::get().async([still_src, img] { *img = load_image_full(still_src); },
                      [this, img, gen, path] {
                          if (gen != gen_) {
                              if (*img) cairo_surface_destroy(*img);
                              return;
                          }
                          if (!*img) {
                              if (on_error) on_error("No se pudo abrir el fondo");
                              return;
                          }
                          if (img_) cairo_surface_destroy(img_);
                          img_ = *img;
                          fitted_ = false;
                          ensure_surface();
                          mark_dirty();
                          drawer_ = Drawer::Own;
                          // Under our surface by now: whoever drew before can stop.
                          Loop::get().after(0.3, [this, gen] {
                              if (gen == gen_) retire(Drawer::Own);
                          });
                      });
    make_palette(still_src, false);
}

void Backdrop::pump() {
    for (auto& o : outs_)
        if (o.dirty && o.surf && o.surf->configured() && !o.surf->frame_pending()) render(o);
    if (!fitted_ && img_ && !outs_.empty()) {
        bool all = true;
        for (auto& o : outs_) all &= !o.dirty && o.surf && o.surf->configured();
        if (all) fit_to_screens();
    }
}

void Backdrop::fit_to_screens() {
    fitted_ = true;
    int need_w = 0, need_h = 0;  // in device pixels, the largest screen
    for (auto& o : outs_) {
        need_w = std::max(need_w, o.surf->width() * o.surf->scale());
        need_h = std::max(need_h, o.surf->height() * o.surf->scale());
    }
    int iw = cairo_image_surface_get_width(img_), ih = cairo_image_surface_get_height(img_);
    if (need_w <= 0 || need_h <= 0 || iw <= 0 || ih <= 0) return;
    double s = std::max(static_cast<double>(need_w) / iw, static_cast<double>(need_h) / ih);  // cover
    if (s > 0.85) return;  // not much bigger than the screens: keep it as it is
    int fw = static_cast<int>(std::ceil(iw * s)), fh = static_cast<int>(std::ceil(ih * s));
    cairo_surface_t* fit = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, fw, fh);
    cairo_t* c = cairo_create(fit);
    cairo_scale(c, s, s);
    cairo_set_source_surface(c, img_, 0, 0);
    cairo_pattern_set_filter(cairo_get_source(c), CAIRO_FILTER_GOOD);
    cairo_paint(c);
    cairo_destroy(c);
    cairo_surface_destroy(img_);
    img_ = fit;
    malloc_trim(0);  // the full picture's pages back to the system
}

void Backdrop::render(Out& o) {
    LayerSurface* surf = o.surf.get();
    cairo_t* cr = surf->begin_frame();
    if (!cr) return;
    o.dirty = false;
    float w = static_cast<float>(surf->width()), h = static_cast<float>(surf->height());
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_rgb(cr, 0.05, 0.05, 0.06);
    cairo_paint(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
    if (img_) {
        int iw = cairo_image_surface_get_width(img_), ih = cairo_image_surface_get_height(img_);
        if (iw > 0 && ih > 0) {
            double s = std::max(w / iw, h / ih);
            cairo_save(cr);
            cairo_translate(cr, (w - iw * s) / 2, (h - ih * s) / 2);
            cairo_scale(cr, s, s);
            cairo_set_source_surface(cr, img_, 0, 0);
            cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
            cairo_paint(cr);
            cairo_restore(cr);
        }
    }
    surf->end_frame({});
    if (on_shown) {
        std::string shown = path_;
        Loop::get().post([this, shown] {
            if (on_shown) on_shown(shown);
        });
    }
}

void Backdrop::save_state() const {
    make_dirs(state_dir());
    json j{{"path", path_}, {"thumb", path_}, {"type", is_video(path_) ? "video" : "static"}};
    write_file_atomic(state_path(), j.dump());
}

void Backdrop::make_palette(const std::string& path, bool video) {
    auto out = std::make_shared<bool>(false);
    Loop::get().async(
        [path, video, out] {
            std::string src = path;
            std::string frame = cache_dir() + "/wallpaper-frame.jpg";
            if (video) {
                proc::run({"ffmpeg", "-y", "-v", "error", "-ss", "1", "-i", path, "-frames:v", "1", "-vf", "scale=640:-2", frame},
                          20000);
                if (!file_exists(frame)) return;
                src = frame;
            }
            json pal;
            if (proc::has("matugen")) {
                // Our own minimal config: the user's may be for another matugen.
                std::string conf = cache_dir() + "/matugen.toml";
                if (!file_exists(conf)) write_file_atomic(conf, "[config]\n\n[templates]\n");
                auto r = proc::run({"matugen", "image", src, "-c", conf, "--json", "hex", "-m", "dark", "-t",
                                    "scheme-tonal-spot", "--source-color-index", "0", "--dry-run"},
                                   30000);
                json j = json::parse(r.out, nullptr, false);
                if (j.is_object() && j.contains("colors")) {
                    auto c = [&](const char* k) -> std::string {
                        auto& v = j["colors"][k];
                        if (v.is_object() && v.contains("dark")) {
                            auto& d = v["dark"];
                            if (d.is_object()) return d.value("color", "");
                            if (d.is_string()) return d.get<std::string>();
                        }
                        return "";
                    };
                    pal = {{"primary", c("primary")},
                           {"primaryText", c("on_primary")},
                           {"surface", c("surface")},
                           {"background", c("background")},
                           {"surfaceText", c("on_surface")},
                           {"surfaceVariant", c("surface_variant")},
                           {"surfaceContainer", c("surface_container_highest")},
                           {"outline", c("outline")},
                           {"tertiary", c("tertiary")}};
                    if (pal["primary"].get<std::string>().empty()) pal = json();
                }
            }
            if (pal.is_null()) {
                // No matugen: the picture's own colour as the accent.
                cairo_surface_t* s = load_image_blocking(src, 96);
                if (!s) return;
                Color c = dominant_color(s);
                cairo_surface_destroy(s);
                Color accent = mix(c, Color{1, 1, 1, 1}, 0.45f);
                pal = {{"primary", hex(accent)}, {"surface", "#141218"}, {"background", "#0f0e12"},
                       {"surfaceText", "#e8e3ee"}, {"tertiary", hex(mix(accent, Color{0.6f, 0.75f, 1, 1}, 0.4f))}};
            }
            make_dirs(cache_dir());
            write_file_atomic(palette_path(), pal.dump());
            *out = true;
        },
        [this, out] {
            if (*out && on_palette) on_palette();
        });
}

void Backdrop::step(int dir) {
    std::vector<std::string> all;
    for (auto& d : config().wallpaper_dirs) collect(expand_home(d), all);
    if (all.empty()) return;
    std::sort(all.begin(), all.end());
    auto it = std::find(all.begin(), all.end(), path_);
    long i = it == all.end() ? 0 : (it - all.begin()) + dir;
    long n = static_cast<long>(all.size());
    i = ((i % n) + n) % n;
    set(all[static_cast<size_t>(i)]);
}

}  // namespace island
