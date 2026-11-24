import os
import shutil
import zipfile
from pathlib import Path
import yaml
from huggingface_hub import hf_hub_download

BASE_DIR = Path(__file__).resolve().parent
MODELS_DIR = BASE_DIR / "models"
DATASETS_DIR = BASE_DIR / "datasets"
os.environ.setdefault("YOLO_CONFIG_DIR", str(BASE_DIR / ".ultralytics"))


def main():
    MODELS_DIR.mkdir(parents=True, exist_ok=True)
    DATASETS_DIR.mkdir(parents=True, exist_ok=True)

    print("1) Downloading pretrained Fall Detection model (melihuzunoglu/human-fall-detection)...")
    pt_cache = hf_hub_download(repo_id="melihuzunoglu/human-fall-detection", filename="best.pt")
    pretrained_path = MODELS_DIR / "pretrained_hf.pt"
    active_path = MODELS_DIR / "best.pt"
    shutil.copy2(pt_cache, pretrained_path)
    if not active_path.exists():
        shutil.copy2(pt_cache, active_path)

    sample_cache = hf_hub_download(repo_id="melihuzunoglu/human-fall-detection", filename="sample_image.jpg")
    shutil.copy2(sample_cache, BASE_DIR / "sample_image.jpg")
    print(f"   Saved model -> {active_path} ({active_path.stat().st_size} bytes)")

    print("2) Downloading Fall Detection dataset (DeZan/fall-detection)...")
    zip_cache = hf_hub_download(
        repo_id="DeZan/fall-detection",
        filename="fall detection dataset.zip",
        repo_type="dataset",
    )
    extract_root = DATASETS_DIR / "dezan_fall"
    if not (extract_root / "fall_dataset").exists():
        extract_root.mkdir(parents=True, exist_ok=True)
        with zipfile.ZipFile(zip_cache, "r") as zf:
            zf.extractall(extract_root)

    ds_dir = extract_root / "fall_dataset"
    if not ds_dir.exists():
        candidates = list(extract_root.rglob("images"))
        if candidates:
            ds_dir = candidates[0].parent

    label_ids = set()
    for lbl_file in (ds_dir / "labels").rglob("*.txt"):
        for line in lbl_file.read_text(encoding="utf-8", errors="ignore").splitlines():
            parts = line.strip().split()
            if parts:
                label_ids.add(int(parts[0]))

    train_imgs = list((ds_dir / "images" / "train").glob("*.*"))
    val_imgs = list((ds_dir / "images" / "val").glob("*.*"))
    print(f"   Dataset dir: {ds_dir}")
    print(f"   Train images: {len(train_imgs)} | Val images: {len(val_imgs)} | Class IDs found: {sorted(label_ids)}")

    # Map class IDs from DeZan filenames (fall* vs not fallen*)
    class_examples = {}
    for lbl_file in (ds_dir / "labels" / "val").glob("*.txt"):
        for line in lbl_file.read_text(encoding="utf-8", errors="ignore").splitlines():
            parts = line.strip().split()
            if parts:
                cid = int(parts[0])
                class_examples.setdefault(cid, []).append(lbl_file.name)
    for cid, names in sorted(class_examples.items()):
        print(f"   Class {cid} sample filenames: {names[:5]}")

    data_yaml = {
        "path": str(ds_dir.resolve()),
        "train": "images/train",
        "val": "images/val",
        "nc": max(label_ids) + 1 if label_ids else 2,
        "names": ["fall", "not_fallen"] if sorted(label_ids) == [0, 1] else [f"class_{i}" for i in range((max(label_ids) + 1) if label_ids else 2)],
    }
    yaml_path = ds_dir / "data.yaml"
    yaml_path.write_text(yaml.safe_dump(data_yaml, sort_keys=False), encoding="utf-8")
    print(f"   Generated data.yaml -> {yaml_path}")


if __name__ == "__main__":
    main()
