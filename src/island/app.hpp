#pragma once

#include <map>
#include <set>
#include <memory>
#include <string>
#include <vector>

#include "core/filewatch.hpp"
#include "core/ipc.hpp"
#include "core/loop.hpp"
#include "core/spring.hpp"
#include "core/wayland.hpp"
#include "services/apps.hpp"
#include "services/audio.hpp"
#include "services/bluetooth.hpp"
#include "services/cava.hpp"
#include "services/dbus.hpp"
#include "services/mpris.hpp"
#include "services/network.hpp"
#include "services/niri.hpp"
#include "services/notifications.hpp"
#include "services/power.hpp"
#include "services/store.hpp"
#include "services/sysmon.hpp"
#include "services/timer.hpp"
#include "services/tray.hpp"
#include "services/weather.hpp"
#include "ui/textfield.hpp"
#include "ui/ui.hpp"
#include "ui/widgets.hpp"
#include "island/edgebar.hpp"
#include "suite/backdrop.hpp"
#include "suite/hosted.hpp"
#include "suite/screensaver.hpp"
#include "suite/lock.hpp"
#include "suite/wallpicker.hpp"
#include "suite/capture.hpp"
#include "services/recorder.hpp"

namespace island {

// What the island is showing. Panels are opened by the user and stack (Back
// pops); the rest are layers the island arbitrates between on its own.
enum class View {
    Rest,
    Glance,
    Music,
    Notification,
    Osd,
    // panels
    Control,
    Wifi,
    WifiPassword,
    WifiInfo,
    Bluetooth,
    Sound,
    Weather,
    Calendar,
    DatePicker,
    Board,
    Notes,
    NoteEditor,
    Launcher,
    Session,
    Stats,
    Timer,
    Record,
    Clipboard,
    Inbox,
    Tray,
};

struct TrayItem;
cairo_surface_t* tray_icon(const TrayItem& it);

struct OsdInfo {
    enum Kind { Volume, Brightness, Mic, Battery, Message } kind = Volume;
    float value = 0;       // 0..1, < 0 = no bar
    std::string glyph, label, detail;
    bool muted = false;
    double until = 0;
};

struct LauncherItem {
    std::string glyph;     // nerd-font glyph, or empty when `icon` is set
    std::string icon;      // ImageCache key
    std::string title, subtitle, hint;
    std::function<void()> run;
    bool close = true;
};

class App {
public:
    App();
    ~App();
    bool init();
    void run();
    std::string command(const std::string& cmd);
    // The island's outline: a capsule, or docked to the top edge with ears.
    static void island_outline(cairo_t* cr, const Rect& r, float radius, bool docked);

private:
    // ── platform & services ────────────────────────────────────────────
    Loop loop_;
    Wayland wl_;
    std::unique_ptr<LayerSurface> island_;
    // The resting island's audio spectrum lives on its own little surface on
    // top: while the island is still, only those bars are redrawn (30 times
    // a second), not the whole island and its capsules.
    std::unique_ptr<SubSurface> spec_sub_;
    bool spec_capture_ = false;  // drawing the island: note where the bars go, don't draw them
    bool spec_seen_ = false, spec_live_ = false, spec_dirty_ = false;
    Rect spec_rect_;             // where, in the island surface's coordinates
    int spec_bars_ = 4;
    bool spec_playing_ = false;
    Color spec_color_;
    double spec_last_ = 0;
    void pump_spectrum();
    WallPicker wallpicker_{wl_};
    island::Capture capture_{wl_};
    EdgeBar bar_left_{wl_, true}, bar_right_{wl_, false};
    HostedOverlay settings_{wl_, "niri-island-settings"};
    // The capture bubble: rises from the bottom edge (Mod+S), screenshot or
    // recording, each with its own options.
    HostedOverlay capbar_{wl_, "niri-island-capbar"};
    Spring capbar_t_{0, 420, 34};    // 0 hidden … 1 out
    Spring capbar_h_{0, 420, 36};    // its height, between the two modes
    int capbar_mode_ = 0;            // 0 screenshot, 1 recording
    bool capbar_leaving_ = false;
    Rect capbar_rect_;               // where it is on screen (clicks inside never close it)
    void open_capbar(int mode);  // the bubble alone (start_capture freezes first)
    void close_capbar(bool now = false);
    void draw_capbar(cairo_t* cr, Ui& u, float w, float h, float dt);
    void capbar_key(xkb_keysym_t sym, uint32_t mods);
    Backdrop backdrop_{wl_};
    Screensaver screensaver_{wl_};
    // one more for every other screen; they start and stop together
    std::vector<std::unique_ptr<Screensaver>> savers_;
    void start_screensaver();
    // Idle (from the compositor): the screensaver after N minutes, the lock
    // after M if set. Rebuilt when those settings change.
    std::unique_ptr<IdleWatch> idle_saver_, idle_lock_;
    std::string idle_key_;
    void setup_idle();
    void stop_screensaver();
    std::vector<Overlay*> screensavers();
    island::Lock lock_{wl_};
    float screen_w_ = 0, screen_h_ = 0;  // the output's logical size (from niri)
    void update_wallpaper_pause();
    void lock_screen(double preview = 0);
    void wallpaper_next(int dir = 1);
    int set_section_ = 0;
    TextField set_search_, set_edit_;
    std::string set_edit_key_;
    bool set_edit_commit_ = false;
    Spring set_scroll_{0, 320, 30};
    float set_content_ = 0;
    uint64_t set_save_timer_ = 0;
    int set_pad_mode_ = 0;  // shadow pad drag: 1 the dot, 2 softness arc, 3 opacity arc
    void draw_shadow_pad(cairo_t* cr, Ui& u, const Rect& card);
    void draw_modules_editor(cairo_t* cr, Ui& u, const Rect& card);
    void open_settings();
    void save_config_soon();
    void settings_key(xkb_keysym_t sym, const std::string& utf8, uint32_t mods);
    void draw_settings(cairo_t* cr, Ui& u, float w, float h, float dt);
    std::string layout_key_;
    std::string bar_sig_;
    int config_gen_ = 0;
    std::string bar_signature() const;
    Recorder recorder_;
    // capture panel
    bool cap_windows_ = false;   // choosing a window to capture
    int cap_delay_ = 0;
    // Screenshots on top of one another: one more while the editor is open,
    // never a third; and never two being taken at once.
    int shot_depth_ = 0;
    bool shot_pending_ = false;
    bool cap_edit_ = true;
    int scale() const { return island_ ? island_->scale() : 1; }
    Ui ui_;
    Bus user_bus_, sys_bus_;
    Mpris mpris_;
    Notifications notifs_;
    Audio audio_;
    Power power_;
    Network net_;
    Bluetooth bt_;
    Niri niri_;
    SysMon sysmon_;
    Weather weather_;
    Timer timer_;
    Stopwatch stopwatch_;
    island::Tray tray_;
    Store store_;
    Apps apps_;
    Cava cava_;
    IpcServer ipc_;
    FileWatch files_;

