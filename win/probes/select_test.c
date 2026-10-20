/* select_test.c —— 聚焦实验：SELECT_CANDIDATE 的正确姿势
 * 模式：0=int_value(已证会卡死连接)  1=commit{CandidateWord{id}}  2=commit{value+id}
 * 序列：hello → CREATE → "nihao" → 排空(记下候选 id) → SELECT(mode) → 观察 emit(type=1)
 *       → 再发 'n' 验证管道仍可写。
 */
#include <stdio.h>
#include <string.h>
#include <windows.h>

static HANDLE g_pipe;

/* 枚举 \\.\pipe\ 列出全部；找 wetype.*.session_v2（排除 *_update） */
static int find_pipe(wchar_t *out, int cap)
{
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(L"\\\\.\\pipe\\*", &fd);
    if (h == INVALID_HANDLE_VALUE) { printf("FindFirstFile pipe fail %lu\n", GetLastError()); fflush(stdout); return 0; }
    int found = 0, total = 0;
    do {
        total++;
        printf("  pipe[%d]: %ls\n", total, fd.cFileName);
        if (wcsncmp(fd.cFileName, L"wetype.", 7) != 0) continue;
        int len = (int)wcslen(fd.cFileName);
        if (len <= 24 || wcscmp(fd.cFileName + len - 11, L".session_v2") != 0) continue;
        _snwprintf(out, cap, L"\\\\.\\pipe\\%s", fd.cFileName);
        found = 1;
    } while (FindNextFileW(h, &fd));
    printf("total pipes: %d\n", total); fflush(stdout);
    FindClose(h);
    return found;
}

/* 候选表：来自最近一次 candidateUpdate */
struct CW { unsigned char value[96]; int vlen; unsigned char id[160]; int idlen; };
static struct CW cws[16];
static int ncws;

/* ---------- 通用 pb 迭代 ---------- */
typedef struct { const unsigned char *p; int len; } span_t;
static int pb_next(const unsigned char *p, int len, int *pos,
                   int *field, int *wire, span_t *s, unsigned long long *v)
{
    int i = *pos;
    if (i >= len) return 0;
    unsigned long long tag = 0, val = 0;
    int shift = 0, j = i;
    while (j < len) { unsigned char b = p[j++]; tag |= (unsigned long long)(b & 0x7f) << shift; shift += 7; if (!(b & 0x80)) break; }
    if (j >= len && (p[j-1] & 0x80)) return 0;
    *field = (int)(tag >> 3); *wire = (int)(tag & 7);
    s->p = NULL; s->len = 0; *v = 0;
    if (*wire == 0) {
        shift = 0;
        while (j < len) { unsigned char b = p[j++]; val |= (unsigned long long)(b & 0x7f) << shift; shift += 7; if (!(b & 0x80)) break; }
        *v = val;
    } else if (*wire == 2) {
        unsigned long long l = 0; shift = 0;
        while (j < len) { unsigned char b = p[j++]; l |= (unsigned long long)(b & 0x7f) << shift; shift += 7; if (!(b & 0x80)) break; }
        if (j + (int)l > len) return 0;
        s->p = p + j; s->len = (int)l; j += (int)l;
    } else if (*wire == 1) { if (j + 8 > len) return 0; j += 8;
    } else if (*wire == 5) { if (j + 4 > len) return 0; j += 4;
    } else return 0;
    *pos = j;
    return 1;
}

static void cw_parse(span_t w)
{
    if (ncws >= 16) return;
    struct CW *c = &cws[ncws];
    memset(c, 0, sizeof *c);
    int pos = 0, f, wi; span_t s; unsigned long long v;
    while (pb_next(w.p, w.len, &pos, &f, &wi, &s, &v)) {
        if (f == 1 && wi == 2) { c->vlen = s.len < (int)sizeof c->value ? s.len : (int)sizeof c->value - 1; memcpy(c->value, s.p, c->vlen); }
        else if (f == 2 && wi == 2) { c->idlen = s.len < (int)sizeof c->id ? s.len : (int)sizeof c->id - 1; memcpy(c->id, s.p, c->idlen); }
    }
    ncws++;
}

