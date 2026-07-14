#!/usr/bin/env python3
"""
Steam indirme durumunu JSON olarak verir.

Kaynaklar:
1) Canlı yüzde, hız, indirilen/toplam ve kalan süre:
   Steam penceresinin Windows UI Automation metinleri.
2) Oyun adı, AppID ve yedek ilerleme değerleri:
   steamapps/appmanifest_<appid>.acf
3) Yedek indirme hızı:
   Steam/logs/content_log.txt

Kurulum:
    py -m pip install pywinauto psutil

Kullanım:
    py steam_download_json.py
    py steam_download_json.py --pretty
    py steam_download_json.py --output steam_download.json
    py steam_download_json.py --once --pretty

Not:
    Canlı arayüz değerlerini okuyabilmesi için Steam > İndirmeler sayfasının
    Steam oturumunda en az bir kez açılmış olması gerekebilir.
"""

from __future__ import annotations

import argparse
import datetime as dt
import json
import os
import re
import sys
import tempfile
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Any

import psutil
from pywinauto import Desktop

try:
    import winreg
except ImportError:
    winreg = None


PAIR_RE = re.compile(r'"([^"]+)"\s+"([^"]*)"')

SIZE_PAIR_RE = re.compile(
    r"(?P<done>\d+(?:[.,]\d+)?)\s*(?P<done_unit>B|KB|MB|GB|TB)\s*"
    r"(?:/|of|von|sur|de)\s*"
    r"(?P<total>\d+(?:[.,]\d+)?)\s*(?P<total_unit>B|KB|MB|GB|TB)",
    re.IGNORECASE,
)

SPEED_RE = re.compile(
    r"(?P<value>\d+(?:[.,]\d+)?)\s*(?P<unit>B|KB|MB|GB|TB)\s*/\s*s",
    re.IGNORECASE,
)

PERCENT_RE = re.compile(r"(?<!\d)(?P<value>\d{1,3}(?:[.,]\d+)?)\s*%")

LOG_RATE_PATTERNS = [
    re.compile(
        r"\[(?P<time>\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2})\].*?"
        r"Current download rate:\s*(?P<value>[\d.]+)\s*Mbps",
        re.IGNORECASE,
    ),
    re.compile(
        r"\[(?P<time>\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2})\].*?"
        r"Download rate.*?(?P<value>[\d.]+)\s*Mbps",
        re.IGNORECASE,
    ),
]

TURKISH_ETA_RE = re.compile(
    r"(?:(?P<hours>\d+)\s*(?:sa|saat))?\s*"
    r"(?:(?P<minutes>\d+)\s*(?:dk|dakika))?\s*"
    r"(?:(?P<seconds>\d+)\s*(?:sn|saniye))?\s*"
    r"(?:kaldı|kalan)",
    re.IGNORECASE,
)

ENGLISH_ETA_RE = re.compile(
    r"(?:(?P<hours>\d+)\s*(?:h|hr|hour|hours))?\s*"
    r"(?:(?P<minutes>\d+)\s*(?:m|min|minute|minutes))?\s*"
    r"(?:(?P<seconds>\d+)\s*(?:s|sec|second|seconds))?\s*"
    r"remaining",
    re.IGNORECASE,
)

CLOCK_ETA_RE = re.compile(
    r"(?<!\d)(?:(?P<hours>\d{1,2}):)?"
    r"(?P<minutes>\d{1,2}):(?P<seconds>\d{2})(?!\d)"
)

STATUS_KEYWORDS = [
    ("paused", ("duraklatıldı", "duraklat", "paused")),
    ("verifying", ("doğrulanıyor", "doğrulama", "verifying", "validating")),
    ("installing", ("kuruluyor", "yükleniyor", "installing", "patching")),
    ("downloading", ("indiriliyor", "indiriliyor", "downloading")),
    ("queued", ("sırada", "queued")),
]


@dataclass
class DownloadItem:
    app_id: str
    name: str
    manifest_path: Path
    steamapps_path: Path
    state_flags: int
    bytes_to_download: int
    bytes_downloaded: int
    bytes_to_stage: int
    bytes_staged: int

    @property
    def active(self) -> bool:
        download_incomplete = (
            self.bytes_to_download > 0
            and self.bytes_downloaded < self.bytes_to_download
        )
        stage_incomplete = (
            self.bytes_to_stage > 0
            and self.bytes_staged < self.bytes_to_stage
        )
        downloading_dir = (
            self.steamapps_path / "downloading" / self.app_id
        ).exists()
        state_says_downloading = bool(self.state_flags & 0x100000)

        return (
            download_incomplete
            or stage_incomplete
            or downloading_dir
            or state_says_downloading
        )


