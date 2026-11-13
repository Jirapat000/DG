# Drain Watch — ESP32-CAM trash detector

A small starter system for monitoring drain grates from ESP32-CAM JPEG snapshots. The Python service stores each image and its analysis in SQLite, estimates trash coverage inside a per-device polygon ROI, serves a web dashboard, and can broadcast LINE OA waste alerts to all friends of the Official Account.

## Quick start

1. Create a virtual environment and install dependencies: `python -m venv .venv`, then `.venv\\Scripts\\Activate.ps1` on Windows (or `source .venv/bin/activate` elsewhere), then `pip install -r requirements.txt`.
2. Copy `.env.example` to `.env`, set `MODEL_PATH` to trained YOLO detection weights, and configure `CLASS_MAP_JSON` to map model class IDs to `trash`, `leaves`, and/or `plastic`.
3. Run `uvicorn app.main:app --host 0.0.0.0 --port 8000` and open `http://localhost:8000`.
4. Configure `config/devices.json` with each camera's normalized ROI polygon, then set `API_HOST`, `API_PORT`, and `API_PATH` in the firmware. Firmware uploads require HTTPS with a trusted root CA. For local development, use a desktop HTTP client against the API.

The included bootstrap workflow trains a starter one-class detector on the CC BY 4.0 FloatWaste Indonesia river-litter subset. Its source imagery is not captured at drain grates, so use it as a transfer-learning baseline and fine-tune on your own ESP32-CAM images before relying on alerts. This detection model has boxes rather than masks; the API can only estimate ROI coverage from boxes. Use a field-trained segmentation model for accurate blocked-area measurement.

For the full Thai system, model, data provenance, training alternatives, field validation and operations guide, read [`docs/SYSTEM_MANUAL_TH.md`](docs/SYSTEM_MANUAL_TH.md). The standalone browser test is deployed at [drain-trash-model-test.vercel.app](https://drain-trash-model-test.vercel.app) (Vercel SSO protection applies). It runs the ONNX model in-browser; the full FastAPI, database and LINE capture service run separately.

The training data is FloatWaste CC BY 4.0, with changes documented in `training/README.md`. Ultralytics states YOLO code and trained models are AGPL-3.0 by default; review its [license terms](https://www.ultralytics.com/license) before proprietary, commercial, or broad external use.

Open `http://localhost:8000/test` for the standalone model test page. It accepts JPEG, PNG, and WebP uploads up to the configured `MAX_IMAGE_MB` size (8 MB by default), shows annotated detections, confidence, and estimated coverage, and does not save test uploads to capture history or send alerts.

See `training/README.md` for provenance, dataset preparation, and training commands.

## API

- `GET /api/health` — service/model state.
- `GET /api/devices` — configured devices and ROI state.
- `POST /api/captures` — multipart form: `device_id`, `captured_at` (ISO 8601), `image` (JPEG), and optional `device_token`.
- `POST /api/telemetry` — multipart sensor-only event: `device_id`, `captured_at`, `water_level`, `flow_rate`, `flow_status`, and `device_token`.
- `GET /api/captures?limit=50` — newest capture records.
- `GET /api/captures/{id}/image?annotated=true` — original or annotated image.

If `DEVICE_TOKENS_JSON` is configured, uploads require the matching device token. LINE uses Messaging API broadcast; set `LINE_CHANNEL_ACCESS_TOKEN`. Alerts reach friends of the Official Account and include text only unless `PUBLIC_BASE_URL` points to a host that serves this API's capture images. Keep `.env` and credentials private.

## ESP32-CAM

`firmware/esp32_cam/esp32_cam.ino` targets the common AI Thinker ESP32-CAM pin layout. Copy `firmware/esp32_cam/secrets.h.example` to `secrets.h` and set Wi-Fi/API credentials and the trusted root CA there; `secrets.h` is excluded from Git. Camera-only capture is enabled by default; flow/float sensors are optional and disabled until `ENABLE_FLOW_SENSORS` is set to `1`. Setup and backend deployment steps are in [`docs/ESP32_BACKEND_SETUP_TH.md`](docs/ESP32_BACKEND_SETUP_TH.md). The firmware validates the server certificate; do not replace this with an insecure TLS mode for deployment.

## Storage and retention

Images are saved under `data/images/`; metadata is stored in `data/drain_watch.sqlite3`. Configure a backup and retention policy for deployment. The starter does not automatically delete evidence images.

## Docker

```powershell
Copy-Item .env.example .env
docker compose up --build
```

This starts the FastAPI service on `http://localhost:8000`; open `/test` for API-backed upload testing.

## Production deployment (Docker host + HTTPS)

The full service needs a Linux Docker host with a persistent disk and a public DNS name. Vercel currently hosts only the browser model playground; its serverless filesystem is not the persistent SQLite/image storage this API expects. This repository includes a production Docker Compose stack with Caddy-managed HTTPS:

1. Point the domain's A/AAAA records to the Docker host, and allow inbound TCP 80/443 (UDP 443 is optional for HTTP/3).
2. Copy `.env.production.example` to `.env.production` on the host. Set `PUBLIC_DOMAIN`, a long admin password, a unique 32+ character token for each configured camera in `DEVICE_TOKENS_JSON`, and the LINE channel access token. Do not use the token previously pasted into chat; rotate it in LINE Developers first.
3. Run `docker compose --env-file .env.production -f docker-compose.prod.yml up -d --build`.
4. Open `https://your-domain/` and sign in with the Basic Auth username/password. Check `/api/health`; upload endpoints authenticate the camera with its device token.
5. Back up the named `drain-watch-data` volume and test restore. Set a retention/monitoring policy before field operation.

This prepares the project for production deployment but does not itself create a server, DNS record, paid storage, or deployment. The admin dashboard/API use HTTPS Basic Auth; LINE alerts are text broadcasts. Sensor-only `/api/telemetry` records water/flow events and can notify on high water, slow flow, or no flow before the ESP32 enters sleep.
