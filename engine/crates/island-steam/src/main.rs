//! niri-island-steam: the picker's line to the Steam Workshop.
//!
//! Lives while the picker shows the Workshop: one JSON request per line on
//! stdin, one JSON event per line on stdout; exits when stdin closes. Talks to
//! the running Steam client through Steamworks as Wallpaper Engine (431960),
//! so it needs Steam open and the account owning it; Steam downloads.
//!
//! Requests:
//!   {"op":"search","query":"…","sort":0..4,"page":1,"mature":false}
//!   {"op":"subscribe","id":"123"}      subscribes, downloads, reports progress
//!   {"op":"unsubscribe","id":"123"}    unsubscribes and deletes the download
//!   {"op":"state"}                      subscribed ids
//! Events: {"ev":"ready"|"results"|"progress"|"done"|"unsubscribed"|"state"|"error", …}
//!
//! Search and download follow skwd-steam (skwd-deck, MIT).

use std::collections::HashMap;
use std::io::{BufRead, Write};
use std::sync::mpsc;
use std::time::{Duration, Instant};

use serde_json::{Value, json};
use steamworks::{
    AppIDs, AppId, Client, ItemState, PublishedFileId, UGCQueryType, UGCStatisticType, UGCType,
};

const APP_ID: u32 = 431_960;

fn emit(value: &Value) {
    let mut out = std::io::stdout().lock();
    let _ = writeln!(out, "{value}");
    let _ = out.flush();
}

fn error(id: &str, message: impl Into<String>) {
    emit(&json!({"ev": "error", "id": id, "message": message.into()}));
}

fn sort(code: u64) -> UGCQueryType {
    match code {
        1 => UGCQueryType::RankedByPublicationDate,
        2 => UGCQueryType::RankedByVote,
        3 => UGCQueryType::RankedByTotalUniqueSubscriptions,
        _ => UGCQueryType::RankedByTrend,
    }
}

// What Wallpaper Engine tags an item with: its kind, and its rating.
fn kind_of(tags: &[String]) -> &'static str {
    for tag in tags {
        match tag.as_str() {
            "Scene" => return "scene",
            "Video" => return "video",
            "Web" => return "web",
            "Application" => return "application",
            _ => {}
        }
    }
    ""
}

fn mature(tags: &[String]) -> bool {
    tags.iter().any(|tag| tag == "Mature" || tag == "Questionable")
}

/// Callback results come back here, tagged with what asked for them.
enum Reply {
    Results(String, Result<Vec<Value>, String>),
    Subscribed(String, Result<(), String>),
    Unsubscribed(String, String, Result<(), String>),
}

struct Download {
    started: Instant,
    idle_since: Option<Instant>,
    last: (u64, u64),
}

fn search(client: &Client, request: &Value, tag: String, replies: mpsc::Sender<Reply>) {
    let query = request["query"].as_str().unwrap_or("").trim().to_string();
    let page = request["page"].as_u64().unwrap_or(1).max(1) as u32;
    let allow_mature = request["mature"].as_bool().unwrap_or(false);
    let we = AppId(APP_ID);
    let exact = query.parse::<u64>().ok();
    let handle = match exact {
        Some(id) => client.ugc().query_item(PublishedFileId(id)),
        None => client.ugc().query_all(
            sort(request["sort"].as_u64().unwrap_or(0)),
            UGCType::Items,
            AppIDs::Both { creator: we, consumer: we },
            page,
        ),
    };
    let mut handle = match handle {
        Ok(handle) => handle,
        Err(e) => {
            let _ = replies.send(Reply::Results(tag, Err(format!("{e:?}"))));
            return;
        }
    };
    if exact.is_none() {
        handle = handle.set_return_long_description(false).set_return_metadata(false).set_match_any_tag(false);
        if !query.is_empty() {
            handle = handle.set_search_text(&query);
        }
        // applications are not wallpapers we can show
        handle = handle.add_excluded_tag("Application");
        if !allow_mature {
            handle = handle.add_excluded_tag("Mature").add_excluded_tag("Questionable");
        }
        if request["sort"].as_u64().unwrap_or(0) == 0 {
            handle = handle.set_ranked_by_trend_days(7);
        }
    }
    let client2 = client.clone();
    handle.fetch(move |result| {
        let mapped = result.map_err(|e| format!("{e:?}")).map(|results| {
            let mut out = Vec::with_capacity(results.returned_results() as usize);
            for i in 0..results.returned_results() {
                let Some(item) = results.get(i) else { continue };
                if item.consumer_app_id != Some(we) {
                    continue;
                }
                let state = client2.ugc().item_state(item.published_file_id);
                out.push(json!({
                    "id": item.published_file_id.0.to_string(),
                    "title": item.title,
                    "preview": results.preview_url(i).unwrap_or_default(),
                    "size": u64::from(item.file_size),
                    "subs": results.statistic(i, UGCStatisticType::Subscriptions).unwrap_or(0),
                    "score": item.score,
                    "type": kind_of(&item.tags),
                    "mature": mature(&item.tags),
                    "subscribed": state.contains(ItemState::SUBSCRIBED),
                    "installed": state.contains(ItemState::INSTALLED),
                }));
            }
            out
        });
        let _ = replies.send(Reply::Results(tag, mapped));
    });
}