@dataclass(frozen=True)
class UiText:
    name: str
    control_type: str
    process_id: int


def find_steam_root() -> Path:
    if winreg is None:
        raise RuntimeError("Bu program Windows için hazırlanmıştır.")

    registry_locations = [
        (winreg.HKEY_CURRENT_USER, r"SOFTWARE\Valve\Steam", "SteamPath"),
        (winreg.HKEY_CURRENT_USER, r"SOFTWARE\Valve\Steam", "SteamExe"),
        (
            winreg.HKEY_LOCAL_MACHINE,
            r"SOFTWARE\WOW6432Node\Valve\Steam",
            "InstallPath",
        ),
    ]

    for hive, key_name, value_name in registry_locations:
        try:
            with winreg.OpenKey(hive, key_name) as key:
                value, _ = winreg.QueryValueEx(key, value_name)
        except OSError:
            continue

        path = Path(str(value).replace("/", "\\"))
        if path.suffix.lower() == ".exe":
            path = path.parent

        if path.exists():
            return path

    fallback = (
        Path(os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)"))
        / "Steam"
    )

    if fallback.exists():
        return fallback

    raise FileNotFoundError("Steam kurulum klasörü bulunamadı.")


def find_steamapps_folders(steam_root: Path) -> list[Path]:
    candidates = [steam_root / "steamapps"]
    library_file = steam_root / "steamapps" / "libraryfolders.vdf"

    try:
        text = library_file.read_text(encoding="utf-8", errors="replace")
    except OSError:
        text = ""

    for raw_path in re.findall(r'"path"\s+"([^"]+)"', text, re.IGNORECASE):
        candidates.append(Path(raw_path.replace(r"\\", "\\")) / "steamapps")

    result: list[Path] = []
    seen: set[str] = set()

    for candidate in candidates:
        key = str(candidate.resolve(strict=False)).lower()
        if candidate.exists() and key not in seen:
            result.append(candidate)
            seen.add(key)

    return result


def parse_manifest(path: Path) -> dict[str, str]:
    try:
        text = path.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return {}

    return {key: value for key, value in PAIR_RE.findall(text)}


def to_int(value: str | None) -> int:
    try:
        return int(value or "0")
    except ValueError:
        return 0


def read_active_downloads(steamapps_folders: list[Path]) -> list[DownloadItem]:
    result: list[DownloadItem] = []

    for steamapps in steamapps_folders:
        for manifest in steamapps.glob("appmanifest_*.acf"):
            data = parse_manifest(manifest)
            app_id = data.get("appid") or manifest.stem.removeprefix(
                "appmanifest_"
            )

            item = DownloadItem(
                app_id=app_id,
                name=data.get("name") or f"App {app_id}",
                manifest_path=manifest,
                steamapps_path=steamapps,
                state_flags=to_int(data.get("StateFlags")),
                bytes_to_download=to_int(data.get("BytesToDownload")),
                bytes_downloaded=to_int(data.get("BytesDownloaded")),
                bytes_to_stage=to_int(data.get("BytesToStage")),
                bytes_staged=to_int(data.get("BytesStaged")),
            )

            if item.active:
                result.append(item)

    return sorted(result, key=lambda item: item.name.lower())


def normalize_text(value: object) -> str:
    return " ".join(str(value or "").replace("\u200b", "").split()).strip()


def is_steam_process(process_id: int) -> bool:
    try:
        process_name = psutil.Process(process_id).name().lower()
    except (psutil.NoSuchProcess, psutil.AccessDenied):
        return False

    return process_name in {
        "steam.exe",
        "steamwebhelper.exe",
        "steamservice.exe",
    }


