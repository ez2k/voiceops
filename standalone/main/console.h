/*
 * USB 시리얼 설정 콘솔 — 웹 설정 페이지(web/index.html)가 이 명령들을 보낸다.
 *
 *   cfg show                      현재 설정 보기 (API 키·비밀번호는 가림)
 *   cfg set <키> <값>             wifi_ssid, wifi_pass, api_key, model, rings
 *   cfg prompt get                지시문을 base64 줄로 출력
 *   cfg prompt begin|data <b64>|end   지시문 바꾸기
 *   cfg prompt reset              기본 지시문으로 되돌리기
 *   cfg reboot                    재부팅 (바뀐 설정 적용)
 *   pair                          새 폰 페어링 허용
 *   status                        블루투스·Wi-Fi·메모리 상태
 *
 * 응답은 "cfg ok …", "cfg err …", "cfg value …" 처럼 "cfg " 로 시작하는 줄이다.
 */
#pragma once

#include "settings.h"

void console_start(const settings_t *running);