    // ── surface & frame ────────────────────────────────────────────────
    static constexpr int kSurfaceW = 940;
    static constexpr int kSurfaceH = 640;
    bool dirty_ = true;
    double last_frame_ = 0;
    double low_rate_until_ = 0;
    uint64_t low_rate_timer_ = 0;
    Rect last_bbox_{};
    std::vector<Rect> last_input_;
    float anim_time_ = 0;
    std::map<std::string, cairo_surface_t*> shadow_cache_;  // nine-slice sources, per size
    // Morph snapshots: outgoing and incoming content, drawn once per transition.
    cairo_surface_t* snap_prev_ = nullptr;
    cairo_surface_t* snap_next_ = nullptr;
    View snap_next_view_ = View::Rest;
    float snap_next_w_ = 0, snap_next_h_ = 0;
    std::string clock_key_;

    // ── layer arbitration ──────────────────────────────────────────────
    std::vector<View> nav_;            // open panels, top = shown
    View shown_ = View::Rest;
    View prev_ = View::Rest;
    float morph_ = 1.0f;               // 0 → 1 cross-fade between prev_ and shown_
    Spring w_{132, 420, 31}, h_{32, 420, 32};
    float target_w_ = 132, target_h_ = 32;
    float prev_w_ = 132, prev_h_ = 32;
    bool pointer_in_ = false;
    bool pointer_over_island_ = false;
    double hover_since_ = 0;
    bool summary_ = false;             // glance / music after a dwell
    uint64_t hover_timer_ = 0;
    OsdInfo osd_;
    uint64_t osd_timer_ = 0;

