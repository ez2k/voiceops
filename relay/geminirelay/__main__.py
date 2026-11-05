"""python -m geminirelay — 환경변수: GEMINI_API_KEY (필수), RELAY_TOKEN (필수)"""

import argparse
import asyncio
import logging
import os
import sys
from pathlib import Path

from google import genai

from .server import PATH, make_server


def main() -> None:
    p = argparse.ArgumentParser(description="ESP32 단독형 펌웨어 ⇄ Gemini Live 중계 서버")
    p.add_argument("--host", default="127.0.0.1", help="듣는 주소 (기본: 이 서버 안에서만 — 앞에 Caddy 같은 TLS 프록시를 둔다)")
    p.add_argument("--port", type=int, default=8765)
    p.add_argument("--model", default=os.environ.get("GEMINI_MODEL", "gemini-3.8-live"), help="Gemini Live 모델")
    p.add_argument("--data-dir", type=Path, default=Path(os.environ.get("RELAY_DATA_DIR", "calls")),
                   help="접수 메모(notes.jsonl), 대화록, 통화 녹음(WAV)을 남길 곳")
    p.add_argument("--no-record", action="store_true", help="통화 녹음(WAV)을 남기지 않는다")
    args = p.parse_args()

    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(message)s")
    token = os.environ.get("RELAY_TOKEN", "")
    if len(token) < 16:
        sys.exit("RELAY_TOKEN 환경변수에 16자 이상의 토큰을 넣으세요 (기기 설정의 API 키 칸에 같은 값을 넣습니다)")
    if not os.environ.get("GEMINI_API_KEY"):
        sys.exit("GEMINI_API_KEY 환경변수가 필요합니다")

    async def run() -> None:
        async with make_server(args.host, args.port, token, genai.Client(), args.model, args.data_dir,
                               record=not args.no_record) as server:
            logging.info("듣는 중: ws://%s:%d%s (모델 %s)", args.host, args.port, PATH, args.model)
            await server.serve_forever()

    try:
        asyncio.run(run())
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
