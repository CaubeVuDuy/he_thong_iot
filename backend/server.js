// server.js - Back-end Node.js cho Hệ thống IoT Viện Dưỡng Lão (LAN + Gmail 2FA + RBAC)
const express = require('express');
const net = require('net');
const http = require('http');
const fs = require('fs');
const crypto = require('crypto');
const mqtt = require('mqtt');
const sqlite3 = require('sqlite3').verbose();
const nodemailer = require('nodemailer');
const cors = require('cors');
const path = require('path');

const app = express();
const HTTP_PORT = process.env.PORT || 3000;
const MQTT_PORT = process.env.MQTT_PORT || 1883;

const CONFIG_PATH = process.env.AUTH_CONFIG_PATH || path.join(__dirname, 'auth_config.json');
const CAPTURES_DIR = process.env.CAPTURES_DIR || path.join(__dirname, 'captures');
fs.mkdirSync(CAPTURES_DIR, { recursive: true });

app.use(express.json());
app.use(cors());
app.use(express.static(path.join(__dirname, '../frontend')));

// ================= 1. KHỞI TẠO DATABASE SQLITE =================
const dbPath = process.env.DB_PATH || path.join(__dirname, 'database.sqlite');
const db = new sqlite3.Database(dbPath, (err) => {
    if (err) console.error('❌ Lỗi kết nối SQLite:', err.message);
    else console.log('✅ Đã kết nối cơ sở dữ liệu SQLite thành công!');
});

db.serialize(() => {
    db.run(`CREATE TABLE IF NOT EXISTS vitals_history (
        id INTEGER PRIMARY KEY AUTOINCREMENT,
        timestamp DATETIME DEFAULT CURRENT_TIMESTAMP,
        bpm INTEGER,
        spo2 INTEGER,
        source TEXT DEFAULT 'WIRED_SENSOR'
    )`);

    db.run(`CREATE TABLE IF NOT EXISTS fall_events (
        id INTEGER PRIMARY KEY AUTOINCREMENT,
        timestamp DATETIME DEFAULT CURRENT_TIMESTAMP,
        impact_g REAL,
        tilt_angle REAL,
        ai_score REAL,
        source TEXT DEFAULT 'WIRELESS_SENSOR',
        photo_path TEXT
    )`);

    // Bổ sung cột photo_path nếu dùng lại file database.sqlite cũ
    db.all(`PRAGMA table_info(fall_events)`, [], (err, cols) => {
        if (!err && Array.isArray(cols) && !cols.some(c => c.name === 'photo_path')) {
            db.run(`ALTER TABLE fall_events ADD COLUMN photo_path TEXT`);
        }
    });

    db.run(`CREATE TABLE IF NOT EXISTS login_history (
        id INTEGER PRIMARY KEY AUTOINCREMENT,
        timestamp TEXT NOT NULL,
        account_number TEXT,
        password_masked TEXT DEFAULT '********',
        gmail TEXT,
        role TEXT,
        status TEXT NOT NULL,
        detail TEXT,
        ip_address TEXT
    )`);

    db.run(`CREATE TABLE IF NOT EXISTS node_errors (
        id INTEGER PRIMARY KEY AUTOINCREMENT,
        timestamp TEXT NOT NULL,
        node_id TEXT NOT NULL,
        event_type TEXT NOT NULL,
        severity TEXT NOT NULL,
        detail TEXT
    )`);

    db.run(`CREATE TABLE IF NOT EXISTS user_sessions (
        token TEXT PRIMARY KEY,
        account_number TEXT NOT NULL,
        role TEXT NOT NULL,
        display_name TEXT,
        gmail TEXT,
        created_at TEXT NOT NULL,
        expires_at INTEGER NOT NULL
    )`);

    db.run(`UPDATE user_sessions SET role = 'DOCTOR' WHERE role = 'NURSE'`);
    db.run(`UPDATE login_history SET role = 'DOCTOR' WHERE role = 'NURSE'`);
});

// ================= 2. HÀM TIỆN ÍCH & CẤU HÌNH =================
function formatLocalDateTime(d = new Date()) {
    const p = (n) => String(n).padStart(2, '0');
    return `${d.getFullYear()}-${p(d.getMonth() + 1)}-${p(d.getDate())} ${p(d.getHours())}:${p(d.getMinutes())}:${p(d.getSeconds())}`;
}

function getCutoffDateStr(daysAgo) {
    return formatLocalDateTime(new Date(Date.now() - daysAgo * 86400000));
}

function getClientIp(req) {
    const cfIp = req.headers['cf-connecting-ip'];
    const fwd = req.headers['x-forwarded-for'];
    const realIp = req.headers['x-real-ip'];
    const raw = (cfIp || (Array.isArray(fwd) ? fwd[0] : fwd) || realIp || req.socket.remoteAddress || '').split(',')[0].trim();
    const cleaned = raw.replace(/^::ffff:/i, '');
    if (cleaned === '::1') return '127.0.0.1';
    return cleaned || '127.0.0.1';
}

function loadAuthConfig() {
    const raw = fs.readFileSync(CONFIG_PATH, 'utf8');
    return JSON.parse(raw);
}

function isPlaceholder(val) {
    return !val || typeof val !== 'string' || val.trim() === '' || val.includes('DIEN_');
}

function pruneOldTechLogs() {
    const cutoff30d = getCutoffDateStr(30);
    db.run(`DELETE FROM login_history WHERE timestamp < ?`, [cutoff30d]);
    db.run(`DELETE FROM node_errors WHERE timestamp < ?`, [cutoff30d]);
}

const errorCooldownMap = new Map();
function recordNodeError(nodeId, eventType, severity, detail, minCooldownMs = 15000) {
    const key = `${nodeId}:${eventType}:${detail}`;
    const now = Date.now();
    const last = errorCooldownMap.get(key) || 0;
    if (now - last < minCooldownMs) return;
    errorCooldownMap.set(key, now);

    const ts = formatLocalDateTime(new Date(now));
    db.run(
        `INSERT INTO node_errors (timestamp, node_id, event_type, severity, detail) VALUES (?, ?, ?, ?, ?)`,
        [ts, nodeId, eventType, severity, detail]
    );
    console.warn(`⚠️ [${severity}] [${nodeId}] ${eventType}: ${detail}`);
}

const authFailures = new Map();
function recordLoginAttempt({ accountNumber, gmail, role, status, detail, ip }) {
    const ts = formatLocalDateTime(new Date());
    db.run(
        `INSERT INTO login_history (timestamp, account_number, password_masked, gmail, role, status, detail, ip_address) VALUES (?, ?, ?, ?, ?, ?, ?, ?)`,
        [ts, accountNumber || '-', '********', gmail || '-', role || '-', status, detail || '', ip || '-']
    );

    const key = `${ip}:${accountNumber || 'unknown'}`;
    if (status === 'SUCCESS') {
        authFailures.delete(key);
        return;
    }

    const now = Date.now();
    const rec = authFailures.get(key);
    if (!rec || now - rec.firstAt > 5 * 60000) {
        authFailures.set(key, { count: 1, firstAt: now });
    } else {
        rec.count += 1;
        if (rec.count >= 3) {
            recordNodeError(
                'WEB_AUTH',
                'AUTH_BRUTE_FORCE',
                'CRITICAL',
                `Phát hiện ${rec.count} lần thử đăng nhập thất bại liên tiếp từ IP ${ip} (TK: ${accountNumber || '-'}, lý do: ${status})`,
                30000
            );
        }
    }
}

// ================= 3. TRẠNG THÁI HỆ THỐNG & SỨC KHỎE 3 NODE =================
let latestVitals = { bpm: null, spo2: null, updated_at: null };
let currentFallStatus = 'SAFE';
let buzzerState = 'OFF';
let lastFallEvent = null;
let latestBelt = null;
let lastValidImpactG = null;
let lastValidTilt = null;
let beltNoiseStreak = 0;
let cameraIp = null;
const CAMERA_PORT_SUFFIX = process.env.CAMERA_PORT ? `:${process.env.CAMERA_PORT}` : '';
let cameraAutoLive = false;
let lastFallPhotoUrl = null;
let lastManualPhotoUrl = null;
let mqttClient = null;