    // workspaces bubble
    Spring ws_pill_{0, 320, 24}, ws_alpha_{1, 260, 26};

    // ── per-view state ─────────────────────────────────────────────────
    // notification
    bool notif_expanded_ = false;
    uint32_t notif_shown_id_ = 0;
    double notif_shown_at_ = 0;
    // control centre
    int cc_month_offset_ = 0;
    Spring cc_notif_scroll_{0, 320, 30};
    float cc_notif_content_ = 0;
    // wifi
    TextField pass_field_;
    std::string pass_ssid_;
    bool pass_visible_ = false;
    WifiDetails wifi_info_;
    bool wifi_qr_ = false;
    bool wifi_censor_ = true;
    double toast_until_ = 0;
    std::string toast_text_;
    Spring wifi_scroll_{0, 320, 30}, bt_scroll_{0, 320, 30};
    // calendar & tasks
    int cal_offset_ = 0;
    int cal_day_ = -1;                 // selected day in the shown month, -1 = all upcoming
    TextField task_field_;
    bool task_input_ = false;
    Spring task_scroll_{0, 320, 30};
    // date picker
    int picker_task_ = -1;
    int picker_offset_ = 0;
    int picker_y_ = 0, picker_m_ = 0, picker_d_ = 0;
    // board
    int board_detail_ = -1;
    TextField board_field_;
    bool board_input_ = false;
    int drag_task_ = -1;
    float drag_x_ = 0, drag_y_ = 0, drag_dx_ = 0, drag_dy_ = 0;
    bool drag_moved_ = false;
    TextField board_notes_;
    bool board_notes_focus_ = false;
    // notes
    int note_id_ = -1;
    TextField note_title_, note_body_;
    int note_focus_ = 1;               // 0 title, 1 body
    uint64_t note_save_timer_ = 0;
    Spring notes_scroll_{0, 320, 30};
    // card <-> paper transition
    std::map<int, Rect> note_card_rects_;   // where each card sat in the deck
    Rect note_from_{};                      // the card the editor grew out of
    Spring note_expand_{1, 230, 25};        // 0 card, 1 full paper
    int note_return_id_ = -1;               // the card flying back into the deck
    Spring note_collapse_{0, 230, 25};      // 1 full paper, 0 back in its slot
    bool note_dirty_ = false;
    bool notes_archived_view_ = false;
    // timer panel: 0 choose, 1 timer, 2 stopwatch
    int tm_mode_ = 0;
    Rect tm_from_{};
    Rect tm_cards_[2]{};
    Spring tm_anim_{1, 260, 26};
    int tm_h_ = 0, tm_m_ = 10, tm_s_ = 0;
    // discreet notification: the sender's icon shaking beside the island
    std::string ping_icon_, ping_app_;
    std::shared_ptr<cairo_surface_t> ping_pixels_;
    double ping_at_ = -10;
    Spring ping_alpha_{0, 380, 26};
    uint64_t ping_timer_ = 0;
    // sound panel
    int snd_tab_ = 0;  // 0 outputs, 1 inputs, 2 apps
    Spring snd_tab_x_{0, 320, 30};
    Spring snd_scroll_{0, 320, 30};
    double note_saved_at_ = 0;
    // launcher
    TextField query_;
    int sel_ = 0;
    std::vector<LauncherItem> results_;
    std::vector<std::string> clip_items_;
    bool clip_loading_ = false;
    // clipboard panel
    struct ClipItem {
        std::string line, id, content, meta, thumb;
        int kind = 0;  // 0 text, 1 image, 2 link, 3 colour
        Color color{};
    };
    std::vector<ClipItem> clip_list_;
    TextField clip_query_;
    int clip_filter_ = 0;  // 0 all, 1 text, 2 images, 3 links
    Spring clip_scroll_{0, 320, 30};
    double clip_wipe_armed_ = 0;
    bool clip_panel_loading_ = false;
    // two panes: the list, and the selected item in full
    std::string clip_sel_id_;
    bool clip_search_focus_ = false;    // keyboard taken only to search
    int clip_key_nav_ = 0;
    uint64_t clip_target_win_ = 0;
    std::string clip_target_app_;  // the window to paste into (focus may wander meanwhile)
    int session_sel_ = 0;   // keyboard selection in the session panel
    void session_activate(int i);              // pending from the keyboard: -1 up, 1 down, 2 paste
    Spring clip_list_scroll_{0, 320, 30}, clip_text_scroll_{0, 320, 30};
    std::string clip_detail_id_, clip_detail_text_;
    cairo_surface_t* clip_detail_img_ = nullptr;
    bool clip_detail_loading_ = false;
    double clip_last_click_ = 0;
    std::string clip_last_click_id_;
    void clip_select(const ClipItem& it);
    void clip_paste(const ClipItem& it);   // copy, close, and paste into the input that had focus
    void load_clipboard();
    void clip_copy(const ClipItem& it);
    void clip_delete(const ClipItem& it);
    void draw_clipboard(cairo_t* cr, float w, float h);
    // inbox and tray panels (views/inbox.cpp)
    Spring inbox_scroll_{0, 320, 30};
    float inbox_content_ = 0;
    std::string tray_menu_key_;
    std::vector<TrayMenuItem> tray_menu_;
    std::vector<int> tray_menu_path_;
    bool tray_menu_loading_ = false, tray_menu_pending_ = false;
    void draw_inbox(cairo_t* cr, float w, float h);
    void draw_tray(cairo_t* cr, float w, float h);
    void open_tray_menu(const std::string& key);
    Spring launcher_scroll_{0, 360, 32};
    // session
    std::string confirm_;
    uint64_t confirm_timer_ = 0;
    // timer panel
    std::string timer_preset_;

