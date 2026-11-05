#!/usr/bin/env bash
# AI CallBridge — Gemini 중계 서버 한 번에 설치 (Debian·Ubuntu)
#
#   curl -fsSL https://raw.githubusercontent.com/ez2k/voiceops/main/relay/install.sh | sudo bash
#
# 옵션 (bash -s -- 뒤에):
#   --domain relay.example.com   이 서버를 가리키는 도메인 (없으면 <공인IP>.sslip.io 를 씀)
#   --no-tls                     암호화 없이 ws://공인IP:8765 로 연다 (시험용 — 토큰·음성이 그대로 오간다)
#
# 하는 일: 패키지 설치 → 저장소 받기(/opt/voiceops) → 파이썬 가상환경 → Gemini 키 입력·토큰 생성(/etc/geminirelay.env)
#         → systemd 서비스 → (기본) Caddy 로 Let's Encrypt 인증서와 wss:// → 끝에 기기에 넣을 값을 보여 준다.
# 다시 실행하면 코드만 새로 받고 서비스를 다시 띄운다 (키·토큰은 그대로).
set -euo pipefail

REPO=${VOICEOPS_REPO:-https://github.com/ez2k/voiceops.git}   # CI 는 받은 코드로 시험한다
DIR=/opt/voiceops
ENV_FILE=/etc/geminirelay.env
PORT=8765
DOMAIN=""
NO_TLS=0

say()  { printf '\n\033[1m== %s\033[0m\n' "$*"; }
die()  { printf '\n\033[31m설치 중단: %s\033[0m\n' "$*" >&2; exit 1; }

while [ $# -gt 0 ]; do
    case "$1" in
        --domain) DOMAIN="${2:-}"; shift 2 ;;
        --no-tls) NO_TLS=1; shift ;;
        *) die "알 수 없는 옵션: $1" ;;
    esac
done

[ "$(id -u)" -eq 0 ] || die "sudo 로 실행하세요"
command -v apt-get >/dev/null || die "Debian·Ubuntu 용 스크립트입니다 (다른 배포판은 relay/README.md 의 수동 설치를 따르세요)"
command -v systemctl >/dev/null || die "systemd 가 필요합니다"

say "패키지 설치"
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y -qq git curl openssl ca-certificates >/dev/null

# 파이썬은 uv 로 준비한다: 시스템 파이썬이 3.10 이상이면 그것을, 아니면(예: Ubuntu 20.04 의 3.8) 독립 실행형 파이썬을 받는다
if ! command -v uv >/dev/null; then
    case "$(uname -m)" in
        x86_64) UV_ARCH=x86_64 ;;
        aarch64|arm64) UV_ARCH=aarch64 ;;
        *) die "지원하지 않는 CPU: $(uname -m)" ;;
    esac
    tmp=$(mktemp -d)
    curl -fsSL "https://github.com/astral-sh/uv/releases/latest/download/uv-$UV_ARCH-unknown-linux-gnu.tar.gz" | tar -xz -C "$tmp"
    install -m 755 "$tmp/uv-$UV_ARCH-unknown-linux-gnu/uv" /usr/local/bin/uv
    rm -rf "$tmp"
fi

say "코드 받기 ($DIR)"
id geminirelay >/dev/null 2>&1 || useradd --system --home-dir "$DIR" --shell /usr/sbin/nologin geminirelay
if [ -d "$DIR/.git" ]; then
    git -C "$DIR" pull -q --ff-only
else
    git clone -q --depth 1 "$REPO" "$DIR"
fi
export UV_PYTHON_INSTALL_DIR="$DIR/.python"   # 받은 파이썬을 서비스 사용자도 읽을 수 있는 곳에
rm -rf "$DIR/relay/.venv"
uv venv -q --python ">=3.10" "$DIR/relay/.venv"
uv pip install -q --python "$DIR/relay/.venv/bin/python" -r "$DIR/relay/requirements.txt"
echo "파이썬: $("$DIR/relay/.venv/bin/python" -V)"

say "Gemini 키와 기기 토큰 ($ENV_FILE)"
if [ ! -f "$ENV_FILE" ]; then
    [ -r /dev/tty ] || die "Gemini 키를 입력받을 터미널이 없습니다. GEMINI_API_KEY=… 를 $ENV_FILE 에 직접 쓰고 다시 실행하세요"
    printf 'Gemini API 키 (https://aistudio.google.com/apikey, 입력은 화면에 안 보입니다): '
    read -rs GEMINI_KEY </dev/tty
    echo
    [ -n "$GEMINI_KEY" ] || die "Gemini API 키가 비었습니다"
    umask 077
    printf 'GEMINI_API_KEY=%s\nRELAY_TOKEN=%s\n# GEMINI_MODEL=gemini-3.8-live\n' "$GEMINI_KEY" "$(openssl rand -hex 24)" > "$ENV_FILE"
    umask 022
    echo "만들었습니다."
else
    echo "이미 있습니다 — 그대로 씁니다."
fi
chmod 600 "$ENV_FILE"
TOKEN=$(sed -n 's/^RELAY_TOKEN=//p' "$ENV_FILE")

say "서비스 (geminirelay)"
install -m 644 "$DIR/relay/deploy/geminirelay.service" /etc/systemd/system/geminirelay.service
mkdir -p /etc/systemd/system/geminirelay.service.d
if [ "$NO_TLS" -eq 1 ]; then
    # 암호화 없이 밖에서 바로 받는다
    printf '[Service]\nExecStart=\nExecStart=%s/relay/.venv/bin/python -m geminirelay --host 0.0.0.0 --port %s --data-dir /var/lib/geminirelay\n' \
        "$DIR" "$PORT" > /etc/systemd/system/geminirelay.service.d/listen.conf
