// web/index.html 의 설정 흐름을 가짜 시리얼 기기(펌웨어 console.c 규칙을 흉내)로 시험한다.
const fs = require("fs");
const { JSDOM } = require("jsdom");

const path = require("path");
// 배포 때 CI 가 하는 것처럼 빌드 번호 자리를 채운다
const PAGE_BUILD = "b7-abc1234";
const html = fs.readFileSync(process.argv[2] || path.join(__dirname, "..", "index.html"), "utf8")
  .replace(/<script type="module"[^>]*><\/script>/, "")
  .replace(/__BUILD__/g, PAGE_BUILD).replace(/__BUILT__/g, "2026-11-04 13:40 KST");

// ---- 가짜 기기 ----
const DEFAULT_PROMPT = "# 역할\n기본 지시문입니다. 안녕하세요!\n";
const device = { nvs: {}, prompt: null, collecting: null, rebooted: 0, lines: [], version: "b5-0000000" };
let emit;  // 기기 -> 페이지

function b64ToUtf8(b64) { return Buffer.from(b64, "base64").toString("utf8"); }

function deviceHandle(line) {
  device.lines.push(line);
  const out = (s) => emit(s + "\r\n");
  let m;
  if ((m = line.match(/^cfg set (\S+) (.*)$/))) {
    device.nvs[m[1]] = m[2];
    out(`cfg ok ${m[1]}`);
  } else if (line === "cfg show") {
    out(`cfg value wifi_ssid=${device.nvs.wifi_ssid || ""}`);
    out(`cfg value wifi_pass=${device.nvs.wifi_pass ? "(saved)" : ""}`);
    const k = device.nvs.api_key || "";
    out(`cfg value api_key=${k.length > 8 ? k.slice(0, 4) + "..." + k.slice(-4) : k ? "(saved)" : ""}`);
    out(`cfg value model=${device.nvs.model || "gemini-3.8-live"}`);
    out(`cfg value rings=${device.nvs.rings || 2}`);
    out(`cfg value coex=${device.nvs.coex || "balance"}`);
    if (device.version !== undefined) out(`cfg value version=${device.version}`);  // 이전 펌웨어는 버전을 안 알려 준다
    out(`cfg end show`);
  } else if (line === "cfg prompt get") {
    const b64 = Buffer.from(device.prompt ?? DEFAULT_PROMPT, "utf8").toString("base64");
    for (let i = 0; i < b64.length; i += 300) out(`cfg prompt data ${b64.slice(i, i + 300)}`);
    out("cfg prompt end");
  } else if (line === "cfg prompt begin") {
    device.collecting = ""; out("cfg ok prompt begin");
  } else if ((m = line.match(/^cfg prompt data (\S+)$/))) {
    device.collecting += m[1];
  } else if (line === "cfg prompt end") {
    device.prompt = b64ToUtf8(device.collecting); out(`cfg ok prompt ${Buffer.byteLength(device.prompt)}`);
  } else if (line === "cfg prompt reset") {
    device.prompt = null; out("cfg ok prompt reset");
  } else if (line === "cfg reboot") {
    out("cfg ok reboot"); device.rebooted++;
    setTimeout(() => out("I (321) main: heap after start: free=1234\r\nCALLBRIDGE READY ${device.version}"), 50);
  } else if (line === "pair") {
    out("cfg ok pair");
  } else if (line === "aitest" || line === "aitest call") {
    out("cfg ok aitest");
  }
}

class FakePort {
  constructor() {
    let ctrl;
    this.readable = new ReadableStream({ start(c) { ctrl = c; } });
    emit = (text) => ctrl.enqueue(new TextEncoder().encode(text));
    let buf = "";
    this.writable = new WritableStream({
      write(chunk) {
        buf += new TextDecoder().decode(chunk);
        let i;
        while ((i = buf.indexOf("\n")) >= 0) { const l = buf.slice(0, i); buf = buf.slice(i + 1); deviceHandle(l); }
      },
    });
    this.signals = null;
  }
  async open(opts) { this.baud = opts.baudRate; }
  async setSignals(s) { this.signals = s; }
  async close() {}
}

let port = null;

// 페이지 스크립트는 navigator.serial 을 로드 시점에 확인하므로, 가짜 시리얼을 심은 뒤 실행한다
const script = html.match(/<script>([\s\S]*?)<\/script>/)[1];
const dom2 = new JSDOM(html.replace(/<script>[\s\S]*?<\/script>/, ""), { runScripts: "outside-only", pretendToBeVisual: true });
const win = dom2.window;
Object.defineProperty(win.navigator, "serial", { value: { requestPort: async () => (port = new FakePort()) } });
Object.assign(win, { ReadableStream, WritableStream, TextEncoder, TextDecoder });  // jsdom 에 없는 웹 API
win.eval(script);
const $ = (id) => win.document.getElementById(id);
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const assert = (cond, msg) => { if (!cond) { console.error("FAIL:", msg); process.exitCode = 1; } else console.log("ok:", msg); };

