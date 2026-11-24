import argparse
import time
import urllib.request
from pathlib import Path

BASE_DIR = Path(__file__).resolve().parent
CUSTOM_DIR = BASE_DIR / "datasets" / "custom_room"


def main():
    parser = argparse.ArgumentParser(description="Capture real room images from ESP32-CAM for YOLO fine-tuning")
    parser.add_argument("--cam-ip", required=True, help="LAN IP of ESP32-CAM, e.g. 172.20.10.3")
    parser.add_argument("--split", choices=["train", "val"], default="train")
    parser.add_argument("--count", type=int, default=20, help="Number of snapshots to capture")
    parser.add_argument("--interval", type=float, default=1.0, help="Seconds between captures")
    args = parser.parse_args()

    img_dir = CUSTOM_DIR / "images" / args.split
    lbl_dir = CUSTOM_DIR / "labels" / args.split
    img_dir.mkdir(parents=True, exist_ok=True)
    lbl_dir.mkdir(parents=True, exist_ok=True)

    url = f"http://{args.cam_ip}/capture"
    print(f"Capturing {args.count} images from {url} into {img_dir}")
    print("Label captured images with classes: 0=fallen, 1=sitting, 2=standing, then run train_finetune.py")

    saved = 0
    for i in range(args.count):
        try:
            with urllib.request.urlopen(url, timeout=5) as resp:
                data = resp.read()
            if len(data) < 100:
                print(f"[{i+1}/{args.count}] Skipped empty frame")
            else:
                out_file = img_dir / f"esp32_{int(time.time() * 1000)}.jpg"
                out_file.write_bytes(data)
                saved += 1
                print(f"[{i+1}/{args.count}] Saved {out_file.name} ({len(data)} bytes)")
        except Exception as exc:
            print(f"[{i+1}/{args.count}] Capture error: {exc}")
        if i < args.count - 1:
            time.sleep(args.interval)

    print(f"Done. Saved {saved} images.")


if __name__ == "__main__":
    main()
