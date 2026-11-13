"""Fetch the CC BY 4.0 Indonesia subset of FloatWaste and convert COCO boxes to YOLO."""
from __future__ import annotations

import json
import shutil
from pathlib import Path

from huggingface_hub import snapshot_download

REPO_ID = "OliviaWuYJ/SC-floatwaste"
REPO_URL = "https://huggingface.co/datasets/OliviaWuYJ/SC-floatwaste"
OUTPUT = Path(__file__).parent / "dataset"


def main() -> None:
    snapshot = Path(snapshot_download(
        repo_id=REPO_ID,
        repo_type="dataset",
        allow_patterns=["annotations/instances_train.json", "annotations/instances_val.json", "images/Indonesia/**", "LICENSE", "README.md"],
    ))
    image_root = OUTPUT / "images"
    label_out = OUTPUT / "labels"
    image_root.mkdir(parents=True, exist_ok=True)
    counts = {"train": 0, "val": 0}
    annotations_used = 0
    source_images_used: list[dict] = []

    for source_split, annotation_file in (("train", "instances_train.json"), ("val", "instances_val.json")):
        coco = json.loads((snapshot / "annotations" / annotation_file).read_text(encoding="utf-8"))
        anns_by_image: dict[int, list[dict]] = {}
        for ann in coco.get("annotations", []):
            anns_by_image.setdefault(int(ann["image_id"]), []).append(ann)
        split_list: list[str] = []
        for image in coco.get("images", []):
            if image.get("region") != "Indonesia":
                continue
            source = snapshot / image["file_name"]
            if not source.is_file():
                continue
            image_id = int(image["id"])
            destination_name = f"{image_id:06d}{source.suffix.lower() or '.jpg'}"
            destination = image_root / source_split / destination_name
            destination.parent.mkdir(parents=True, exist_ok=True)
            if not destination.exists():
                shutil.copy2(source, destination)
            label_path = label_out / source_split / f"{Path(destination_name).stem}.txt"
            label_path.parent.mkdir(parents=True, exist_ok=True)
            rows = []
            width, height = float(image["width"]), float(image["height"])
            for ann in anns_by_image.get(image_id, []):
                box = ann.get("bbox") or []
                if len(box) != 4 or width <= 0 or height <= 0:
                    continue
                x, y, w, h = (float(value) for value in box)
                if w <= 0 or h <= 0:
                    continue
                cx, cy = (x + w / 2) / width, (y + h / 2) / height
                nw, nh = w / width, h / height
                if not (0 <= cx <= 1 and 0 <= cy <= 1 and 0 < nw <= 1 and 0 < nh <= 1):
                    continue
                rows.append(f"0 {cx:.6f} {cy:.6f} {nw:.6f} {nh:.6f}")
                annotations_used += 1
            label_path.write_text("\n".join(rows), encoding="utf-8")
            split_list.append(f"images/{source_split}/{destination_name}")
            source_images_used.append({"source_image": image["file_name"], "file_name": f"images/{source_split}/{destination_name}",
                                       "region": image.get("region"), "site": image.get("site"),
                                       "source_path": image.get("source_path")})
        counts[source_split] = len(split_list)

    if counts["train"] == 0 or counts["val"] == 0 or annotations_used == 0:
        raise RuntimeError(f"No usable Indonesia FloatWaste data was prepared: {counts}, annotations={annotations_used}")
    (OUTPUT / "data.yaml").write_text(
        f"path: {OUTPUT.resolve().as_posix()}\ntrain: images/train\nval: images/val\nnames:\n  0: trash\n",
        encoding="utf-8")
    for cache in (OUTPUT / "labels.cache", label_out / "train.cache", label_out / "val.cache"):
        if cache.exists():
            cache.unlink()
    source_license = snapshot / "LICENSE"
    if source_license.is_file():
        shutil.copy2(source_license, OUTPUT / "FLOATWASTE_LICENSE")
    (OUTPUT / "attribution.json").write_text(json.dumps({
        "dataset": "FloatWaste: a multi-region benchmark for detecting floating debris",
        "dataset_url": REPO_URL,
        "license": "CC BY 4.0",
        "citation": "Wu, Yujiao et al., FloatWaste: a multi-region riverine floating-waste detection benchmark. Dataset card notes that final author-list/year citation fields are still pending. Dataset record: https://doi.org/10.5281/zenodo.21934286",
        "adaptation": "Only Indonesia-region images retained; all annotated litter categories merged into one class 'trash'; COCO bounding boxes converted to YOLO detection labels.",
        "split_counts": counts,
        "annotations_used": annotations_used,
        "images": source_images_used,
    }, ensure_ascii=False, indent=2), encoding="utf-8")
    print(f"Prepared FloatWaste Indonesia subset at {OUTPUT.resolve()}: {counts}; {annotations_used} litter boxes")


if __name__ == "__main__":
    main()
