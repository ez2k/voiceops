# AI CallBridge

휴대폰을 "차량용 핸즈프리"처럼 ESP32에 블루투스로 연결해, 010 회선으로 오는 전화를 AI가 받게 합니다.

```
단독형   휴대폰 ⇄ 블루투스 HFP ⇄ ESP32-WROOM-32 ⇄ Wi-Fi ⇄ OpenAI Realtime (전화 음질 μ-law 8kHz)
                                                         └ 또는 ⇄ 중계 서버(relay/) ⇄ Gemini Live
PC 연결형 휴대폰 ⇄ 블루투스 HFP ⇄ ESP32-WROOM-32 ⇄ USB ⇄ PC 브리지 ⇄ Gemini Live (또는 PC 스피커·마이크)
```

| | 단독형 (`standalone/`) | PC 연결형 (`usb-bridge/` + `bridge/`) |
|---|---|---|
| 필요한 것 | ESP32 + USB 전원 | ESP32 + 항상 켜 둔 PC |
| 설치 | **웹 브라우저로 설치·설정** | ESP-IDF 빌드 + 파이썬 |
| 통화 기록 | 시리얼 로그 (대화·접수 메모) | WAV 녹음 + 대화록 + 접수 메모 파일 |
| 상태 | **실험 단계** (아래 "확인할 점") | 기능 완성, 실기기 검증 전 |

**웹 설치·설정 페이지: https://ez2k.github.io/voiceops/** (PC의 크롬·엣지)

두 펌웨어는 블루투스 통화 부분(`components/phone`)을 같이 씁니다.

---

## 단독형 (ESP32 하나로)

