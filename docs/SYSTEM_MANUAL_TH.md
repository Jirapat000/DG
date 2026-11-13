# คู่มือระบบตรวจจับขยะหน้าตะแกรงท่อด้วย ESP32-CAM

คู่มือนี้อธิบายระบบที่อยู่ใน repository ปัจจุบัน รวมวิธีเตรียมข้อมูล ฝึกและประเมินโมเดล การเชื่อมกล้อง API เว็บ การแจ้งเตือน การทดสอบ และแนวทาง deploy รุ่นทดสอบ

> **สถานะโมเดล:** `models/drain_trash.pt` และ `models/drain_trash.onnx` เป็นโมเดลเริ่มต้นที่ฝึกกับภาพขยะลอยน้ำในแม่น้ำจาก FloatWaste Indonesia ไม่ใช่ภาพหน้าตะแกรงท่อ โมเดลมี class เดียว (`trash`) และ annotation เป็น bounding box พื้นที่ที่ API คำนวณจึงเป็นค่าประมาณจากกรอบ ห้ามใช้เป็นค่าระดับการอุดตันที่ผ่านการรับรองภาคสนาม

## 1. ภาพรวมสถาปัตยกรรม

```text
ESP32-CAM + float switch + YF-201C ── JPEG + sensor fields / HTTPS ──> FastAPI
                                ├── YOLO detector ──> class, confidence, box, ROI coverage estimate
                                ├── SQLite + disk ──> ภาพ, ผล, เวลา, สถานะเตือน
                                ├── Dashboard `/` ──> สถานะกล้องและประวัติ
                                └── LINE Messaging API ──> Push เมื่อขยะเกินเกณฑ์

ผู้ทดสอบ ── upload ──> `/test` ──> `/api/test-image` ──> ภาพตีกรอบและผล
                                                (เส้นทางนี้ไม่บันทึก event และไม่ส่ง LINE)
```

อุปกรณ์ทำหน้าที่ตั้งกล้อง ถ่าย JPEG และส่งข้อมูลเท่านั้น การประมวลผล AI อยู่ที่ API เป็นสถาปัตยกรรม edge capture + cloud inference ลดภาระ ESP32-CAM และเปลี่ยนรุ่นโมเดลได้โดยไม่ต้อง flash firmware ใหม่

| ส่วน | ไฟล์/ตำแหน่ง | หน้าที่ |
|---|---|---|
| API และ database | `app/main.py` | รับรูป, ตรวจรูป, inference, ROI, history, alert |
| Dashboard | `app/dashboard.html` | สถานะอุปกรณ์และภาพเหตุการณ์ |
| หน้าทดสอบ local | `app/test.html` | upload ภาพและแสดงกรอบ/คะแนน |
| กล้อง | `firmware/esp32_cam/esp32_cam.ino` | ถ่ายภาพและ POST multipart ไป API |
| ROI/จุดติดตั้ง | `config/devices.json` | device ID, ชื่อ, สถานที่, polygon ROI |
| โมเดล PyTorch | `models/drain_trash.pt` | ใช้โดย FastAPI/Ultralytics |
| โมเดล browser CPU | `models/drain_trash.onnx` | ONNX export สำหรับ inference แบบ ONNX Runtime |
| เตรียมข้อมูล | `training/prepare_floatwaste.py` | กรอง Indonesia และแปลง COCO bbox เป็น YOLO |
| ฝึก | `training/train_floatwaste.py` | fine-tune YOLO11n 1 class |
| ชุดข้อมูล | `training/dataset/` | สร้างตอนเตรียมข้อมูล ไม่ commit เข้า Git |
| น้ำหนักระหว่างฝึก/กราฟ | `runs/detect/floatwaste-indonesia/` | best/last, results.csv และ validation artifacts |
| ตัวอย่างภาพ | `runs/detect/random_validation_preview.jpg` | ภาพสุ่มที่ตีกรอบแล้ว |

### Float switch และ YF-201C

Firmware อ่านสวิตช์ลูกลอยแบบหน้าสัมผัสปิดลง GND ที่ GPIO13 (`water_level=1` หมายถึงลูกลอยทำงาน/น้ำสูง) และนับ pulse ของ YF-201C ที่ GPIO14 ช่วงละ 5 วินาที ส่ง `flow_rate` (L/min) พร้อมสถานะ `ok`, `slow` หรือ `no_flow` ไปกับ capture API ค่าเหล่านี้เก็บใน SQLite และแสดงบน Dashboard

GPIO13/14 เป็นขา SD ของ ESP32-CAM จึงใช้ได้เมื่อไม่ต่อ microSD; ตรวจสอบบอร์ดจริงก่อนต่อสาย สัญญาณเข้า ESP32 ต้องไม่เกินระดับ 3.3 V และต้องต่อกราวด์ร่วม ค่า 450 pulses/L เป็นค่าเริ่มต้น ต้องสอบเทียบ YF-201C ด้วยปริมาตรจริง ค่า `slow` เริ่มต้นคือ 1–4 pulses ต่อ 5 วินาที (ที่ 450 pulses/L เท่ากับประมาณ 0.027–0.107 L/min) ส่วนศูนย์ pulse เป็น `no_flow`; ควรปรับตามหน้างาน

