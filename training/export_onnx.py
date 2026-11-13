"""Export the detector to ONNX and copy it into the static Vercel test bundle."""
from __future__ import annotations

import shutil
from pathlib import Path

from ultralytics import YOLO

ROOT = Path(__file__).resolve().parents[1]
WEIGHTS = ROOT / "models" / "drain_trash.pt"
DESTINATION = ROOT / "vercel-test" / "drain_trash.onnx"


def main() -> None:
    if not WEIGHTS.is_file():
        raise FileNotFoundError(f"Trained detector weights not found: {WEIGHTS}")
    exported = Path(YOLO(str(WEIGHTS)).export(format="onnx", imgsz=640, simplify=True, opset=17))
    DESTINATION.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(exported, DESTINATION)
    print(f"Browser test model copied to {DESTINATION} ({DESTINATION.stat().st_size:,} bytes)")


if __name__ == "__main__":
    main()
