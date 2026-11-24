import asyncio
import base64
import os
import threading
import time
from contextlib import asynccontextmanager
from pathlib import Path

BASE_DIR = Path(__file__).resolve().parent
os.environ.setdefault("YOLO_CONFIG_DIR", str(BASE_DIR / ".ultralytics"))

import cv2
import numpy as np
import torch
import uvicorn
from fastapi import FastAPI, HTTPException, Query, Request
from ultralytics import YOLO

MODEL_PATH = Path(os.environ.get("YOLO_MODEL_PATH", str(BASE_DIR / "models" / "best.pt")))
HOST = os.environ.get("YOLO_WORKER_HOST", "127.0.0.1")
PORT = int(os.environ.get("YOLO_WORKER_PORT", "8001"))
DEFAULT_CONF = float(os.environ.get("YOLO_CONF", "0.35"))
DEFAULT_IOU = float(os.environ.get("YOLO_IOU", "0.45"))

infer_lock = threading.Lock()
state = {
    "model": None,
    "model_path": str(MODEL_PATH),
    "device": 0 if torch.cuda.is_available() else "cpu",
    "device_name": torch.cuda.get_device_name(0) if torch.cuda.is_available() else "CPU",
    "names": {},
    "loaded_at": None,
    "last_infer_ms": None,
    "total_inferences": 0,
}


def is_fall_label(label: str) -> bool:
    low = (label or "").strip().lower()
    if "not" in low and "fall" in low:
        return False
    return any(tok in low for tok in ("fallen", "fall", "lying", "laying"))


def tighten_fall_bbox(x1: float, y1: float, x2: float, y2: float, w: int, h: int) -> tuple[float, float, float, float]:
    bw = x2 - x1
    bh = y2 - y1
    if bw < 40 or bh < 40:
        return x1, y1, x2, y2

    inset_l = 0.02 * bw if x1 <= 15 else 0.065 * bw
    inset_r = 0.065 * bw if x2 >= w - 15 else 0.075 * bw
    inset_t = 0.02 * bh if y1 <= 15 else 0.07 * bh
    inset_b = 0.195 * bh if y2 >= h - 25 else 0.10 * bh

    nx1 = max(0.0, min(float(w), x1 + inset_l))
    ny1 = max(0.0, min(float(h), y1 + inset_t))
    nx2 = max(nx1 + 20.0, min(float(w), x2 - inset_r))
    ny2 = max(ny1 + 20.0, min(float(h), y2 - inset_b))
    return nx1, ny1, nx2, ny2


def dedupe_fall_detections(detections: list[dict]) -> list[dict]:
    kept_fall: list[dict] = []
    others: list[dict] = []
    for d in detections:
        if not d.get("is_fall"):
            others.append(d)
            continue
        bx1, by1, bx2, by2 = d.get("bbox", [0, 0, 0, 0])
        area_a = max(1.0, (bx2 - bx1) * (by2 - by1))
        duplicate = False
        for k in kept_fall:
            kx1, ky1, kx2, ky2 = k.get("bbox", [0, 0, 0, 0])
            area_k = max(1.0, (kx2 - kx1) * (ky2 - ky1))
            ix1, iy1 = max(bx1, kx1), max(by1, ky1)
            ix2, iy2 = min(bx2, kx2), min(by2, ky2)
            if ix2 > ix1 and iy2 > iy1:
                inter = (ix2 - ix1) * (iy2 - iy1)
                if inter / min(area_a, area_k) > 0.5:
                    duplicate = True
                    break
        if not duplicate:
            kept_fall.append(d)
    return kept_fall + others


def load_model(path: Path) -> None:
    if not path.exists():
        raise FileNotFoundError(f"Model weights not found at {path}")
    model = YOLO(str(path))
    dummy = np.zeros((480, 640, 3), dtype=np.uint8)
    model.predict(
        source=dummy,
        imgsz=640,
        conf=DEFAULT_CONF,
        iou=DEFAULT_IOU,
        device=state["device"],
        verbose=False,
    )
    with infer_lock:
        state["model"] = model
        state["model_path"] = str(path)
        state["names"] = {int(k): str(v) for k, v in model.names.items()}
        state["loaded_at"] = time.strftime("%Y-%m-%d %H:%M:%S")


def run_yolo_predict(frame: np.ndarray, conf: float, iou: float):
    with infer_lock:
        return state["model"].predict(
            source=frame,
            imgsz=640,
            conf=conf,
            iou=iou,
            device=state["device"],
            verbose=False,
        )