// Cấu hình AI Worker Python (YOLOv8/11 chạy trên GPU laptop khi có té ngã)
const YOLO_WORKER_URL = process.env.YOLO_WORKER_URL || 'http://127.0.0.1:8001';
const YOLO_INFER_COOLDOWN_MS = 90;
let yoloInferInFlight = false;
let lastYoloInferAt = 0;
let activeFallEventId = null;
let activeFallPhotoFilename = null;
let activeFallBestConf = 0;
let lastAnnotatedSaveAt = 0;
let latestYoloState = {
    active: false,
    workerOnline: false,
    fallDetected: false,
    inferenceMs: null,
    imageWidth: 0,
    imageHeight: 0,
    detections: [],
    updatedAt: null,
    error: null
};

function resetYoloState() {
    activeFallEventId = null;
    activeFallPhotoFilename = null;
    activeFallBestConf = 0;
    lastAnnotatedSaveAt = 0;
    latestYoloState = {
        active: false,
        workerOnline: latestYoloState.workerOnline,
        fallDetected: false,
        inferenceMs: null,
        imageWidth: 0,
        imageHeight: 0,
        detections: [],
        updatedAt: null,
        error: null
    };
}

function triggerYoloDetect(jpegBuf, force = false, targetFilename = null, targetFallRowId = null) {
    if (!jpegBuf || jpegBuf.length < 100) return;
    if (!cameraAutoLive && currentFallStatus !== 'FALL_DETECTED') return;
    if (!force && yoloInferInFlight) return;
    const now = Date.now();
    if (!force && (now - lastYoloInferAt < YOLO_INFER_COOLDOWN_MS)) return;

    yoloInferInFlight = true;
    lastYoloInferAt = now;

    const targetUrl = new URL('/detect?conf=0.35&annotate=true', YOLO_WORKER_URL);
    const reqYolo = http.request(
        targetUrl,
        {
            method: 'POST',
            agent: false,
            timeout: 2500,
            headers: {
                'Content-Type': 'application/octet-stream',
                'Content-Length': jpegBuf.length
            }
        },
        (resYolo) => {
            const chunks = [];
            resYolo.on('data', (c) => chunks.push(c));
            resYolo.on('end', () => {
                yoloInferInFlight = false;
                if (!cameraAutoLive && currentFallStatus !== 'FALL_DETECTED') {
                    resetYoloState();
                    return;
                }
                if (resYolo.statusCode !== 200) {
                    latestYoloState = {
                        ...latestYoloState,
                        active: Boolean(cameraAutoLive),
                        workerOnline: false,
                        error: `YOLO Worker HTTP ${resYolo.statusCode}`
                    };
                    return;
                }
                try {
                    const parsed = JSON.parse(Buffer.concat(chunks).toString('utf8'));
                    const dets = Array.isArray(parsed.detections) ? parsed.detections : [];
                    latestYoloState = {
                        active: Boolean(cameraAutoLive),
                        workerOnline: true,
                        fallDetected: Boolean(parsed.fall_detected),
                        inferenceMs: parsed.inference_ms ?? null,
                        imageWidth: parsed.image_width || 0,
                        imageHeight: parsed.image_height || 0,
                        detections: dets,
                        updatedAt: Date.now(),
                        error: null
                    };

                    if (parsed.fall_detected && parsed.annotated_jpeg_b64) {
                        const fallDets = dets.filter((d) => d && d.is_fall);
                        const maxFallConf = fallDets.reduce((acc, d) => Math.max(acc, Number(d.confidence) || 0), 0);
                        const nowMs = Date.now();
                        const shouldSaveAnnotated = Boolean(
                            targetFilename ||
                            activeFallBestConf === 0 ||
                            (maxFallConf > activeFallBestConf + 0.02 && nowMs - lastAnnotatedSaveAt > 800)
                        );
                        if (shouldSaveAnnotated) {
                            const fileToWrite = targetFilename || activeFallPhotoFilename || `fall_${nowMs}.jpg`;
                            const rowIdToUpdate = targetFallRowId || activeFallEventId;
                            const annotatedBuf = Buffer.from(parsed.annotated_jpeg_b64, 'base64');
                            if (annotatedBuf.length >= 100) {
                                activeFallPhotoFilename = fileToWrite;
                                activeFallBestConf = Math.max(activeFallBestConf, maxFallConf);
                                lastAnnotatedSaveAt = nowMs;
                                fs.writeFile(path.join(CAPTURES_DIR, fileToWrite), annotatedBuf, (wErr) => {
                                    if (!wErr) {
                                        const photoUrl = `/api/camera/captures/${fileToWrite}`;
                                        lastFallPhotoUrl = photoUrl;
                                        if (lastFallEvent) lastFallEvent.photo_url = photoUrl;
                                        if (rowIdToUpdate) {
                                            db.run(`UPDATE fall_events SET photo_path = ? WHERE id = ?`, [fileToWrite, rowIdToUpdate]);
                                        }
                                    }
                                });
                            }
                        }
                    }
                } catch (e) {
                    latestYoloState = {
                        ...latestYoloState,
                        active: Boolean(cameraAutoLive),
                        workerOnline: false,
                        error: 'Lỗi phân tích JSON từ YOLO Worker'
                    };
                }
            });
        }
    );

    reqYolo.on('timeout', () => {
        reqYolo.destroy();
    });

    reqYolo.on('error', (err) => {
        yoloInferInFlight = false;
        if (!cameraAutoLive && currentFallStatus !== 'FALL_DETECTED') return;
        latestYoloState = {
            ...latestYoloState,
            active: Boolean(cameraAutoLive),
            workerOnline: false,
            detections: [],
            error: `Chưa kết nối YOLO Worker (${err.message})`
        };
    });

    reqYolo.write(jpegBuf);
    reqYolo.end();
}

// Quản lý 1 luồng upstream duy nhất từ ESP32-CAM để tránh kẹt socket đơn luồng
const streamClients = new Set();
let upstreamCamReq = null;
let latestStreamJpeg = null;
let streamParseBuf = Buffer.alloc(0);

function parseStreamFrames(chunk) {
    streamParseBuf = Buffer.concat([streamParseBuf, chunk]);
    if (streamParseBuf.length > 512 * 1024) {
        streamParseBuf = streamParseBuf.subarray(streamParseBuf.length - 128 * 1024);
    }
    while (true) {
        const hdrStart = streamParseBuf.indexOf('Content-Length:');
        if (hdrStart < 0) break;
        const hdrEnd = streamParseBuf.indexOf('\r\n\r\n', hdrStart);
        if (hdrEnd < 0) break;
        const hdrStr = streamParseBuf.subarray(hdrStart, hdrEnd).toString('ascii');
        const m = hdrStr.match(/Content-Length:\s*(\d+)/i);
        if (!m) {
            streamParseBuf = streamParseBuf.subarray(hdrEnd + 4);
            continue;
        }
        const len = parseInt(m[1], 10);
        const bodyStart = hdrEnd + 4;
        if (streamParseBuf.length < bodyStart + len) break;
        latestStreamJpeg = Buffer.from(streamParseBuf.subarray(bodyStart, bodyStart + len));
        streamParseBuf = streamParseBuf.subarray(bodyStart + len);
        if (cameraAutoLive) {
            triggerYoloDetect(latestStreamJpeg, false);
        }
    }
}

function stopUpstreamCamStream() {
    if (upstreamCamReq) {
        upstreamCamReq.destroy();
        upstreamCamReq = null;
    }
    latestStreamJpeg = null;
    streamParseBuf = Buffer.alloc(0);
}