/* 解析整条 RespBase：type==2 时刷新候选表；返回最后遇到的 detail type */
static int parse_resp(const unsigned char *msg, int len)
{
    int last_type = -1;
    int pos = 0, f, wi; span_t s; unsigned long long v;
    while (pb_next(msg, len, &pos, &f, &wi, &s, &v)) {
        if (f != 2 || wi != 2 || !s.p) continue;               /* RespBase.resp */
        int p2 = 0, f2, w2; span_t s2; unsigned long long v2;
        while (pb_next(s.p, s.len, &p2, &f2, &w2, &s2, &v2)) {
            if (f2 != 2 || w2 != 2) continue;                  /* Detail */
            int type = -1; span_t cand = { NULL, 0 };
            int p3 = 0, f3, w3; span_t s3; unsigned long long v3;
            while (pb_next(s2.p, s2.len, &p3, &f3, &w3, &s3, &v3)) {
                if (f3 == 1 && w3 == 0) type = (int)v3;
                else if (f3 == 2 && w3 == 2) cand = s3;
            }
            if (type >= 0) last_type = type;
            if (type == 2 && cand.p) {                         /* candidateUpdate */
                ncws = 0;
                int p4 = 0, f4, w4; span_t s4; unsigned long long v4;
                while (pb_next(cand.p, cand.len, &p4, &f4, &w4, &s4, &v4))
                    if (f4 == 1 && w4 == 2) cw_parse(s4);
            }
        }
    }
    return last_type;
}

/* ---------- 编码 ---------- */
static int put_varint(unsigned char *p, unsigned long long v)
{ int n=0; do { unsigned char b=v&0x7f; v>>=7; if(v) b|=0x80; p[n++]=b; } while(v); return n; }
static int put_tag(unsigned char *p, int f, int w){ return put_varint(p,((unsigned long long)f<<3)|w); }
static int put_vf(unsigned char *p, int f, unsigned long long v){ int n=put_tag(p,f,0); return n+put_varint(p+n,v); }
static int put_bf(unsigned char *p, int f, const void*d, int l){ int n=put_tag(p,f,2); n+=put_varint(p+n,l); if(l>0)memcpy(p+n,d,l); return n+l; }

static int build_cmd2(unsigned char *out, int ctype, int has_key, unsigned kc,
                      int has_int, int iv,
                      const struct CW *commit,   /* 非 NULL: commit{value?,id} */
                      unsigned pid, unsigned tid)
{
    unsigned char detail[512], appinfo[96], cv1[640];
    int nd=0, na=0, nc=0;
    nd += put_vf(detail+nd, 1, (unsigned)ctype);
    if (has_key) {
        unsigned char ke[32]; int nk=0;
        nk += put_vf(ke+nk, 1, kc);
        nk += put_vf(ke+nk, 3, 8);
        nd += put_bf(detail+nd, 2, ke, nk);
    }
    if (has_int) nd += put_vf(detail+nd, 3, (unsigned)iv);
    if (commit) {
        unsigned char cw[384]; int w=0;
        if (commit->vlen > 0) w += put_bf(cw+w, 1, commit->value, commit->vlen);
        if (commit->idlen > 0) w += put_bf(cw+w, 2, commit->id, commit->idlen);
        nd += put_bf(detail+nd, 5, cw, w);
    }
    na += put_vf(appinfo+na, 1, pid);
    na += put_vf(appinfo+na, 2, tid);
    na += put_bf(appinfo+na, 4, "wetype_probe", 12);
    na += put_vf(appinfo+na, 5, 1);
    nc += put_bf(cv1+nc, 1, detail, nd);
    nc += put_bf(cv1+nc, 2, appinfo, na);
    { int n = put_vf(out, 1, 1); n += put_bf(out+n, 2, cv1, nc); return n; }
}

static int read_msg(unsigned char *buf, DWORD cap, DWORD *out, int timeout_ms, int *ptype)
{
    DWORD n=0, avail=0;
    *ptype = -2;
    for (int w=0; w<timeout_ms; w+=25) {
        if (PeekNamedPipe(g_pipe, buf, cap, &n, &avail, NULL) && avail>0) {
            if (!ReadFile(g_pipe, buf, cap, out, NULL)) return -1;
            *ptype = parse_resp(buf, (int)*out);
            return 0;
        }
        Sleep(25);
    }
    return 1;
}

