# เตรียม Backend และ ESP32-CAM สำหรับติดตั้งจริง

## สถานะปัจจุบัน

- เว็บทดสอบโมเดลอยู่บน Vercel แล้ว แต่เป็นหน้าเว็บประมวลผล ONNX ใน browser ไม่ใช่ API รับภาพจากบอร์ด
- API รับภาพ/ประมวลผล/เก็บประวัติ/ส่ง LINE มีอยู่ใน `app/main.py` และแพ็ก Docker พร้อม HTTPS ผ่าน Caddy
- API ยังไม่ได้เผยแพร่บนโดเมนสาธารณะ จึงยังไม่มี `API_HOST` หรือ Root CA จริงสำหรับใส่เฟิร์มแวร์
- ยังไม่มี Wi-Fi หน้างาน, device token, LINE channel access token, โดเมน/เครื่อง Linux และยืนยันรุ่นบอร์ด/กล้องจริง

## โครงสร้าง deploy ที่ใช้กับโปรเจกต์นี้

ใช้ Vercel สำหรับหน้าเว็บทดสอบตามเดิม และใช้ Docker host ที่มี persistent disk สำหรับ FastAPI เพราะบริการนี้เก็บ SQLite และภาพใน `/app/data` และโหลดโมเดล PyTorch ตอนเริ่มทำงาน ส่วน Vercel Function มีข้อจำกัด request body 4.5 MB ซึ่งไม่เหมาะกับภาพ JPEG ที่บอร์ดส่ง และ filesystem ของ function ไม่ใช่ persistent volume สำหรับฐานข้อมูล/ภาพ

ใน repository มี `Dockerfile`, `docker-compose.prod.yml`, `Caddyfile` และ `.env.production.example` เตรียมไว้แล้ว ขั้นตอนเปิดบริการ:

1. เตรียม Linux VM/เครื่อง Docker ที่มี public IP และ persistent disk; จด public IP ไว้
2. สร้างโดเมนหรือ subdomain เช่น `api.<โดเมนของคุณ>` แล้วชี้ DNS A record ไป public IP ของเครื่อง
3. เปิด inbound TCP 80 และ 443 ที่ firewall/ผู้ให้บริการ VM
4. บนเครื่อง server คัดลอก `.env.production.example` เป็น `.env.production` แล้วกำหนด `PUBLIC_DOMAIN`, `ADMIN_PASSWORD`, `DEVICE_TOKENS_JSON`, และ `LINE_CHANNEL_ACCESS_TOKEN` จริง ห้ามนำค่าจริง commit
5. รันคำสั่งจาก README: `docker compose --env-file .env.production -f docker-compose.prod.yml up -d --build`
6. ตรวจ `https://<โดเมน>/api/health` ให้ตอบ `status: ok` และตรวจ log ของ container หาก model ไม่โหลด
7. ตั้ง `PUBLIC_BASE_URL=https://<โดเมน>` ใน `.env.production` หากต้องการให้ LINE ส่งภาพประกอบ แล้ว restart service
8. จึงค่อยใส่โดเมนดังกล่าวใน `API_HOST` และใช้พอร์ต 443 ในไฟล์ `secrets.h`

การสร้าง VM/โดเมน/เปิดพอร์ตและ LINE OA ยังต้องใช้บัญชีของเจ้าของระบบ; ไม่ควรเดาค่า IP, DNS, secret หรือสร้างทรัพยากรที่อาจมีค่าใช้จ่ายแทนโดยไม่มีข้อมูลบัญชี/การตั้งค่าที่ถูกต้อง

## ตั้งค่า ESP32