function ensureUpstreamCamStream() {
    if (upstreamCamReq || !cameraIp || !nodeHealth.ESP32_CAM.online || !nodeHealth.ESP32_CAM.sensorOk) return;
    upstreamCamReq = http.get(`http://${cameraIp}${CAMERA_PORT_SUFFIX}/stream`, { agent: false }, (camRes) => {
        camRes.on('data', (chunk) => {
            parseStreamFrames(chunk);
            for (const clientRes of streamClients) {
                if (!clientRes.writableEnded) clientRes.write(chunk);
            }
        });
        camRes.on('end', () => {
            upstreamCamReq = null;
            for (const clientRes of streamClients) {
                if (!clientRes.writableEnded) clientRes.end();
            }
            streamClients.clear();
        });
    });
    upstreamCamReq.on('error', (err) => {
        upstreamCamReq = null;
        recordNodeError('ESP32_CAM', 'STREAM_ERROR', 'ERROR', `Lỗi luồng video từ ESP32-CAM (${cameraIp}): ${err.message}`, 15000);
        for (const clientRes of streamClients) {
            if (!clientRes.writableEnded) clientRes.end();
        }
        streamClients.clear();
    });
}

const pendingSteps = new Map();
const activeSessions = new Map();

const nodeHealth = {
    BELT_NODE: {
        id: 'BELT_NODE',
        name: 'Mạch không dây',
        online: false,
        sensorOk: true,
        statusText: 'Đang chờ kết nối...',
        errorCode: null,
        lastSeenAt: null,
        lastSeenMs: 0,
        clientId: null,
        offlineGraceUntil: 0
    },
    BEDSIDE_NODE: {
        id: 'BEDSIDE_NODE',
        name: 'Mạch có dây',
        online: false,
        sensorOk: true,
        statusText: 'Đang chờ kết nối...',
        errorCode: null,
        lastSeenAt: null,
        lastSeenMs: 0,
        clientId: null,
        offlineGraceUntil: 0
    },
    ESP32_CAM: {
        id: 'ESP32_CAM',
        name: 'Camera hiện trường',
        online: false,
        sensorOk: true,
        statusText: 'Đang chờ kết nối...',
        errorCode: null,
        lastSeenAt: null,
        lastSeenMs: 0,
        ip: null,
        offlineGraceUntil: 0
    }
};

function touchNodeOnline(nodeKey, statusText) {
    const node = nodeHealth[nodeKey];
    if (!node) return;
    node.online = true;
    node.lastSeenMs = Date.now();
    node.lastSeenAt = formatLocalDateTime(new Date(node.lastSeenMs));
    node.offlineGraceUntil = 0;
    if (statusText && node.sensorOk) {
        node.statusText = statusText;
    }
}

function saveVitals(bpm, spo2, source, measuredAt) {
    const ts = formatLocalDateTime(measuredAt);
    latestVitals = { bpm, spo2, updated_at: measuredAt.toLocaleString('vi-VN') };
    db.run(`INSERT INTO vitals_history (timestamp, bpm, spo2, source) VALUES (?, ?, ?, ?)`, [ts, bpm, spo2, source]);
}

db.get(`SELECT * FROM vitals_history ORDER BY id DESC LIMIT 1`, [], (err, row) => {
    if (!err && row) latestVitals = { bpm: row.bpm, spo2: row.spo2, updated_at: row.timestamp };
});

db.get(`SELECT * FROM fall_events ORDER BY id DESC LIMIT 1`, [], (err, row) => {
    if (!err && row && row.photo_path) {
        const fullPath = path.join(CAPTURES_DIR, path.basename(row.photo_path));
        if (fs.existsSync(fullPath)) {
            lastFallPhotoUrl = `/api/camera/captures/${path.basename(row.photo_path)}`;
        }
    }
});

function saveJpegBuffer(buf, prefix, fallRowId, resolve) {
    if (!buf || buf.length < 100) {
        recordNodeError('ESP32_CAM', 'CAPTURE_EMPTY', 'ERROR', 'Ảnh nhận từ ESP32-CAM bị rỗng hoặc hỏng dữ liệu JPEG', 10000);
        return resolve(null);
    }
    const filename = `${prefix}_${Date.now()}.jpg`;
    const filePath = path.join(CAPTURES_DIR, filename);
    fs.writeFile(filePath, buf, (writeErr) => {
        if (writeErr) {
            console.error('❌ Lỗi lưu ảnh chụp:', writeErr.message);
            return resolve(null);
        }
        const photoUrl = `/api/camera/captures/${filename}`;
        if (prefix === 'fall') {
            lastFallPhotoUrl = photoUrl;
            if (lastFallEvent) lastFallEvent.photo_url = photoUrl;
            activeFallPhotoFilename = filename;
            if (fallRowId) {
                activeFallEventId = fallRowId;
                db.run(`UPDATE fall_events SET photo_path = ? WHERE id = ?`, [filename, fallRowId]);
            }
            if (currentFallStatus === 'FALL_DETECTED') {
                triggerYoloDetect(buf, true, filename, fallRowId);
            }
        } else {
            lastManualPhotoUrl = photoUrl;
        }
        console.log(`📸 Đã chụp và lưu ảnh phòng (${buf.length} bytes): ${filename}`);
        resolve(photoUrl);
    });
}

// ================= 4. CHỤP ẢNH CAMERA TỰ ĐỘNG KHI TÉ NGÃ =================
function captureRoomPhoto(prefix = 'fall', fallRowId = null) {
    return new Promise((resolve) => {
        if (!cameraIp || !nodeHealth.ESP32_CAM.online) {
            recordNodeError('ESP32_CAM', 'CAPTURE_SKIPPED', 'WARNING', 'Không thể chụp ảnh hiện trường vì ESP32-CAM đang ngoại tuyến', 15000);
            return resolve(null);
        }
        if (!nodeHealth.ESP32_CAM.sensorOk) {
            recordNodeError('ESP32_CAM', 'CAPTURE_FAILED', 'ERROR', 'Không thể chụp ảnh vì cảm biến camera đang báo lỗi', 15000);
            return resolve(null);
        }

        // Nếu đang stream live, lấy ngay frame mới nhất từ luồng để tránh kẹt socket trên ESP32-CAM
        if (upstreamCamReq && latestStreamJpeg && latestStreamJpeg.length >= 100) {
            return saveJpegBuffer(Buffer.from(latestStreamJpeg), prefix, fallRowId, resolve);
        }

        const attemptCapture = (attempt) => {
            let settled = false;
            const retryOrFail = (code, msg) => {
                if (settled) return;
                settled = true;
                if (attempt < 1) {
                    return setTimeout(() => attemptCapture(attempt + 1), 350);
                }
                recordNodeError('ESP32_CAM', code, 'ERROR', msg, 15000);
                resolve(null);
            };

            const reqCam = http.get(`http://${cameraIp}${CAMERA_PORT_SUFFIX}/capture`, { agent: false, timeout: 4000 }, (camRes) => {
                if (camRes.statusCode !== 200) {
                    camRes.resume();
                    return retryOrFail('CAPTURE_HTTP_ERROR', `ESP32-CAM trả mã lỗi HTTP ${camRes.statusCode} khi chụp ảnh`);
                }

                const chunks = [];
                camRes.on('data', (chunk) => chunks.push(chunk));
                camRes.on('end', () => {
                    if (settled) return;
                    settled = true;
                    saveJpegBuffer(Buffer.concat(chunks), prefix, fallRowId, resolve);
                });
            });

            reqCam.on('timeout', () => {
                reqCam.destroy();
                retryOrFail('CAPTURE_TIMEOUT', `Quá thời gian kết nối khi chụp ảnh từ ${cameraIp}/capture`);
            });

            reqCam.on('error', (err) => {
                retryOrFail('CAPTURE_ERROR', `Lỗi kết nối tới Camera (${cameraIp}): ${err.message}`);
            });
        };

        attemptCapture(0);
    });
}

