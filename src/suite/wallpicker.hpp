#pragma once

#include <functional>
#include <memory>
#include <nlohmann/json_fwd.hpp>
#include <string>
#include <unordered_map>
#include <vector>

#include "suite/overlay.hpp"

namespace island {

// Honeycomb and cards: the pictures stand in this many rows.
constexpr int kTileRows = 3;

struct Wallpaper {
    std::string path, name, thumb;  // remote: path is the full image's URL, thumb its preview URL
    bool video = false;
    bool engine = false;            // a Wallpaper Engine item: path is its folder, preview its picture
    std::string preview;
    bool remote = false;            // from wallhaven, not downloaded yet
    std::string resolution;
    bool mature = false;            // wallhaven sketchy / nsfw
    float hue = -1, sat = 0, val = 0;  // of the thumbnail's dominant colour
    int64_t mtime = 0;
    // Steam Workshop: its id, kind (scene, video, web), size, subscribers;
    // what Steam has of it; download progress (-1: none going)
    std::string ws_id, ws_type;
    uint64_t ws_size = 0, ws_subs = 0;
    bool subscribed = false, installed = false;
    float progress = -1;
    bool unsupported = false;  // a scene our engine cannot draw yet (its preview stands in)
    bool moving() const { return video || engine; }
    // what to show it with: the file itself, or a still of it
    const std::string& image() const { return moving() ? thumb : path; }
};

// Every wallpaper in the folders, two ways: a carousel of skewed, backlit
// slices (the default, like skwd-wall) or a honeycomb grid with filters.
// Applying goes through the island's backdrop (or a configured command).
class Steam;

class WallPicker : public Overlay {
public:
    explicit WallPicker(Wayland& wl);
    ~WallPicker() override;
    void show();
    void show_wallhaven() { set_remote(true); }
    void show_engine(bool on = true);  // the Steam source: Wallpaper Engine downloads, the Workshop
    void prewarm();
    void backdrop_shown(const std::string& path);
    void dismiss() { slices_close(false); }
    std::function<void(const std::string& msg)> on_toast;
    std::function<void(const std::string& path)> on_apply;  // the island's own backdrop

protected:
    void draw(cairo_t* cr, float w, float h, float dt) override;
    void key(xkb_keysym_t sym, const std::string& utf8, uint32_t mods) override;
    void canvas_scroll(float dy) override;
    void opened() override;
    void closed() override;
    bool animating() const override;
    bool instant_open() const override { return true; }  // it animates itself out of the island
    void canvas_button(Button b, bool pressed, float x, float y) override;
    const char* name() const override { return "niri-island-wallpapers"; }

private:
    std::vector<Wallpaper> walls_;
    std::vector<int> visible_;
    bool scanning_ = false;
    int thumbs_left_ = 0;
    int kind_ = 0;          // 0 all, 1 images, 2 videos (Wallpaper Engine items live in the Steam source)
    int hue_ = -1;          // -1 any, 0..11 hue buckets, 12 greys
    std::string search_;
    int sel_ = -1;
    std::string current_;
    // How the pictures look; everything else (out of the island, the
    // filters, the backlight, the endless horizontal row) is shared.
    int style_ = 0;  // 0 slices, 1 honeycomb (skwd-wall), 2 cards (wallhaven)
    uint64_t release_timer_ = 0;  // frees the caches a while after closing
    void prune_thumbs(const std::string& dir);
    Spring colpos_{0, 460, 34};  // honeycomb/cards: the column in the middle (float)
    // their pictures cut to shape, and their backlight (made by workers)
    cairo_surface_t* tile_cut(const std::string& thumb, int kind, float tw, float th);
    cairo_surface_t* tile_glow(int kind, float tw, float th, Color col);
    void draw_tiles(cairo_t* cr, float W, float H, float t_open, float t_leave, float ie, bool input);
    // ── slices (the default style) ──
    Spring pos_{0, 460, 34};          // carousel position (float index)
    bool moved_ = false;
    double open_t0_ = 0, leave_t0_ = 0, filter_t0_ = 0;
    bool leaving_ = false, left_ = false;
    float last_w_ = 0, last_h_ = 0;
    // applying: the chosen slice straightens and grows to fill the screen
    bool inserting_ = false, inserted_ = false;
    double ins_t0_ = 0;
    int ins_index_ = -1;
    std::string applied_path_;
    bool backdrop_ready_ = false;
    // Everything heavy is made off the main thread and kept between openings.
    cairo_surface_t* big_fit_ = nullptr;
    std::string big_path_, big_loading_;
    std::unordered_map<std::string, cairo_surface_t*> slice_cache_, glow_cache_, shape_cache_;
    std::unordered_map<std::string, bool> pending_;
    int inflight_ = 0;
    cairo_surface_t* veil_ = nullptr;
    std::string veil_key_;
    void free_caches();
    cairo_surface_t* slice_image(const std::string& thumb, int w, int h, float sk);  // nullptr until made
    cairo_surface_t* glow_image(float w, float h, float sk, Color c);
    cairo_surface_t* shape_image(float w, float h, float sk);
    void make_async(const std::string& key, std::function<cairo_surface_t*()> work,
                    std::unordered_map<std::string, cairo_surface_t*>* into);
    void request_big();
    void draw_slices(cairo_t* cr, float w, float h, float dt);
    void draw_filter_bar(cairo_t* cr, float W, float H, float t_in);
    void key_slices(xkb_keysym_t sym, const std::string& utf8);
    void slices_move(int delta);
    void slices_close(bool apply);
    void set_filter(int kind, int hue);

