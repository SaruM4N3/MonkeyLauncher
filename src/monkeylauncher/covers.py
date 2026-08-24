import json
import re
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path

from .config import COVERS_DIR, game_key
from .logging_setup import log

# ── Cover art (Steam store — no API key needed) ─────────────────────────────
def cover_cache_path(exe):
    return COVERS_DIR / f'{game_key(exe)}.jpg'

def search_steam_appid(query):
    """Returns the Steam appid of the best match for `query`, or None."""
    url = ('https://store.steampowered.com/api/storesearch/?'
          + urllib.parse.urlencode({'term': query, 'l': 'english', 'cc': 'US'}))
    req = urllib.request.Request(url, headers={'User-Agent': 'MonkeyLauncher'})
    with urllib.request.urlopen(req, timeout=10) as resp:
        items = json.load(resp).get('items', [])
    apps = [i for i in items if i.get('type') == 'app']
    return apps[0]['id'] if apps else None

def _clean_query(name):
    name = re.sub(r'[_.]+', ' ', name)
    name = re.sub(r'\([^)]*\)', ' ', name)           # (3), (Repack)…
    name = re.sub(r'\[[^\]]*\]', ' ', name)          # [FitGirl], [GOG]…
    name = re.sub(r'\bv\d+(\.\d+)+\b', ' ', name, flags=re.IGNORECASE)  # v1.2.56034
    name = re.sub(r'\b(build|update)\b.*', ' ', name, flags=re.IGNORECASE)
    return re.sub(r'\s+', ' ', name).strip()

def default_game_name(exe):
    """Best-guess display name for a game when no manual override is set —
    same folder-name heuristic used for cover search, since the release
    folder is usually the real title (unlike the exe itself)."""
    name = _clean_query(Path(exe).parent.name)
    return name or Path(exe).stem

def _cover_query_candidates(exe):
    """Executable filenames are often generic or engine-default names
    (RGame.exe, EOSAuthLauncher.exe…) — the folder the game was released
    under is usually a much better match for the real title. Try that
    first, then fall back to the exe's own name."""
    exe_path = Path(exe)
    candidates = []
    parent_query = _clean_query(exe_path.parent.name)
    if parent_query:
        candidates.append(parent_query)
    stem_query = _clean_query(exe_path.stem)
    if stem_query and stem_query.lower() != parent_query.lower():
        candidates.append(stem_query)
    return candidates

def fetch_cover(exe):
    """Returns a Path to a cached cover image for `exe`, downloading it from
    the Steam store on first use (these are almost always cracked builds of
    games that also have an official Steam listing). Returns None if no
    match was found or the lookup failed (logged, never raised)."""
    cache_path = cover_cache_path(exe)
    if cache_path.exists():
        return cache_path

    candidates = _cover_query_candidates(exe)
    try:
        appid, matched_query = None, None
        for query in candidates:
            appid = search_steam_appid(query)
            if appid:
                matched_query = query
                break
        if not appid:
            log.debug(f"Steam store: no match for {candidates}")
            return None

        # Not every app has every asset uploaded — prefer the portrait
        # library cover, but fall back to whatever landscape art exists
        # rather than showing nothing.
        data = None
        for asset in ('library_600x900.jpg', 'header.jpg', 'library_hero.jpg'):
            image_url = f'https://cdn.cloudflare.steamstatic.com/steam/apps/{appid}/{asset}'
            req = urllib.request.Request(image_url, headers={'User-Agent': 'MonkeyLauncher'})
            try:
                with urllib.request.urlopen(req, timeout=15) as resp:
                    data = resp.read()
                break
            except urllib.error.HTTPError as e:
                if e.code != 404:
                    raise
        if data is None:
            log.debug(f"Steam store: no image asset for '{matched_query}' (appid={appid})")
            return None

        COVERS_DIR.mkdir(parents=True, exist_ok=True)
        cache_path.write_bytes(data)
        log.debug(f"Cached cover for '{matched_query}' (appid={appid}) -> {cache_path}")
        return cache_path
    except (urllib.error.URLError, urllib.error.HTTPError, TimeoutError,
            KeyError, ValueError, OSError) as e:
        log.warning(f"Steam store cover lookup failed for {candidates}: {e}")
        return None