// ================= 5. KHỞI ĐỘNG LOCAL MQTT BROKER (AEDES) =================
function isAuthorizedMqttClient(clientId) {
    return typeof clientId === 'string' && /^(ESP32_Belt_|ESP32_Bedside_|Backend_Central_Hub_)/.test(clientId);
}

function getNodeKeyFromClientId(clientId) {
    if (!clientId) return null;
    if (clientId.startsWith('ESP32_Belt_')) return 'BELT_NODE';
    if (clientId.startsWith('ESP32_Bedside_')) return 'BEDSIDE_NODE';
    return null;
}

async function startMqttBroker() {
    const { Aedes } = await import('aedes');
    const aedes = await Aedes.createBroker();
    const mqttServer = net.createServer(aedes.handle);

    // Chặn các client lạ cố tình đẩy dữ liệu giả mạo vào MQTT
    aedes.authorizePublish = (client, packet, callback) => {
        if (client && !isAuthorizedMqttClient(client.id)) {
            recordNodeError(
                'MQTT_BROKER',
                'MQTT_UNAUTHORIZED_PUBLISH',
                'CRITICAL',
                `Chặn thiết bị lạ (${client.id}) gửi dữ liệu trái phép vào topic ${packet.topic}`,
                5000
            );
            return callback(new Error('Unauthorized MQTT client'));
        }
        callback(null);
    };

    mqttServer.listen(MQTT_PORT, '0.0.0.0', () => {
        console.log(`✅ [Tầng Transport] Local MQTT Broker (Aedes) đang lắng nghe tại port ${MQTT_PORT}`);
    });

    aedes.on('client', (client) => {
        console.log(`📡 [Broker] Client kết nối: ${client.id}`);
        if (!isAuthorizedMqttClient(client.id)) {
            recordNodeError(
                'MQTT_BROKER',
                'MQTT_INTRUSION',
                'CRITICAL',
                `Phát hiện thiết bị lạ không hợp lệ kết nối vào MQTT Broker: ${client.id}`,
                5000
            );
            return;
        }
        const nodeKey = getNodeKeyFromClientId(client.id);
        if (nodeKey) {
            nodeHealth[nodeKey].clientId = client.id;
            touchNodeOnline(nodeKey, 'Đã kết nối MQTT');
        }
    });

    aedes.on('clientDisconnect', (client) => {
        console.log(`🔌 [Broker] Client ngắt kết nối: ${client.id}`);
        const nodeKey = getNodeKeyFromClientId(client.id);
        if (nodeKey && nodeHealth[nodeKey].clientId === client.id) {
            nodeHealth[nodeKey].online = false;
            nodeHealth[nodeKey].offlineGraceUntil = Date.now() + 15000;
            nodeHealth[nodeKey].statusText = 'Mất kết nối MQTT (đang chờ tự nối lại)';
        }
    });

    mqttClient = mqtt.connect(`mqtt://127.0.0.1:${MQTT_PORT}`, {
        clientId: 'Backend_Central_Hub_' + Math.random().toString(16).substring(2, 8)
    });

    mqttClient.on('connect', () => {
        console.log('✅ Back-end kết nối thành công tới Local MQTT Broker!');
        mqttClient.subscribe('elderly/node_wireless/fall');
        mqttClient.subscribe('elderly/node_wired/vitals');
        mqttClient.subscribe('elderly/actuator/buzzer_control');
        mqttClient.subscribe('elderly/system/node_health');
    });

    mqttClient.on('message', (topic, message) => {
        try {
            const payload = JSON.parse(message.toString());

            // 1. Telemetry & Cảnh báo té ngã từ ĐAI ĐEO KHÔNG DÂY
            if (topic === 'elderly/node_wireless/fall') {
                touchNodeOnline('BELT_NODE');
                const nowStr = new Date().toLocaleTimeString('vi-VN');
                const rawImpact = Number(payload.impact_g ?? 1.0);
                const rawTilt = Number(payload.tilt_angle ?? 0);
                const isFallMsg = (payload.status === 'FALL_DETECTED' || payload.is_fallen === true);

                if (rawImpact < 0.4) {
                    beltNoiseStreak++;
                    if (beltNoiseStreak === 10) {
                        recordNodeError('BELT_NODE', 'I2C_NOISE', 'WARNING', 'Phát hiện chuỗi mẫu nhiễu I2C (< 0.4G) từ cảm biến MPU6050', 30000);
                    }
                } else {
                    beltNoiseStreak = 0;
                }

                if (isFallMsg) {
                    const wasAlreadyFallen = (currentFallStatus === 'FALL_DETECTED');
                    console.log(`🚨 [${nowStr}] [CẢNH BÁO TÉ NGÃ TỪ ĐAI LƯNG]:`, payload);
                    currentFallStatus = 'FALL_DETECTED';
                    buzzerState = 'ON';

                    latestBelt = {
                        status: 'FALL_DETECTED',
                        state_desc: 'TE NGA NGUY HIEM',
                        impact_g: rawImpact >= 0.4 ? rawImpact : (lastValidImpactG ?? 2.6),
                        tilt_angle: rawImpact >= 0.4 ? rawTilt : (lastValidTilt ?? 60.0),
                        ai_score: payload.ai_score ?? 1.0,
                        updated_at: nowStr
                    };
                    lastFallEvent = {
                        impact_g: latestBelt.impact_g,
                        tilt_angle: latestBelt.tilt_angle,
                        ai_score: latestBelt.ai_score,
                        timestamp: nowStr,
                        photo_url: lastFallPhotoUrl
                    };

                    if (!wasAlreadyFallen) {
                        activeFallPhotoFilename = null;
                        activeFallBestConf = 0;
                        lastAnnotatedSaveAt = 0;
                        db.run(
                            `INSERT INTO fall_events (timestamp, impact_g, tilt_angle, ai_score, source) VALUES (?, ?, ?, ?, ?)`,
                            [formatLocalDateTime(new Date()), latestBelt.impact_g, latestBelt.tilt_angle, latestBelt.ai_score, 'WIRELESS_MPU6050'],
                            function (err) {
                                const newId = err ? null : this.lastID;
                                activeFallEventId = newId;
                                captureRoomPhoto('fall', newId).finally(() => {
                                    cameraAutoLive = true;
                                });
                            }
                        );
                    } else {
                        cameraAutoLive = true;
                    }

                    mqttClient.publish('elderly/actuator/buzzer_control', JSON.stringify({
                        command: 'BUZZER_ON',
                        reason: 'FALL_ALERT_TRIGGER'
                    }));
                } else if (payload.status === 'SAFE') {
                    if (currentFallStatus === 'FALL_DETECTED') {
                        if (rawImpact >= 0.4 && rawImpact <= 8.0) {
                            lastValidImpactG = rawImpact;
                            lastValidTilt = rawTilt;
                        }
                        if (latestBelt) {
                            latestBelt.status = 'FALL_DETECTED';
                            latestBelt.state_desc = 'TE NGA NGUY HIEM';
                            latestBelt.updated_at = nowStr;
                        }
                        return;
                    }

                    const reportedDesc = String(payload.state_desc || '').trim().toUpperCase();
                    let resolvedState = (reportedDesc === 'DANG DI BO' || reportedDesc === 'DUNG / NGOI YEN')
                        ? reportedDesc
                        : (latestBelt ? latestBelt.state_desc : 'DUNG / NGOI YEN');
                    if (resolvedState === 'TE NGA NGUY HIEM') resolvedState = 'DUNG / NGOI YEN';

                    if (rawImpact >= 0.4 && rawImpact <= 8.0) {
                        if (reportedDesc !== 'DANG DI BO' && reportedDesc !== 'DUNG / NGOI YEN') {
                            if (lastValidImpactG !== null && lastValidTilt !== null) {
                                const deltaG = Math.abs(rawImpact - lastValidImpactG);
                                const deltaTilt = Math.abs(rawTilt - lastValidTilt);

                                if (resolvedState === 'DANG DI BO') {
                                    const stillNow = (deltaG < 0.032) && (deltaTilt < 2.3) && (rawImpact >= 0.89) && (rawImpact <= 1.23);
                                    resolvedState = stillNow ? 'DUNG / NGOI YEN' : 'DANG DI BO';
                                } else {
                                    const startMoving = (deltaG >= 0.038) || (deltaTilt >= 2.6) || (rawImpact < 0.88) || (rawImpact > 1.24);
                                    resolvedState = startMoving ? 'DANG DI BO' : 'DUNG / NGOI YEN';
                                }
                            } else {
                                resolvedState = 'DUNG / NGOI YEN';
                            }
                        }
                        lastValidImpactG = rawImpact;
                        lastValidTilt = rawTilt;
                    }

                    currentFallStatus = 'SAFE';
                    latestBelt = {
                        status: 'SAFE',
                        state_desc: resolvedState,
                        impact_g: lastValidImpactG ?? 1.0,
                        tilt_angle: lastValidTilt ?? 0.0,
                        ai_score: payload.ai_score ?? 0,
                        updated_at: nowStr
                    };
                }
            }
            // 2. Chỉ số sức khỏe từ TRẠM ĐẦU GIƯỜNG
            else if (topic === 'elderly/node_wired/vitals') {
                touchNodeOnline('BEDSIDE_NODE', 'Thiết bị đo sẵn sàng');
                console.log('❤️ [DỮ LIỆU SỨC KHỎE TỪ TRẠM GIƯỜNG]:', payload);
                if (payload.bpm && payload.bpm > 0) {
                    const ageMs = Number(payload.age_ms) || 0;
                    saveVitals(payload.bpm, payload.spo2, 'WIRED_MAX30102', new Date(Date.now() - ageMs));
                }
            }
            // 3. Báo cáo trạng thái & lỗi phần cứng từ các Node
            else if (topic === 'elderly/system/node_health') {
                const nodeKey = payload.node;
                if (nodeHealth[nodeKey]) {
                    const status = String(payload.status || 'OK').toUpperCase();
                    const detail = String(payload.detail || '');
                    const errorCode = String(payload.error_code || 'SENSOR_ERROR');
                    nodeHealth[nodeKey].online = true;
                    nodeHealth[nodeKey].lastSeenMs = Date.now();
                    nodeHealth[nodeKey].lastSeenAt = formatLocalDateTime(new Date());
                    nodeHealth[nodeKey].offlineGraceUntil = 0;

                    if (status === 'ERROR' || status === 'WARNING') {
                        nodeHealth[nodeKey].sensorOk = (status !== 'ERROR');
                        nodeHealth[nodeKey].errorCode = errorCode;
                        nodeHealth[nodeKey].statusText = detail || errorCode;
                        recordNodeError(nodeKey, errorCode, status, detail || errorCode, 20000);
                    } else {
                        nodeHealth[nodeKey].sensorOk = true;
                        nodeHealth[nodeKey].errorCode = null;
                        nodeHealth[nodeKey].statusText = detail || 'Hoạt động bình thường';
                    }
                }
            }
            // 4. Theo dõi trạng thái còi
            else if (topic === 'elderly/actuator/buzzer_control') {
                if (payload.command === 'BUZZER_OFF') buzzerState = 'OFF';
                else if (payload.command === 'BUZZER_ON') buzzerState = 'ON';
            }
        } catch (err) {
            recordNodeError('MQTT_BROKER', 'MALFORMED_JSON', 'WARNING', `Gói tin MQTT không hợp lệ trên topic ${topic}: ${err.message}`, 10000);
        }
    });
}