def collect_steam_ui_texts() -> list[UiText]:
    result: list[UiText] = []
    seen: set[tuple[int, str, str]] = set()

    try:
        windows = Desktop(backend="uia").windows()
    except Exception:
        return result

    for window in windows:
        try:
            process_id = int(window.element_info.process_id)
        except Exception:
            continue

        if not is_steam_process(process_id):
            continue

        controls = [window]

        try:
            controls.extend(window.descendants())
        except Exception:
            pass

        for control in controls:
            try:
                name = normalize_text(control.element_info.name)
            except Exception:
                name = ""

            if not name:
                try:
                    name = normalize_text(control.window_text())
                except Exception:
                    name = ""

            if not name:
                continue

            try:
                control_type = normalize_text(
                    control.element_info.control_type
                )
            except Exception:
                control_type = ""

            key = (process_id, control_type, name)
            if key in seen:
                continue

            seen.add(key)
            result.append(
                UiText(
                    name=name,
                    control_type=control_type,
                    process_id=process_id,
                )
            )

    return result


def decimal_number(value: str) -> float:
    value = value.strip()

    if "," in value and "." in value:
        if value.rfind(",") > value.rfind("."):
            value = value.replace(".", "").replace(",", ".")
        else:
            value = value.replace(",", "")
    else:
        value = value.replace(",", ".")

    return float(value)


def unit_multiplier(unit: str) -> int:
    return {
        "B": 1,
        "KB": 1024,
        "MB": 1024**2,
        "GB": 1024**3,
        "TB": 1024**4,
    }[unit.upper()]


def size_to_bytes(value: str, unit: str) -> int:
    return int(decimal_number(value) * unit_multiplier(unit))


def bytes_to_gb(value: int | float) -> float:
    return round(float(value) / (1024**3), 3)


def bytes_to_mb(value: int | float) -> float:
    return round(float(value) / (1024**2), 3)


def calculate_percent(done: int, total: int) -> float | None:
    if total <= 0:
        return None
    return round(min(100.0, max(0.0, done * 100.0 / total)), 2)


def format_duration(seconds: int | None) -> str | None:
    if seconds is None:
        return None

    seconds = max(0, int(seconds))
    hours, remainder = divmod(seconds, 3600)
    minutes, seconds = divmod(remainder, 60)

    if hours:
        return f"{hours}sa {minutes}dk {seconds}sn"
    if minutes:
        return f"{minutes}dk {seconds}sn"
    return f"{seconds}sn"


def parse_eta_seconds(text: str) -> int | None:
    for pattern in (TURKISH_ETA_RE, ENGLISH_ETA_RE):
        for match in pattern.finditer(text):
            hours = int(match.groupdict().get("hours") or 0)
            minutes = int(match.groupdict().get("minutes") or 0)
            seconds = int(match.groupdict().get("seconds") or 0)

            if hours or minutes or seconds:
                return hours * 3600 + minutes * 60 + seconds

    clock_match = CLOCK_ETA_RE.search(text)
    if clock_match:
        hours = int(clock_match.group("hours") or 0)
        minutes = int(clock_match.group("minutes"))
        seconds = int(clock_match.group("seconds"))
        return hours * 3600 + minutes * 60 + seconds

    return None


def parse_status(text: str) -> str | None:
    lowered = text.casefold()

    for status, keywords in STATUS_KEYWORDS:
        if any(keyword in lowered for keyword in keywords):
            return status

    return None


def context_for_item(
    item: DownloadItem,
    ui_texts: list[UiText],
    active_count: int,
) -> str:
    names = [entry.name for entry in ui_texts]
    folded_item_name = item.name.casefold()

    for index, name in enumerate(names):
        if folded_item_name in name.casefold():
            start = max(0, index - 12)
            end = min(len(names), index + 30)
            return " | ".join(names[start:end])

    # Steam aynı anda çoğunlukla tek ağ indirmesi yaptığı için tek aktif öğede
    # bütün görünen Steam metni kullanılabilir.
    if active_count == 1:
        return " | ".join(names)

    return ""


def parse_live_fields(context: str) -> dict[str, Any]:
    result: dict[str, Any] = {}

    size_match = SIZE_PAIR_RE.search(context)
    if size_match:
        result["downloaded_bytes"] = size_to_bytes(
            size_match.group("done"),
            size_match.group("done_unit"),
        )
        result["total_bytes"] = size_to_bytes(
            size_match.group("total"),
            size_match.group("total_unit"),
        )

    speed_match = SPEED_RE.search(context)
    if speed_match:
        result["speed_bytes_per_second"] = size_to_bytes(
            speed_match.group("value"),
            speed_match.group("unit"),
        )

    percent_match = PERCENT_RE.search(context)
    if percent_match:
        value = decimal_number(percent_match.group("value"))
        if 0 <= value <= 100:
            result["progress_percent"] = round(value, 2)

    eta_seconds = parse_eta_seconds(context)
    if eta_seconds is not None:
        result["remaining_seconds"] = eta_seconds

    status = parse_status(context)
    if status:
        result["status"] = status

    return result


