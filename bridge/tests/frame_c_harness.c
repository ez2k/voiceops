/*
 * 펌웨어의 frame.c 를 PC 에서 돌려 파이썬 구현과 맞는지 확인하는 테스트 도구.
 *   encode : stdin 의 바이트를 AUDIO 프레임 하나로 만들어 stdout 에 쓴다
 *   parse  : stdin 바이트 스트림에서 프레임을 꺼내 "type len hex" 줄로 출력한다
 */
#include <stdio.h>
#include <string.h>

#include "frame.h"

int main(int argc, char **argv)
{
    static uint8_t in[8192];
    size_t n = fread(in, 1, sizeof(in), stdin);

    if (argc > 1 && strcmp(argv[1], "encode") == 0) {
        static uint8_t out[FRAME_MAX_PAYLOAD + FRAME_OVERHEAD];
        size_t len = frame_encode(FRAME_AUDIO, in, (uint16_t)n, out, sizeof(out));
        fwrite(out, 1, len, stdout);
        return len ? 0 : 1;
    }

    static frame_parser_t p;
    frame_parser_reset(&p);
    for (size_t i = 0; i < n; i++) {
        if (frame_parser_feed(&p, in[i])) {
            printf("%u %u ", p.type, p.len);
            for (uint16_t j = 0; j < p.len; j++) {
                printf("%02x", p.payload[j]);
            }
            printf("\n");
        }
    }
    return 0;
}