// Bộ giám sát Watchdog kiểm tra mất kết nối / quá hạn tín hiệu các Node
setInterval(() => {
    const now = Date.now();
    Object.values(nodeHealth).forEach((node) => {
        if (node.offlineGraceUntil > 0 && now >= node.offlineGraceUntil && !node.online) {
            node.offlineGraceUntil = 0;
            node.statusText = 'Mất kết nối mạng / Sập nguồn';
            recordNodeError(node.id, 'CONNECTION_LOST', 'ERROR', `${node.name} bị ngắt kết nối và không tự phục hồi sau 15 giây`, 30000);
        } else if (node.online && node.lastSeenMs > 0) {
            const timeoutMs = (node.id === 'ESP32_CAM') ? 35000 : 25000;
            if (now - node.lastSeenMs > timeoutMs) {
                node.online = false;
                node.statusText = `Mất tín hiệu quá ${Math.round(timeoutMs / 1000)}s`;
                recordNodeError(node.id, 'NODE_TIMEOUT', 'ERROR', `${node.name} mất tín hiệu quá ${Math.round(timeoutMs / 1000)} giây`, 30000);
                if (node.id === 'ESP32_CAM') {
                    for (const clientRes of streamClients) {
                        if (!clientRes.writableEnded) clientRes.end();
                    }
                    streamClients.clear();
                    stopUpstreamCamStream();
                }
            }
        }
    });

    // Dọn các phiên bước đăng nhập hết hạn
    for (const [k, v] of pendingSteps.entries()) {
        if (now > v.expiresAt) pendingSteps.delete(k);
    }
}, 5000);

// ================= 6. HỆ THỐNG XÁC THỰC 3 BƯỚC + GMAIL OTP + PHÂN QUYỀN =================
function getSessionFromToken(token, cb) {
    if (!token) return cb(null);
    const cached = activeSessions.get(token);
    if (cached) {
        if (Date.now() > cached.expiresAt) {
            activeSessions.delete(token);
            db.run(`DELETE FROM user_sessions WHERE token = ?`, [token]);
            return cb(null);
        }
        return cb(cached);
    }
    db.get(`SELECT * FROM user_sessions WHERE token = ?`, [token], (err, row) => {
        if (err || !row || Date.now() > row.expires_at) {
            if (row) db.run(`DELETE FROM user_sessions WHERE token = ?`, [token]);
            return cb(null);
        }
        const sess = {
            token: row.token,
            accountNumber: row.account_number,
            role: row.role,
            displayName: row.display_name,
            gmail: row.gmail,
            expiresAt: row.expires_at
        };
        activeSessions.set(token, sess);
        cb(sess);
    });
}

function extractToken(req) {
    const authHeader = req.headers.authorization || '';
    if (authHeader.startsWith('Bearer ')) return authHeader.slice(7).trim();
    if (typeof req.query.token === 'string' && req.query.token.trim()) return req.query.token.trim();
    return null;
}

function requireAuth(allowedRoles = null) {
    return (req, res, next) => {
        const token = extractToken(req);
        getSessionFromToken(token, (sess) => {
            if (!sess) {
                return res.status(401).json({ error: 'Chưa đăng nhập hoặc phiên đã hết hạn.' });
            }
            if (Array.isArray(allowedRoles) && !allowedRoles.includes(sess.role)) {
                recordNodeError(
                    'WEB_AUTH',
                    'API_FORBIDDEN_ACCESS',
                    'CRITICAL',
                    `Tài khoản ${sess.accountNumber} (${sess.role}) cố truy cập trái phép API ${req.path} từ IP ${getClientIp(req)}`,
                    10000
                );
                return res.status(403).json({ error: 'Bạn không có quyền truy cập chức năng này.' });
            }
            req.user = sess;
            next();
        });
    };
}

