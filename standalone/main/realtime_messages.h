/* OpenAI Realtime 으로 보내는 고정 JSON 조각 (host_test 에서 JSON 유효성을 검사한다) */
#pragma once

#define RT_CALL_RATE   8000   /* 통화 음성 (블루투스 HFP CVSD) */
#define RT_AUDIO_TYPE  "audio/pcmu"   /* 주고받는 음성: G.711 μ-law 8kHz — 전화망과 같은 형식, 1초 8KB */
#define RT_VOICE       "marin"

/* 통화 음성 전송: PREFIX + base64(μ-law) + SUFFIX */
#define RT_AUDIO_PREFIX "{\"type\":\"input_audio_buffer.append\",\"audio\":\""
#define RT_AUDIO_SUFFIX "\"}"

/* 대화에 사용자 메시지를 넣고(첫 신호), 대답을 요청한다 */
#define RT_GREETING_JSON \
    "{\"type\":\"conversation.item.create\",\"item\":{\"type\":\"message\",\"role\":\"user\",\"content\":[" \
    "{\"type\":\"input_text\",\"text\":\"(전화가 연결되었습니다. 지금 첫 인사를 하세요.)\"}]}}"

/* 속도 시험의 첫 신호: 끊김이 잘 드러나도록 AI 가 길게 말하게 한다 */
#define RT_SPEED_TEST_JSON \
    "{\"type\":\"conversation.item.create\",\"item\":{\"type\":\"message\",\"role\":\"user\",\"content\":[" \
    "{\"type\":\"input_text\",\"text\":\"(연결 속도 시험입니다. 이번만 짧게 답하라는 규칙은 잊고, 지금부터 30초쯤 " \
    "쉬지 말고 회사와 서비스 소개를 길게 이어서 말해 주세요.)\"}]}}"

#define RT_RESPONSE_CREATE_JSON "{\"type\":\"response.create\"}"

/* AI 가 쓸 수 있는 도구 (session.tools) */
#define RT_TOOLS_JSON \
    "[" \
    "{\"type\":\"function\",\"name\":\"end_call\"," \
    "\"description\":\"통화를 끝낸다. 용건이 끝나 작별 인사를 마친 뒤에만 호출한다.\"," \
    "\"parameters\":{\"type\":\"object\",\"properties\":{}}}," \
    "{\"type\":\"function\",\"name\":\"transfer_to_human\"," \
    "\"description\":\"사람 직원에게 통화를 넘긴다. 상대가 사람을 원하거나 지시문으로 처리할 수 없는 일일 때 호출한다. 호출하기 전에 담당자에게 연결해 드리겠다고 말한다.\"," \
    "\"parameters\":{\"type\":\"object\",\"properties\":{\"reason\":{\"type\":\"string\",\"description\":\"넘기는 이유\"}},\"required\":[\"reason\"]}}," \
    "{\"type\":\"function\",\"name\":\"save_note\"," \
    "\"description\":\"사람이 이어받아야 할 용건(예약, 문의, 회신 요청 등)을 접수 메모로 저장한다.\"," \
    "\"parameters\":{\"type\":\"object\",\"properties\":{" \
    "\"name\":{\"type\":\"string\",\"description\":\"상대 이름 (말했다면)\"}," \
    "\"callback_number\":{\"type\":\"string\",\"description\":\"회신 번호 (발신번호와 다를 때)\"}," \
    "\"request\":{\"type\":\"string\",\"description\":\"용건 요약\"}},\"required\":[\"request\"]}}" \
    "]"
