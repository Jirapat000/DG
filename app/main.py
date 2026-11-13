from __future__ import annotations

import asyncio
import json
import os
import sqlite3
import tempfile
import uuid
import base64
import secrets
import logging
import threading
from contextlib import asynccontextmanager
from datetime import datetime, timedelta, timezone
from pathlib import Path
from typing import Any

import numpy as np
import requests
from dotenv import load_dotenv
from fastapi import Depends, FastAPI, File, Form, Header, HTTPException, Query, UploadFile
from fastapi.security import HTTPBasic, HTTPBasicCredentials
from fastapi.responses import FileResponse, HTMLResponse, PlainTextResponse
from PIL import Image, UnidentifiedImageError
from shapely.geometry import Polygon
from starlette.concurrency import run_in_threadpool

load_dotenv()
ROOT = Path(__file__).resolve().parent.parent
DATA_DIR = Path(os.getenv("DATA_DIR", str(ROOT / "data"))).resolve()
IMAGE_DIR = DATA_DIR / "images"
DB_PATH = DATA_DIR / "drain_watch.sqlite3"
DEVICE_CONFIG = Path(os.getenv("DEVICE_CONFIG", str(ROOT / "config/devices.json")))
THRESHOLD = float(os.getenv("ALERT_THRESHOLD", "0.50"))
COOLDOWN_MINUTES = max(0, int(os.getenv("ALERT_COOLDOWN_MINUTES", "15")))
MAX_IMAGE_BYTES = max(1, int(os.getenv("MAX_IMAGE_MB", "8"))) * 1024 * 1024
MAX_STORED_CAPTURES = max(1, int(os.getenv("MAX_STORED_CAPTURES", "100")))
CAPTURE_RETENTION_DAYS = max(1, int(os.getenv("CAPTURE_RETENTION_DAYS", "1")))
MAX_CAPTURE_STORAGE_BYTES = max(1, int(os.getenv("MAX_CAPTURE_STORAGE_MB", "100"))) * 1024 * 1024
CONFIDENCE = float(os.getenv("CONFIDENCE_THRESHOLD", "0.35"))
APP_ENV = os.getenv("APP_ENV", "development").strip().lower()
ADMIN_USERNAME = os.getenv("ADMIN_USERNAME", "").strip()
ADMIN_PASSWORD = os.getenv("ADMIN_PASSWORD", "")
admin_auth = HTTPBasic(auto_error=False)
logger = logging.getLogger(__name__)
_retention_lock = threading.Lock()


def load_json_env(key: str, fallback: Any) -> Any:
    try:
        return json.loads(os.getenv(key, "")) if os.getenv(key) else fallback
    except json.JSONDecodeError as exc:
        raise RuntimeError(f"{key} must contain valid JSON") from exc


DEVICE_TOKENS: dict[str, str] = load_json_env("DEVICE_TOKENS_JSON", {})
CLASS_MAP: dict[int, str] = {int(k): str(v).lower() for k, v in load_json_env("CLASS_MAP_JSON", {"0": "trash"}).items()}


def require_admin(credentials: HTTPBasicCredentials | None = Depends(admin_auth)) -> None:
    if APP_ENV != "production":
        return
    username_ok = bool(credentials) and secrets.compare_digest(credentials.username, ADMIN_USERNAME)
    password_ok = bool(credentials) and secrets.compare_digest(credentials.password, ADMIN_PASSWORD)
    if not (username_ok and password_ok):
        raise HTTPException(status_code=401, detail="Authentication required",
                            headers={"WWW-Authenticate": 'Basic realm="Drain Watch"'})


def validate_production_config() -> None:
    if APP_ENV != "production":
        return
    if len(ADMIN_USERNAME) < 3 or len(ADMIN_PASSWORD) < 16:
        raise RuntimeError("Production requires ADMIN_USERNAME and an ADMIN_PASSWORD of at least 16 characters")
    configured_devices = devices_config()
    if not DEVICE_TOKENS or any(
        not isinstance(DEVICE_TOKENS.get(device_id), str) or len(DEVICE_TOKENS[device_id]) < 32
        for device_id in configured_devices
    ):
        raise RuntimeError("Production requires a unique DEVICE_TOKENS_JSON token of at least 32 characters for every device")
    if len({DEVICE_TOKENS[device_id] for device_id in configured_devices}) != len(configured_devices):
        raise RuntimeError("Every production device must use a different token")
    if not os.getenv("LINE_CHANNEL_ACCESS_TOKEN", "").strip():
        raise RuntimeError("Production requires LINE_CHANNEL_ACCESS_TOKEN so alerts are not silently disabled")


