# Vercel browser model test

This is a static model playground. It downloads `drain_trash.onnx` (~10.7 MB) and runs ONNX Runtime Web/WASM inside the user's browser. Uploaded images are read by browser JavaScript and are not sent to a capture API. Runtime JavaScript and WASM are loaded from the pinned `onnxruntime-web@1.23.2` jsDelivr path.

The model is a single-class `trash` YOLO11n detector fine-tuned on the CC BY 4.0 FloatWaste Indonesia subset. It detects river litter rather than drain-grate blockages. The page uses letterbox to 640×640, RGB float32 normalization, confidence threshold 0.35, IoU NMS 0.45, and reports box-union coverage over the full image as an estimate. It does not save events or send LINE alerts.

## Update and preview deploy

After training a new `.pt` model, export and copy the browser artifact:

```powershell
& ..\.venv\Scripts\python.exe ..\training\export_onnx.py
```

Then sign in to Vercel CLI, link this folder to the protected project, and make a preview deployment:

```powershell
npm install --global vercel
vercel login
vercel link --project drain-trash-model-test
vercel deploy
```

Vercel currently deploys this folder as a separate, SSO-protected model test project. Do not include `.env`, secrets, images from a private location, or the full training dataset in the static deployment.

## Sources and license

- Dataset: [FloatWaste](https://huggingface.co/datasets/OliviaWuYJ/SC-floatwaste), DOI [10.5281/zenodo.21934286](https://doi.org/10.5281/zenodo.21934286), CC BY 4.0. Only Indonesia-region images were used and all litter categories were merged into one class. The dataset card says its final full author list/year remain pending.
- YOLO11 pretrained weights, Ultralytics framework, and derived weights: [Ultralytics license guidance](https://www.ultralytics.com/license). Ultralytics states AGPL-3.0 is the default for code and trained weights; check obligations or licensing before private/commercial use.
- ONNX Runtime Web: [official web docs](https://onnxruntime.ai/docs/tutorials/web/); JavaScript and WASM are served from a pinned jsDelivr package version.