    // ── wallhaven: browse and download (the carousel shows the results) ──
    bool remote_ = false;
    std::vector<Wallpaper> local_walls_;  // the folders' list, kept while browsing
    int wh_sort_ = 0;
    int wh_mature_ = 0;                   // 0 SFW, 1 + sketchy, 2 + NSFW (needs an API key)                     // 0 top, 1 latest, 2 random, 3 views
    int wh_page_ = 0, wh_last_page_ = 1, wh_gen_ = 0;
    bool wh_loading_ = false, wh_failed_ = false;
    std::string wh_query_;
    uint64_t wh_timer_ = 0;
    std::string wh_seed_;
    bool downloading_ = false;
    void set_remote(bool on);
    void wh_fetch(bool more);
    void wh_apply(int index);
    void wh_thumbs(int gen);
    // wallhaven shows as a scrolling grid of cards
    int grid_cols_ = 5;
    float grid_row_h_ = 0, grid_top_ = 0, grid_view_h_ = 0;
    Spring grid_scroll_{0, 320, 30};
    double grid_t0_ = 0;
    void draw_wh_grid(cairo_t* cr, float W, float H, float ie);
    void grid_reveal(int index);
    float grid_content_h() const {
        int rows = (static_cast<int>(visible_.size()) + grid_cols_ - 1) / std::max(1, grid_cols_);
        return rows * grid_row_h_;
    }

    // ── Steam: Wallpaper Engine downloads, or the Workshop (a grid, like wallhaven) ──
    bool steam_ = false;
    bool ws_browse_ = false;  // the Workshop's results rather than the downloads
    int ws_sort_ = 0, ws_page_ = 0, ws_gen_ = 0;  // trend, newest, votes, subscribers
    bool ws_mature_ = false, ws_loading_ = false, ws_more_ = true;
    std::string ws_query_, ws_error_;
    uint64_t ws_timer_ = 0;
    std::string ws_apply_after_;  // to apply once it is downloaded
    std::unique_ptr<Steam> steam_proc_;
    void set_steam(bool on);
    void ws_downloads();          // walls_ = the downloaded items
    void ws_set_browse(bool on);
    void ws_fetch(bool more);
    bool ws_helper();             // the helper, started if needed
    void ws_event(const nlohmann::json& ev);
    void ws_get(int index, bool apply_after);
    void ws_unsubscribe(int index);
    void ws_thumbs(int gen);
    void ws_type_key(xkb_keysym_t sym, const std::string& utf8, bool& used);
    // wallhaven and the Workshop show as a grid; Steam's downloads, like the
    // folders, in the chosen style (the panel comes with a right click)
    bool grid_mode() const { return remote_ || (steam_ && ws_browse_); }
    void side_panel(int vis) {
        if (!steam_) return;
        sel_ = vis;
        pos_.set(static_cast<float>(vis));
        colpos_.set(static_cast<float>(vis / kTileRows));
        open_panel(vis);
    }

    // ── the side panel: one click on a card in the grid ──
    bool panel_ = false;
    Spring panel_t_{0, 300, 28};
    int panel_index_ = -1;  // into walls_
    std::vector<cairo_surface_t*> anim_;  // its animated preview (a GIF), made by a worker
    std::vector<int> anim_ms_;
    std::string anim_key_;
    double anim_t0_ = 0;
    void open_panel(int vis);
    void close_panel();
    void free_anim();
    void draw_panel(cairo_t* cr, float W, float H, float fade);
    void panel_primary(int index);  // ✓ / ▶ / ⬇: whatever finishes it
    static constexpr float kPanelW = 360;

    // ── trying one on the desktop: the picker steps aside; ✓ keeps it, ✕ goes back ──
    bool trying_ = false;
    Spring try_t_{0, 320, 30};
    std::string try_prev_, try_path_;
    int try_index_ = -1;
    bool try_busy_ = false;  // fetching the full picture first
    void try_on(int index);
    void try_end(bool keep);
    void draw_try(cairo_t* cr, float W, float H);
    std::string wh_full_path(const Wallpaper& w) const;  // wallhaven: its full picture in the cache

    void scan();
    void refilter();
    bool has_engine_ = false;  // Wallpaper Engine items among the local ones
    bool has_engine() const { return has_engine_; }
    void apply(int index, bool stay);
    void ensure_visible(int pos);
    std::string current_wallpaper() const;
};

}  // namespace island