def annotate_fall_frame(frame: np.ndarray, detections: list[dict]) -> str | None:
    fall_dets = [d for d in detections if d.get("is_fall")]
    if not fall_dets:
        return None

    out = frame.copy()
    h, w = out.shape[:2]
    red_bgr = (68, 68, 239)

    for det in fall_dets:
        bx1, by1, bx2, by2 = det.get("bbox", [0, 0, 0, 0])
        x1 = max(0, min(w - 1, int(round(bx1))))
        y1 = max(0, min(h - 1, int(round(by1))))
        x2 = max(0, min(w - 1, int(round(bx2))))
        y2 = max(0, min(h - 1, int(round(by2))))
        if x2 <= x1 or y2 <= y1:
            continue

        cv2.rectangle(out, (x1, y1), (x2, y2), red_bgr, 3)

        label_txt = "te nga"
        font = cv2.FONT_HERSHEY_SIMPLEX
        font_scale = 0.58
        thickness = 2
        (tw, th), _ = cv2.getTextSize(label_txt, font, font_scale, thickness)
        pad_x, pad_y = 8, 6
        box_h = th + pad_y * 2
        box_w = tw + pad_x * 2
        ly1 = y1 - box_h if y1 >= box_h + 2 else y1 + 2
        ly2 = min(h - 1, ly1 + box_h)
        lx2 = min(w - 1, x1 + box_w)

        cv2.rectangle(out, (x1, ly1), (lx2, ly2), red_bgr, -1)
        cv2.putText(
            out,
            label_txt,
            (x1 + pad_x, ly2 - pad_y),
            font,
            font_scale,
            (255, 255, 255),
            thickness,
            cv2.LINE_AA,
        )

    ok, enc = cv2.imencode(".jpg", out, [int(cv2.IMWRITE_JPEG_QUALITY), 92])
    if not ok:
        return None
    return base64.b64encode(enc.tobytes()).decode("ascii")


@asynccontextmanager
async def lifespan(_app: FastAPI):
    load_model(MODEL_PATH)
    print(f"[YOLO Worker] Loaded {state['model_path']} on {state['device_name']} | classes={state['names']}", flush=True)
    yield


app = FastAPI(title="IoHT YOLO Fall Detection Worker", lifespan=lifespan)


@app.get("/health")
def health():
    return {
        "ok": state["model"] is not None,
        "model_path": state["model_path"],
        "device": state["device_name"],
        "cuda": torch.cuda.is_available(),
        "classes": state["names"],
        "loaded_at": state["loaded_at"],
        "last_infer_ms": state["last_infer_ms"],
        "total_inferences": state["total_inferences"],
    }


@app.post("/reload")
def reload_weights():
    load_model(MODEL_PATH)
    return {"ok": True, "loaded_at": state["loaded_at"], "classes": state["names"]}


@app.post("/detect")
async def detect_fall(
    request: Request,
    conf: float = Query(DEFAULT_CONF, ge=0.05, le=0.95),
    iou: float = Query(DEFAULT_IOU, ge=0.1, le=0.95),
    annotate: bool = Query(False),
):
    if state["model"] is None:
        raise HTTPException(status_code=503, detail="Model not loaded")

    raw_bytes = await request.body()
    if not raw_bytes or len(raw_bytes) < 100:
        raise HTTPException(status_code=400, detail="Empty or invalid image payload")

    arr = np.frombuffer(raw_bytes, dtype=np.uint8)
    frame = cv2.imdecode(arr, cv2.IMREAD_COLOR)
    if frame is None:
        raise HTTPException(status_code=400, detail="Cannot decode JPEG image")

    h, w = frame.shape[:2]
    t0 = time.perf_counter()

    results = await asyncio.to_thread(run_yolo_predict, frame, conf, iou)

    infer_ms = round((time.perf_counter() - t0) * 1000.0, 2)
    detections = []
    fall_detected = False

    if results and results[0].boxes is not None:
        boxes = results[0].boxes
        for box in boxes:
            cls_id = int(box.cls[0].item())
            score = float(box.conf[0].item())
            label = state["names"].get(cls_id, str(cls_id))
            x1, y1, x2, y2 = [float(v) for v in box.xyxy[0].tolist()]
            x1 = max(0.0, min(float(w), x1))
            y1 = max(0.0, min(float(h), y1))
            x2 = max(0.0, min(float(w), x2))
            y2 = max(0.0, min(float(h), y2))
            is_fall = is_fall_label(label)
            if is_fall:
                fall_detected = True
                x1, y1, x2, y2 = tighten_fall_bbox(x1, y1, x2, y2, w, h)
            detections.append(
                {
                    "class_id": cls_id,
                    "label": label,
                    "is_fall": is_fall,
                    "confidence": round(score, 4),
                    "bbox": [round(x1, 1), round(y1, 1), round(x2, 1), round(y2, 1)],
                    "bbox_norm": [
                        round(x1 / w, 5),
                        round(y1 / h, 5),
                        round(x2 / w, 5),
                        round(y2 / h, 5),
                    ],
                }
            )

    detections.sort(key=lambda d: (not d["is_fall"], -d["confidence"]))
    detections = dedupe_fall_detections(detections)
    state["last_infer_ms"] = infer_ms
    state["total_inferences"] += 1

    annotated_jpeg_b64 = None
    if annotate and fall_detected:
        annotated_jpeg_b64 = annotate_fall_frame(frame, detections)

    return {
        "ok": True,
        "inference_ms": infer_ms,
        "image_width": w,
        "image_height": h,
        "fall_detected": fall_detected,
        "detections": detections,
        "annotated_jpeg_b64": annotated_jpeg_b64,
    }


if __name__ == "__main__":
    uvicorn.run(app, host=HOST, port=PORT, log_level="warning")