fn main() {
    if std::env::args().nth(1).as_deref() == Some("--version") {
        println!("niri-island-steam {}", env!("CARGO_PKG_VERSION"));
        return;
    }
    let client = match Client::init_app(APP_ID) {
        Ok(client) => client,
        Err(e) => {
            error("", format!("Steam no está abierto o la cuenta no tiene Wallpaper Engine ({e:?})"));
            std::process::exit(1);
        }
    };
    if !client.user().logged_on() {
        error("", "Steam no tiene sesión iniciada");
        std::process::exit(1);
    }
    emit(&json!({"ev": "ready", "account": client.user().steam_id().raw().to_string()}));

    // stdin on its own thread; the main loop pumps Steam's callbacks
    let (lines_tx, lines) = mpsc::channel::<String>();
    std::thread::spawn(move || {
        for line in std::io::stdin().lock().lines() {
            let Ok(line) = line else { break };
            if lines_tx.send(line).is_err() {
                break;
            }
        }
    });
    let (replies_tx, replies) = mpsc::channel::<Reply>();
    let mut downloads: HashMap<String, Download> = HashMap::new();
    let mut last_progress = Instant::now();

    loop {
        client.run_callbacks();
        loop {
            match lines.try_recv() {
                Ok(line) => handle(&client, &line, &replies_tx),
                Err(mpsc::TryRecvError::Empty) => break,
                Err(mpsc::TryRecvError::Disconnected) => return,  // the picker is gone
            }
        }
        while let Ok(reply) = replies.try_recv() {
            match reply {
                Reply::Results(tag, Ok(items)) => emit(&json!({"ev": "results", "tag": tag, "items": items})),
                Reply::Results(tag, Err(e)) => error(&tag, format!("búsqueda: {e}")),
                Reply::Subscribed(id, Ok(())) => {
                    let Ok(raw) = id.parse::<u64>() else { continue };
                    if client.ugc().download_item(PublishedFileId(raw), true) {
                        downloads.insert(id, Download { started: Instant::now(), idle_since: None, last: (0, 0) });
                    } else {
                        error(&id, "Steam rechazó la descarga");
                    }
                }
                Reply::Subscribed(id, Err(e)) => error(&id, format!("suscripción: {e}")),
                Reply::Unsubscribed(id, folder, Ok(())) => {
                    // Steam leaves the files behind for a while: they go now
                    let path = std::path::Path::new(&folder);
                    if path.parent().and_then(|p| p.file_name()).is_some_and(|n| n == "431960")
                        && path.file_name().is_some_and(|n| n.to_string_lossy() == id)
                    {
                        let _ = std::fs::remove_dir_all(path);
                    }
                    emit(&json!({"ev": "unsubscribed", "id": id}));
                }
                Reply::Unsubscribed(id, _, Err(e)) => error(&id, format!("desuscripción: {e}")),
            }
        }
        // downloads: progress a few times a second, done when installed
        if !downloads.is_empty() && last_progress.elapsed() > Duration::from_millis(250) {
            last_progress = Instant::now();
            downloads.retain(|id, d| {
                let item = PublishedFileId(id.parse().unwrap_or(0));
                let state = client.ugc().item_state(item);
                let (current, total) = client.ugc().item_download_info(item).unwrap_or((0, 0));
                if state.contains(ItemState::INSTALLED)
                    && !state.contains(ItemState::DOWNLOADING)
                    && !state.contains(ItemState::NEEDS_UPDATE)
                {
                    let folder = client.ugc().item_install_info(item).map(|i| i.folder).unwrap_or_default();
                    emit(&json!({"ev": "done", "id": id, "folder": folder}));
                    return false;
                }
                if (current, total) != d.last {
                    d.last = (current, total);
                    d.idle_since = None;
                }
                let active = state.contains(ItemState::DOWNLOADING)
                    || state.contains(ItemState::DOWNLOAD_PENDING)
                    || total > 0;
                let progress = if total > 0 { (current as f64 / total as f64).clamp(0.0, 1.0) } else { 0.0 };
                emit(&json!({"ev": "progress", "id": id, "progress": progress, "total": total}));
                if active {
                    d.idle_since = None;
                } else if d.idle_since.get_or_insert_with(Instant::now).elapsed() > Duration::from_secs(20) {
                    error(id, "Steam no empezó la descarga");
                    return false;
                }
                if d.started.elapsed() > Duration::from_secs(60 * 30) {
                    error(id, "la descarga tardó demasiado");
                    return false;
                }
                true
            });
        }
        std::thread::sleep(Duration::from_millis(15));
    }
}

fn handle(client: &Client, line: &str, replies: &mpsc::Sender<Reply>) {
    let Ok(request) = serde_json::from_str::<Value>(line) else { return };
    let id = request["id"].as_str().unwrap_or("").to_string();
    match request["op"].as_str().unwrap_or("") {
        "search" => search(client, &request, request["tag"].as_str().unwrap_or("").to_string(), replies.clone()),
        "subscribe" => {
            let Ok(raw) = id.parse::<u64>() else { return error(&id, "id no válido") };
            let tx = replies.clone();
            let tag = id.clone();
            client.ugc().subscribe_item(PublishedFileId(raw), move |r| {
                let _ = tx.send(Reply::Subscribed(tag, r.map_err(|e| format!("{e:?}"))));
            });
        }
        "unsubscribe" => {
            let Ok(raw) = id.parse::<u64>() else { return error(&id, "id no válido") };
            let folder = client.ugc().item_install_info(PublishedFileId(raw)).map(|i| i.folder).unwrap_or_default();
            let tx = replies.clone();
            let tag = id.clone();
            client.ugc().unsubscribe_item(PublishedFileId(raw), move |r| {
                let _ = tx.send(Reply::Unsubscribed(tag, folder, r.map_err(|e| format!("{e:?}"))));
            });
        }
        "state" => {
            let ids: Vec<String> = client.ugc().subscribed_items(true).into_iter().map(|i| i.0.to_string()).collect();
            emit(&json!({"ev": "state", "subscribed": ids}));
        }
        _ => {}
    }
}
