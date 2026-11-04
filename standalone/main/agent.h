/*
 * 통화 흐름: 벨 N번 → AI 가 받기 → Gemini 와 음성 연결 → 도구(끊기·인계·메모) 처리
 *
 * 폰·Gemini 콜백은 모두 큐로 받아 agent 태스크 하나에서 순서대로 처리한다.
 */
#pragma once

#include "settings.h"

/* 블루투스(phone)와 Gemini 세션을 준비하고 agent 태스크를 시작한다. */
void agent_start(const settings_t *settings);

/* 전화 없이 Gemini 인사 음성을 받아 수신 속도를 잰다 (결과는 로그). 통화 중에는 무시한다. */
void agent_speed_test(void);
