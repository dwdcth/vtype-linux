/* NtQueryDirectoryFile 枚举 \??\Pipe */
#include <stdio.h>
#include <windows.h>

typedef struct _USTR { unsigned short Len, MaxLen; wchar_t *Buf; } USTR;
typedef struct _OA { unsigned long Len; void *Root; USTR *Name; unsigned long Attr; void *Sd, *Sqs; } OA;
typedef struct _IOB { void *a; void *b; } IOB;
typedef struct _FDI { unsigned long Next; unsigned long Index; long long Create; long long Access; long long Write; long long Change; long long Size; long long Alloc; unsigned long Attr; unsigned long NameLen; wchar_t Name[1]; } FDI;

typedef long (WINAPI *FN_OpenFile)(void **, unsigned long, OA *, IOB *, unsigned long, unsigned long);
typedef long (WINAPI *FN_QDir)(void *, void *, void *, void *, IOB *, void *, unsigned long, int, unsigned char, USTR *);

int main(void)
{
    HMODULE nt = GetModuleHandleA("ntdll.dll");
    FN_OpenFile pOpen = (FN_OpenFile)GetProcAddress(nt, "NtOpenFile");
    FN_QDir pQ = (FN_QDir)GetProcAddress(nt, "NtQueryDirectoryFile");
    if (!pOpen || !pQ) { printf("no apis\n"); return 1; }

    wchar_t nm[] = L"\\??\\Pipe";
    USTR us = { sizeof nm - 2, sizeof nm, nm };
    OA oa = { sizeof oa, NULL, &us, 0x40, NULL, NULL };
    IOB io = { 0 };
    void *h = NULL;
    long st = pOpen(&h, 0x0001 /*FILE_READ_DATA*/ | 0x00100000 /*SYNCHRONIZE*/, &oa, &io,
                    3 /*READ|WRITE share*/, 0x21 /*DIRECTORY|SYNCHRONOUS_IO_NONALERT*/);
    if (st) { printf("NtOpenFile failed 0x%lx\n", st); return 1; }

    unsigned char buf[8192];
    wchar_t star[] = L"*";
    USTR pattern = { sizeof star - 2, sizeof star, star };
    for (int restart = 1;; restart = 0) {
        IOB io2 = { 0 };
        st = pQ(h, NULL, NULL, NULL, &io2, buf, sizeof buf, 1 /*FileDirectoryInformation*/, TRUE, restart ? &pattern : NULL);
        if (st) break;
        FDI *e = (FDI *)buf;
        if (!e->Next && !e->NameLen) break;
        char out[512];
        int n = WideCharToMultiByte(CP_UTF8, 0, e->Name, e->NameLen / 2, out, sizeof out - 1, NULL, NULL);
        if (n > 0) { out[n] = 0; printf("%s\n", out); }
        if (!e->Next) break;
    }
    return 0;
}
