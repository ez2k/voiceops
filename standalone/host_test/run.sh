#!/bin/sh
# 단독형 펌웨어의 순수 C 모듈을 PC 에서 컴파일해 시험한다.
set -e
cd "$(dirname "$0")"
MAIN=../main
OUT=${TMPDIR:-/tmp}/callbridge_host_test
cc -std=c11 -D_DEFAULT_SOURCE -Wall -Wextra -Werror -I "$MAIN" \
   test_main.c "$MAIN/b64.c" "$MAIN/g711.c" "$MAIN/rt_stream.c" \
   -o "$OUT"
"$OUT"

# OpenAI Realtime 프로토콜 처리(realtime.c)를 가짜 WebSocket 으로 — 시스템의 cJSON 을 쓴다 (Ubuntu: libcjson-dev)
CJSON_CFLAGS=${CJSON_CFLAGS:--I/usr/include/cjson}
CJSON_LIBS=${CJSON_LIBS:--lcjson}
cc -std=gnu17 -D_GNU_SOURCE -Wall -Wextra -Werror -Wno-unused-parameter -I esp_stubs -I "$MAIN" $CJSON_CFLAGS \
   test_realtime.c "$MAIN/realtime.c" "$MAIN/rt_stream.c" "$MAIN/b64.c" "$MAIN/g711.c" "$MAIN/pcm_ring.c" \
   $CJSON_LIBS -o "$OUT-realtime"
"$OUT-realtime" > "$OUT-realtime.log" || { cat "$OUT-realtime.log"; exit 1; }
tail -n 1 "$OUT-realtime.log"

# 펌웨어가 보내는 고정 JSON 이 올바른지
"$OUT" dump | python3 -c '
import json, sys
lines = sys.stdin.read().splitlines()
audio, greeting, speed_test, create, tools = (json.loads(l) for l in lines)
assert audio == {"type": "input_audio_buffer.append", "audio": "AQIDBA=="}
for item in (greeting, speed_test):
    assert item["type"] == "conversation.item.create"
    assert item["item"]["content"][0]["type"] == "input_text"
assert create == {"type": "response.create"}
assert all(t["type"] == "function" and t["parameters"]["type"] == "object" for t in tools)
names = [t["name"] for t in tools]
assert names == ["end_call", "transfer_to_human", "save_note"], names
print("message JSON ok:", names)
'