def read_recent_log_speed(
    steam_root: Path,
    max_age_seconds: int = 120,
) -> float | None:
    log_path = steam_root / "logs" / "content_log.txt"

    try:
        with log_path.open("rb") as file:
            file.seek(0, os.SEEK_END)
            size = file.tell()
            file.seek(max(0, size - 2 * 1024 * 1024))
            text = file.read().decode("utf-8", errors="replace")
    except OSError:
        return None

    newest: tuple[dt.datetime, float] | None = None

    for pattern in LOG_RATE_PATTERNS:
        for match in pattern.finditer(text):
            try:
                timestamp = dt.datetime.strptime(
                    match.group("time"),
                    "%Y-%m-%d %H:%M:%S",
                )
                bytes_per_second = (
                    float(match.group("value")) * 1_000_000 / 8
                )
            except ValueError:
                continue

            if newest is None or timestamp > newest[0]:
                newest = (timestamp, bytes_per_second)

    if newest is None:
        return None

    age = (dt.datetime.now() - newest[0]).total_seconds()
    if not 0 <= age <= max_age_seconds:
        return None

    return newest[1]


def steam_is_running() -> bool:
    for process in psutil.process_iter(["name"]):
        try:
            if (process.info["name"] or "").lower() == "steam.exe":
                return True
        except (psutil.NoSuchProcess, psutil.AccessDenied):
            continue

    return False


def build_snapshot(
    steam_root: Path,
    steamapps_folders: list[Path],
    previous_manifest_bytes: dict[str, tuple[int, float]],
    include_ui_text: bool,
) -> dict[str, Any]:
    now_monotonic = time.monotonic()
    items = read_active_downloads(steamapps_folders)
    ui_texts = collect_steam_ui_texts()
    log_speed = read_recent_log_speed(steam_root)

    downloads: list[dict[str, Any]] = []

    for item in items:
        context = context_for_item(item, ui_texts, len(items))
        live = parse_live_fields(context)

        downloaded_bytes = int(
            live.get("downloaded_bytes", item.bytes_downloaded)
        )
        total_bytes = int(
            live.get("total_bytes", item.bytes_to_download)
        )

        speed_bytes_per_second = float(
            live.get("speed_bytes_per_second", 0)
        )
        speed_source = "steam_ui" if speed_bytes_per_second > 0 else None

        previous = previous_manifest_bytes.get(item.app_id)
        if speed_bytes_per_second <= 0 and previous:
            previous_bytes, previous_time = previous
            elapsed = now_monotonic - previous_time
            delta = item.bytes_downloaded - previous_bytes

            if elapsed > 0 and delta > 0:
                speed_bytes_per_second = delta / elapsed
                speed_source = "manifest_delta"

        if (
            speed_bytes_per_second <= 0
            and len(items) == 1
            and log_speed is not None
        ):
            speed_bytes_per_second = log_speed
            speed_source = "content_log"

        previous_manifest_bytes[item.app_id] = (
            item.bytes_downloaded,
            now_monotonic,
        )

        progress_percent = live.get("progress_percent")
        if progress_percent is None:
            progress_percent = calculate_percent(
                downloaded_bytes,
                total_bytes,
            )

        remaining_bytes = max(0, total_bytes - downloaded_bytes)

        remaining_seconds = live.get("remaining_seconds")
        remaining_source = (
            "steam_ui" if remaining_seconds is not None else None
        )

        if (
            remaining_seconds is None
            and speed_bytes_per_second > 0
            and remaining_bytes > 0
        ):
            remaining_seconds = int(
                remaining_bytes / speed_bytes_per_second
            )
            remaining_source = "calculated"

        status = live.get("status")
        if not status:
            if speed_bytes_per_second > 0:
                status = "downloading"
            elif item.active:
                status = "paused_or_queued"
            else:
                status = "unknown"

        entry: dict[str, Any] = {
            "app_id": item.app_id,
            "name": item.name,
            "store_url": (
                f"https://store.steampowered.com/app/{item.app_id}"
            ),
            "status": status,
            "progress_percent": progress_percent,
            "downloaded_bytes": downloaded_bytes,
            "downloaded_gb": bytes_to_gb(downloaded_bytes),
            "total_bytes": total_bytes,
            "total_gb": bytes_to_gb(total_bytes),
            "remaining_bytes": remaining_bytes,
            "remaining_gb": bytes_to_gb(remaining_bytes),
            "speed_bytes_per_second": int(speed_bytes_per_second),
            "speed_mb_s": bytes_to_mb(speed_bytes_per_second),
            "speed_source": speed_source,
            "remaining_seconds": remaining_seconds,
            "remaining_text": format_duration(remaining_seconds),
            "remaining_source": remaining_source,
            "disk": {
                "processed_bytes": item.bytes_staged,
                "processed_gb": bytes_to_gb(item.bytes_staged),
                "total_bytes": item.bytes_to_stage,
                "total_gb": bytes_to_gb(item.bytes_to_stage),
                "progress_percent": calculate_percent(
                    item.bytes_staged,
                    item.bytes_to_stage,
                ),
            },
            "manifest": {
                "path": str(item.manifest_path),
                "state_flags": item.state_flags,
            },
        }

        if include_ui_text:
            entry["debug_ui_context"] = context

        downloads.append(entry)

    active_ids = {item.app_id for item in items}
    for app_id in list(previous_manifest_bytes):
        if app_id not in active_ids:
            previous_manifest_bytes.pop(app_id, None)

    return {
        "timestamp": dt.datetime.now().astimezone().isoformat(
            timespec="seconds"
        ),
        "steam_running": steam_is_running(),
        "active_download_count": len(downloads),
        "downloads": downloads,
    }


