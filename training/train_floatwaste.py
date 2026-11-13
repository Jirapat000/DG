"""Train a one-class YOLO detector for litter from the Indonesia FloatWaste subset."""
from __future__ import annotations

import argparse
import shutil
from pathlib import Path

from ultralytics import YOLO


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--data", type=Path, default=Path(__file__).parent / "dataset/data.yaml")
    parser.add_argument("--epochs", type=int, default=80)
    parser.add_argument("--imgsz", type=int, default=640)
    parser.add_argument("--batch", type=int, default=4)
    parser.add_argument("--device", default="0")
    args = parser.parse_args()
    project_root = Path(__file__).resolve().parents[1]
    model = YOLO("yolo11n.pt")
    result = model.train(data=str(args.data), epochs=args.epochs, imgsz=args.imgsz, batch=args.batch,
                         device=args.device, workers=2, cache=False, patience=15,
                         project=str(project_root / "runs" / "detect"), name="floatwaste-indonesia", exist_ok=True)
    best = Path(result.save_dir) / "weights" / "best.pt"
    if not best.is_file():
        raise FileNotFoundError(f"Training completed without best.pt at {best}")
    destination = project_root / "models" / "drain_trash.pt"
    destination.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(best, destination)
    print(f"Starter detector weights saved to {destination}")
    print('Configure CLASS_MAP_JSON={"0":"trash"} for this one-class detector.')


if __name__ == "__main__":
    main()
