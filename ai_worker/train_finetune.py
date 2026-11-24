import argparse
import os
import shutil
from pathlib import Path
import yaml

BASE_DIR = Path(__file__).resolve().parent
os.environ.setdefault("YOLO_CONFIG_DIR", str(BASE_DIR / ".ultralytics"))

from ultralytics import YOLO  # noqa: E402

MODELS_DIR = BASE_DIR / "models"
DEZAN_DIR = BASE_DIR / "datasets" / "dezan_fall" / "fall_dataset"
CUSTOM_DIR = BASE_DIR / "datasets" / "custom_room"
ALIGNED_DIR = BASE_DIR / "datasets" / "fall_aligned"
CLASS_NAMES = ["fallen", "sitting", "standing"]
# DeZan uses 0=fallen, 1=standing, 2=sitting; align to model's 0=fallen, 1=sitting, 2=standing
DEZAN_TO_MODEL_CLS = {0: 0, 1: 2, 2: 1}


def copy_split_with_remap(src_img_dir: Path, src_lbl_dir: Path, dst_img_dir: Path, dst_lbl_dir: Path, remap: dict | None, prefix: str):
    if not src_img_dir.exists():
        return 0
    count = 0
    for img_path in sorted(src_img_dir.iterdir()):
        if img_path.suffix.lower() not in {".jpg", ".jpeg", ".png", ".bmp", ".webp"}:
            continue
        lbl_path = src_lbl_dir / f"{img_path.stem}.txt"
        if not lbl_path.exists():
            continue
        lines_out = []
        for raw_line in lbl_path.read_text(encoding="utf-8", errors="ignore").splitlines():
            parts = raw_line.strip().split()
            if len(parts) < 5:
                continue
            cls_id = int(float(parts[0]))
            if remap is not None:
                cls_id = remap.get(cls_id, cls_id)
            lines_out.append(f"{cls_id} " + " ".join(parts[1:5]))
        if not lines_out:
            continue
        out_name = f"{prefix}_{img_path.name}"
        shutil.copy2(img_path, dst_img_dir / out_name)
        (dst_lbl_dir / f"{prefix}_{img_path.stem}.txt").write_text("\n".join(lines_out) + "\n", encoding="utf-8")
        count += 1
    return count


def prepare_aligned_dataset() -> Path:
    if ALIGNED_DIR.exists():
        shutil.rmtree(ALIGNED_DIR)
    for split in ("train", "val"):
        (ALIGNED_DIR / "images" / split).mkdir(parents=True, exist_ok=True)
        (ALIGNED_DIR / "labels" / split).mkdir(parents=True, exist_ok=True)

    tr_n = copy_split_with_remap(
        DEZAN_DIR / "images" / "train",
        DEZAN_DIR / "labels" / "train",
        ALIGNED_DIR / "images" / "train",
        ALIGNED_DIR / "labels" / "train",
        DEZAN_TO_MODEL_CLS,
        "dezan",
    )
    va_n = copy_split_with_remap(
        DEZAN_DIR / "images" / "val",
        DEZAN_DIR / "labels" / "val",
        ALIGNED_DIR / "images" / "val",
        ALIGNED_DIR / "labels" / "val",
        DEZAN_TO_MODEL_CLS,
        "dezan",
    )

    c_tr = copy_split_with_remap(
        CUSTOM_DIR / "images" / "train",
        CUSTOM_DIR / "labels" / "train",
        ALIGNED_DIR / "images" / "train",
        ALIGNED_DIR / "labels" / "train",
        None,
        "room",
    )
    c_va = copy_split_with_remap(
        CUSTOM_DIR / "images" / "val",
        CUSTOM_DIR / "labels" / "val",
        ALIGNED_DIR / "images" / "val",
        ALIGNED_DIR / "labels" / "val",
        None,
        "room",
    )

    yaml_path = ALIGNED_DIR / "data.yaml"
    yaml_cfg = {
        "path": str(ALIGNED_DIR.resolve()),
        "train": "images/train",
        "val": "images/val",
        "nc": len(CLASS_NAMES),
        "names": CLASS_NAMES,
    }
    yaml_path.write_text(yaml.safe_dump(yaml_cfg, sort_keys=False), encoding="utf-8")
    print(f"Prepared aligned dataset: train={tr_n + c_tr} (custom={c_tr}), val={va_n + c_va} (custom={c_va})")
    return yaml_path


def main():
    parser = argparse.ArgumentParser(description="Fine-tune YOLO fall detection model on GPU")
    parser.add_argument("--base-weights", default=str(MODELS_DIR / "pretrained_hf.pt"))
    parser.add_argument("--epochs", type=int, default=45)
    parser.add_argument("--batch", type=int, default=16)
    parser.add_argument("--imgsz", type=int, default=640)
    args = parser.parse_args()

    data_yaml = prepare_aligned_dataset()
    base_weights = Path(args.base_weights)
    if not base_weights.exists():
        raise FileNotFoundError(f"Base weights not found: {base_weights}")

    print(f"Evaluating baseline weights: {base_weights}")
    baseline_model = YOLO(str(base_weights))
    base_metrics = baseline_model.val(data=str(data_yaml), imgsz=args.imgsz, device=0, workers=0, verbose=False)
    base_fallen_ap50 = float(base_metrics.box.ap50[0]) if len(base_metrics.box.ap50) else 0.0
    print(f"Baseline mAP50={base_metrics.box.map50:.4f}, mAP50-95={base_metrics.box.map:.4f}, fallen_AP50={base_fallen_ap50:.4f}")

    runs_dir = BASE_DIR / "runs"
    print(f"Starting GPU fine-tuning for {args.epochs} epochs (workers=0)...")
    model = YOLO(str(base_weights))
    results = model.train(
        data=str(data_yaml),
        epochs=args.epochs,
        patience=15,
        imgsz=args.imgsz,
        batch=args.batch,
        device=0,
        workers=0,
        project=str(runs_dir),
        name="fall_finetune",
        exist_ok=True,
        mosaic=0.8,
        degrees=10.0,
        hsv_v=0.4,
        fliplr=0.5,
        lr0=0.002,
        verbose=True,
    )

    trained_best = Path(results.save_dir) / "weights" / "best.pt"
    if not trained_best.exists():
        raise RuntimeError(f"Training completed without producing {trained_best}")

    eval_model = YOLO(str(trained_best))
    new_metrics = eval_model.val(data=str(data_yaml), imgsz=args.imgsz, device=0, workers=0, verbose=False)
    new_fallen_ap50 = float(new_metrics.box.ap50[0]) if len(new_metrics.box.ap50) else 0.0
    print(f"Fine-tuned mAP50={new_metrics.box.map50:.4f}, mAP50-95={new_metrics.box.map:.4f}, fallen_AP50={new_fallen_ap50:.4f}")

    target_best = MODELS_DIR / "best.pt"
    shutil.copy2(trained_best, target_best)
    print(f"Updated active model -> {target_best}")


if __name__ == "__main__":
    main()