// Bước 1: Kiểm tra Số tài khoản + Mật khẩu
app.post('/api/auth/step1', (req, res) => {
    const accountNumber = String(req.body.accountNumber || '').trim();
    const password = String(req.body.password || '');
    const ip = getClientIp(req);

    let cfg;
    try {
        cfg = loadAuthConfig();
    } catch (e) {
        return res.status(500).json({ error: 'Không đọc được file cấu hình backend/auth_config.json.' });
    }

    const account = (cfg.accounts || []).find(
        (a) => String(a.accountNumber).trim() === accountNumber && a.password === password
    );

    if (!account) {
        const existingAcc = (cfg.accounts || []).find((a) => String(a.accountNumber).trim() === accountNumber);
        recordLoginAttempt({
            accountNumber,
            gmail: existingAcc ? existingAcc.gmail : '-',
            role: existingAcc ? existingAcc.role : 'UNKNOWN',
            status: 'FAILED_CREDENTIALS',
            detail: 'Sai số tài khoản hoặc mật khẩu ở Bước 1',
            ip
        });
        return res.status(401).json({ error: 'Sai số tài khoản hoặc mật khẩu. Vui lòng nhập lại.' });
    }

    const stepToken = crypto.randomBytes(24).toString('hex');
    pendingSteps.set(stepToken, {
        stage: 'NEED_GMAIL',
        accountNumber: String(account.accountNumber).trim(),
        role: account.role,
        displayName: account.displayName || account.role,
        expectedGmail: String(account.gmail || '').trim(),
        ip,
        otp: null,
        otpExpiresAt: 0,
        otpAttemptsLeft: cfg.otp?.maxAttempts || 5,
        expiresAt: Date.now() + 10 * 60000
    });

    res.json({ success: true, stepToken, nextStep: 2 });
});

// Bước 2: Đối chiếu Gmail & Gửi mã OTP 6 số thật qua SMTP Gmail
app.post('/api/auth/step2', async (req, res) => {
    const stepToken = String(req.body.stepToken || '').trim();
    const enteredGmail = String(req.body.gmail || '').trim();
    const ip = getClientIp(req);

    const step = pendingSteps.get(stepToken);
    if (!step || Date.now() > step.expiresAt) {
        return res.status(401).json({ error: 'Phiên xác thực đã hết hạn. Vui lòng đăng nhập lại từ Bước 1.' });
    }

    let cfg;
    try {
        cfg = loadAuthConfig();
    } catch (e) {
        return res.status(500).json({ error: 'Lỗi đọc file cấu hình auth_config.json.' });
    }

    if (isPlaceholder(step.expectedGmail)) {
        return res.status(500).json({
            error: `Chưa cấu hình Gmail thật cho tài khoản ${step.accountNumber} trong file backend/auth_config.json.`
        });
    }

    if (enteredGmail.toLowerCase() !== step.expectedGmail.toLowerCase()) {
        recordLoginAttempt({
            accountNumber: step.accountNumber,
            gmail: enteredGmail || '-',
            role: step.role,
            status: 'FAILED_GMAIL',
            detail: 'Nhập sai Gmail đăng ký ở Bước 2',
            ip
        });
        return res.status(400).json({ error: 'Địa chỉ Gmail không khớp với tài khoản đã đăng ký. Vui lòng nhập lại.' });
    }

    const senderEmail = String(cfg.smtp?.senderEmail || '').trim();
    const rawAppPass = String(cfg.smtp?.appPassword || '').replace(/\s+/g, '');
    if (isPlaceholder(senderEmail) || isPlaceholder(rawAppPass)) {
        return res.status(500).json({
            error: 'Chưa cấu hình Gmail gửi hoặc App Password 16 ký tự trong file backend/auth_config.json.'
        });
    }

    const otpCode = String(crypto.randomInt(100000, 1000000));
    const expiryMinutes = Number(cfg.otp?.expiryMinutes) || 3;

    try {
        const transportOpts = cfg.smtp?.host
            ? {
                host: cfg.smtp.host,
                port: Number(cfg.smtp.port) || 2525,
                secure: Boolean(cfg.smtp.secure),
                ignoreTLS: Boolean(cfg.smtp.ignoreTLS)
            }
            : {
                service: 'gmail',
                auth: { user: senderEmail, pass: rawAppPass }
            };
        const transporter = nodemailer.createTransport(transportOpts);

        await transporter.sendMail({
            from: `"Hệ Thống IoT Viện Dưỡng Lão" <${senderEmail}>`,
            to: step.expectedGmail,
            subject: '[IoT Viện Dưỡng Lão] Mã OTP xác minh đăng nhập',
            text: `Xin chào ${step.displayName} (Số tài khoản: ${step.accountNumber}),\n\nMã OTP 6 số xác nhận đăng nhập của bạn là: ${otpCode}\nMã có hiệu lực trong ${expiryMinutes} phút.\n\nNếu bạn không thực hiện đăng nhập này, vui lòng báo cho Kỹ thuật viên kiểm tra.`,
            html: `
                <div style="font-family: Arial, sans-serif; max-width: 480px; margin: 0 auto; padding: 20px; border: 1px solid #e2e8f0; border-radius: 10px;">
                    <h2 style="color: #0284c7; margin-top: 0;">Xác Minh Đăng Nhập Hệ Thống IoT</h2>
                    <p>Xin chào <strong>${step.displayName}</strong> (Tài khoản: <strong>${step.accountNumber}</strong>),</p>
                    <p>Mã OTP 6 số của bạn là:</p>
                    <div style="font-size: 32px; font-weight: bold; letter-spacing: 8px; text-align: center; background: #f1f5f9; color: #0f172a; padding: 14px; border-radius: 8px; margin: 16px 0;">
                        ${otpCode}
                    </div>
                    <p style="color: #475569; font-size: 13px;">Mã có hiệu lực trong <strong>${expiryMinutes} phút</strong>. Không chia sẻ mã này cho người khác.</p>
                </div>
            `
        });
    } catch (mailErr) {
        console.error('❌ Lỗi gửi email OTP:', mailErr.message);
        recordNodeError('SYSTEM_SMTP', 'SMTP_SEND_FAILED', 'ERROR', `Không gửi được OTP qua Gmail (${senderEmail}): ${mailErr.message}`, 10000);
        return res.status(500).json({
            error: `Không gửi được email OTP (${mailErr.message}). Kiểm tra lại kết nối 3G/Internet và App Password 16 ký tự.`
        });
    }

    step.stage = 'NEED_OTP';
    step.otp = otpCode;
    step.otpExpiresAt = Date.now() + expiryMinutes * 60000;
    step.otpAttemptsLeft = Number(cfg.otp?.maxAttempts) || 5;

    console.log(`📧 Đã gửi mã OTP 6 số tới ${step.expectedGmail} cho TK ${step.accountNumber}`);
    res.json({
        success: true,
        nextStep: 3,
        expiresInSec: expiryMinutes * 60,
        message: `Đã gửi mã OTP 6 số tới ${step.expectedGmail}. Kiểm tra Hộp thư đến (hoặc mục Spam).`
    });
});