การต่อสายสำหรับ firmware ปัจจุบัน:

| อุปกรณ์ | การต่อ | จุดตรวจสอบก่อนจ่ายไฟ |
|---|---|---|
| ESP32-CAM AI Thinker | 5V และ GND จากแหล่งจ่ายที่นิ่ง; GND ร่วมกับเซนเซอร์ | ใช้ไฟ 5V ที่รองรับช่วงกระแสพุ่งตอนเปิด Wi-Fi/กล้อง; ห้ามจ่ายแรงดัน 5V เข้าขา GPIO |
| ลูกลอย 52 mm แบบ dry contact | สายหนึ่งไป GPIO13, อีกสายไป GND | firmware ใช้ `INPUT_PULLUP`; ต้องอ่าน LOW เมื่อระดับน้ำยกลูกลอยขึ้น และ HIGH เมื่อปกติ ทดสอบ continuity/ทิศทางลูกลอยก่อนติดตั้ง |
| YF-201C | VCC ตามป้ายของรุ่น, GND ร่วม; pulse output ผ่าน level shifter/ตัวแบ่งแรงดัน 5V→3.3V ไป GPIO14 | ห้ามต่อสัญญาณ high 5V เข้าขา ESP32 โดยตรง; ตรวจรุ่นจริงและขา wire color จาก datasheet ของสินค้าที่ซื้อ |

แฟลช AI Thinker: เลือกบอร์ด `AI Thinker ESP32-CAM` และเปิด PSRAM; FTDI ต่อ 5V, GND, TX↔U0R และ RX↔U0T; ดึง GPIO0 ลง GND ขณะเข้าโหมดแฟลช แล้วถอด GPIO0 ออกจาก GND และรีเซ็ตเพื่อรันปกติ ตรวจชื่อ Wi-Fi 2.4 GHz, domain, device ID/token และ trusted root CA ใน firmware ก่อนใช้งานจริง

เมื่อตัวอย่าง flow ได้ศูนย์ pulse และลูกลอยอยู่สถานะปกติ Firmware จะพยายามเชื่อม Wi-Fi และส่ง sensor-only event ไป `/api/telemetry` ก่อนปิด Wi-Fi/กล้องและเข้า ESP32 deep sleep โดยตั้ง GPIO13 (EXT0, active-low) ให้ปลุกเมื่อสวิตช์ลูกลอยปิดลง GND พร้อมตั้ง timer ปลุกซ้ำทุก `CAPTURE_INTERVAL_MS` เพื่อตรวจ flow เผื่อมีน้ำไหลโดยไม่ยกลูกลอย ถ้าลูกลอยทำงานแต่ flow เป็นศูนย์ จะไม่หลับ แต่ถ่าย/ส่งภาพและ sensor data ต่อ เพื่อคงการตรวจกรณีน้ำขังหรือท่ออุดตัน การเชื่อม Wi-Fi มี timeout; ถ้าเชื่อมไม่ได้จะลองใหม่หลัง timer 60 วินาที

ลูกลอย 52 mm ให้ค่า binary สูง/ปกติ ไม่ใช่ความลึกน้ำ LINE เตือนเมื่อ coverage จากภาพเกิน `ALERT_THRESHOLD` หรือระดับน้ำสูง หรือ flow ช้า/ไม่ไหล โดยมี cooldown ต่อกล้อง; sensor-only event ไม่มีภาพแนบ สำหรับ YF-201C ให้จ่ายไฟตามป้ายรุ่นและต่อ GND ร่วมกับ ESP32 แต่ห้ามต่อ output 5V เข้าขา ESP32 โดยตรง ให้ใช้ level shifter หรือวงจรแบ่งแรงดัน 5V-to-3.3V ก่อน GPIO14 ทั้ง GPIO13/14 เป็นขา SD ของ ESP32-CAM จึงใช้ได้เมื่อไม่ต่อ microSD

ค่า 450 pulses/L เป็นเพียงค่าเริ่มต้นใน firmware; YF-series แต่ละรุ่น/ผู้ขายระบุค่าไม่ตรงกัน ให้สอบเทียบโดยจับน้ำปริมาตรที่ทราบค่าและคำนวณ `pulses_per_liter = total_pulses / measured_liters` จากนั้นแก้ `FLOW_PULSES_PER_LITER` และกำหนดเกณฑ์ slow จากการวัด flow ปกติของท่อจริงก่อนเปิดแจ้งเตือน

## 2. ขั้นตอนข้อมูลและการฝึกที่ใช้จริง

### แหล่งข้อมูลและการอนุญาตใช้

