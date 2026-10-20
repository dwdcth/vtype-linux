/* 用 ntdll 枚举 \??\Pipe 对象目录 */
#include <stdio.h>
#include <windows.h>

typedef struct _USTR { unsigned short Len, MaxLen; wchar_t *Buf; } USTR;
typedef struct _OA { unsigned long Len; void *Root; USTR *Name; unsigned long Attr; void *Sd, *Sqs; } OA;
typedef struct _ODI { USTR Name; USTR TypeName; } ODI;

typedef long (WINAPI *FN_Open)(void **, unsigned long, OA *);
typedef long (WINAPI *FN_Query)(void *, void *, unsigned long, unsigned char, unsigned char, unsigned long *, unsigned long *);

int main(void)
{
    HMODULE nt = GetModuleHandleA("ntdll.dll");
    FN_Open pOpen = (FN_Open)GetProcAddress(nt, "NtOpenDirectoryObject");
    FN_Query pQuery = (FN_Query)GetProcAddress(nt, "NtQueryDirectoryObject");
    if (!pOpen || !pQuery) { printf("no ntdll apis\n"); return 1; }

    wchar_t nm[] = L"\\??\\Pipe";
    USTR us = { sizeof nm - 2, sizeof nm, nm };
    OA oa = { sizeof oa, NULL, &us, 0x40, NULL, NULL };
    void *h = NULL;
    long st = pOpen(&h, 1, &oa);
    if (st) { printf("NtOpenDirectoryObject failed 0x%lx\n", st); return 1; }

    unsigned char buf[4096];
    unsigned long ctx = 0, ret = 0;
    for (;;) {
        st = pQuery(h, buf, sizeof buf, 0, ctx == 0, &ctx, &ret);
        if (st) break;
        ODI *e = (ODI *)buf;
        while ((unsigned char *)e < buf + ret && e->Name.Len) {
            char out[512];
            int n = WideCharToMultiByte(CP_UTF8, 0, e->Name.Buf, e->Name.Len / 2, out, sizeof out - 1, NULL, NULL);
            if (n > 0) { out[n] = 0; printf("%s\n", out); }
            e++;
        }
    }
    return 0;
}