// Bước 3: Kiểm tra mã OTP 6 số và cấp phiên đăng nhập
app.post('/api/auth/step3', (req, res) => {
    const stepToken = String(req.body.stepToken || '').trim();
    const enteredOtp = String(req.body.otp || '').trim();
    const ip = getClientIp(req);

    const step = pendingSteps.get(stepToken);
    if (!step || step.stage !== 'NEED_OTP') {
        return res.status(401).json({ error: 'Phiên xác thực không hợp lệ. Vui lòng đăng nhập lại từ Bước 1.' });
    }

    if (Date.now() > step.otpExpiresAt) {
        pendingSteps.delete(stepToken);
        recordLoginAttempt({
            accountNumber: step.accountNumber,
            gmail: step.expectedGmail,
            role: step.role,
            status: 'FAILED_OTP_EXPIRED',
            detail: 'Mã OTP đã hết hạn',
            ip
        });
        return res.status(400).json({ error: 'Mã OTP đã hết hạn. Vui lòng quay lại gửi mã mới.', expired: true });
    }

    if (enteredOtp !== step.otp) {
        step.otpAttemptsLeft -= 1;
        recordLoginAttempt({
            accountNumber: step.accountNumber,
            gmail: step.expectedGmail,
            role: step.role,
            status: 'FAILED_OTP',
            detail: `Nhập sai mã OTP (còn ${Math.max(step.otpAttemptsLeft, 0)} lần thử)`,
            ip
        });

        if (step.otpAttemptsLeft <= 0) {
            pendingSteps.delete(stepToken);
            return res.status(429).json({
                error: 'Nhập sai mã OTP quá số lần quy định. Phiên đăng nhập đã bị hủy.',
                resetRequired: true
            });
        }
        return res.status(400).json({ error: `Mã OTP 6 số không đúng (còn ${step.otpAttemptsLeft} lần thử).` });
    }

    pendingSteps.delete(stepToken);
    const sessionToken = crypto.randomBytes(32).toString('hex');
    const createdAt = formatLocalDateTime(new Date());
    const expiresAt = Date.now() + 8 * 3600000;

    const sessionObj = {
        token: sessionToken,
        accountNumber: step.accountNumber,
        role: step.role,
        displayName: step.displayName,
        gmail: step.expectedGmail,
        expiresAt
    };
    activeSessions.set(sessionToken, sessionObj);
    db.run(
        `INSERT INTO user_sessions (token, account_number, role, display_name, gmail, created_at, expires_at) VALUES (?, ?, ?, ?, ?, ?, ?)`,
        [sessionToken, step.accountNumber, step.role, step.displayName, step.expectedGmail, createdAt, expiresAt]
    );

    recordLoginAttempt({
        accountNumber: step.accountNumber,
        gmail: step.expectedGmail,
        role: step.role,
        status: 'SUCCESS',
        detail: 'Xác thực 2 lớp Gmail OTP thành công',
        ip
    });

    res.json({
        success: true,
        token: sessionToken,
        user: {
            accountNumber: step.accountNumber,
            role: step.role,
            displayName: step.displayName,
            gmail: step.expectedGmail
        }
    });
});

app.get('/api/auth/me', requireAuth(), (req, res) => {
    res.json({
        user: {
            accountNumber: req.user.accountNumber,
            role: req.user.role,
            displayName: req.user.displayName,
            gmail: req.user.gmail
        }
    });
});

app.post('/api/auth/logout', requireAuth(), (req, res) => {
    activeSessions.delete(req.user.token);
    db.run(`DELETE FROM user_sessions WHERE token = ?`, [req.user.token]);
    res.json({ success: true });
});

// ================= 7. API PHẦN CỨNG CAMERA & BÁC SĨ (DOCTOR) =================
// ESP32-CAM tự gọi mỗi 10s để đăng ký IP & báo cáo lỗi khởi tạo cảm biến ảnh
app.get('/api/camera/register', (req, res) => {
    const ip = String(req.query.ip || '').trim();
    const okParam = req.query.ok;
    const errParam = String(req.query.err || '').trim();
    const rawSensor = String(req.query.sensor || '').trim().toUpperCase();
    const sensorName = /^[A-Z0-9_-]{2,16}$/.test(rawSensor) && rawSensor !== 'ERR' && rawSensor !== 'UNKNOWN' ? rawSensor : '';

    if (/^\d{1,3}(\.\d{1,3}){3}$/.test(ip)) {
        if (cameraIp !== ip) {
            console.log(`📹 [ESP32-CAM] Đã nhận IP Camera: http://${ip}`);
        }
        cameraIp = ip;
        nodeHealth.ESP32_CAM.ip = ip;
        nodeHealth.ESP32_CAM.name = 'Camera hiện trường';
        nodeHealth.ESP32_CAM.online = true;
        nodeHealth.ESP32_CAM.lastSeenMs = Date.now();
        nodeHealth.ESP32_CAM.lastSeenAt = formatLocalDateTime(new Date());
        nodeHealth.ESP32_CAM.offlineGraceUntil = 0;

        if (okParam === '0') {
            nodeHealth.ESP32_CAM.sensorOk = false;
            nodeHealth.ESP32_CAM.errorCode = errParam || 'CAMERA_INIT_FAIL';
            nodeHealth.ESP32_CAM.statusText = `Lỗi khởi tạo cảm biến camera (${nodeHealth.ESP32_CAM.errorCode})`;
            recordNodeError('ESP32_CAM', 'CAMERA_INIT_FAIL', 'ERROR', nodeHealth.ESP32_CAM.statusText, 20000);
        } else {
            nodeHealth.ESP32_CAM.sensorOk = true;
            nodeHealth.ESP32_CAM.errorCode = null;
            nodeHealth.ESP32_CAM.statusText = `Sẵn sàng (${ip})`;
        }
    }
    res.json({ success: true, camera_ip: cameraIp });
});

// Chỉ BÁC SĨ mới được xem live stream Camera (dùng 1 upstream socket chia sẻ)
app.get('/api/camera/stream', requireAuth(['DOCTOR']), (req, res) => {
    if (!cameraIp || !nodeHealth.ESP32_CAM.online) {
        return res.status(503).send('Camera ESP32-CAM chưa kết nối.');
    }
    if (!nodeHealth.ESP32_CAM.sensorOk) {
        return res.status(503).send('Cảm biến Camera đang báo lỗi phần cứng.');
    }

    res.writeHead(200, {
        'Content-Type': 'multipart/x-mixed-replace;boundary=123456789000000000000987654321',
        'Cache-Control': 'no-cache, no-store, must-revalidate',
        'Pragma': 'no-cache'
    });

    streamClients.add(res);
    ensureUpstreamCamStream();

    req.on('close', () => {
        streamClients.delete(res);
        if (streamClients.size === 0) {
            stopUpstreamCamStream();
        }
    });
});

app.post('/api/camera/capture', requireAuth(['DOCTOR']), async (req, res) => {
    const photoUrl = await captureRoomPhoto('manual', null);
    if (!photoUrl) {
        return res.status(503).json({ error: 'Không chụp được ảnh từ ESP32-CAM. Kiểm tra lại kết nối Camera.' });
    }
    res.json({ success: true, photoUrl });
});

app.get('/api/camera/captures/:filename', requireAuth(['DOCTOR']), (req, res) => {
    const filename = path.basename(req.params.filename || '');
    if (!/^[a-zA-Z0-9_.-]+\.jpg$/.test(filename)) {
        return res.status(400).json({ error: 'Tên file không hợp lệ.' });
    }
    const filePath = path.join(CAPTURES_DIR, filename);
    if (!fs.existsSync(filePath)) {
        return res.status(404).json({ error: 'Không tìm thấy ảnh chụp.' });
    }
    res.setHeader('Cache-Control', 'no-cache, no-store, must-revalidate');
    res.sendFile(filePath);
});

app.get('/api/camera/detections', requireAuth(['DOCTOR']), (req, res) => {
    if (!cameraAutoLive) {
        return res.json({
            active: false,
            workerOnline: latestYoloState.workerOnline,
            fallDetected: false,
            inferenceMs: null,
            imageWidth: 0,
            imageHeight: 0,
            detections: [],
            updatedAt: null,
            error: null
        });
    }
    res.json({
        ...latestYoloState,
        active: true
    });
});

app.get('/api/status', requireAuth(['DOCTOR']), (req, res) => {
    res.json({
        vitals: latestVitals,
        fall_status: currentFallStatus,
        buzzer_status: buzzerState,
        last_event: lastFallEvent,
        belt: latestBelt,
        belt_online: Boolean(nodeHealth.BELT_NODE.online),
        camera: {
            ip: cameraIp,
            online: nodeHealth.ESP32_CAM.online,
            sensorOk: nodeHealth.ESP32_CAM.sensorOk,
            statusText: nodeHealth.ESP32_CAM.statusText,
            autoLive: cameraAutoLive,
            lastFallPhotoUrl,
            lastManualPhotoUrl,
            yolo: {
                active: Boolean(cameraAutoLive),
                workerOnline: latestYoloState.workerOnline,
                fallDetected: latestYoloState.fallDetected,
                inferenceMs: latestYoloState.inferenceMs,
                error: latestYoloState.error
            }
        }
    });
});