- **FloatWaste**: [dataset card](https://huggingface.co/datasets/OliviaWuYJ/SC-floatwaste), [งาน/record](https://doi.org/10.5281/zenodo.21934286), ใบอนุญาตที่ dataset ระบุคือ CC BY 4.0 ภาพชุดนี้เป็นขยะลอยน้ำ ไม่ใช่ตะแกรงท่อ มีป้าย COCO bounding box; ใช้เฉพาะ subset `region == Indonesia` เพื่อสร้าง baseline ที่เกี่ยวกับสภาพพื้นที่ใกล้เคียง
- **น้ำหนักตั้งต้น**: `yolo11n.pt` จาก Ultralytics เป็น pretrained YOLO11 detector ที่เริ่มจาก COCO weights แล้ว fine-tune ต่อกับ FloatWaste Indonesia; ไม่ได้นำภาพ COCO ไปเพิ่มในชุดฝึกที่จัดเตรียมรอบนี้ ([YOLO11 docs](https://docs.ultralytics.com/models/yolo11))
- **TACO**: [repository และ annotation](https://github.com/pedropro/TACO) เป็นข้อมูล litter ที่มี mask แต่ annotation JSON ที่ตรวจระหว่างงานนี้มี `licenses` ว่างและฟิลด์ license ของภาพเป็น null จึงไม่ได้นำรูปไปฝึกจนกว่าจะตรวจสอบสิทธิ์แยกเป็นรายภาพ
- ภาพตัวอย่างตะแกรงอุดตันที่ค้นพบใน [Nature Conservancy](https://www.nature.org/en-us/about-us/where-we-work/united-states/new-jersey/stories-in-new-jersey/adopt-a-catchbasin/) ให้เครดิตภาพ Shutterstock จึงใช้เป็นเพียงภาพอ้างอิงแนวปัญหา ไม่ได้ดาวน์โหลดหรือรวมใน training set
- แหล่งเทคนิค: [Ultralytics detect](https://docs.ultralytics.com/tasks/detect), [Ultralytics segment](https://docs.ultralytics.com/tasks/segment), [dataset formats](https://docs.ultralytics.com/datasets/detect), [train mode](https://docs.ultralytics.com/modes/train), [validation](https://docs.ultralytics.com/modes/val), [export](https://docs.ultralytics.com/modes/export)

รักษา `training/dataset/attribution.json` และไฟล์ `training/dataset/FLOATWASTE_LICENSE` ไว้เมื่อเผยแพร่สำเนาข้อมูล ถ้าแชร์น้ำหนักโมเดลต่อให้ใส่ที่มา dataset, คำอธิบายว่ากรองเฉพาะ Indonesia และรวมทุกหมวด litter เป็น class `trash` รวมถึงใบอนุญาตที่เกี่ยวข้อง

### ใบอนุญาตของ framework/model

Ultralytics ระบุว่า YOLO code และ trained/fine-tuned weights อยู่ภายใต้ **AGPL-3.0 เป็นค่าเริ่มต้น** เว้นแต่มี Enterprise License; แนวทางของผู้พัฒนาคือใช้ได้เมื่อยอมเผยแพร่ corresponding source ของงานที่อยู่ภายใต้ AGPL หรือซื้อ license ที่ครอบคลุม private/commercial use ดู [Ultralytics License](https://www.ultralytics.com/license) และ [YOLO11 model page](https://docs.ultralytics.com/models/yolo11) ก่อนนำไปใช้ในองค์กร/ระบบปิด/เชิงพาณิชย์ ประเด็นนี้แยกจาก FloatWaste CC BY 4.0 และต้องพิจารณาทั้ง framework, weights และวิธี deploy เอง

### เตรียมชุดข้อมูล

สคริปต์ดาวน์โหลดไฟล์ annotation สอง split และรูปในโฟลเดอร์ Indonesia จาก Hugging Face แล้วแปลง bbox จาก COCO `[x, y, width, height]` เป็น YOLO normalized `[class_id, center_x, center_y, width, height]` ที่ทุกพิกัดหารด้วยขนาดภาพ ทุกหมวด litter ที่มี annotation ถูก merge เป็น class เดียว `trash` เพื่อสร้าง baseline

ผลการเตรียมครั้งนี้: **862 ภาพ** (train 772, validation 90) และ **9,276 boxes** โดย split ตาม annotation เดิม ไม่สุ่มย้ายภาพระหว่าง train/validation

การรันซ้ำ (PowerShell; ต้องมี `.venv` และ network สำหรับ download):

```powershell
& .\.venv\Scripts\python.exe training\prepare_floatwaste.py
```

รูปแบบโฟลเดอร์ที่ได้:

```text
training/dataset/
  data.yaml
  attribution.json
  FLOATWASTE_LICENSE
  images/train/*
  images/val/*
  labels/train/*.txt
  labels/val/*.txt
```

แต่ละ label มีหนึ่งบรรทัดต่อวัตถุ: `0 cx cy width height` ค่าทั้งหมดอยู่ในช่วง 0–1; รูปไม่มีวัตถุมีไฟล์ label ว่างหรือไม่มี annotation ตามรูปแบบที่ Ultralytics รองรับ

### การฝึกโมเดลที่ใช้จริง

```powershell
& .\.venv\Scripts\python.exe training\train_floatwaste.py --epochs 80 --imgsz 640 --batch 4 --device 0
```

ค่าที่ใช้ใน `training/train_floatwaste.py`:

- `yolo11n.pt` เป็น pretrained detector ตั้งต้น (Ultralytics COCO weights)
- `epochs=80`, `imgsz=640`, `batch=4`, `device=0` (GPU ตัวที่ 0), `workers=2`, `patience=15`, `cache=False`
- train ด้วย Ultralytics; เลือก checkpoint ที่ validation fitness ดีที่สุดเป็น `best.pt` แล้วคัดลอกไป `models/drain_trash.pt`
- ผลและ log อยู่ `runs/detect/floatwaste-indonesia/`; ใช้ `results.csv`, PR curves, confusion matrix, val batch prediction ดูคุณภาพ ไม่ควรใช้ loss ค่าเดียวตัดสิน
- เครื่องที่สร้างโมเดลนี้เป็น NVIDIA RTX 3050 Laptop 6 GB VRAM; ได้ใช้ CUDA PyTorch 2.15.0+cu132, Python 3.14.10, Ultralytics 8.4.204

ตัวเลข validation ล่าสุด: Precision **0.72171**, Recall **0.57176**, mAP50 **0.62625**, mAP50-95 **0.36416** บน 90 ภาพ/850 instances ของ validation subset ค่านี้เป็น **box metrics ของ FloatWaste** ไม่ใช่ความแม่นยำกับตะแกรงจริง และไม่ใช่ segmentation mAP

ตั้งต้น environment อัตโนมัติบน Windows:

```powershell
Set-ExecutionPolicy -Scope Process Bypass
.\training\bootstrap_and_train.ps1
```

สคริปต์ติดตั้ง Python เฉพาะโปรเจกต์ใน `.runtime/`, สร้าง `.venv/`, ติดตั้ง dependency, ดึงชุดข้อมูล, แล้วเรียก train; อ่านสคริปต์ก่อนใช้กับเครื่องอื่น ตรวจ Python/CUDA ที่ตรงกับ GPU/driver ในเครื่องนั้น

### Export ONNX สำหรับ CPU/browser

```powershell
& .\.venv\Scripts\yolo.exe export model=models\drain_trash.pt format=onnx imgsz=640 simplify=True opset=17
```

ผล export ปัจจุบัน `models/drain_trash.onnx` ประมาณ 10.2 MB และ output tensor มี shape `(1, 5, 8400)` คือ 4 ค่ากรอบ + score ของ class เดียว บน server จริงควร benchmark ONNX runtime ตาม CPU/อุปกรณ์เป้าหมาย และเทียบผลกับ `.pt` ก่อนเปลี่ยน production weights

## 3. วิธีพัฒนาโมเดลให้เหมาะกับภาพหน้าท่อ

1. ติดตั้งกล้องในตำแหน่งจริง ล็อกมุม/โฟกัส/ความสูง แล้วเก็บภาพเป็นรอบ ครอบคลุมกลางวัน กลางคืน ไฟส่อง ฝน เงา น้ำขุ่น แสงสะท้อน ใบไม้เปียก พลาสติก/ถุง ขยะทับซ้อน ตะแกรงสะอาด และหลายระดับการอุดตัน
2. เก็บภาพจากหลายวันและหลายระดับฝน โดยแบ่ง train/validation/test ตามวันหรือจุดติดตั้ง (group split) เพื่อป้องกันภาพคล้ายกันหลุดไปอยู่ทั้ง train และ test
3. ทำ annotation สองชนิดแยกกัน: (a) object boxes/class สำหรับประเภทขยะ (b) polygon mask สำหรับพื้นที่ขยะ/ตะแกรงและ ground-truth coverage การมี box อย่างเดียวไม่เท่ากับ mask
4. เลือก class taxonomy ที่ทีมดูแลแยกได้จริง เช่น `trash`, `leaves`, `plastic` แต่มีตัวอย่างและ annotation พอในทุก class; ถ้าข้อมูลน้อย เริ่ม `trash` class เดียวแล้วเพิ่มเมื่อมีข้อมูล
5. ทำ ROI calibration ต่อ camera ด้วย polygon normalized พิกัด 0–1 ใน `config/devices.json`; ติดเครื่องหมายมุม ROI ในภาพ reference และทวนหลังกล้องขยับ
6. ฝึก candidate หลายตัวโดยคง test set ไว้ไม่แตะ: pretrained YOLO detection baseline, YOLO segmentation, ขนาด nano/small, ค่า `imgsz` 640/960 (ถ้า VRAM พอ), augmentation แสง/blur/rain ที่สมจริง
7. ปรับ confidence และ threshold จาก validation PR curve ตามต้นทุน false negative/false positive อย่าปรับ threshold จาก test set
8. ประเมิน recall ต่อ blockage จริง, precision, mAP50/mAP50-95, confusion matrix, false alarm ต่อกล้อง-วัน, latency, mask IoU/Dice, coverage MAE (เทียบเปอร์เซ็นต์ที่ผู้ติดป้ายวัด), กลางวัน/กลางคืนแยกกลุ่ม
9. shadow run: แสดงผลแต่ยังไม่ให้ทีมปฏิบัติการพึ่งการแจ้งเตือนหลายสัปดาห์ ทบทวน false alarms/missed events แล้วแก้ annotation/model/threshold ก่อนเปิดเตือนจริง

**แนวโมเดล/วิธีวัดที่เลือกได้:**

| ทางเลือก | ใช้เมื่อ | ข้อจำกัด/แนวปฏิบัติ |
|---|---|---|
| YOLO object detection (ปัจจุบัน) | นับ/ระบุตำแหน่งวัตถุเร็ว ป้ายเป็นกรอบทำง่าย | box ครอบพื้นที่ว่าง จึง coverage มักสูงเกินจริง; ใช้เป็น triage |
| YOLO segmentation | ต้องการ mask ต่อวัตถุและพื้นที่ขยะ | ต้องมี polygon masks จากกล้องจริง; วัด union of masks ∩ ROI / ROI pixels |
| Semantic segmentation | ตัดสิน pixel เป็น grate/trash/water/background; เหมาะกับพื้นที่สะสมต่อเนื่อง | ต้อง label ทุกพิกเซล/พื้นที่ และกำหนด class ของน้ำ/ช่องตะแกรง |
| Classical CV/สี/threshold | กล้องคงที่มากและขยะมีสี/พื้นผิวแยกได้ชัด เป็น baseline อธิบายง่าย | แพ้แสง ฝน เงา น้ำขุ่น เปลี่ยนฉาก; อย่าใช้เดี่ยวเมื่อสภาพแปรผัน |
| Temporal filtering | ลดแจ้งเตือนจากกรอบวัตถุชั่วคราว | ใช้ hysteresis เช่นเกินเกณฑ์หลายเฟรมในช่วงเวลา หรือยืนยัน 2 ใน 3 รอบ; ต้องเฝ้าระวังเหตุเร่งด่วน |

สูตร segmentation: `coverage = count((trash_mask AND roi_mask)) / count(roi_mask)` สำหรับ mask binary ที่ resolution เดียวกัน หากใช้ bbox ต้อง union พิกเซลภายในกรอบก่อน intersection และติดป้ายสถานะว่า **estimate** จนกว่าจะวัดเทียบ mask truth

## 4. การติดตั้งและตั้งค่า API/Web

### รันในเครื่อง (Python)

```powershell
& .\.venv\Scripts\Activate.ps1
Copy-Item .env.example .env
# แก้ .env และ config/devices.json ให้ตรงเครื่องจริง
python -m uvicorn app.main:app --host 127.0.0.1 --port 8000
```

เข้า dashboard `http://localhost:8000/`, หน้าทดสอบ `http://localhost:8000/test`, health `http://localhost:8000/api/health` การรันก่อนหน้าของ Codex ไม่ได้ผูกเป็น Windows service; ปิด terminal/รีสตาร์ตเครื่องอาจหยุด ต้องสั่งรันใหม่

### Environment variables

| ตัวแปร | ค่าเริ่มต้น | ความหมาย |
|---|---|---|
| `DATA_DIR` | `./data` | โฟลเดอร์รูปและ SQLite |
| `DEVICE_CONFIG` | `./config/devices.json` | JSON ตั้งค่ากล้องและ ROI |
| `MODEL_PATH` | `./models/drain_trash.pt` | weights ของ Ultralytics |
| `CLASS_MAP_JSON` | `{"0":"trash"}` | map model class ID ไปชื่อที่ API รองรับ |
| `CONFIDENCE_THRESHOLD` | `0.35` | confidence ต่ำสุดก่อนแสดงกรอบ |
| `ALERT_THRESHOLD` | `0.50` | เกณฑ์ coverage มากกว่า 50% |
| `ALERT_COOLDOWN_MINUTES` | `15` | หน่วง alert ซ้ำต่อ device หลังส่งสำเร็จ |
| `MAX_IMAGE_MB` | `8` | ขนาดภาพสูงสุดที่รับ |
| `MAX_STORED_CAPTURES` | `100` | จำนวนเหตุการณ์สูงสุดที่เก็บ; เกินแล้วลบรายการเก่าที่สุด |
| `CAPTURE_RETENTION_DAYS` | `1` | อายุข้อมูลสูงสุดเป็นวัน; ลบเมื่อเกินกำหนด |
| `MAX_CAPTURE_STORAGE_MB` | `100` | พื้นที่สูงสุดรวมไฟล์ภาพต้นฉบับและภาพ annotated; เกินแล้วลบรายการเก่าที่สุด |
| `DEVICE_TOKENS_JSON` | `{}` | optional device ID -> token; ไม่ตั้งหมายถึงไม่ตรวจ token |
| `LINE_CHANNEL_ACCESS_TOKEN` | ว่าง | secret token ของ LINE Messaging API; ห้ามใส่ใน Git |
| `PUBLIC_BASE_URL` | ว่าง | optional HTTPS base URL สำหรับแนบภาพ; text broadcast ไม่ต้องใช้ |

`.env` และ token จริงต้องอยู่ใน secret store/เครื่อง deploy ที่เชื่อถือ ห้าม commit หรือฝังใน firmware source ที่แชร์สาธารณะ Broadcast ส่งไปยังผู้ที่เป็นเพื่อน LINE OA ณ เวลาส่ง ไม่ต้องตั้ง recipient ID หากแนบรูป ต้องให้ URL รูปเป็น HTTPS สาธารณะเพื่อให้ LINE ดึงภาพได้

### จุดติดตั้ง/ROI

`config/devices.json` ตัวอย่าง:

```json
{
  "drain-01": {
    "name": "Drain camera 01",
    "location": "Set installation location",
    "roi": [[0.12,0.18],[0.88,0.18],[0.96,0.88],[0.06,0.88]]
  }
}
```

ROI เป็น polygon พิกัดสัดส่วนภาพ: `[0,0]` คือซ้ายบน, `[1,1]` คือขวาล่าง ต้องเรียงจุดรอบ polygon และไม่ตัดไขว้กัน ปรับตามภาพที่ถ่ายจากกล้องนั้น หน้าทดสอบ `/test` ใช้ภาพเต็มเป็น ROI เพื่อให้ทดลองได้ทันที ดังนั้นค่า coverage บนหน้านี้ไม่ใช่ coverage ของตะแกรงจริง

### HTTP API

| Method/Path | หน้าที่ |
|---|---|
| `GET /api/health` | model loaded, path, threshold, LINE configured |
| `GET /api/devices` | รายการ device/ROI |
| `POST /api/captures` | multipart `device_id`, ISO `captured_at`, `image` JPEG และ optional `device_token`; วิเคราะห์, บันทึก, อาจแจ้ง LINE |
| `POST /api/test-image` | multipart `image` JPEG/PNG/WebP; ส่ง annotation data URI, boxes, confidence, coverage estimate; ไม่บันทึก/ไม่เตือน |
| `GET /api/captures?limit=50` | history สูงสุด 200 รายการ |
| `GET /api/captures/{id}/image?annotated=true` | รูปต้นฉบับหรือรูปตีกรอบ |

database ตาราง `captures` เก็บ ID, device, captured/received time, path รูป/รูป annotated, model status, coverage, threshold, alert_sent/status, detections JSON, error ข้อความ `analyzed_bbox_estimate` ระบุชัดว่าเป็นการประมาณจากกล่อง

### Firmware ESP32-CAM

ไฟล์ `firmware/esp32_cam/esp32_cam.ino` ตั้งค่า Wi-Fi, endpoint, device ID, token, interval, root CA และ AI Thinker ESP32-CAM pin map จาก source ก่อน compile ผ่าน Arduino IDE หรือ PlatformIO โดยติดตั้ง ESP32 Arduino core และเลือกบอร์ด AI Thinker ESP32-CAM, partition/PSRAM ให้ถูกต้อง

ตรวจ `API_HOST`, `API_PORT`, `API_PATH` และ URL ให้ตรง public HTTPS endpoint; ตั้ง trusted root CA ให้ตรง chain ของ TLS certificate ห้ามใช้ insecure TLS สำหรับการใช้งานจริง วัดไฟเลี้ยงและ Wi-Fi RSSI ติดตั้งกล้องใน housing กันน้ำ และจัดแสงเพื่อไม่ให้ IR/แฟลชสะท้อนน้ำ

ESP32-FQBN/แพ็กเกจ, pin mapping และการส่ง JPEG ยึด [Espressif HTTP FAQ](https://docs.espressif.com/projects/esp-faq/en/latest/software-framework/protocols/http.html) และ [esp32-camera driver](https://github.com/espressif/esp32-camera) ก่อนติดตั้งจริงให้ตรวจเวอร์ชัน Espressif core/ไลบรารีกับ board package ณ วัน build

## 5. ทดลองและตรวจคุณภาพ

### UI test

เปิด `/test`, อัปโหลดภาพ ≤ 8 MB, ตรวจชื่อ class/คะแนน/กรอบ/ขนาดภาพ และ error หากรูปเสีย ให้เทียบกับภาพต้นฉบับ คะแนนเป็น confidence ไม่ใช่ probability ที่ calibrate แล้ว; หน้านี้ใช้ ROI เต็มภาพและไม่มีการแจ้งเตือน

### ทดสอบผ่าน API

```powershell
curl.exe -X POST http://localhost:8000/api/test-image -F "image=@C:\path\to\image.jpg"
```

สำหรับ endpoint กล้อง:

```powershell
curl.exe -X POST http://localhost:8000/api/captures `
  -F "device_id=drain-01" `
  -F "captured_at=2026-11-07T10:00:00+07:00" `
  -F "image=@C:\path\to\image.jpg;type=image/jpeg"
```

หากตั้ง `DEVICE_TOKENS_JSON` ให้เพิ่ม `-F "device_token=..."` การเรียก `/api/captures` จะเขียน history และอาจส่ง LINE จริง; เริ่มทดสอบด้วย `/api/test-image` ก่อน

### Acceptance/checklist

- [ ] `GET /api/health` มี `model_loaded: true`
- [ ] ภาพสะอาด/ไม่มีขยะถูกสุ่มตรวจหลายฉากและบันทึก false positive
- [ ] ภาพเบลอ, JPEG เสีย, ชนิดไฟล์ผิด, ไฟล์เกินขนาด ได้ error ที่เข้าใจได้
- [ ] ทดสอบกลางวัน/กลางคืน/ฝน/เงา/แสงสะท้อน และขยะหลายชิ้น/ทับซ้อน
- [ ] API แสดง class, confidence, box และ coverage พร้อมบอกว่าเป็น estimate หากเป็น detection model
- [ ] ทดสอบภาพจากกล้องจริงด้วย ROI ที่กำหนด ถูก/ผิดกรณี ≥ threshold
- [ ] endpoint test ไม่เพิ่ม row ใน DB และไม่ส่ง LINE
- [ ] ทดสอบกล้องหลุด Wi-Fi, API timeout, HTTPS cert หมดอายุ, power reset และ retry ไม่ส่งซ้ำถี่
- [ ] เปิดแจ้งเตือนเฉพาะหลังกำหนดวิธีดูแล false alarms และบันทึกผลทดลอง

## 6. LINE OA

สร้าง LINE Official Account และเปิด Messaging API channel, ออก channel access token แล้วตั้ง `LINE_CHANNEL_ACCESS_TOKEN` โค้ดใช้ broadcast จึงส่งข้อความเตือนไปยังเพื่อน OA ทั้งหมดโดยไม่ต้องมี recipient ID รูปภาพแนบได้เมื่อ `PUBLIC_BASE_URL` เป็น public HTTPS host ที่ serve capture image; หากไม่มี จะส่ง text alert อย่างเดียว ดู [LINE broadcast messages](https://developers.line.biz/en/docs/messaging-api/sending-messages/) และ [Messaging API reference](https://developers.line.biz/en/reference/messaging-api/#send-broadcast-message)

โค้ดปัจจุบัน broadcast text (และรูปเมื่อกำหนด URL ที่ใช้ได้) เมื่อ coverage มากกว่า threshold; cooldown query อิงเหตุการณ์ที่ alert_sent=1 ล่าสุดต่อ device หาก token ยังไม่ครบ status จะเป็น `not_configured` ข้อความ broadcast คิดตามโควตา/แผนข้อความ OA และส่งถึงผู้ใช้ที่เป็นเพื่อน OA; ตรวจ quota ก่อนเปิดใช้งานจริง การแจ้งเตือนนี้ยังไม่ทำ signature webhook, retry key, queue, dead-letter, หรือ rate-limit management

## 7. Deploy

### Vercel: หน้า test ที่ deploy แล้ว

หน้า browser tester แยกอยู่ใน `vercel-test/` และ deploy แล้วเป็นโปรเจกต์ `drain-trash-model-test`:

- URL หลัก: [https://drain-trash-model-test.vercel.app](https://drain-trash-model-test.vercel.app)
- Vercel เปิด SSO protection ให้โปรเจกต์นี้ ผู้ที่มีบัญชี/สิทธิ์ที่อนุญาตจึงเข้า alias หลักได้ มี share link อายุสั้นสำหรับ reviewer แทนการปิด protection
- หน้าใช้ ONNX Runtime Web 1.23.2 รัน YOLO ONNX ด้วย WASM ใน browser; ข้อมูลภาพที่ผู้ใช้เลือกไม่ถูก POST ไป server
- model artifact ขนาดประมาณ 10.7 MB ถูก serve เป็น static file ชื่อ `drain_trash.onnx`; runtime JavaScript และ WASM โหลดจาก jsDelivr CDN ต้องต่ออินเทอร์เน็ตตอนเปิดหน้า/โหลดครั้งแรก
- client ทำ letterbox เป็น 640×640, RGB float32 /255, รัน ONNX, confidence threshold 0.35, NMS IoU 0.45, คืนกรอบสู่ขนาดภาพเดิม และคำนวณ union-of-boxes coverage estimate บนภาพเต็ม
- ตัวเลข score ใน browser test เทียบกับ ONNX/PyTorch local ได้ใกล้กันใน sample ที่ทดสอบ แต่ยังต้องเทียบหลายภาพ/หลาย browser ก่อนถือว่า browser pipeline เทียบเท่ากันโดยทั่วไป

**ผลตรวจ deploy จริง:** deployment READY ที่ `drain-trash-model-test.vercel.app`; เปิดหน้าได้ HTTP 200; อัปโหลดภาพ validation `000021.jpg` ใน browser แล้วเห็น 2 กรอบ (`trash` confidence 77.2% และ 55.3%), coverage estimate 0.2%, ขนาด 3840×2160 ตัวรูปประมวลผลอยู่ใน browser การลองนี้ไม่ผ่าน FastAPI `/api/test-image` และไม่บันทึกภาพ

พัฒนา/อัปเดตหน้า static บน Vercel ด้วย CLI ได้จากโฟลเดอร์ `vercel-test`:

```powershell
npm install --global vercel
vercel login
Set-Location E:\DG\vercel-test
vercel link --project drain-trash-model-test
vercel deploy
```

คำสั่ง `vercel deploy` สร้าง preview สำหรับตรวจ; หลังเปิด URL preview ให้ตรวจ model download และทดลอง upload ใหม่ ก่อนเปลี่ยน production deployment ใด ๆ อัปเดตโมเดลโดยรัน `training/export_onnx.py` ซึ่งสร้าง `models/drain_trash.onnx` และคัดลอกไป `vercel-test/drain_trash.onnx` จากนั้น deploy โฟลเดอร์ test ใหม่

ระบบเต็ม FastAPI `/`, `/api/captures`, SQLite/history และ LINE **ยังรัน local/container แยก**; หน้า Vercel คือ standalone model playground เท่านั้น Vercel Functions มีข้อจำกัดด้าน bundle, request/response body และ filesystem ชั่วคราว จึงยังไม่ได้ย้าย capture API/database/LINE ไป Vercel ดู [FastAPI on Vercel](https://vercel.com/docs/frameworks/backend/fastapi), [Python runtime](https://vercel.com/docs/functions/runtimes/python), [Function limitations](https://vercel.com/docs/functions/limitations)

ถ้าต้องการ deploy **API ที่รับภาพจาก ESP32-CAM และส่ง LINE** ให้ใช้ container host ที่รองรับ Python/PyTorch, persistent volume หรือ object storage สำหรับรูป, database ถาวร, HTTPS และ secret env vars; ใช้ Vercel สำหรับ UI/rewrite ได้ แต่ตั้ง backend API URL แยก ห้ามถือว่า SQLite/file ใน Vercel function เป็น durable storage

### Docker สำหรับรันระบบเต็มรูปแบบ

สร้าง Dockerfile และ compose (รายละเอียดในไฟล์ข้างเคียง) แล้วใช้:

```powershell
Copy-Item .env.example .env
# ตรวจ MODEL_PATH, DATA_DIR, LINE/secret/ROI ก่อน
docker compose up --build
```

เปิด `http://localhost:8000/test`; volume เก็บภาพ/SQLite ไว้ระหว่าง container restart ตัว image ใส่ weights ไว้ตอน build จึง rebuild หลังอัปเดตโมเดล เตรียม HTTPS reverse proxy และ firewall/auth ก่อน expose ออก internet

## 8. Operational, security, retention

- `/api/test-image` เปิดได้โดยไม่ auth ใน prototype เหมาะเฉพาะ local/preview ที่จำกัดผู้ใช้; ก่อน public ให้เพิ่ม authentication, rate limit, file limits, content-type sniffing, per-IP quota, CSRF policy และ deployment protection
- `/api/captures` ไม่มี token protection จนกว่าจะตั้ง `DEVICE_TOKENS_JSON`; rotate token หาก firmware/device สูญหาย และใช้ HTTPS ทุกครั้งบน internet
- อย่าเก็บ channel token, Wi-Fi password, recipient IDs, device token ใน repo/ภาพหน้าจอ/URL; เพิ่ม secrets ผ่าน environment ของโฮสต์
- ไฟล์ภาพอาจมีข้อมูลสถานที่/เวลา; กำหนด retention, consent, backup encryption และ deletion process ตามนโยบายหน่วยงานก่อน production
- ระบบลบ capture เก่าตอนเริ่ม backend, ทุกชั่วโมง และหลังรับภาพใหม่ โดยคุมจำนวน อายุ และขนาดไฟล์ภาพตาม `MAX_STORED_CAPTURES`, `CAPTURE_RETENTION_DAYS`, `MAX_CAPTURE_STORAGE_MB`; ค่าเริ่มต้น 100 รายการ, 1 วัน, 100 MB การลบแถวใน SQLite คืนพื้นที่หน้า database ให้ใช้ซ้ำ แต่ขนาดไฟล์ SQLite บนดิสก์อาจไม่ย่อลงทันที
- Render Free ใช้ filesystem ชั่วคราวและไม่มี persistent disk; ข้อมูลที่ยังอยู่ในช่วง retention ก็อาจหายเมื่อ service restart/redeploy จึงไม่ใช้แทน backup
- ความล้มเหลว inference ถูกเก็บ error class ใน capture; เพิ่ม structured logging/monitoring และ alarm เมื่อ camera stale หรือ disk/DB เต็ม
- สถานะ online ปัจจุบันอิงการรับภาพ ไม่ใช่ heartbeat แยก; production ควรบันทึก `last_seen`, uptime, firmware version, RSSI, camera health และ capture latency

## 9. แหล่งอ้างอิง

1. [FloatWaste dataset card and CC BY 4.0 metadata](https://huggingface.co/datasets/OliviaWuYJ/SC-floatwaste)
2. [FloatWaste research dataset record](https://doi.org/10.5281/zenodo.21934286)
3. [TACO official repository](https://github.com/pedropro/TACO)
4. [Ultralytics object detection](https://docs.ultralytics.com/tasks/detect)
5. [Ultralytics instance segmentation](https://docs.ultralytics.com/tasks/segment)
6. [Ultralytics datasets](https://docs.ultralytics.com/datasets/detect)
7. [Ultralytics training](https://docs.ultralytics.com/modes/train), [validation](https://docs.ultralytics.com/modes/val), [export](https://docs.ultralytics.com/modes/export)
8. [Espressif HTTP/HTTPS FAQ](https://docs.espressif.com/projects/esp-faq/en/latest/software-framework/protocols/http.html)
9. [Espressif esp32-camera driver](https://github.com/espressif/esp32-camera)
10. [LINE Messaging API: send messages](https://developers.line.biz/en/docs/messaging-api/sending-messages/)
11. [LINE Messaging API image message reference](https://developers.line.biz/en/reference/messaging-api/#image-message)
12. [Vercel FastAPI framework](https://vercel.com/docs/frameworks/backend/fastapi), [Python runtime](https://vercel.com/docs/functions/runtimes/python), [Function limitations](https://vercel.com/docs/functions/limitations)
13. [Vercel CLI deploy](https://vercel.com/docs/cli/deploy), [Docker Compose GPU support](https://docs.docker.com/compose/how-tos/gpu-support/)
14. [Ultralytics YOLO11 model/pretraining](https://docs.ultralytics.com/models/yolo11), [Ultralytics licensing](https://www.ultralytics.com/license)
15. [ONNX Runtime Web overview](https://onnxruntime.ai/docs/tutorials/web/), [deploying ONNX Runtime Web](https://onnxruntime.ai/docs/tutorials/web/deploy.html)