    // ── lifecycle ──────────────────────────────────────────────────────
    void wire_services();
    void wire_wayland();
    void create_surface();
    int before_poll();
    void request() { dirty_ = true; }
    void render();
    void tick_second();

    // ── arbitration & navigation ───────────────────────────────────────
    View decide() const;
    bool is_panel(View v) const { return v >= View::Control; }
    bool panel_open() const { return !nav_.empty(); }
    void open(View v, bool replace_stack = false);
    void back();
    void close_all();
    void on_view_enter(View v);
    void on_view_leave(View v);
    void update_keyboard();
    void sizes_for(View v, float& w, float& h);
    void show_osd(OsdInfo info, double seconds = 1.8);
    void show_toast(const std::string& text);

    // ── input ──────────────────────────────────────────────────────────
    void on_motion(double x, double y);
    void on_button(Button b, bool pressed);
    void on_scroll(double dx, double dy, bool discrete);
    void on_key(xkb_keysym_t sym, const std::string& utf8, uint32_t mods);
    void island_click(Button b);
    Rect island_rect() const;
    Rect bubble_rect() const;
    Rect ping_rect() const;
    // bar modules and layouts (views/bar.cpp)
    bool attached() const;
    bool sides_docked() const;
    float bar_h() const;
    float shape_radius(float h) const;  // the island's corner radius at height h  // the resting island's height (configurable)
    // Corner radius for a shape of height h: rest pills and capsules go from
    // softened-square to full round; panels from 9 to 34 px.
    static float corner(float h, bool panel);
    Rect cap_geom(float x, float w) const;  // a side capsule's rect (docked / pill)
    Color glass_body() const;  // the island's body colour, translucent with glass
    const std::string& bar_layout() const;
    void capsule(cairo_t* cr, const Rect& r);
    // strength: 1 for open panels, less for pills; the rest comes from the settings.
    void island_shadow(cairo_t* cr, const Rect& r, float radius, bool docked, float strength);
    float ws_width() const;
    struct Cap {
        std::vector<std::string> ids;
        bool joined = false;  // status modules sharing one capsule
        float w = 0;
    };
    float module_w(const std::string& id) const;
    std::vector<Cap> side_caps(bool left) const;
    float caps_span(const std::vector<Cap>& caps, float gap) const;
    std::vector<Rect> side_rects(bool left) const;
    void draw_module(cairo_t* cr, Ui& u, const std::string& id, const Rect& r, float origin);
    void draw_cap(cairo_t* cr, Ui& u, const Rect& r, const Cap& c, float origin, bool background);
    void draw_side(cairo_t* cr, bool left);
    void draw_island_sides(cairo_t* cr, float w, float h);
    void draw_workspaces(cairo_t* cr, Ui& u, const Rect& r);
    // Panels opened from a corner strip grow under the button they came from.
    Spring anchor_x_{-1, 260, 28};
    Spring anchor_y_{0, 260, 28};
    bool anchor_pending_ = false;
    float anchor_target_ = -1;
    bool anchored_ = false;
    bool osd_pinned_ = false;  // opened by a click: an outside click closes it
    float surface_w() const;
    void anchor_at(float screen_x);
    Rect status_rect() const;
    void paint_edge(cairo_t* cr, Ui& u, float w, float h, bool left, std::vector<Rect>& input);
    void apply_layout();
    void draw_rest_inner(cairo_t* cr, float w, float h);
    float one_island_side() const;  // each side's width in the "island" layout (0 otherwise)
    Spring st_alpha_{0, 300, 28};
    void draw_ping(cairo_t* cr);
    bool text_input_active() const;
    void paste_into(TextField* f);