app.get('/api/history', requireAuth(['DOCTOR']), (req, res) => {
    db.all(`SELECT * FROM vitals_history ORDER BY id DESC LIMIT 10`, [], (err, vitals) => {
        if (err) return res.status(500).json({ error: err.message });
        db.all(`SELECT * FROM fall_events ORDER BY id DESC LIMIT 10`, [], (err2, falls) => {
            if (err2) return res.status(500).json({ error: err2.message });
            const mappedFalls = (falls || []).map((f) => ({
                ...f,
                photo_url: f.photo_path ? `/api/camera/captures/${path.basename(f.photo_path)}` : null
            }));
            res.json({ vitals, falls: mappedFalls });
        });
    });
});

app.post('/api/actuator/buzzer', requireAuth(['DOCTOR']), (req, res) => {
    const { command } = req.body;

    if (command === 'BUZZER_OFF') {
        currentFallStatus = 'SAFE';
        buzzerState = 'OFF';
        cameraAutoLive = false;
        resetYoloState();
        if (streamClients.size === 0 && upstreamCamReq) {
            stopUpstreamCamStream();
        }
        if (latestBelt) {
            latestBelt.status = 'SAFE';
            latestBelt.state_desc = 'DUNG / NGOI YEN';
        }
        if (mqttClient) mqttClient.publish('elderly/actuator/buzzer_control', JSON.stringify({ command: 'BUZZER_OFF' }));
        console.log('🔇 Bác sĩ gửi lệnh TẮT CÒI (RESET)');
    } else if (command === 'BUZZER_ON') {
        buzzerState = 'ON';
        if (mqttClient) mqttClient.publish('elderly/actuator/buzzer_control', JSON.stringify({ command: 'BUZZER_ON' }));
        console.log('📢 Bác sĩ gửi lệnh BẬT CÒI');
    }

    res.json({ success: true, status: buzzerState, cameraAutoLive });
});

app.post('/api/simulate', requireAuth(['DOCTOR']), (req, res) => {
    const { type } = req.body;

    if (type === 'FALL') {
        currentFallStatus = 'FALL_DETECTED';
        buzzerState = 'ON';
        const impact = parseFloat((2.5 + Math.random() * 1.2).toFixed(2));
        const aiConf = parseFloat((0.85 + Math.random() * 0.12).toFixed(2));
        lastFallEvent = {
            impact_g: impact,
            ai_score: aiConf,
            timestamp: new Date().toLocaleTimeString('vi-VN'),
            photo_url: lastFallPhotoUrl
        };
        activeFallPhotoFilename = null;
        activeFallBestConf = 0;
        lastAnnotatedSaveAt = 0;
        db.run(
            `INSERT INTO fall_events (timestamp, impact_g, tilt_angle, ai_score, source) VALUES (?, ?, ?, ?, ?)`,
            [formatLocalDateTime(new Date()), impact, 65.5, aiConf, 'SIMULATED_WIRELESS'],
            function (err) {
                const newId = err ? null : this.lastID;
                activeFallEventId = newId;
                captureRoomPhoto('fall', newId).finally(() => {
                    cameraAutoLive = true;
                });
            }
        );
        if (mqttClient) mqttClient.publish('elderly/actuator/buzzer_control', JSON.stringify({ command: 'BUZZER_ON' }));
        return res.json({ success: true, message: 'Đã giả lập sự cố té ngã!' });
    } else if (type === 'VITALS') {
        const bpm = Math.floor(70 + Math.random() * 15);
        const spo2 = Math.floor(96 + Math.random() * 3);
        saveVitals(bpm, spo2, 'SIMULATED_WIRED', new Date());
        return res.json({ success: true, message: 'Đã cập nhật chỉ số đo mới!' });
    } else if (type === 'SAFE') {
        currentFallStatus = 'SAFE';
        buzzerState = 'OFF';
        cameraAutoLive = false;
        resetYoloState();
        if (latestBelt) {
            latestBelt.status = 'SAFE';
            latestBelt.state_desc = 'DUNG / NGOI YEN';
        }
        if (mqttClient) mqttClient.publish('elderly/actuator/buzzer_control', JSON.stringify({ command: 'BUZZER_OFF' }));
        return res.json({ success: true, message: 'Đã đặt lại trạng thái An Toàn!' });
    }

    res.status(400).json({ error: 'Loại giả lập không hợp lệ' });
});

// ================= 8. API CHO NGƯỜI THÂN (CHỈ LỊCH SỬ 7 NGÀY GẦN NHẤT) =================
app.get('/api/family/history', requireAuth(['FAMILY']), (req, res) => {
    const cutoff7d = getCutoffDateStr(7);
    const nowStr = formatLocalDateTime(new Date());

    db.all(
        `SELECT id, timestamp, impact_g, tilt_angle, source FROM fall_events WHERE timestamp >= ? ORDER BY id DESC`,
        [cutoff7d],
        (err, falls7d) => {
            if (err) return res.status(500).json({ error: err.message });
            db.all(
                `SELECT id, timestamp, bpm, spo2, source FROM vitals_history WHERE timestamp >= ? ORDER BY id DESC`,
                [cutoff7d],
                (err2, vitals7d) => {
                    if (err2) return res.status(500).json({ error: err2.message });
                    res.json({
                        windowStart: cutoff7d,
                        windowEnd: nowStr,
                        falls7d: falls7d || [],
                        vitals7d: vitals7d || []
                    });
                }
            );
        }
    );
});

// ================= 9. API CHO KỸ THUẬT VIÊN (30 NGÀY LOG ĐĂNG NHẬP & LỖI NODE) =================
app.get('/api/tech/dashboard', requireAuth(['TECH']), (req, res) => {
    pruneOldTechLogs();
    const cutoff30d = getCutoffDateStr(30);
    const nowStr = formatLocalDateTime(new Date());

    db.all(
        `SELECT id, timestamp, account_number, password_masked, gmail, role, status, detail, ip_address FROM login_history WHERE timestamp >= ? ORDER BY id DESC`,
        [cutoff30d],
        (err, loginHistory30d) => {
            if (err) return res.status(500).json({ error: err.message });
            db.all(
                `SELECT id, timestamp, node_id, event_type, severity, detail FROM node_errors WHERE timestamp >= ? ORDER BY id DESC`,
                [cutoff30d],
                (err2, nodeErrors30d) => {
                    if (err2) return res.status(500).json({ error: err2.message });
                    res.json({
                        windowStart: cutoff30d,
                        windowEnd: nowStr,
                        nodes: [nodeHealth.BELT_NODE, nodeHealth.BEDSIDE_NODE, nodeHealth.ESP32_CAM],
                        loginHistory30d: loginHistory30d || [],
                        nodeErrors30d: nodeErrors30d || []
                    });
                }
            );
        }
    );
});

// ================= 10. KHỞI CHẠY TỔNG HỢP =================
startMqttBroker().then(() => {
    app.listen(HTTP_PORT, () => {
        console.log(`\n=============================================================`);
        console.log(`🏥 HỆ THỐNG IoT VIỆN DƯỠNG LÃO - LAN + GMAIL 2FA + RBAC`);
        console.log(`📡 Local MQTT Broker: mqtt://0.0.0.0:${MQTT_PORT}`);
        console.log(`💻 Web Dashboard PC:  http://localhost:${HTTP_PORT}`);
        console.log(`=============================================================\n`);
    });
}).catch((err) => {
    console.error('❌ Lỗi khởi động Aedes Broker:', err);
});
