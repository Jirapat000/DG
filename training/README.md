# Starter trash detector

## Data source and limits

The starter model uses the Indonesia-region subset of FloatWaste, a CC BY 4.0 dataset of floating litter in rivers. The dataset contains 7,906 images total; the Indonesian region contributes 862 labeled images and is the closest openly licensed imagery located during research. Its images show floating river debris, not drain grates, and annotations are boxes rather than segmentation masks. Treat the resulting weights as a transfer-learning baseline, not a validated drain-blockage detector. [Dataset card and licence](https://huggingface.co/datasets/OliviaWuYJ/SC-floatwaste).

TACO was considered because it includes litter masks, but the official annotation JSON downloaded during this task has an empty `licenses` list and image records with no license value. Its images were therefore excluded from training. The search also surfaced example photos of blocked drains, but the Nature Conservancy page credits its image to Shutterstock; those browser images were not copied into the dataset. [TACO official repository](https://github.com/pedropro/TACO), [Nature Conservancy catch basin page](https://www.nature.org/en-us/about-us/where-we-work/united-states/new-jersey/stories-in-new-jersey/adopt-a-catchbasin/).

## Prepare and train

On Windows PowerShell, run `./training/bootstrap_and_train.ps1`. It installs Python 3.14.10 under the ignored `.runtime/` folder, creates `.venv/`, installs project packages, CUDA-enabled PyTorch and ONNX packages, downloads only the Indonesia region plus COCO annotations from FloatWaste, converts all labeled litter boxes to one `trash` class, trains YOLO11n on the RTX GPU, then exports ONNX for browser testing. The system Python installation is not changed. Dataset files, runs, and downloaded pretrained weights remain local and are excluded from Git.

The script saves the trained weights to `models/drain_trash.pt` and produces a training validation report in `runs/detect/floatwaste-indonesia/`. Configure `.env` with:

```dotenv
MODEL_PATH=./models/drain_trash.pt
CLASS_MAP_JSON={"0":"trash"}
```

To adjust training parameters, run `training/train_floatwaste.py` with `--epochs`, `--imgsz`, `--batch`, and `--device`. Re-export after a new training run with `training/export_onnx.py`; it writes `models/drain_trash.onnx` and copies the browser deployment artifact into `vercel-test/drain_trash.onnx`. The deployed model needs to be redeployed after replacing that copy. Check validation precision/recall and inspect false positives before using alerts. Split membership is preserved from the source dataset; report the Indonesia-region nature of the training data when sharing results. The generated `training/dataset/attribution.json` keeps image provenance and the required attribution statement.

The standalone test page is deployed to Vercel at [https://drain-trash-model-test.vercel.app](https://drain-trash-model-test.vercel.app) with SSO protection. It uses ONNX Runtime Web to run inference in the user's browser. To create a preview after signing into Vercel CLI, run `vercel link --project drain-trash-model-test` from `vercel-test/`, then `vercel deploy`. See `docs/SYSTEM_MANUAL_TH.md` for browser preprocessing/postprocessing and the separation between this static test and the FastAPI capture system.

License sources differ: FloatWaste data is CC BY 4.0; Ultralytics says YOLO code and trained weights are AGPL-3.0 by default. Review its [license guidance](https://www.ultralytics.com/license) before using the model/application in a closed-source or commercial service.

## Improve for drain grates

Capture images from the actual ESP32-CAM angle with clean grates, partial/full blockages, wet leaves, plastic, night lighting, glare, rain and different water levels. Annotate trash polygons in a segmentation tool and train a segmentation model using those site images. Bounding-box area tends to overestimate covered pixels, so keep alerts in supervised trial mode until the site-specific mask model is validated against manually measured grate coverage.
