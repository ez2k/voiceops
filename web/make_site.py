"""웹 설치 페이지(site/)를 만든다. CI(web-flasher.yml)가 펌웨어 빌드 뒤에 부른다.

  python3 web/make_site.py <ESP-IDF 빌드 폴더> <출력 폴더> <빌드 번호> <빌드 시각>

- 부트로더·파티션 표·앱을 따로 굽도록 매니페스트를 만든다 (주소는 빌드가 만든 flash_args 그대로).
  하나로 합친 이미지를 0번지부터 구우면 그 사이의 NVS(설정·페어링)까지 0xFF 로 덮여 지워진다.
- index.html·manifest.json 의 빌드 번호 자리를 채운다.
"""
import json
import re
import shutil
import sys
from pathlib import Path

WEB = Path(__file__).resolve().parent


def main(build_dir: Path, out: Path, build: str, built: str) -> None:
    out.mkdir(parents=True, exist_ok=True)

    parts = []
    for line in (build_dir / "flash_args").read_text().splitlines():
        m = re.fullmatch(r"(0x[0-9a-fA-F]+)\s+(\S+)", line.strip())
        if m:
            src = build_dir / m.group(2)
            shutil.copy(src, out / src.name)
            parts.append({"path": src.name, "offset": int(m.group(1), 16)})
    parts.sort(key=lambda p: p["offset"])
    if len(parts) != 3:
        sys.exit(f"flash_args 에서 부트로더·파티션 표·앱 3개를 기대했는데 {len(parts)}개: {parts}")

    manifest = json.loads((WEB / "manifest.json").read_text(encoding="utf-8"))
    manifest["version"] = build
    manifest["builds"][0]["parts"] = parts
    (out / "manifest.json").write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")

    page = (WEB / "index.html").read_text(encoding="utf-8")
    page = page.replace("__BUILD__", build).replace("__BUILT__", built)
    if "__BUILD__" in page or "__BUILT__" in page:
        sys.exit("index.html 에 빌드 번호 자리가 남았습니다")
    (out / "index.html").write_text(page, encoding="utf-8")

    for p in parts:
        print(f"{p['offset']:#08x}  {p['path']}  ({(out / p['path']).stat().st_size} bytes)")


if __name__ == "__main__":
    if len(sys.argv) != 5:
        sys.exit(__doc__)
    main(Path(sys.argv[1]), Path(sys.argv[2]), sys.argv[3], sys.argv[4])
