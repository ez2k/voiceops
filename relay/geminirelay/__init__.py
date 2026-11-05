"""ESP32 단독형 펌웨어 ⇄ Gemini Live 중계 서버.

기기와는 OpenAI Realtime 방식(펌웨어가 쓰는 부분만, μ-law 8kHz)으로, Gemini 와는 Live API 로 이야기한다.
그래서 펌웨어는 AI 서버 주소만 바꾸면 OpenAI 와 Gemini 를 오갈 수 있다.
"""
