# Gemini 중계 서버 (relay)

단독형 펌웨어는 통화 중에 초당 약 25KB밖에 받지 못합니다. Gemini Live는 AI 음성을 24kHz로만 보내서 그대로는 끊깁니다.
이 서버가 중간에서 **Gemini 음성을 전화 음질(G.711 μ-law 8kHz, 초당 약 11KB)로 바꿔** 기기에 보냅니다.

```
폰 ⇄ ESP32 ──(OpenAI Realtime 방식, μ-law 8kHz)──> 중계 서버 ──(Gemini Live, 24kHz)──> Gemini
```

기기와는 OpenAI Realtime 방식(펌웨어가 쓰는 부분만)으로 이야기하므로, 펌웨어는 **AI 서버 주소만 바꾸면** OpenAI와 Gemini를 오갈 수 있습니다.
Gemini API 키는 이 서버에만 두고, 기기에는 이 서버용 토큰만 넣습니다.

서버가 하는 일:
- 음성 변환: 상대 음성 μ-law → PCM 8kHz (Gemini가 받아서 변환), AI 음성 24kHz → 8kHz → μ-law (0.1초씩 묶어서)
- 첫 인사, 도구(통화 끝내기·사람 인계·접수 메모), 끼어들기, 자막, 토큰 사용량을 펌웨어가 아는 모양으로 전달
- **접수 메모를 `notes.jsonl`에, 통화마다 대화록을 `날짜-시각_번호.txt`로 남김** (`relay/calls/`)

## 한 번에 설치 (Debian·Ubuntu)

설치할 디렉토리로 가서 아래 한 줄을 실행하고 Gemini API 키를 입력하면 끝입니다. 마지막에 **기기에 넣을 AI 서버 주소와 토큰**을 보여 줍니다.

```sh
cd ~        # 원하는 디렉토리
curl -fsSL https://raw.githubusercontent.com/ez2k/voiceops/main/relay/install.sh | sudo bash
```

- 지금 디렉토리 아래 `voiceops/` 에 설치합니다 (이미 voiceops 저장소 안에서 실행하면 그 저장소를 씁니다). 서비스는 sudo 를 실행한 사용자 권한으로 돕니다.
- 그 안에 다 있습니다: 코드, 파이썬(`.python/`, 필요할 때만), 가상환경(`relay/.venv`), 키·토큰(`relay/geminirelay.env`), 접수 메모·대화록(`relay/calls/`). 시스템에는 서비스 파일(`/etc/systemd/system/geminirelay.service`)과 Caddy 설정만 생깁니다.
- 파이썬 3.12 는 [uv](https://github.com/astral-sh/uv) 로 준비합니다. 시스템에 없으면(예: Ubuntu 20.04 의 3.8) 독립 실행형 파이썬을 받아 `.python/` 에 둡니다.
- 암호화는 Caddy 의 Let's Encrypt 인증서로 합니다 (`wss://`). 도메인이 있으면 `… | sudo bash -s -- --domain relay.example.com`, 없으면 `<공인IP>.sslip.io` (IP 를 이름으로 바꿔 주는 무료 DNS)를 씁니다.
- 80·443 을 이미 다른 웹 서버(nginx 등)가 쓰고 있으면 Caddy 를 설치하지 않고, 그 서버에 넣을 설정을 보여 줍니다. 이미 쓰던 Caddy 설정도 바꾸지 않습니다.
- 클라우드 서버라면 보안 그룹(방화벽)에서 80·443 을 열어 두세요.
- 같은 디렉토리에서 다시 실행하면 코드만 새로 받고 서비스를 다시 띄웁니다 (키·토큰은 그대로).

## 손으로 설치 (다른 배포판, Python 3.10 이상)

```sh
git clone https://github.com/ez2k/voiceops.git && cd voiceops
python3 -m venv relay/.venv && relay/.venv/bin/pip install -r relay/requirements.txt
printf 'GEMINI_API_KEY=%s\nRELAY_TOKEN=%s\n' "여기에-Gemini-API-키" "$(openssl rand -hex 24)" > relay/geminirelay.env
chmod 600 relay/geminirelay.env && cat relay/geminirelay.env   # RELAY_TOKEN 값을 기기에 넣는다
sed -e "s|@DIR@|$PWD|g" -e "s|@USER@|$USER|g" -e "s|@HOST@|127.0.0.1|g" relay/deploy/geminirelay.service \
    | sudo tee /etc/systemd/system/geminirelay.service >/dev/null
sudo systemctl daemon-reload && sudo systemctl enable --now geminirelay
journalctl -u geminirelay -f      # 연결·대화·도구 로그
```

## 암호화 (권장: 도메인 + Caddy)

중계 서버는 기본으로 이 서버 안(`127.0.0.1:8765`)에서만 듣습니다. 앞에 [Caddy](https://caddyserver.com/)를 두면 Let's Encrypt 인증서로 `wss://`가 됩니다.

1. 도메인(예: `relay.example.com`)을 이 서버 IP로 연결하고, 방화벽에서 80·443을 엽니다.
2. Caddy를 설치하고 `deploy/Caddyfile`을 `/etc/caddy/Caddyfile`로 복사한 뒤 도메인을 바꿉니다 → `sudo systemctl reload caddy`
   - 기기에 넣은 루트 인증서가 Let's Encrypt(ISRG)라서 `cert_issuer acme` 줄을 지워서는 안 됩니다.

도메인 없이 시험만 할 때는 `--host 0.0.0.0` 으로 띄우고 `ws://서버IP:8765/v1/realtime` 을 씁니다. **토큰과 통화 음성이 암호화되지 않으니** 시험용으로만 쓰세요.

## 기기 설정 (웹 설정 페이지 ②)

| 칸 | 값 |
|---|---|
| AI 서버 | `wss://relay.example.com/v1/realtime` |
| API 키 | `relay/geminirelay.env` 의 `RELAY_TOKEN` |
| 모델 | 아무거나 — `gemini…`로 시작하면 그 모델, 아니면 서버의 `GEMINI_MODEL` |

## 시험

```sh
pip install -r requirements.txt pytest
python -m pytest        # 가짜 기기 + 가짜 Gemini 로 왕복, μ-law 를 펌웨어 C 구현과 65536개 값 전부 비교
```

## 확인할 점
- Gemini **무료 등급은 데이터가 구글 제품 개선에 쓰입니다.** 고객 통화는 유료 등급 키를 쓰세요.
- Gemini Live 세션은 길이 제한이 있습니다(약 15분). 넘으면 연결이 끊기고 기기는 사람에게 넘깁니다.
- 끼어들기는 Gemini가 스스로 처리합니다. 기기가 보내는 `conversation.item.truncate` 는 무시합니다.
- 토큰 사용량은 Gemini가 알려 주는 값을 음성·글로 나눠 전달합니다 (통화 끝 로그의 `session closed — 토큰: …`).
