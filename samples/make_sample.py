from pathlib import Path
import zipfile
import io

from PIL import Image, ImageDraw
import cv2
import numpy as np

out_dir = Path(__file__).resolve().parent
out_dir.mkdir(exist_ok=True)

img = Image.new("RGB", (640, 480), (30, 90, 160))
d = ImageDraw.Draw(img)
d.rectangle([40, 40, 600, 440], outline=(255, 220, 80), width=4)
d.text((80, 200), "LIVP SAMPLE STILL", fill=(255, 255, 255))
buf = io.BytesIO()
img.save(buf, format="JPEG", quality=90)
jpg = buf.getvalue()

tmp = out_dir / "_tmp.mp4"
fourcc = cv2.VideoWriter_fourcc(*"mp4v")
vw = cv2.VideoWriter(str(tmp), fourcc, 15.0, (640, 480))
for i in range(45):
    frame = np.zeros((480, 640, 3), dtype=np.uint8)
    frame[:, :] = (20, min(255, 40 + i * 3), 120)
    cv2.putText(
        frame,
        f"LIVE frame {i}",
        (120, 240),
        cv2.FONT_HERSHEY_SIMPLEX,
        1.2,
        (255, 255, 255),
        2,
    )
    vw.write(frame)
vw.release()
mov_bytes = tmp.read_bytes()
tmp.unlink(missing_ok=True)

livp = out_dir / "sample.livp"
with zipfile.ZipFile(livp, "w", compression=zipfile.ZIP_DEFLATED) as z:
    z.writestr("IMG_SAMPLE.JPG", jpg)
    z.writestr("IMG_SAMPLE.MOV", mov_bytes)
print("wrote", livp, livp.stat().st_size)