    // ── drawing ────────────────────────────────────────────────────────
    void draw_view(cairo_t* cr, View v, float w, float h);
    void draw_shadow(cairo_t* cr, const Rect& r, float radius, float alpha, float spread);
    cairo_surface_t* snapshot(View v, float w, float h);
    void draw_rest(cairo_t* cr, float w, float h);
    void draw_glance(cairo_t* cr, float w, float h);
    void draw_music(cairo_t* cr, float w, float h);
    void draw_notification(cairo_t* cr, float w, float h);
    void draw_osd(cairo_t* cr, float w, float h);
    void draw_control(cairo_t* cr, float w, float h);
    void draw_wifi(cairo_t* cr, float w, float h);
    void draw_wifi_password(cairo_t* cr, float w, float h);
    void draw_wifi_info(cairo_t* cr, float w, float h);
    void draw_bluetooth(cairo_t* cr, float w, float h);
    void draw_sound(cairo_t* cr, float w, float h);
    void draw_weather(cairo_t* cr, float w, float h);
    void draw_calendar(cairo_t* cr, float w, float h);
    void draw_picker(cairo_t* cr, float w, float h);
    void draw_board(cairo_t* cr, float w, float h);
    void draw_notes(cairo_t* cr, float w, float h);
    void draw_note_editor(cairo_t* cr, float w, float h);
    void draw_launcher(cairo_t* cr, float w, float h);
    void draw_session(cairo_t* cr, float w, float h);
    void draw_stats(cairo_t* cr, float w, float h);
    void draw_timer(cairo_t* cr, float w, float h);
    void draw_timer_setup(cairo_t* cr, float w, float h);
    void draw_stopwatch(cairo_t* cr, float w, float h);
    void draw_clock_strip(cairo_t* cr, const Rect& r);   // active timer/stopwatch in the summaries
    void draw_rec_strip(cairo_t* cr, const Rect& r);     // a recording, with its controls
    std::vector<std::string> perf_metrics_of(const std::string& id) const;  // "perf" or "perf:<metric>"
    float perf_num_w(const std::string& metric) const;
    float perf_width(const std::vector<std::string>& metrics) const;
    float perf_ring_margin() const;
    float perf_cap_h() const;
    void draw_perf(cairo_t* cr, Ui& u, const Rect& r, float origin, const std::vector<std::string>& metrics);
    // Modules with options (gear in the modules list): how open each card is.
    std::map<std::string, Spring> mod_open_;
    float module_options_h(const std::string& id) const;  // the options' height when open
    std::string module_option_words(const std::string& id) const;  // for the settings search
    std::set<std::string> mod_auto_open_;                 // opened by a search
    float module_extra_h(const std::string& id) const;     // …as it is right now
    void draw_module_options(cairo_t* cr, Ui& u, const std::string& id, const Rect& area);
    bool perf_on() const;
    int summary_strips() const { return (clock_active() ? 1 : 0) + (recorder_.active() ? 1 : 0); }
    // Below the glance or the player, from `y`: the strips that are running.
    void draw_summary_strips(cairo_t* cr, float w, float y, float step);
    bool clock_active() const { return timer_.running() || stopwatch_.running(); }
    void timer_mode(int mode, const Rect& from);

