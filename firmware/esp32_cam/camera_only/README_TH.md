# เฟิร์มแวร์กล้องอย่างเดียว

Sketch นี้ทำงานตามลำดับ: ESP32-CAM ถ่าย JPEG → ส่ง HTTPS ไป Render → backend วิเคราะห์ขยะและส่ง LINE เมื่อเข้าเงื่อนไข

ไม่มีโค้ดอ่านหรือส่งค่าจากเซนเซอร์ระดับน้ำ/อัตราการไหล และไม่ต้องใช้ SD card

## ตั้งค่าและแฟลช

1. เปิด `camera_only.ino` ใน Arduino IDE โดยเปิดจากโฟลเดอร์นี้
2. คัดลอก `secrets.h.example` เป็น `secrets.h` แล้วใส่ชื่อ/รหัส Wi-Fi 2.4 GHz และ device token ให้ตรงกับ `DEVICE_TOKENS_JSON` ใน Render
3. เลือกบอร์ด `AI Thinker ESP32-CAM` และเปิด PSRAM หากมีตัวเลือก
4. ต่อ USB-to-Serial: 5V ไป 5V, GND ไป GND, TX ไป U0R, RX ไป U0T; ดึง GPIO0 ลง GND ระหว่างอัปโหลด
5. หลังอัปโหลดเสร็จ ถอด GPIO0 จาก GND แล้วกด RESET
6. เปิด Serial Monitor ที่ 115200 baud; ควรเห็น Wi-Fi connected และ `API response: HTTP/1.1 200 OK`

ค่าการถ่ายเริ่มต้นทุก 5 นาที ปรับได้ที่ `CAPTURE_INTERVAL_MS` ในไฟล์ `.ino` ค่าความไว/ROI/เกณฑ์แจ้งเตือน LINE ปรับที่ backend ไม่ใช่บนกล้อง

## แยกความรับผิดชอบ

- กล้องส่งเฉพาะ `device_id`, `captured_at`, `device_token` และ JPEG
- การตรวจภาพ การบันทึกเหตุการณ์ และ LINE OA ทำที่ backend
- เก็บ `secrets.h` เป็นไฟล์ local เท่านั้น ห้าม commit หรือแชร์ภาพหน้าจอที่เห็นค่าในไฟล์นี้
