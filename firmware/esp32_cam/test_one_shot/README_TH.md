# ต้นแบบ ESP32-CAM ส่งภาพให้โมเดลวิเคราะห์

สเก็ตช์นี้ทำเฉพาะ `ถ่ายภาพ → ส่ง HTTPS ไป API → รับผลวิเคราะห์` ไม่มีเซนเซอร์, แฟลช, SD card, telemetry, deep sleep, คำสั่งเว็บ หรือการถ่ายซ้ำตามเวลา ใช้ภาพ VGA หนึ่ง framebuffer และลดกำลังส่ง Wi-Fi สำหรับทดสอบต้นแบบ

เมื่อเปิดเครื่องจะถ่ายและส่งหนึ่งภาพ หากต้องการถ่ายซ้ำ ให้พิมพ์ `c` ใน Serial Monitor

## เตรียมและอัปโหลด

1. ใช้ `test_one_shot.ino` กับ `secrets.h` ที่ตั้งค่า Wi-Fi 2.4 GHz, `API_HOST`, `DEVICE_ID` และ `DEVICE_TOKEN` ให้ตรงกับบริการ Render
2. เปิดไฟล์ใน Arduino IDE เลือก `AI Thinker ESP32-CAM` และพอร์ต COM
3. ปิด Serial Monitor ก่อนอัปโหลด หาก COM ถูกใช้งานอยู่; ต่อ GPIO0 ลง GND ระหว่างอัปโหลด แล้วถอดออกและกด RESET หลังอัปโหลดเสร็จ
4. เปิด Serial Monitor ที่ 115200 baud

## ผลที่ควรเห็น

- `CAMERA: ready` กล้องเริ่มทำงาน
- `WIFI CONNECTED` ต่อเราเตอร์สำเร็จ
- `API STATUS: HTTP/1.1 200 OK` และ JSON ใน `API BODY` คือ API รับภาพและวิเคราะห์แล้ว
- `401` ตรวจ `DEVICE_TOKEN`; `413` ภาพใหญ่เกิน; `502` backend ไม่ตอบ; `no response` ตรวจไฟเลี้ยง/Wi-Fi/TLS
- `Brownout detector was triggered` คือบอร์ดรีเซ็ตเพราะแรงดันตก ให้แก้แหล่งจ่าย/สายก่อน ไม่ควรปิด brownout detector

ส่ง log ตั้งแต่ `RESET REASON` ถึง `API BODY END` มาเพื่อไล่ปัญหาขั้นถัดไปได้ โดยอย่าส่งค่าจาก `secrets.h`
