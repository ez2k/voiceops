/* Gemini Live 로 보내는 고정 JSON 조각 (host_test 에서 JSON 유효성을 검사한다) */
#pragma once

#define GEMINI_CALL_RATE 8000

/* 통화 음성 전송: PREFIX + base64(PCM) + SUFFIX. 서버가 8kHz 를 16kHz 로 바꿔 받는다. */
#define GEMINI_AUDIO_PREFIX "{\"realtimeInput\":{\"audio\":{\"mimeType\":\"audio/pcm;rate=8000\",\"data\":\""
#define GEMINI_AUDIO_SUFFIX "\"}}}"

/* 세션 준비가 끝나면 보내는 첫 인사 신호 */
#define GEMINI_GREETING_JSON \
    "{\"clientContent\":{\"turns\":[{\"role\":\"user\",\"parts\":[{\"text\":" \
    "\"(전화가 연결되었습니다. 지금 첫 인사를 하세요.)\"}]}],\"turnComplete\":true}}"

/* 속도 시험의 첫 신호: 끊김이 잘 드러나도록 AI 가 길게 말하게 한다 */
#define GEMINI_SPEED_TEST_JSON \
    "{\"clientContent\":{\"turns\":[{\"role\":\"user\",\"parts\":[{\"text\":" \
    "\"(연결 속도 시험입니다. 이번만 짧게 답하라는 규칙은 잊고, 지금부터 30초쯤 쉬지 말고 " \
    "회사와 서비스 소개를 길게 이어서 말해 주세요.)\"}]}],\"turnComplete\":true}}"

/* AI 가 쓸 수 있는 도구 (setup.tools) */
#define GEMINI_TOOLS_JSON \
    "[{\"functionDeclarations\":[" \
    "{\"name\":\"end_call\"," \
    "\"description\":\"통화를 끝낸다. 용건이 끝나 작별 인사를 마친 뒤에만 호출한다.\"," \
    "\"parameters_json_schema\":{\"type\":\"object\",\"properties\":{}}}," \
    "{\"name\":\"transfer_to_human\"," \
    "\"description\":\"사람 직원에게 통화를 넘긴다. 상대가 사람을 원하거나 지시문으로 처리할 수 없는 일일 때 호출한다. 호출하기 전에 담당자에게 연결해 드리겠다고 말한다.\"," \
    "\"parameters_json_schema\":{\"type\":\"object\",\"properties\":{\"reason\":{\"type\":\"string\",\"description\":\"넘기는 이유\"}},\"required\":[\"reason\"]}}," \
    "{\"name\":\"save_note\"," \
    "\"description\":\"사람이 이어받아야 할 용건(예약, 문의, 회신 요청 등)을 접수 메모로 저장한다.\"," \
    "\"parameters_json_schema\":{\"type\":\"object\",\"properties\":{" \
    "\"name\":{\"type\":\"string\",\"description\":\"상대 이름 (말했다면)\"}," \
    "\"callback_number\":{\"type\":\"string\",\"description\":\"회신 번호 (발신번호와 다를 때)\"}," \
    "\"request\":{\"type\":\"string\",\"description\":\"용건 요약\"}},\"required\":[\"request\"]}}" \
    "]}]"
