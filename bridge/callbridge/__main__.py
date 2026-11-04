"""AI CallBridge PC 브리지

    python -m callbridge --port COM5                          # PC 스피커·마이크로 통화
    python -m callbridge --port COM5 --ai gemini --auto-answer 2   # Gemini Live AI 가 응대
"""

import argparse
import os
from pathlib import Path

from .app import Bridge
from .audio import LocalAudio
from .link import Link

HELP = """\
명령:
  answer          걸려온 전화 받기
  hangup          거절 / 끊기
  dial 01012345678  전화 걸기
  dtmf 123#       통화 중 번호 누르기 (ARS)
  audio off       통화는 유지하고 소리만 폰 본체로 넘기기 (사람에게 인계)
  audio on        소리를 다시 PC 로 가져오기
  pair            새 폰 페어링 허용
  status          연결 상태 보기
  quit            종료
"""


def main() -> None:
    ap = argparse.ArgumentParser(prog="callbridge", description="AI CallBridge PC 브리지")
    ap.add_argument("--port", required=True, help="ESP32 시리얼 포트 (예: COM5, /dev/cu.usbserial-0001)")
    ap.add_argument("--ai", choices=["gemini"], help="AI 가 통화를 응대 (생략하면 PC 스피커·마이크)")
    ap.add_argument("--prompt", type=Path, default=Path("prompt.md"), help="AI 지시문 파일 (기본: prompt.md)")
    ap.add_argument("--auto-answer", type=int, default=0, metavar="N", help="벨이 N번 울리면 자동으로 받기 (기본: 끔)")
    ap.add_argument("--record-dir", type=Path, default=Path("calls"), help="통화 기록 저장 폴더 (기본: calls)")
    args = ap.parse_args()

    if args.ai == "gemini":
        if not (os.environ.get("GEMINI_API_KEY") or os.environ.get("GOOGLE_API_KEY")):
            ap.error("GEMINI_API_KEY 환경변수에 Gemini API 키를 넣어 주세요")
        from .gemini import GeminiAgent

        instructions = args.prompt.read_text(encoding="utf-8")
        notes = args.record_dir / "notes.jsonl"

        def factory(call, control):
            return GeminiAgent(call, control, instructions, notes)
    else:

        def factory(call, control):
            return LocalAudio()

    link = Link(args.port)
    bridge = Bridge(link, factory, args.record_dir, args.auto_answer)
    link.start(bridge.handle_frame)

    print(HELP)
    if args.ai:
        print(f"AI 응대: {args.ai} (지시문: {args.prompt})\n")
    link.send_command("status")
    try:
        while True:
            line = input().strip()
            if line in ("quit", "exit"):
                break
            if line:
                bridge.command(line)
    except (KeyboardInterrupt, EOFError):
        pass
    finally:
        link.close()
        bridge.close()


if __name__ == "__main__":
    main()