static int send_msg(const unsigned char *m, int len)
{
    DWORD n=0;
    printf("  [write %d bytes...]\n", len); fflush(stdout);
    if (!WriteFile(g_pipe, m, len, &n, NULL) || n!=(DWORD)len) { printf("  [write FAIL %lu]\n", GetLastError()); return -1; }
    FlushFileBuffers(g_pipe);
    printf("  [write ok]\n"); fflush(stdout);
    return 0;
}

int main(int argc, char **argv)
{
    const char *a1 = argc>1 ? argv[1] : NULL;
    int mode = a1 ? atoi(a1) : 1;
    unsigned char req[768], buf[65536];
    DWORD got=0; int t;

    setvbuf(stdout,NULL,_IONBF,0);
    wchar_t w[512];
    MultiByteToWideChar(CP_UTF8, 0,
        "\\\\.\\pipe\\wetype.838180e977848124a1b2f9a7258c4e70.session_v2",
        -1, w, 512);
    HANDLE h2 = INVALID_HANDLE_VALUE;
    DWORD lasterr = 0;
    for (int i = 0; i < 60; i++) {
        h2 = CreateFileW(w, GENERIC_READ | GENERIC_WRITE, 0, NULL,
                         OPEN_EXISTING, 0, NULL);
        if (h2 != INVALID_HANDLE_VALUE) break;
        lasterr = GetLastError();
        if (i % 10 == 0) printf("open retry %d err=%lu\n", i, lasterr);
        Sleep(1000);
    }
    g_pipe = h2;
    if (g_pipe == INVALID_HANDLE_VALUE) { printf("open fail err=%lu\n", lasterr); return 1; }
    DWORD pm=PIPE_READMODE_MESSAGE; SetNamedPipeHandleState(g_pipe,&pm,NULL,NULL);
    unsigned pid=GetCurrentProcessId(), tid=GetCurrentThreadId();
    printf("connected pid=%u mode=%d\n",pid,mode);

    if(read_msg(buf,sizeof buf,&got,2000,&t)==0) printf("hello %luB type=%d\n",got,t);
    send_msg(req, build_cmd2(req,3,0,0,0,0,NULL,pid,tid));               /* CREATE */
    read_msg(buf,sizeof buf,&got,600,&t);
    for(const char*k="nihao";*k;k++){
        send_msg(req, build_cmd2(req,1,1,(unsigned char)*k,0,0,NULL,pid,tid));
        read_msg(buf,sizeof buf,&got,700,&t);
    }
    printf("draining:\n");
    while(read_msg(buf,sizeof buf,&got,400,&t)==0) printf("  push %luB type=%d cands=%d\n",got,t,ncws);
    if (ncws > 0) {
        printf("cand[0] value(%d)=\"%.*s\" idlen=%d\n", cws[0].vlen, cws[0].vlen, cws[0].value, cws[0].idlen);
    } else { printf("NO CANDIDATES captured!\n"); return 2; }

    if(mode==0){
        printf("== SELECT int_value=0 ==\n");
        send_msg(req, build_cmd2(req,2,0,0,1,0,NULL,pid,tid));
    } else if(mode==1){
        printf("== SELECT commit{id} ==\n");
        send_msg(req, build_cmd2(req,2,0,0,0,0,&cws[0],pid,tid));
    } else {
        printf("== SELECT int_value=1(1-based) ==\n");
        send_msg(req, build_cmd2(req,2,0,0,1,1,NULL,pid,tid));
    }
    for(int i=0;i<8;i++){
        int r=read_msg(buf,sizeof buf,&got,500,&t);
        if(r!=0){ printf("  (no more, r=%d)\n",r); break; }
        printf("  post-select push %luB type=%d %s\n",got,t,t==1?"<<< EMIT!":"");
    }
    printf("== aliveness: send 'n' ==\n");
    if(send_msg(req, build_cmd2(req,1,1,'n',0,0,NULL,pid,tid))<0){ printf("PIPE DEAD\n"); return 3; }
    for(int i=0;i<3;i++){
        int r=read_msg(buf,sizeof buf,&got,600,&t);
        if(r!=0){ printf("  (dead? r=%d)\n",r); break; }
        printf("  alive push %luB type=%d\n",got,t);
    }
    printf("done\n");
    return 0;
}
