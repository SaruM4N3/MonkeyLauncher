#include "covers.hpp"

#include <nlohmann/json.hpp>
#include <regex>
#include <stdexcept>

#include "config.hpp"
#include "http.hpp"
#include "logging.hpp"

namespace ml {

fs::path cover_cache_path(const std::string& exe) {
    return covers_dir() / (game_key(exe) + ".jpg");
}

// Returns the Steam appid of the best match for `query`, or 0.
// Throws std::runtime_error on network/HTTP/parse failure.
static long search_steam_appid(const std::string& query) {
    const std::string url = "https://store.steampowered.com/api/storesearch/?term=" +
                            url_encode(query) + "&l=english&cc=US";
    HttpResponse resp = http_get(url, 10);
    if (resp.status >= 400) throw std::runtime_error("HTTP Error " + std::to_string(resp.status));
    nlohmann::json j = nlohmann::json::parse(resp.body);   // throws on bad JSON
    if (!j.contains("items") || !j["items"].is_array()) return 0;
    for (const auto& item : j["items"])
        if (item.value("type", "") == "app" && item.contains("id")) return item["id"].get<long>();
    return 0;
}

static std::string re_replace(const std::string& s, const char* pattern, bool icase = false) {
    return std::regex_replace(
        s, std::regex(pattern, icase ? std::regex::ECMAScript | std::regex::icase
                                     : std::regex::ECMAScript), " ");
}

static std::string clean_query(std::string name) {
    name = re_replace(name, "[_.]+");
    name = re_replace(name, "\\([^)]*\\)");                // (3), (Repack)…
    name = re_replace(name, "\\[[^\\]]*\\]");              // [FitGirl], [GOG]…
    name = re_replace(name, "\\bv\\d+(\\.\\d+)+\\b", true);   // v1.2.56034
    name = re_replace(name, "\\b(build|update)\\b[^\\n]*", true);
    return strip(re_replace(name, "\\s+"));
}

// Same folder-name heuristic used for cover search, since the release
// folder is usually the real title (unlike the exe itself).
std::string default_game_name(const std::string& exe) {
    const fs::path p(exe);
    std::string name = clean_query(p.parent_path().filename().string());
    return name.empty() ? p.stem().string() : name;
}

// Executable filenames are often generic or engine-default names
// (RGame.exe, EOSAuthLauncher.exe…) — the folder the game was released
// under is usually a much better match for the real title. Try that
// first, then fall back to the exe's own name.
static std::vector<std::string> cover_query_candidates(const std::string& exe) {
    const fs::path p(exe);
    std::vector<std::string> candidates;
    const std::string parent_query = clean_query(p.parent_path().filename().string());
    if (!parent_query.empty()) candidates.push_back(parent_query);
    const std::string stem_query = clean_query(p.stem().string());
    if (!stem_query.empty() && to_lower(stem_query) != to_lower(parent_query))
        candidates.push_back(stem_query);
    return candidates;
}

std::optional<fs::path> fetch_cover(const std::string& exe) {
    const fs::path cache_path = cover_cache_path(exe);
    std::error_code ec;
    if (fs::exists(cache_path, ec)) return cache_path;

    const auto candidates = cover_query_candidates(exe);
    try {
        long appid = 0;
        std::string matched_query;
        for (const auto& query : candidates) {
            appid = search_steam_appid(query);
            if (appid) { matched_query = query; break; }
        }
        if (!appid) {
            log_debug("Steam store: no match for [{}]", join(candidates, ", "));
            return std::nullopt;
        }

        // Not every app has every asset uploaded — prefer the portrait
        // library cover, but fall back to whatever landscape art exists
        // rather than showing nothing.
        std::optional<std::string> data;
        for (const char* asset : {"library_600x900.jpg", "header.jpg", "library_hero.jpg"}) {
            const std::string url = "https://cdn.cloudflare.steamstatic.com/steam/apps/" +
                                    std::to_string(appid) + "/" + asset;
            HttpResponse resp = http_get(url, 15);
            if (resp.status == 404) continue;
            if (resp.status >= 400) throw std::runtime_error("HTTP Error " + std::to_string(resp.status));
            data = std::move(resp.body);
            break;
        }
        if (!data) {
            log_debug("Steam store: no image asset for '{}' (appid={})", matched_query, appid);
            return std::nullopt;
        }

        fs::create_directories(covers_dir(), ec);
        if (!write_text_file(cache_path, *data)) throw std::runtime_error("cannot write cover file");
        log_debug("Cached cover for '{}' (appid={}) -> {}", matched_query, appid, cache_path.string());
        return cache_path;
    } catch (const std::exception& e) {
        log_warning("Steam store cover lookup failed for [{}]: {}", join(candidates, ", "), e.what());
        return std::nullopt;
    }
}

}  // namespace ml
