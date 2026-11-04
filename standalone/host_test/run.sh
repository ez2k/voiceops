#!/bin/sh
# 단독형 펌웨어의 순수 C 모듈을 PC 에서 컴파일해 시험한다.
set -e
cd "$(dirname "$0")"
MAIN=../main
OUT=${TMPDIR:-/tmp}/callbridge_host_test
cc -std=c11 -D_DEFAULT_SOURCE -Wall -Wextra -Werror -I "$MAIN" \
   test_main.c "$MAIN/b64.c" "$MAIN/decimator.c" "$MAIN/gemini_stream.c" \
   -lm -o "$OUT"
"$OUT"

# 펌웨어가 보내는 고정 JSON 이 올바른지
"$OUT" dump | python3 -c '
import json, sys
lines = sys.stdin.read().splitlines()
audio, greeting, tools = (json.loads(l) for l in lines)
assert audio["realtimeInput"]["audio"]["mimeType"] == "audio/pcm;rate=8000"
assert audio["realtimeInput"]["audio"]["data"] == "AQIDBA=="
assert greeting["clientContent"]["turnComplete"] is True
names = [f["name"] for f in tools[0]["functionDeclarations"]]
assert names == ["end_call", "transfer_to_human", "save_note"], names
print("message JSON ok:", names)
'