(async () => {
  device.nvs = { wifi_ssid: "office", wifi_pass: "pw", api_key: "AIzaSyABCDEFGH1234" };

  $("connect").click();
  await sleep(2200);
  assert(port.baud === 115200, "115200 으로 연다");
  assert(port.signals && port.signals.dataTerminalReady === false && port.signals.requestToSend === false, "DTR/RTS 를 내린다");
  assert($("wifi_ssid").value === "office", "Wi-Fi 이름을 불러온다");
  assert($("api_key").placeholder.includes("AIza...1234"), "API 키는 가려서 보여 준다");
  assert($("prompt").value === DEFAULT_PROMPT, "지시문(한글)을 불러온다");
  assert(!$("save").disabled, "저장 버튼이 켜진다");
  assert($("page-build").textContent === PAGE_BUILD, "페이지에 빌드 번호를 보여 준다");
  assert($("page-built").textContent.includes("KST"), "빌드 시각도 보여 준다");
  assert($("device-build").textContent.includes("b5-0000000") && $("device-build").textContent.includes(PAGE_BUILD)
         && $("device-build").className === "warn", `기기 펌웨어가 다르면 알린다: ${$("device-build").textContent}`);

  device.version = PAGE_BUILD;  // ①로 업데이트했다고 치고, 저장 후 재시작에서 새 버전을 읽는다

  // 설정 바꾸고 저장: 긴 한글 지시문 + 새 API 키, 비밀번호는 비워 둔다
  const newPrompt = "# 역할\n" + "당신은 OO회사의 AI 직원입니다. 견적·A/S 문의를 받습니다.\n".repeat(60);
  $("prompt").value = newPrompt;
  $("api_key").value = "AIzaNEWKEY999999";
  $("rings").value = "3";
  assert($("coex").value === "balance", "무선 우선을 불러온다");
  $("coex").value = "wifi";
  $("save").click();
  await sleep(4000);

  assert(device.prompt === newPrompt, `지시문이 바이트 그대로 저장된다 (${Buffer.byteLength(newPrompt)}B)`);
  assert(device.nvs.api_key === "AIzaNEWKEY999999", "새 API 키 저장");
  assert(device.nvs.wifi_pass === "pw", "비워 둔 비밀번호는 그대로");
  assert(device.nvs.rings === "3", "벨 횟수 저장");
  assert(device.nvs.coex === "wifi", "무선 우선 저장");
  assert(device.rebooted === 1, "저장 후 재시작");
  assert($("device-build").textContent.includes("(최신)") && $("device-build").className === "ok",
         `재시작 뒤 새 버전을 읽는다: ${$("device-build").textContent}`);
  assert($("status").className === "ok", `상태 표시: ${$("status").textContent}`);
  assert(device.lines.filter((l) => l.startsWith("cfg prompt data")).every((l) => l.length - 16 <= 300), "지시문 줄은 300자 이하");

  // 지시문 비우고 저장 → 기본으로
  $("prompt").value = "";
  $("save").click();
  await sleep(2500);
  assert(device.prompt === null, "빈 지시문은 기본으로 되돌린다");

  $("pair").click();
  await sleep(100);
  assert(device.lines.includes("pair"), "새 폰 페어링 명령");
  $("speed-btn").click();
  await sleep(100);
  assert(device.lines.includes("aitest") && $("status").textContent.includes("속도 시험"), "AI 속도 시험 명령");
  $("call-test-btn").click();
  await sleep(100);
  assert(device.lines.includes("aitest call") && $("status").textContent.includes("통화 흉내"), "통화 흉내 시험 명령");

  // 연결 끊기 → 빌드 번호를 모르는 이전 펌웨어에 다시 연결
  $("disconnect").click();
  await sleep(100);
  assert($("device-build").textContent === "", "연결을 끊으면 기기 펌웨어 표시를 지운다");
  device.version = undefined;
  $("connect").click();
  await sleep(2200);
  assert($("device-build").textContent.includes("이전 펌웨어") && $("device-build").className === "warn",
         `버전을 안 알려 주는 펌웨어는 업데이트를 권한다: ${$("device-build").textContent}`);
  process.exit();
})();