    // shared pieces
    void draw_month(cairo_t* cr, const Rect& r, int offset, int selected_day, bool interactive, const char* id,
                    std::function<void(int y, int m, int d)> on_pick, std::function<void(int delta)> on_nav,
                    int mark_y = -1, int mark_m = -1, int mark_d = -1);
    void draw_art(cairo_t* cr, const Rect& r, float radius);
    void draw_spectrum(cairo_t* cr, const Rect& r, int bars, bool playing, Color c);
    void draw_text_field(cairo_t* cr, uint64_t id, const Rect& r, TextField& f, const std::string& placeholder,
                         bool focused, bool secret = false, float font_px = 13.5f);
    Color media_accent();
    std::string notif_icon_key(const Notification& n) const;

    // ── actions ────────────────────────────────────────────────────────
    void activate_notification(const NotificationPtr& n);
    void open_wifi_info(const WifiNetwork& net);
    void open_password(const std::string& ssid);
    void open_note(int id);
    void flush_note();
    void submit_task_input();
    void submit_board_input();
    void open_picker(int task_id);
    void run_session(const std::string& action);
    void set_airplane(bool on);
    void open_wallpaper_picker();
    // `counted`: the capture already went through the limit (from the bubble).
    void take_screenshot(int mode, int delay, uint64_t window_id = 0, bool edit = true, bool counted = false);
    // Mod+S: the screen freezes as it is, and the bubble asks what to do.
    void start_capture(int mode);
    void capture_action(int mode);          // region / screen, on the frozen screen
    void save_screenshot(cairo_surface_t* img);
    void capture_keys(bool on);             // keys go to the capture alone
    bool capture_keys_ = false;
    // Demo mode (debug-demo on|off), for screenshots: personal things are left
    // out of what is drawn — the weather's place, Wi-Fi names, notifications,
    // tasks, the name on the lock screen. Nothing stored changes.
    bool demo_ = false;
    // A window is full screen (a video, a game): nothing of the island shows —
    // keys still work — unless a panel was opened on purpose.
    bool fullscreen_hide() const { return nav_.empty() && niri_.fullscreen(screen_w_, screen_h_); }
    void start_recording();
    void with_output(std::function<void(const std::string&)> fn);
    void notify_file(const std::string& title, const std::string& path, cairo_surface_t* thumb, const std::string& body);
    void draw_record(cairo_t* cr, float w, float h);
    bool airplane() const;
    void refresh_launcher();
    void launcher_activate(int index);
    std::string session_label(const std::string& action) const;
};

}  // namespace island