### 준비물
- **ESP32-WROOM-32 개발보드** (ESP32-DevKitC 등. S3·C3·C6는 클래식 블루투스가 없어 안 됩니다)
- 데이터 전송이 되는 USB 케이블, 2.4GHz Wi-Fi
- 전용 휴대폰 (블루투스 핸즈프리를 지원하는 일반 스마트폰)
- [OpenAI 플랫폼](https://platform.openai.com/api-keys)에서 만든 API 키 (선불 충전 필요)
- 설치·설정할 때만: PC의 **크롬 또는 엣지** (Web Serial 지원 브라우저)

### 1. 웹 설치 페이지 만들기 (한 번만)
1. 이 폴더 전체를 GitHub 저장소에 올립니다.
2. 저장소 **Settings → Pages → Source**를 **GitHub Actions**로 바꿉니다.
3. `main` 브랜치에 올라가면 `.github/workflows/web-flasher.yml`이 ESP-IDF v6.1로 펌웨어를 빌드하고, 웹 페이지(`web/`)와 함께 `https://<계정>.github.io/<저장소>/`에 배포합니다. (Actions 탭에서 진행 상황 확인)

ESP-IDF가 설치된 PC라면 직접 구울 수도 있습니다: `cd standalone && idf.py build flash monitor`

### 2. 펌웨어 설치
1. 배포된 페이지를 크롬·엣지로 엽니다.
2. ESP32를 USB로 꽂고 **① ESP32에 설치** → 포트 선택.
3. 처음 설치할 때는 "지우기"를 선택하세요. 업데이트할 때 지우지 않으면 설정과 페어링이 남습니다.
4. 페이지 맨 위에 이 페이지가 설치하는 **펌웨어 빌드 번호**(`b<실행 번호>-<커밋>`, 빌드 시각)가 보입니다. ② 기기 연결을 하면 기기에 들어 있는 빌드 번호도 보여 주고, 다르면 업데이트하라고 알려 줍니다. 부팅 로그의 `App version:`, `CALLBRIDGE READY <빌드>` 도 같은 값입니다.

### 3. 설정
1. **② 기기 연결** → 같은 포트 선택. 현재 설정과 기본 지시문을 불러옵니다.
2. Wi-Fi 이름·비밀번호, AI 서버(기본 OpenAI 그대로), OpenAI API 키, 모델(`gpt-realtime-2.1-mini` 또는 `gpt-realtime-2.1`), 자동응답까지 벨 횟수(1번 ≈ 3초)를 넣습니다.
   - Gemini를 쓰려면 리눅스 서버에 **중계 서버**([relay/README.md](relay/README.md))를 띄우고, AI 서버에 그 주소(`wss://…/v1/realtime`), API 키 칸에 중계 서버 토큰을 넣습니다.
3. **AI 지시문**에 회사명, 인사말(AI 고지 포함), 영업시간, 자주 묻는 질문, 처리 규칙을 씁니다 (최대 8KB).
4. **저장하고 재시작**. 로그에 `wifi: connected` 가 보이면 됩니다.

설정은 기기(NVS)에 저장되고 재시작하면 적용됩니다. 비밀번호·API 키 칸을 비워 두면 기존 값을 그대로 씁니다.

### 4. 폰 페어링
1. 처음 켜진 ESP32는 페어링 모드입니다 (로그: `페어링 모드`).
2. 폰의 블루투스 설정에서 **AI CallBridge**를 선택합니다.
3. 로그에 `폰 연결 상태: slc` 가 뜨면 완료. 이후에는 자동으로 다시 붙습니다.
4. 다른 폰으로 바꾸려면 웹 페이지의 **새 폰 페어링**을 누릅니다.

### 5. 테스트 (웹 페이지 ③ 로그를 보면서)

| 해볼 일 | 로그에 보여야 할 것 |
|---|---|
| 다른 전화로 이 폰에 전화 | `전화가 옵니다 (6초 뒤 AI 가 받습니다)`, `발신 번호: …`, 시간이 되면 `AI 가 전화를 받습니다` |
| (폰이 벨소리를 기기로 보내는 경우) | `음성 채널 열림 (벨소리) — 받을 때까지 대기` — 정상입니다 |
| 받은 직후 | `realtime: connected, sending session.update` → `session ready` → `AI 응대 시작` |
| AI 인사 듣기 | `realtime: AI: 안녕하세요, OO회사 AI 직원입니다…` |
| 말을 끊어 보기 | `상대가 끼어들어 AI 말을 … ms 에서 멈춥니다` |
| "영업시간이 어떻게 돼요?" | 지시문의 영업시간으로 답함 |
| "견적 받고 싶어요, 저는 홍길동이에요" | `AI 도구: save_note …`, `접수 메모 (발신 …): {…}` |
| "사람이랑 통화할게요" | `사람에게 인계 …` → 소리가 **폰 본체**로 넘어감 (폰을 들고 이어서 통화) |
| "됐습니다, 감사합니다" | `AI 도구: end_call` → `통화 끝` |
| 통화 끝 | `session closed — 토큰: 입력 음성 …·글 … (그중 캐시 …), 출력 음성 …·글 …` (요금 확인용) |
| 통화 중 5초마다 | `call stats: heap free=… min=… big=… \| ai rx=…KB/s buffered=…ms prebuf=…ms underruns=… send_fails=…` |

### 단독형에서 확인할 점 (실험의 핵심)
- **메모리**: PSRAM이 없는 칩에 블루투스·Wi-Fi·TLS를 함께 올립니다. 로그의 `min=` 값이 수 KB 이하로 떨어지거나, `big=`(가장 큰 빈 블록)이 17KB 아래로 내려가 TLS가 서버 레코드를 못 받거나, 재부팅이 반복되면 메모리 부족입니다 → 2칩(ESP32 + ESP32-S3) 구성으로 넘어가는 신호입니다.
- **무선 공존 (AI 서버를 OpenAI 로 바꾼 이유)**: 블루투스 통화와 Wi-Fi가 안테나 하나를 나눠 쓰는 동안 ESP32가 받는 속도는 실측 22~30KB/s였습니다 (전화 없을 때는 1초 최고 108KB/s). Gemini Live는 AI 음성을 24kHz로만 보내 base64로 약 64KB/s가 필요해서 끊겼고, TCP·Wi-Fi 설정을 바꿔도 나아지지 않았습니다. OpenAI Realtime은 전화망과 같은 G.711 μ-law 8kHz로 주고받을 수 있어 AI 음성이 약 11KB/s면 됩니다. 블루투스 통화가 원래 8kHz라 상대가 듣는 음질은 같습니다. `call stats`의 `underruns`가 늘지 않아야 합니다. 웹 페이지의 **통화 흉내 시험**으로 전화 없이 확인할 수 있습니다 (폰이 통화 없이 음성 채널을 열어 줄 때).
- **말 끊기 반응**: AI 음성 대기열(1초)이 차 있으면 끼어들기가 최대 1~2초 늦게 반영될 수 있습니다.
- **접수 메모**: 지금은 로그에만 남습니다 (웹훅·구글 시트 저장은 다음 단계).
- **인계**: 소리가 폰 본체로 넘어가므로 폰 근처에 사람이 있어야 합니다. Wi-Fi가 끊겨 있거나 API 키가 없으면 AI가 받지 않고 그냥 울리게 둡니다.
- **요금**: [OpenAI 가격표](https://developers.openai.com/api/docs/pricing) 기준 100만 토큰당 mini 음성 입력 $10·출력 $20, 2.1 은 $32·$64. 상대 음성은 0.1초에 1토큰, AI 음성은 0.05초에 1토큰이고 대답마다 그동안의 대화를 (대부분 캐시 가격으로) 다시 읽습니다. 3분 통화 한 통에 mini 약 $0.05, 2.1 약 $0.18 (추정). 통화가 끝날 때 로그에 실제 토큰 수가 나옵니다.
- **끼어들기**: 상대가 말을 시작하면 AI 음성을 바로 멈추고, 상대가 실제로 들은 만큼만 대화에 남깁니다 (`conversation.item.truncate`).

### 시리얼 설정 명령 (웹 페이지가 내부적으로 쓰는 것)
115200bps 시리얼 터미널에서 직접 입력해도 됩니다.

| 명령 | 하는 일 |
|---|---|
| `cfg show` | 현재 설정 (비밀번호·키는 가림)과 펌웨어 빌드 번호 |
| `cfg set wifi_ssid <값>` | `wifi_ssid`, `wifi_pass`, `ai_url`(`wss://`·`ws://`), `api_key`, `model`, `rings`, `coex`(`balance`·`wifi`·`bt`) |
| `cfg prompt get` / `begin` / `data <base64>` / `end` / `reset` | 지시문 읽기·바꾸기·기본값 복원 |
| `cfg reboot` | 재시작 (설정 적용) |
| `pair` | 새 폰 페어링 허용 |
| `aitest` | 전화 없이 AI 음성을 20초 받아 수신 속도를 잼 (결과: `속도 시험 끝 … 1초 최고 …KB/s`) |
| `aitest call` | 전화 없이 폰의 통화 음성 채널만 열고 실제 통화처럼 20초 시험 — 블루투스가 무선을 차지하는 상황을 재현 (결과: `통화 흉내 시험 끝 … 끊김 …번`). 폰이 통화 없이 채널을 열어 주지 않으면 그렇게 알려 줌 |
| `status` | 블루투스·Wi-Fi·메모리 상태 |

---

## PC 연결형 (ESP32 + PC)

### 1. 펌웨어 굽기 (ESP-IDF v6.1 필요 → [설치 가이드](https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32/get-started/index.html))
```bash
cd usb-bridge
idf.py set-target esp32
idf.py build
idf.py -p COM5 flash          # 맥: idf.py -p /dev/cu.usbserial-XXXX flash
```
`idf.py monitor`는 쓰지 마세요. USB 시리얼을 이진 프로토콜로 쓰기 때문에 깨진 글자만 보입니다. 로그는 PC 브리지 화면에 `·`로 표시됩니다.

### 2. PC 브리지 실행
```bash
cd bridge
python -m venv .venv
# 윈도우: .venv\Scripts\activate     맥: source .venv/bin/activate
pip install -r requirements.txt
python -m serial.tools.list_ports                                   # 포트 확인

# AI 응대 (prompt.md 를 회사 내용으로 고친 뒤)
# 윈도우 PowerShell: $env:GEMINI_API_KEY="키"     맥: export GEMINI_API_KEY=키
python -m callbridge --port COM5 --ai gemini --auto-answer 2

# 사람이 PC 스피커·마이크로 통화 (음성 경로 확인용)
python -m callbridge --port COM5
```
페어링은 단독형과 같습니다 (새 폰은 브리지에서 `pair` 입력).

**명령**: `answer`, `hangup`, `dial <번호>`, `dtmf <숫자>`, `audio off`(소리를 폰 본체로), `audio on`, `pair`, `status`, `quit`

**기록**: `calls/날짜-시간_번호.wav`(스테레오 녹음: 왼쪽 상대, 오른쪽 우리), `.txt`(AI 대화록), `calls/notes.jsonl`(접수 메모)

**PC ⇄ ESP32 프로토콜**: USB 시리얼 921600bps, 프레임 `A5 5A | type | len(2, LE) | payload | crc16(2, LE)` (CRC-16/CCITT-FALSE). type: `0x01` 이벤트, `0x02` 로그, `0x03` PCM 음성, `0x10` 명령. 폰에서 음성 프레임이 하나 오면 같은 길이의 우리 쪽 음성을 하나 돌려보내 시계 차이로 지연이 쌓이지 않게 합니다.

---

## 폴더 구조
```
components/phone/        공용: 블루투스 HFP 핸즈프리, 페어링, 재연결, 통화 음성 (콜백 인터페이스)
standalone/              단독형 펌웨어
  main/agent.c           통화 흐름: 벨 N번 → AI 받기 → 도구(끊기·인계·메모)
  main/realtime.c        OpenAI Realtime WebSocket 세션 (μ-law 8kHz, 도구, 끼어들기, 토큰 집계)
  main/rt_stream.c       음성 메시지를 통째로 담지 않고 흘려가며 해독
  main/g711.c            G.711 μ-law ⇄ 16bit PCM
  main/console.c         시리얼 설정 명령 (cfg …)
  main/settings.c        설정 저장 (NVS)
  main/prompt.md         기본 AI 지시문 (펌웨어에 내장)
  host_test/             PC에서 도는 C 테스트
usb-bridge/              PC 연결형 펌웨어 (USB 프레임 링크)
bridge/                  PC 브리지 (파이썬: 링크, Gemini endpoint, 녹음)
relay/                   Gemini 중계 서버 (리눅스: OpenAI Realtime 방식 ⇄ Gemini Live, μ-law 8kHz, 메모·대화록)
web/                     웹 설치·설정 페이지 (ESP Web Tools + Web Serial)
.github/workflows/       펌웨어 빌드 + 웹 페이지 배포
```

## 테스트
- `standalone/host_test/run.sh` — base64, μ-law, 메시지 스트리밍 해독, 고정 JSON 유효성, 그리고 `realtime.c` 전체를 가짜 WebSocket 으로 (세션 설정·첫 인사·끼어들기·도구·토큰 집계). cJSON 이 필요합니다 (Ubuntu: `libcjson-dev`)
- `cd web/test && npm install && npm test` — 웹 설정 페이지를 가짜 기기로 (불러오기·저장·지시문 한글 왕복)
- `cd relay && python -m pytest` — 중계 서버를 실제 WebSocket 으로 띄워 가짜 기기·가짜 Gemini 로 왕복, μ-law 를 펌웨어 C 구현과 전부 비교
- `cd bridge && pip install pytest && python -m pytest` — PC 브리지 전체 (C 프레임 코덱 교차 검증, 가짜 Gemini 세션)

두 펌웨어 모두 GitHub Actions(ESP-IDF v6.1)에서 빌드되고, 위 테스트와 AI 서버 인증서 확인(내장 루트로 api.openai.com 체인 검증)도 CI에서 돌아갑니다 (`.github/workflows/checks.yml`). 단독형의 OpenAI 연결은 **실제 통화로는 아직 검증 전**입니다.