else
    rm -f /etc/systemd/system/geminirelay.service.d/listen.conf
fi
systemctl daemon-reload
systemctl enable -q geminirelay
systemctl restart geminirelay

# 서버가 떴는지: 토큰 없이 두드리면 401 이어야 한다
for _ in $(seq 1 20); do
    code=$(curl -s -o /dev/null -w '%{http_code}' "http://127.0.0.1:$PORT/v1/realtime" || true)
    [ "$code" = "401" ] && break
    sleep 1
done
[ "$code" = "401" ] || { journalctl -u geminirelay -n 30 --no-pager; die "중계 서버가 뜨지 않았습니다 (위 로그 참고)"; }
echo "중계 서버가 127.0.0.1:$PORT 에서 돌고 있습니다."

PUBLIC_IP=$(curl -fsS -4 --max-time 10 https://api.ipify.org || true)

if [ "$NO_TLS" -eq 1 ]; then
    command -v ufw >/dev/null && ufw status | grep -q "Status: active" && ufw allow "$PORT/tcp" >/dev/null
    URL="ws://${PUBLIC_IP:-서버IP}:$PORT/v1/realtime"
else
    say "암호화 (Caddy + Let's Encrypt)"
    if [ -z "$DOMAIN" ]; then
        [ -n "$PUBLIC_IP" ] || die "공인 IP 를 알 수 없습니다. --domain 으로 도메인을 주세요"
        DOMAIN="$(echo "$PUBLIC_IP" | tr . -).sslip.io"   # 도메인이 없을 때: IP 를 이름으로 바꿔 주는 무료 DNS
        echo "도메인이 없어 $DOMAIN 을 씁니다."
    fi
    busy=$(ss -ltnp 2>/dev/null | awk '$4 ~ /:(80|443)$/' | grep -v caddy || true)
    if [ -n "$busy" ]; then
        echo "80·443 포트를 이미 다른 프로그램이 쓰고 있어 Caddy 를 설치하지 않습니다:"
        echo "$busy"
        echo
        echo "쓰고 있는 웹 서버에 아래를 추가하세요 (nginx 예시, 인증서는 Let's Encrypt 로):"
        cat <<EOT
    location /v1/realtime {
        proxy_pass http://127.0.0.1:$PORT;
        proxy_http_version 1.1;
        proxy_set_header Upgrade \$http_upgrade;
        proxy_set_header Connection "upgrade";
        proxy_read_timeout 3600s;
    }
EOT
        URL="wss://$DOMAIN/v1/realtime"
    else
        caddy_existed=0
        command -v caddy >/dev/null && caddy_existed=1
        if [ "$caddy_existed" -eq 0 ]; then
            apt-get install -y -qq debian-keyring debian-archive-keyring apt-transport-https gnupg >/dev/null
            curl -1sLf https://dl.cloudsmith.io/public/caddy/stable/gpg.key \
                | gpg --dearmor --yes -o /usr/share/keyrings/caddy-stable-archive-keyring.gpg
            curl -1sLf https://dl.cloudsmith.io/public/caddy/stable/debian.deb.txt > /etc/apt/sources.list.d/caddy-stable.list
            apt-get update -qq
            apt-get install -y -qq caddy >/dev/null
        fi
        # 이미 쓰던 Caddy 설정(다른 사이트)은 건드리지 않는다: 처음 설치했거나, 기본 설정이거나, 지난번에 우리가 쓴 것만 바꾼다
        if [ "$caddy_existed" -eq 1 ] && [ -f /etc/caddy/Caddyfile ] \
            && ! grep -q "/usr/share/caddy" /etc/caddy/Caddyfile && ! grep -q "127.0.0.1:$PORT" /etc/caddy/Caddyfile; then
            echo "이미 쓰던 Caddy 설정이 있어 바꾸지 않습니다. /etc/caddy/Caddyfile 에 아래를 추가하고 'sudo systemctl reload caddy':"
            printf '\n%s {\n\treverse_proxy 127.0.0.1:%s\n}\n\n' "$DOMAIN" "$PORT"
        else
            sed "s/relay.example.com/$DOMAIN/" "$DIR/relay/deploy/Caddyfile" > /etc/caddy/Caddyfile
        fi
        command -v ufw >/dev/null && ufw status | grep -q "Status: active" && ufw allow 80,443/tcp >/dev/null
        systemctl enable -q caddy
        systemctl restart caddy
        echo "인증서를 받는 중 (최대 90초)…"
        code=""
        for _ in $(seq 1 45); do
            code=$(curl -s -o /dev/null -w '%{http_code}' "https://$DOMAIN/v1/realtime" || true)
            [ "$code" = "401" ] && break
            sleep 2
        done
        if [ "$code" != "401" ]; then
            journalctl -u caddy -n 20 --no-pager || true
            echo
            echo "아직 https://$DOMAIN 에 닿지 않습니다. 확인할 것: 도메인이 이 서버 IP($PUBLIC_IP)를 가리키는지,"
            echo "클라우드 방화벽(보안 그룹)에서 80·443 이 열려 있는지. 고친 뒤 'sudo systemctl restart caddy'."
        fi
        URL="wss://$DOMAIN/v1/realtime"
    fi
fi

say "끝 — 웹 설정 페이지 ② 에 넣을 값"
cat <<EOT
  AI 서버 : $URL
  API 키  : $TOKEN
  모델    : 그대로 두어도 됩니다 (서버가 Gemini 모델로 바꿔 씁니다)

  로그     : journalctl -u geminirelay -f
  메모·대화록: /var/lib/geminirelay
  업데이트 : 이 명령을 다시 실행
EOT
