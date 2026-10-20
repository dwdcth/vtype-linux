/* 帧格式暴力测试：对 session_v2 管道逐种试探，收到任何回信即命中 */
#include <stdio.h>
#include <string.h>
#include <windows.h>

/* CREATE_SESSION 的 CommandBase 序列化（由宿主侧 python 预生成，参数传入太麻烦，
 * 这里内嵌探针脚本生成的一份：version=1, cmd{appinfo{process_name="probe", client_type=1},
 * cmd[0]{command_type=3}} */
static const char *PIPE = "\\\\.\\pipe\\wetype.838180e977848124a1b2f9a7258c4e70.session_v2";

static int try_frame(const char *tag, const unsigned char *frame, DWORD flen)
{
    wchar_t w[512];
    MultiByteToWideChar(CP_UTF8, 0, PIPE, -1, w, 512);
    HANDLE h = CreateFileW(w, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) { printf("%s: open fail %lu\n", tag, GetLastError()); return 0; }
    DWORD mode = PIPE_READMODE_MESSAGE, n = 0;
    SetNamedPipeHandleState(h, &mode, NULL, NULL);
    /* 吃掉服务器首推 */
    Sleep(300);
    unsigned char junk[8192]; DWORD avail = 0;
    if (PeekNamedPipe(h, junk, sizeof junk, &n, &avail, NULL) && avail) ReadFile(h, junk, sizeof junk, &n, NULL);
    if (!WriteFile(h, frame, flen, &n, NULL)) { printf("%s: write fail\n", tag); CloseHandle(h); return 0; }
    Sleep(600);
    avail = 0;
    if (PeekNamedPipe(h, junk, sizeof junk, &n, &avail, NULL) && avail) {
        ReadFile(h, junk, sizeof junk, &n, NULL);
        printf("%s: REPLY %lu bytes:", tag, n);
        for (DWORD i = 0; i < n && i < 48; i++) printf(" %02X", junk[i]);
        printf("\n"); fflush(stdout);
        CloseHandle(h);
        return 1;
    }
    printf("%s: no reply\n", tag); fflush(stdout);
    CloseHandle(h);
    return 0;
}

int main(int argc, char **argv)
{
    /* 基准 protobuf（命令行传入十六进制） */
    if (argc < 2) { printf("usage: framing <hex-protobuf>\n"); return 2; }
    unsigned char pb[4096]; int plen = 0;
    for (const char *p = argv[1]; *p && plen < (int)sizeof pb; ) {
        unsigned v; if (sscanf(p, "%2x", &v) != 1) break;
        pb[plen++] = (unsigned char)v; p += 2;
    }
    printf("protobuf %d bytes\n", plen);

    unsigned char buf[4200]; int n;

    n = plen; memcpy(buf, pb, plen);
    try_frame("A-bare", buf, n);

    n = 0; buf[n++]=plen & 0xff; buf[n++]=(plen>>8)&0xff; buf[n++]=(plen>>16)&0xff; buf[n++]=(plen>>24)&0xff;
    memcpy(buf+n, pb, plen); n += plen;
    try_frame("B-u32le-len", buf, n);

    n = 0; buf[n++]=(plen>>24)&0xff; buf[n++]=(plen>>16)&0xff; buf[n++]=(plen>>8)&0xff; buf[n++]=plen&0xff;
    memcpy(buf+n, pb, plen); n += plen;
    try_frame("C-u32be-len", buf, n);

    n = 0; for (int i=0;i<8;i++) buf[n++] = (plen >> (8*i)) & 0xff;
    memcpy(buf+n, pb, plen); n += plen;
    try_frame("D-u64le-len", buf, n);

    n = 0; buf[n++]=plen & 0xff; buf[n++]=(plen>>8)&0xff; buf[n++]=0; buf[n++]=0;
    buf[n++]=0; buf[n++]=0; buf[n++]=0; buf[n++]=0;
    memcpy(buf+n, pb, plen); n += plen;
    try_frame("E-u32len+pad4", buf, n);

    n = 0; buf[n++]=0x08; buf[n++]=0x01;
    memcpy(buf+n, pb, plen); n += plen;
    try_frame("F-prefix-0801", buf, n);

    n = 0; memset(buf, 0, 32); memcpy(buf, pb, plen < 32 ? 0 : 0);
    n = 32; memcpy(buf+32, pb, plen); n += plen;
    try_frame("G-32B-zero-hdr", buf, n);

    return 0;
}