1. ตรวจแผงวงจรและชื่อ sensor กล้องจริงก่อนเลือก firmware: ไฟล์ `.ino` นี้ใช้ pin map ของ **AI Thinker ESP32-CAM + OV2640** โดยเฉพาะ
2. คัดลอก `firmware/esp32_cam/secrets.h.example` เป็น `firmware/esp32_cam/secrets.h` แล้วกรอก Wi-Fi, API host, token, และ Root CA; หากยังไม่มี `secrets.h` โค้ดจะ include ตัวอย่างให้คอมไพล์ได้ แต่ค่า placeholder ใช้เชื่อม API จริงไม่ได้
3. บอร์ดทำงานเป็น Wi-Fi station และเชื่อมต่อกับเราเตอร์/access point 2.4 GHz ที่มีอินเทอร์เน็ต ให้กรอกชื่อเครือข่ายใน `WIFI_SSID` และรหัสผ่านใน `WIFI_PASSWORD` ของ `secrets.h`; ไม่ต้องตั้ง ESP32 เป็น hotspot สำหรับระบบนี้
4. `DEVICE_ID` ต้องตรงกับ key ใน `config/devices.json` และ key เดียวกันใน `DEVICE_TOKENS_JSON` บน server
5. `API_HOST` ใส่เฉพาะชื่อ host ไม่ใส่ `https://`; API paths ตั้งไว้เป็น `/api/captures` และ `/api/telemetry`
6. เลือกบอร์ดที่ตรงรุ่นใน Arduino IDE และติดตั้ง Arduino-ESP32 core กับ camera component/library เวอร์ชันที่รองรับโค้ดนี้ จากนั้นเปิด Serial Monitor 115200 baud
7. ค่าเริ่มต้นปิด flow/float sensor (`ENABLE_FLOW_SENSORS 0`) เพื่อให้ถ่ายภาพตามรอบได้โดยไม่ต้องต่อเซนเซอร์น้ำ หากต่อเซนเซอร์ทั้งสองและตรวจ wiring แล้ว จึงเปลี่ยนเป็น `1`
8. ทดสอบส่งภาพ ตรวจประวัติบน dashboard และทดสอบการแจ้งเตือน ก่อนติดตั้ง ณ จุดจริง

ไฟล์ `secrets.h` ถูก exclude ใน `.gitignore`; เก็บไฟล์นี้ไว้เฉพาะเครื่องที่ใช้แฟลชบอร์ด

## รุ่นบอร์ดที่แจ้งมา

ชื่อ “ESP32-S WiFi Bluetooth” ยังบอกไม่ได้แน่ชัดว่าเป็น ESP32-CAM หรือมี camera sensor หรือไม่ ชื่อ ESP32/ESP32-S เพียงอย่างเดียวไม่ยืนยันว่ามีกล้องติดตั้งอยู่ หากเป็นบอร์ด ESP32 devkit ปกติที่ไม่มี sensor จะถ่ายภาพไม่ได้ ต้องเพิ่มกล้องและตรวจ pinout/PSRAM/รุ่นชิปก่อนเขียน pin map ใหม่ อย่าใช้ pin map AI Thinker กับบอร์ดทั่วไปโดยไม่ตรวจรุ่น

เพื่อยืนยัน ต้องใช้รูปถ่ายชัด ๆ ของด้านหน้าและด้านหลังบอร์ด (ให้เห็นข้อความบน PCB, โมดูลกล้อง/สายแพ และหมายเลขบนชิป) หรือ link หน้าสินค้า/รหัสรุ่นเต็ม หากมี OV2640 ที่ต่อแยก ให้ถ่ายรูปการต่อสายด้วย

## ค่า secret ที่สร้าง

- `DEVICE_TOKEN`: สร้าง random ยาวอย่างน้อย 32 ตัวอักษร; ใช้ค่าคู่กันใน `secrets.h` และ `DEVICE_TOKENS_JSON`
- `ADMIN_PASSWORD`: อย่างน้อย 16 ตัวอักษรสำหรับหน้า dashboard production
- `LINE_CHANNEL_ACCESS_TOKEN`: LINE Developers Console ของ Messaging API channel ที่ผูกกับ OA
- Root CA: ใช้ CA chain ที่เชื่อถือได้สำหรับ API host; ห้ามวาง private key หรือใช้ใบรับรองของโดเมนอื่น