def devices_config() -> dict[str, dict[str, Any]]:
    try:
        raw = json.loads(DEVICE_CONFIG.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise RuntimeError(f"Cannot read device config {DEVICE_CONFIG}: {exc}") from exc
    if not isinstance(raw, dict):
        raise RuntimeError("Device config must be a JSON object keyed by device ID")
    for device_id, device in raw.items():
        pts = device.get("roi")
        polygon = Polygon(pts or [])
        if not device_id or not polygon.is_valid or polygon.area <= 0:
            raise RuntimeError(f"Device {device_id!r} must have a valid ROI polygon")
        if any(not (0 <= float(x) <= 1 and 0 <= float(y) <= 1) for x, y in pts):
            raise RuntimeError(f"Device {device_id!r} ROI coordinates must be normalized 0..1")
    return raw


def db() -> sqlite3.Connection:
    conn = sqlite3.connect(DB_PATH, timeout=20)
    conn.row_factory = sqlite3.Row
    return conn


def init_db() -> None:
    DATA_DIR.mkdir(parents=True, exist_ok=True)
    IMAGE_DIR.mkdir(parents=True, exist_ok=True)
    with db() as conn:
        conn.execute("""CREATE TABLE IF NOT EXISTS captures (
            id TEXT PRIMARY KEY, device_id TEXT NOT NULL, captured_at TEXT NOT NULL,
            received_at TEXT NOT NULL, image_path TEXT NOT NULL, annotated_path TEXT,
            model_status TEXT NOT NULL, coverage REAL, alert_threshold REAL NOT NULL,
            alert_sent INTEGER NOT NULL DEFAULT 0, alert_status TEXT NOT NULL,
            detections_json TEXT NOT NULL, error TEXT
        )""")
        conn.execute("CREATE INDEX IF NOT EXISTS idx_captures_device_time ON captures(device_id, received_at DESC)")
        conn.execute("CREATE INDEX IF NOT EXISTS idx_captures_time ON captures(received_at DESC)")
        conn.execute("""CREATE TABLE IF NOT EXISTS capture_commands (
            id TEXT PRIMARY KEY, device_id TEXT NOT NULL, status TEXT NOT NULL,
            created_at TEXT NOT NULL, updated_at TEXT NOT NULL,
            capture_id TEXT, result_json TEXT, error TEXT
        )""")
        conn.execute("CREATE INDEX IF NOT EXISTS idx_capture_commands_device_status ON capture_commands(device_id, status, created_at)")
        conn.execute("""CREATE TABLE IF NOT EXISTS device_status (
            device_id TEXT PRIMARY KEY, last_seen TEXT NOT NULL, device_mode TEXT NOT NULL,
            water_level INTEGER, flow_rate REAL, flow_status TEXT, flash_on INTEGER NOT NULL DEFAULT 0,
            flash_requested INTEGER NOT NULL DEFAULT 0, sleep_ready INTEGER NOT NULL DEFAULT 0,
            camera_ready INTEGER NOT NULL DEFAULT 0
        )""")
        conn.execute("""CREATE TABLE IF NOT EXISTS alert_cooldowns (
            device_id TEXT PRIMARY KEY, last_alert_at TEXT NOT NULL
        )""")
        columns = {row[1] for row in conn.execute("PRAGMA table_info(captures)")}
        for name, sql_type in (("water_level", "INTEGER"), ("flow_rate", "REAL"), ("flow_status", "TEXT")):
            if name not in columns:
                conn.execute(f"ALTER TABLE captures ADD COLUMN {name} {sql_type}")
        device_columns = {row[1] for row in conn.execute("PRAGMA table_info(device_status)")}
        if "sleep_ready" not in device_columns:
            conn.execute("ALTER TABLE device_status ADD COLUMN sleep_ready INTEGER NOT NULL DEFAULT 0")
        if "camera_ready" not in device_columns:
            conn.execute("ALTER TABLE device_status ADD COLUMN camera_ready INTEGER NOT NULL DEFAULT 0")


def _managed_image_path(raw_path: str | None) -> Path | None:
    if not raw_path:
        return None
    try:
        path = Path(raw_path).resolve()
        if path.parent != IMAGE_DIR.resolve() or path.suffix.lower() != ".jpg":
            return None
        return path
    except (OSError, RuntimeError):
        return None


def _file_size(path: Path) -> int:
    try:
        return path.stat().st_size if path.is_file() else 0
    except OSError:
        return 0


def prune_captures(now: datetime | None = None) -> int:
    """Keep recent captures within count, age, and image-storage limits."""
    current = now or datetime.now(timezone.utc)
    cutoff = current - timedelta(days=CAPTURE_RETENTION_DAYS)
    max_bytes = MAX_CAPTURE_STORAGE_BYTES
    deleted_rows: list[sqlite3.Row] = []
    kept_rows: list[sqlite3.Row] = []
    used_bytes = 0

    with _retention_lock:
        with db() as conn:
            rows = conn.execute(
                "SELECT id, received_at, image_path, annotated_path FROM captures ORDER BY received_at DESC, rowid DESC"
            ).fetchall()
            for row in rows:
                try:
                    received = datetime.fromisoformat(row["received_at"])
                    if received.tzinfo is None:
                        received = received.replace(tzinfo=timezone.utc)
                except (TypeError, ValueError):
                    received = datetime.min.replace(tzinfo=timezone.utc)

                paths = [p for p in (
                    _managed_image_path(row["image_path"]),
                    _managed_image_path(row["annotated_path"]),
                ) if p is not None]
                row_bytes = sum(_file_size(p) for p in paths)
                exceeds_limits = (
                    received < cutoff
                    or len(kept_rows) >= MAX_STORED_CAPTURES
                    or used_bytes + row_bytes > max_bytes
                )
                if exceeds_limits:
                    deleted_rows.append(row)
                else:
                    kept_rows.append(row)
                    used_bytes += row_bytes

            if deleted_rows:
                conn.executemany("DELETE FROM captures WHERE id=?", [(row["id"],) for row in deleted_rows])

        removed_files = 0
        for row in deleted_rows:
            for raw_path in (row["image_path"], row["annotated_path"]):
                path = _managed_image_path(raw_path)
                if path is not None and path.exists():
                    try:
                        path.unlink()
                        removed_files += 1
                    except OSError as exc:
                        logger.warning("Could not remove expired capture image %s: %s", path.name, exc.__class__.__name__)

        # Remove crash-leftover JPEGs only after they are at least one hour old,
        # so a concurrent upload that has not inserted its row is not affected.
        referenced = {
            p.resolve()
            for row in kept_rows
            for p in (_managed_image_path(row["image_path"]), _managed_image_path(row["annotated_path"]))
            if p is not None
        }
        orphan_cutoff = current.timestamp() - 3600
        for path in IMAGE_DIR.glob("*.jpg"):
            try:
                if path.resolve() not in referenced and path.stat().st_mtime < orphan_cutoff:
                    path.unlink()
                    removed_files += 1
            except OSError as exc:
                logger.warning("Could not remove orphan capture image %s: %s", path.name, exc.__class__.__name__)

        if deleted_rows or removed_files:
            logger.info("Capture retention removed %s records and %s image files", len(deleted_rows), removed_files)
    return len(deleted_rows)


async def retention_worker() -> None:
    while True:
        await asyncio.sleep(3600)
        try:
            await run_in_threadpool(prune_captures)
        except Exception:
            logger.exception("Periodic capture retention cleanup failed")


class TrashSegmenter:
    def __init__(self) -> None:
        self.model = None
        path = Path(os.getenv("MODEL_PATH", str(ROOT / "models" / "drain_trash.pt")))
        if path and path.is_file():
            from ultralytics import YOLO
            self.model = YOLO(str(path))

    def analyze(self, image_path: Path, roi_points: list[list[float]]) -> dict[str, Any]:
        if self.model is None:
            return {"status": "model_unavailable", "coverage": None, "detections": [], "annotated": None,
                    "error": "Set MODEL_PATH to trained segmentation weights."}
        image = Image.open(image_path).convert("RGB")
        width, height = image.size
        result = self.model.predict(source=str(image_path), conf=CONFIDENCE, verbose=False)[0]
        masks = result.masks
        if masks is None:
            import cv2
            annotated = cv2.cvtColor(result.plot(), cv2.COLOR_BGR2RGB)
            roi_mask_img = Image.new("1", (width, height), 0)
            from PIL import ImageDraw
            ImageDraw.Draw(roi_mask_img).polygon([(x * width, y * height) for x, y in roi_points], fill=1)
            roi_mask = np.asarray(roi_mask_img, dtype=bool)
            covered = np.zeros((height, width), dtype=bool)
            detections: list[dict[str, Any]] = []
            if result.boxes is not None:
                for idx, cls_tensor in enumerate(result.boxes.cls):
                    cls_id = int(cls_tensor.item())
                    label = CLASS_MAP.get(cls_id, str(result.names.get(cls_id, cls_id)).lower())
                    if label not in {"trash", "leaves", "plastic"}:
                        continue
                    x1, y1, x2, y2 = result.boxes.xyxy[idx].detach().cpu().tolist()
                    left, top = max(0, int(x1)), max(0, int(y1))
                    right, bottom = min(width, int(np.ceil(x2))), min(height, int(np.ceil(y2)))
                    if right <= left or bottom <= top:
                        continue
                    covered[top:bottom, left:right] = True
                    detections.append({"class": label, "confidence": round(float(result.boxes.conf[idx].item()), 4),
                                       "box": [round(float(v), 1) for v in (x1, y1, x2, y2)]})
            roi_area = int(roi_mask.sum())
            coverage = float((covered & roi_mask).sum() / roi_area) if roi_area else 0.0
            return {"status": "analyzed_bbox_estimate" if detections else "analyzed", "coverage": coverage,
                    "detections": detections, "annotated": annotated,
                    "error": "Coverage is an estimate from bounding boxes; use a site-trained segmentation model for accurate area." if detections else None}
        roi_mask_img = Image.new("1", (width, height), 0)
        from PIL import ImageDraw
        ImageDraw.Draw(roi_mask_img).polygon([(x * width, y * height) for x, y in roi_points], fill=1)
        roi_mask = np.asarray(roi_mask_img, dtype=bool)
        covered = np.zeros((height, width), dtype=bool)
        detections: list[dict[str, Any]] = []
        for idx, cls_tensor in enumerate(result.boxes.cls):
            cls_id = int(cls_tensor.item())
            label = CLASS_MAP.get(cls_id, str(result.names.get(cls_id, cls_id)).lower())
            if label not in {"trash", "leaves", "plastic"}:
                continue
            mask = masks.data[idx].detach().cpu().numpy()
            mask_img = Image.fromarray((mask * 255).astype(np.uint8)).resize((width, height), Image.Resampling.NEAREST)
            object_mask = np.asarray(mask_img) > 127
            intersect = object_mask & roi_mask
            covered |= intersect
            box = [round(float(v), 1) for v in result.boxes.xyxy[idx].detach().cpu().tolist()]
            detections.append({"class": label, "confidence": round(float(result.boxes.conf[idx].item()), 4), "box": box})
        roi_area = int(roi_mask.sum())
        coverage = float((covered & roi_mask).sum() / roi_area) if roi_area else 0.0
        import cv2
        return {"status": "analyzed", "coverage": coverage, "detections": detections,
                "annotated": cv2.cvtColor(result.plot(), cv2.COLOR_BGR2RGB), "error": None}


segmenter: TrashSegmenter | None = None


@asynccontextmanager
async def lifespan(_: FastAPI):
    global segmenter
    validate_production_config()
    init_db()
    prune_captures()
    devices_config()
    segmenter = TrashSegmenter()
    cleanup_task = asyncio.create_task(retention_worker())
    try:
        yield
    finally:
        cleanup_task.cancel()
        try:
            await cleanup_task
        except asyncio.CancelledError:
            pass


app = FastAPI(title="Drain Watch API", version="0.1.0", lifespan=lifespan)


def require_device_token(device_id: str, token: str) -> None:
    expected = DEVICE_TOKENS.get(device_id)
    if APP_ENV == "production" and not expected:
        raise HTTPException(503, "Device token is not configured")
    if expected and not secrets.compare_digest(token, expected):
        raise HTTPException(401, "Invalid device token")


@app.post("/api/capture-commands", dependencies=[Depends(require_admin)])
def create_capture_command(device_id: str = Form("drain-01")) -> dict[str, Any]:
    if device_id not in devices_config():
        raise HTTPException(404, "Unknown device")
    now = datetime.now(timezone.utc).isoformat()
    command_id = str(uuid.uuid4())
    with db() as conn:
        conn.execute("DELETE FROM capture_commands WHERE created_at < ?", ((datetime.now(timezone.utc) - timedelta(days=1)).isoformat(),))
        device_state = conn.execute("SELECT device_mode,last_seen FROM device_status WHERE device_id=?", (device_id,)).fetchone()
        if device_state:
            if device_state["device_mode"] == "deep_sleep":
                raise HTTPException(409, "Device is in deep sleep. Wake it with the float switch before requesting a capture.")
            last_seen = datetime.fromisoformat(device_state["last_seen"])
            if last_seen.tzinfo is None:
                last_seen = last_seen.replace(tzinfo=timezone.utc)
            if datetime.now(timezone.utc) - last_seen > timedelta(seconds=45):
                raise HTTPException(409, "Device is offline. Reconnect the ESP32-CAM before requesting a capture.")
        conn.execute("UPDATE capture_commands SET status='expired',updated_at=? WHERE device_id=? AND status='claimed' AND updated_at < ?",
                     (now, device_id, (datetime.now(timezone.utc) - timedelta(minutes=10)).isoformat()))
        active = conn.execute("SELECT id,status FROM capture_commands WHERE device_id=? AND status IN ('queued','claimed') ORDER BY created_at DESC LIMIT 1", (device_id,)).fetchone()
        if active:
            return {"id": active["id"], "device_id": device_id, "status": active["status"]}
        conn.execute("INSERT INTO capture_commands(id,device_id,status,created_at,updated_at) VALUES(?,?,?,?,?)",
                     (command_id, device_id, "queued", now, now))
    return {"id": command_id, "device_id": device_id, "status": "queued"}


@app.get("/api/capture-commands/{command_id}", dependencies=[Depends(require_admin)])
def get_capture_command(command_id: str) -> dict[str, Any]:
    with db() as conn:
        row = conn.execute("SELECT * FROM capture_commands WHERE id=?", (command_id,)).fetchone()
    if not row:
        raise HTTPException(404, "Capture command not found")
    return {"id": row["id"], "device_id": row["device_id"], "status": row["status"],
            "created_at": row["created_at"], "updated_at": row["updated_at"],
            "capture_id": row["capture_id"], "result": json.loads(row["result_json"]) if row["result_json"] else None,
            "error": row["error"]}


@app.get("/api/devices/{device_id}/capture-command", response_class=PlainTextResponse)
def poll_capture_command(device_id: str, x_device_token: str = Header(default="")) -> str:
    require_device_token(device_id, x_device_token)
    now = datetime.now(timezone.utc).isoformat()
    with db() as conn:
        # A queued command older than 10 minutes is stale. Claimed commands are
        # not reissued, preventing duplicate real captures and LINE alerts.
        conn.execute("UPDATE capture_commands SET status='expired',updated_at=? WHERE device_id=? AND status='queued' AND created_at < ?",
                     (now, device_id, (datetime.now(timezone.utc) - timedelta(minutes=10)).isoformat()))
        row = conn.execute("SELECT id FROM capture_commands WHERE device_id=? AND status='queued' ORDER BY created_at LIMIT 1", (device_id,)).fetchone()
        if not row:
            return "NONE"
        conn.execute("UPDATE capture_commands SET status='claimed',updated_at=? WHERE id=? AND status='queued'", (now, row["id"]))
        return "CAPTURE " + row["id"]


@app.get("/api/devices/{device_id}/test-control", response_class=PlainTextResponse)
def poll_test_control(device_id: str, x_device_token: str = Header(default="")) -> str:
    """Combined low-traffic control poll for the battery-powered test sketch."""
    require_device_token(device_id, x_device_token)
    now = datetime.now(timezone.utc).isoformat()
    with db() as conn:
        state = conn.execute("SELECT flash_requested FROM device_status WHERE device_id=?", (device_id,)).fetchone()
        conn.execute("UPDATE capture_commands SET status='expired',updated_at=? WHERE device_id=? AND status='queued' AND created_at < ?",
                     (now, device_id, (datetime.now(timezone.utc) - timedelta(minutes=10)).isoformat()))
        row = conn.execute("SELECT id FROM capture_commands WHERE device_id=? AND status='queued' ORDER BY created_at LIMIT 1", (device_id,)).fetchone()
        capture_id = row["id"] if row else "NONE"
        if row:
            conn.execute("UPDATE capture_commands SET status='claimed',updated_at=? WHERE id=? AND status='queued'", (now, row["id"]))
        flash = "ON" if state and state["flash_requested"] else "OFF"
    return f"FLASH={flash};CAPTURE={capture_id}"


def send_line_alert(device: dict[str, Any], device_id: str, coverage: float, image_url: str,
                    water_level: int | None = None, flow_rate: float | None = None,
                    flow_status: str | None = None, alert_reason: str = "ขยะเกินเกณฑ์") -> tuple[bool, str]:
    token = os.getenv("LINE_CHANNEL_ACCESS_TOKEN", "").strip()
    if not token:
        return False, "not_configured"
    water_text = "สูง" if water_level == 1 else "ปกติ" if water_level == 0 else "ไม่มีข้อมูล"
    flow_text = f"{flow_rate:.2f} L/min ({flow_status or 'ไม่ระบุ'})" if flow_rate is not None else "ไม่มีข้อมูล"
    coverage_text = f"พื้นที่ขยะ {coverage:.1%} (เกณฑ์ {THRESHOLD:.0%})\n" if coverage is not None else ""
    messages = [{"type": "text", "text":
         f"แจ้งเตือน Drain Watch: {device.get('name', device_id)} ({device.get('location', 'ไม่ระบุพิกัด')})\n"
         f"เหตุผล: {alert_reason}\n{coverage_text}"
         f"ระดับน้ำ: {water_text} · การไหล: {flow_text}"}]
    # Broadcast is text-only by default; a hosted image URL is optional.
    origin = os.getenv("PUBLIC_BASE_URL", "").rstrip("/")
    if origin and image_url:
        image_url_public = f"{origin}{image_url}"
        messages.append({"type": "image", "originalContentUrl": image_url_public,
                         "previewImageUrl": image_url_public})
    body = {"messages": messages}
    try:
        response = requests.post("https://api.line.me/v2/bot/message/broadcast", json=body,
                                 headers={"Authorization": f"Bearer {token}"}, timeout=10)
        response.raise_for_status()
        return True, "sent"
    except requests.RequestException as exc:
        return False, f"send_failed: {exc.__class__.__name__}"


def alert_reasons(coverage: float | None, water_level: int | None,
                  flow_status: str | None) -> list[str]:
    reasons = []
    if coverage is not None and coverage > THRESHOLD:
        reasons.append(f"ขยะเกินเกณฑ์ {coverage:.1%} (เกณฑ์ {THRESHOLD:.0%})")
    if water_level == 1:
        reasons.append("ระดับน้ำสูง")
    if flow_status == "slow":
        reasons.append("อัตราการไหลช้ามาก")
    elif flow_status == "no_flow":
        reasons.append("ตรวจไม่พบการไหลของน้ำ")
    return reasons


def alert_cooldown_ok(device_id: str) -> bool:
    if COOLDOWN_MINUTES == 0:
        return True
    with db() as conn:
        latest = conn.execute(
            "SELECT received_at FROM captures WHERE device_id=? AND alert_sent=1 ORDER BY received_at DESC LIMIT 1",
            (device_id,),
        ).fetchone()
        state_alert = conn.execute("SELECT last_alert_at FROM alert_cooldowns WHERE device_id=?", (device_id,)).fetchone()
    latest_at = latest[0] if latest else None
    if state_alert and (not latest_at or state_alert[0] > latest_at):
        latest_at = state_alert[0]
    return not latest_at or datetime.now(timezone.utc) - datetime.fromisoformat(latest_at) >= timedelta(minutes=COOLDOWN_MINUTES)


def mark_alert_sent(device_id: str) -> None:
    now = datetime.now(timezone.utc).isoformat()
    with db() as conn:
        conn.execute("INSERT INTO alert_cooldowns(device_id,last_alert_at) VALUES(?,?) "
                     "ON CONFLICT(device_id) DO UPDATE SET last_alert_at=excluded.last_alert_at", (device_id, now))


@app.get("/", response_class=HTMLResponse, dependencies=[Depends(require_admin)])
def dashboard() -> str:
    return (ROOT / "app/dashboard.html").read_text(encoding="utf-8")


@app.get("/test", response_class=HTMLResponse, dependencies=[Depends(require_admin)])
def model_test_page() -> str:
    return (ROOT / "app/test.html").read_text(encoding="utf-8")


@app.get("/api/health")
def health() -> dict[str, Any]:
    return {"status": "ok", "model_loaded": bool(segmenter and segmenter.model),
            "alert_threshold": THRESHOLD,
            "retention": {"max_captures": MAX_STORED_CAPTURES,
                          "max_age_days": CAPTURE_RETENTION_DAYS,
                          "max_image_storage_mb": MAX_CAPTURE_STORAGE_BYTES // (1024 * 1024)},
            "line_configured": bool(os.getenv("LINE_CHANNEL_ACCESS_TOKEN")),
            "line_delivery": "broadcast_to_oa_friends"}


@app.get("/api/device-status", dependencies=[Depends(require_admin)])
def device_status() -> list[dict[str, Any]]:
    now = datetime.now(timezone.utc)
    with db() as conn:
        rows = {row["device_id"]: dict(row) for row in conn.execute("SELECT * FROM device_status")}
    result = []
    for device_id, device in devices_config().items():
        row = rows.get(device_id)
        if not row:
            result.append({"id": device_id, "name": device.get("name", device_id),
                           "location": device.get("location", ""), "connection": "offline",
                           "mode": "unknown", "last_seen": None, "water_level": None,
                           "flow_rate": None, "flow_status": None, "flash_on": False,
                           "flash_requested": False, "sleep_ready": False, "camera_ready": False})
            continue
        seen = datetime.fromisoformat(row["last_seen"])
        if seen.tzinfo is None:
            seen = seen.replace(tzinfo=timezone.utc)
        connection = "sleeping" if row["device_mode"] == "deep_sleep" else (
            "online" if now - seen <= timedelta(seconds=45) else "offline")
        result.append({"id": device_id, "name": device.get("name", device_id),
                       "location": device.get("location", ""), "connection": connection,
                       "mode": row["device_mode"], "last_seen": row["last_seen"],
                       "water_level": row["water_level"], "flow_rate": row["flow_rate"],
                       "flow_status": row["flow_status"], "flash_on": bool(row["flash_on"]),
                       "flash_requested": bool(row["flash_requested"]),
                       "sleep_ready": bool(row["sleep_ready"]),
                       "camera_ready": bool(row["camera_ready"])})
    return result


@app.post("/api/devices/{device_id}/flash", dependencies=[Depends(require_admin)])
def request_flash(device_id: str, enabled: bool = Form(...)) -> dict[str, Any]:
    if device_id not in devices_config():
        raise HTTPException(404, "Unknown device")
    with db() as conn:
        conn.execute("INSERT INTO device_status(device_id,last_seen,device_mode,flash_requested) VALUES(?,?,?,?) "
                     "ON CONFLICT(device_id) DO UPDATE SET flash_requested=excluded.flash_requested",
                     (device_id, "1970-01-01T00:00:00+00:00", "unknown", int(enabled)))
        row = conn.execute("SELECT flash_on,device_mode FROM device_status WHERE device_id=?", (device_id,)).fetchone()
    return {"device_id": device_id, "requested": enabled, "applied": bool(row["flash_on"]), "mode": row["device_mode"]}


@app.post("/api/devices/{device_id}/sleep-confirmation/reset")
def reset_sleep_confirmation(device_id: str, camera_ready: bool = Form(False),
                             x_device_token: str = Header(default="")) -> dict[str, Any]:
    """Require a fresh dashboard readiness confirmation after every boot/wake."""
    require_device_token(device_id, x_device_token)
    now = datetime.now(timezone.utc).isoformat()
    with db() as conn:
        conn.execute("INSERT INTO device_status(device_id,last_seen,device_mode,sleep_ready,camera_ready,flash_on,flash_requested) VALUES(?,?,?,0,?,0,0) "
                     "ON CONFLICT(device_id) DO UPDATE SET last_seen=excluded.last_seen,device_mode='active',sleep_ready=0,camera_ready=excluded.camera_ready,flash_on=0,flash_requested=0",
                     (device_id, now, "active", int(camera_ready)))
    return {"device_id": device_id, "sleep_ready": False, "camera_ready": camera_ready,
            "status": "awaiting_confirmation"}


@app.get("/api/devices/{device_id}/sleep-confirmation", response_class=PlainTextResponse)
def get_sleep_confirmation(device_id: str, x_device_token: str = Header(default="")) -> str:
    require_device_token(device_id, x_device_token)
    with db() as conn:
        row = conn.execute("SELECT sleep_ready FROM device_status WHERE device_id=?", (device_id,)).fetchone()
    return "READY" if row and row["sleep_ready"] else "WAIT"


@app.post("/api/devices/{device_id}/sleep-confirmation", dependencies=[Depends(require_admin)])
def set_sleep_confirmation(device_id: str, ready: bool = Form(...)) -> dict[str, Any]:
    if device_id not in devices_config():
        raise HTTPException(404, "Unknown device")
    with db() as conn:
        row = conn.execute("SELECT last_seen,device_mode,camera_ready FROM device_status WHERE device_id=?", (device_id,)).fetchone()
        if not row:
            raise HTTPException(409, "อุปกรณ์ยังไม่ส่งสถานะจากบอร์ด")
        seen = datetime.fromisoformat(row["last_seen"])
        if seen.tzinfo is None:
            seen = seen.replace(tzinfo=timezone.utc)
        if ready and (row["device_mode"] == "deep_sleep" or not row["camera_ready"] or datetime.now(timezone.utc) - seen > timedelta(seconds=45)):
            raise HTTPException(409, "ยืนยันได้เมื่อบอร์ดออนไลน์และกล้องพร้อมเท่านั้น")
        conn.execute("UPDATE device_status SET sleep_ready=? WHERE device_id=?", (int(ready), device_id))
    return {"device_id": device_id, "sleep_ready": ready}


@app.get("/api/devices/{device_id}/flash-desired", response_class=PlainTextResponse)
def desired_flash(device_id: str, x_device_token: str = Header(default="")) -> str:
    require_device_token(device_id, x_device_token)
    with db() as conn:
        row = conn.execute("SELECT flash_requested FROM device_status WHERE device_id=?", (device_id,)).fetchone()
    return "ON" if row and row["flash_requested"] else "OFF"


@app.post("/api/test-image", dependencies=[Depends(require_admin)])
async def test_image(image: UploadFile = File(...)) -> dict[str, Any]:
    """Run an ad-hoc prediction without saving a capture or sending alerts."""
    if not segmenter or not segmenter.model:
        raise HTTPException(503, "Model is not loaded")
    if image.content_type not in {"image/jpeg", "image/jpg", "image/png", "image/webp", "application/octet-stream"}:
        raise HTTPException(415, "Upload a JPEG, PNG, or WebP image")
    payload = await image.read(MAX_IMAGE_BYTES + 1)
    if not payload or len(payload) > MAX_IMAGE_BYTES:
        raise HTTPException(413, f"Image must be between 1 byte and {MAX_IMAGE_BYTES} bytes")
    try:
        from io import BytesIO
        source = Image.open(BytesIO(payload))
        source.verify()
        source = Image.open(BytesIO(payload)).convert("RGB")
        if source.width < 16 or source.height < 16:
            raise ValueError("Image dimensions are too small")
    except (UnidentifiedImageError, OSError, ValueError) as exc:
        raise HTTPException(415, "Invalid image file") from exc

    with tempfile.TemporaryDirectory(prefix="model-test-", dir=DATA_DIR) as tmp:
        image_path = Path(tmp) / "upload.jpg"
        source.save(image_path, "JPEG", quality=92)
        try:
            analysis = await run_in_threadpool(segmenter.analyze, image_path,
                                               [[0, 0], [1, 0], [1, 1], [0, 1]])
        except Exception as exc:
            raise HTTPException(500, f"Inference failed: {exc.__class__.__name__}") from exc
        annotated_data = None
        if analysis["annotated"] is not None:
            output = BytesIO()
            Image.fromarray(analysis["annotated"]).save(output, format="JPEG", quality=86)
            annotated_data = "data:image/jpeg;base64," + base64.b64encode(output.getvalue()).decode("ascii")
    return {"filename": image.filename, "width": source.width, "height": source.height,
            "model_status": analysis["status"], "coverage": analysis["coverage"],
            "detections": analysis["detections"], "error": analysis["error"],
            "annotated_image": annotated_data}


@app.get("/api/devices", dependencies=[Depends(require_admin)])
def list_devices() -> list[dict[str, Any]]:
    return [{"id": device_id, **device} for device_id, device in devices_config().items()]


@app.post("/api/captures")
async def create_capture(device_id: str = Form(...), captured_at: str = Form(...),
                         image: UploadFile = File(...), device_token: str = Form(""),
                         command_id: str | None = Form(None),
                         water_level: int | None = Form(None), flow_rate: float | None = Form(None),
                         flow_status: str | None = Form(None)) -> dict[str, Any]:
    devices = devices_config()
    if device_id not in devices:
        raise HTTPException(404, "Unknown device")
    require_device_token(device_id, device_token)
    if command_id:
        with db() as conn:
            command = conn.execute("SELECT status FROM capture_commands WHERE id=? AND device_id=?", (command_id, device_id)).fetchone()
        if not command or command["status"] != "claimed":
            raise HTTPException(409, "Capture command is missing, expired, or already completed")
    if water_level not in (None, 0, 1):
        raise HTTPException(422, "water_level must be 0, 1, or omitted")
    if flow_rate is not None and flow_rate < 0:
        raise HTTPException(422, "flow_rate must be non-negative")
    if flow_status not in (None, "ok", "slow", "no_flow"):
        raise HTTPException(422, "flow_status must be ok, slow, no_flow, or omitted")
    try:
        timestamp = datetime.fromisoformat(captured_at.replace("Z", "+00:00"))
        if timestamp.tzinfo is None:
            timestamp = timestamp.replace(tzinfo=timezone.utc)
    except ValueError as exc:
        raise HTTPException(422, "captured_at must be ISO 8601") from exc
    content_type = image.content_type or ""
    if content_type not in {"image/jpeg", "image/jpg", "application/octet-stream"}:
        raise HTTPException(415, "Upload a JPEG image")
    payload = await image.read(MAX_IMAGE_BYTES + 1)
    if not payload or len(payload) > MAX_IMAGE_BYTES:
        raise HTTPException(413, f"Image must be between 1 byte and {MAX_IMAGE_BYTES} bytes")
    capture_id = str(uuid.uuid4())
    original_path = IMAGE_DIR / f"{capture_id}.jpg"
    annotated_path = IMAGE_DIR / f"{capture_id}-annotated.jpg"
    try:
        from io import BytesIO
        source = Image.open(BytesIO(payload))
        source.verify()
        source = Image.open(BytesIO(payload)).convert("RGB")
        if source.width < 16 or source.height < 16:
            raise ValueError("Image dimensions are too small")
        source.save(original_path, "JPEG", quality=92)
    except (UnidentifiedImageError, OSError, ValueError) as exc:
        raise HTTPException(415, "Invalid JPEG image") from exc

    assert segmenter is not None
    try:
        analysis = await run_in_threadpool(segmenter.analyze, original_path, devices[device_id]["roi"])
    except Exception as exc:
        analysis = {"status": "inference_error", "coverage": None, "detections": [], "annotated": None,
                    "error": f"Inference failed: {exc.__class__.__name__}"}
    if analysis["annotated"] is not None:
        Image.fromarray(analysis["annotated"]).save(annotated_path, "JPEG", quality=88)

    reasons = alert_reasons(analysis["coverage"], water_level, flow_status)
    alert_sent, alert_status = False, "not_triggered"
    if reasons:
        if alert_cooldown_ok(device_id):
            alert_sent, alert_status = await run_in_threadpool(
                send_line_alert, devices[device_id], device_id, analysis["coverage"],
                f"/api/captures/{capture_id}/image?annotated=true", water_level,
                flow_rate, flow_status, " / ".join(reasons))
        else:
            alert_status = "cooldown"

    if reasons and alert_status != "cooldown":
        mark_alert_sent(device_id)

    received_at = datetime.now(timezone.utc).isoformat()
    with db() as conn:
        conn.execute("""INSERT INTO captures(id,device_id,captured_at,received_at,image_path,annotated_path,
            model_status,coverage,alert_threshold,alert_sent,alert_status,detections_json,error,
            water_level,flow_rate,flow_status)
            VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)""",
            (capture_id, device_id, timestamp.astimezone(timezone.utc).isoformat(), received_at,
             str(original_path), str(annotated_path) if annotated_path.exists() else None,
             analysis["status"], analysis["coverage"], THRESHOLD, int(alert_sent), alert_status,
             json.dumps(analysis["detections"]), analysis["error"], water_level, flow_rate, flow_status))
        if command_id:
            result = {"id": capture_id, "device_id": device_id, "model_status": analysis["status"],
                      "coverage": analysis["coverage"], "threshold": THRESHOLD,
                      "detections": analysis["detections"], "alert_sent": alert_sent,
                      "alert_status": alert_status, "error": analysis["error"]}
            conn.execute("UPDATE capture_commands SET status='completed',capture_id=?,result_json=?,updated_at=? WHERE id=? AND status='claimed'",
                         (capture_id, json.dumps(result), datetime.now(timezone.utc).isoformat(), command_id))
    await run_in_threadpool(prune_captures)
    return {"id": capture_id, "device_id": device_id, "model_status": analysis["status"],
            "coverage": analysis["coverage"], "threshold": THRESHOLD, "detections": analysis["detections"],
            "alert_sent": alert_sent, "alert_status": alert_status, "error": analysis["error"]}


@app.post("/api/telemetry")
async def create_telemetry(device_id: str = Form(...), captured_at: str = Form(...),
                           device_token: str = Form(""), water_level: int | None = Form(None),
                           flow_rate: float | None = Form(None), flow_status: str | None = Form(None),
                           device_mode: str = Form("active"), flash_on: bool = Form(False),
                           camera_ready: bool = Form(False)) -> dict[str, Any]:
    """Update live device state without storing every heartbeat as a capture."""
    devices = devices_config()
    if device_id not in devices:
        raise HTTPException(404, "Unknown device")
    expected_token = DEVICE_TOKENS.get(device_id)
    if APP_ENV == "production" and not expected_token:
        raise HTTPException(503, "Device token is not configured")
    if expected_token and not secrets.compare_digest(device_token, expected_token):
        raise HTTPException(401, "Invalid device token")
    if water_level not in (None, 0, 1) or (flow_rate is not None and flow_rate < 0):
        raise HTTPException(422, "Invalid sensor values")
    if flow_status not in (None, "ok", "slow", "no_flow"):
        raise HTTPException(422, "Invalid flow_status")
    if device_mode not in {"active", "deep_sleep", "standby"}:
        raise HTTPException(422, "device_mode must be active, deep_sleep, or standby")
    try:
        timestamp = datetime.fromisoformat(captured_at.replace("Z", "+00:00"))
        if timestamp.tzinfo is None:
            timestamp = timestamp.replace(tzinfo=timezone.utc)
    except ValueError as exc:
        raise HTTPException(422, "captured_at must be ISO 8601") from exc
    capture_id = str(uuid.uuid4())
    received_at = datetime.now(timezone.utc).isoformat()
    reasons = alert_reasons(None, water_level, flow_status)
    alert_sent, alert_status = False, "not_triggered"
    if reasons:
        if alert_cooldown_ok(device_id):
            alert_sent, alert_status = await run_in_threadpool(
                send_line_alert, devices[device_id], device_id, None, "", water_level,
                flow_rate, flow_status, " / ".join(reasons))
        else:
            alert_status = "cooldown"
    if reasons and alert_status != "cooldown":
        mark_alert_sent(device_id)
    with db() as conn:
        conn.execute("""INSERT INTO device_status(device_id,last_seen,device_mode,water_level,flow_rate,flow_status,flash_on,flash_requested,camera_ready)
            VALUES(?,?,?,?,?,?,?,?,?) ON CONFLICT(device_id) DO UPDATE SET last_seen=excluded.last_seen,
            device_mode=excluded.device_mode,water_level=excluded.water_level,flow_rate=excluded.flow_rate,
            flow_status=excluded.flow_status,flash_on=excluded.flash_on,camera_ready=excluded.camera_ready""",
            (device_id, received_at, device_mode, water_level, flow_rate, flow_status, int(flash_on), int(flash_on), int(camera_ready)))
        if reasons and alert_sent:
            conn.execute("""INSERT INTO captures(id,device_id,captured_at,received_at,image_path,annotated_path,
                model_status,coverage,alert_threshold,alert_sent,alert_status,detections_json,error,
                water_level,flow_rate,flow_status) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)""",
                (capture_id, device_id, timestamp.astimezone(timezone.utc).isoformat(), received_at, "", None,
                 "sensor_event", None, THRESHOLD, int(alert_sent), alert_status, "[]", None,
                 water_level, flow_rate, flow_status))
    return {"id": capture_id, "device_id": device_id, "model_status": "sensor_event",
            "alert_sent": alert_sent, "alert_status": alert_status}


@app.get("/api/captures", dependencies=[Depends(require_admin)])
def list_captures(limit: int = Query(50, ge=1, le=200), device_id: str | None = None) -> list[dict[str, Any]]:
    with db() as conn:
        if device_id:
            rows = conn.execute("SELECT * FROM captures WHERE device_id=? ORDER BY received_at DESC LIMIT ?", (device_id, limit)).fetchall()
        else:
            rows = conn.execute("SELECT * FROM captures ORDER BY received_at DESC LIMIT ?", (limit,)).fetchall()
    return [{"id": r["id"], "device_id": r["device_id"], "captured_at": r["captured_at"],
             "received_at": r["received_at"], "model_status": r["model_status"], "coverage": r["coverage"],
             "alert_threshold": r["alert_threshold"], "alert_sent": bool(r["alert_sent"]),
             "alert_status": r["alert_status"], "detections": json.loads(r["detections_json"]), "error": r["error"],
             "water_level": r["water_level"], "flow_rate": r["flow_rate"], "flow_status": r["flow_status"]}
            for r in rows]


@app.get("/api/captures/{capture_id}/image", dependencies=[Depends(require_admin)])
def capture_image(capture_id: str, annotated: bool = False) -> FileResponse:
    with db() as conn:
        row = conn.execute("SELECT image_path,annotated_path FROM captures WHERE id=?", (capture_id,)).fetchone()
    if not row:
        raise HTTPException(404, "Capture not found")
    candidate = Path(row["annotated_path"]) if annotated and row["annotated_path"] else Path(row["image_path"])
    if not candidate.is_file():
        raise HTTPException(404, "Image not found")
    return FileResponse(candidate, media_type="image/jpeg", headers={"Cache-Control": "private, max-age=300"})