def atomic_write_json(path: Path, payload: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    text = json.dumps(payload, ensure_ascii=False, indent=2)

    with tempfile.NamedTemporaryFile(
        mode="w",
        encoding="utf-8",
        delete=False,
        dir=path.parent,
        prefix=f".{path.name}.",
        suffix=".tmp",
    ) as temporary_file:
        temporary_file.write(text)
        temporary_path = Path(temporary_file.name)

    os.replace(temporary_path, path)


def print_json(payload: dict[str, Any], pretty: bool) -> None:
    print(
        json.dumps(
            payload,
            ensure_ascii=False,
            indent=2 if pretty else None,
            separators=None if pretty else (",", ":"),
        ),
        flush=True,
    )


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Steam indirme durumunu JSON olarak verir."
    )
    parser.add_argument(
        "--interval",
        type=float,
        default=1.0,
        help="Yenileme aralığı, saniye. Varsayılan: 1",
    )
    parser.add_argument(
        "--once",
        action="store_true",
        help="Tek bir JSON çıktısı verip kapanır.",
    )
    parser.add_argument(
        "--pretty",
        action="store_true",
        help="JSON çıktısını girintili yazdırır.",
    )
    parser.add_argument(
        "--output",
        type=Path,
        help="Son durumu belirtilen JSON dosyasına atomik olarak yazar.",
    )
    parser.add_argument(
        "--include-ui-text",
        action="store_true",
        help="Eşleştirmede kullanılan Steam arayüz metnini JSON'a ekler.",
    )
    args = parser.parse_args()

    try:
        root = find_steam_root()
        folders = find_steamapps_folders(root)
    except Exception as error:
        error_payload = {
            "timestamp": dt.datetime.now().astimezone().isoformat(
                timespec="seconds"
            ),
            "steam_running": steam_is_running(),
            "error": str(error),
            "active_download_count": 0,
            "downloads": [],
        }
        print_json(error_payload, args.pretty)
        return 1

    previous_manifest_bytes: dict[str, tuple[int, float]] = {}
    previous_json: str | None = None

    try:
        while True:
            snapshot = build_snapshot(
                steam_root=root,
                steamapps_folders=folders,
                previous_manifest_bytes=previous_manifest_bytes,
                include_ui_text=args.include_ui_text,
            )

            if args.output:
                atomic_write_json(args.output, snapshot)

            comparable = dict(snapshot)
            comparable.pop("timestamp", None)
            current_json = json.dumps(
                comparable,
                ensure_ascii=False,
                sort_keys=True,
            )

            # Konsolu gereksiz yere doldurmamak için yalnızca veri değişince basar.
            if current_json != previous_json or args.once:
                print_json(snapshot, args.pretty)
                previous_json = current_json

            if args.once:
                return 0

            time.sleep(max(0.25, args.interval))

    except KeyboardInterrupt:
        return 0


if __name__ == "__main__":
    raise SystemExit(main())
