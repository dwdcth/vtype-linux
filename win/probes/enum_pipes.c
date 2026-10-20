/* enum_pipes.c —— wine 下管道枚举实验：FindFirstFile 报 66，试 NT 原生目录枚举 */
#include <stdio.h>
#include <windows.h>
#include <winternl.h>

typedef LONG NTSTATUS_;
#define STATUS_MORE_ENTRIES ((NTSTATUS_)0x105)

typedef struct _ODI {
    UNICODE_STRING Name;
    UNICODE_STRING TypeName;
} ODI;

extern NTSTATUS_ __stdcall NtQueryDirectoryObject(HANDLE, PVOID, ULONG, BOOLEAN,
                                                   BOOLEAN, PULONG, PULONG);
extern NTSTATUS_ __stdcall NtOpenDirectoryObject(PHANDLE, ACCESS_MASK,
                                                  POBJECT_ATTRIBUTES);

static void dump_dir(const wchar_t *path)
{
    UNICODE_STRING us;
    us.Length = (USHORT)(wcslen(path) * 2);
    us.MaximumLength = us.Length + 2;
    us.Buffer = (PWSTR)path;
    OBJECT_ATTRIBUTES oa;
    InitializeObjectAttributes(&oa, &us, 0, NULL, NULL);
    HANDLE h = NULL;
    NTSTATUS_ st = NtOpenDirectoryObject(&h, 1 /*DIRECTORY_QUERY*/, &oa);
    printf("%ls: open=%08lx\n", path, (long)st);
    if (st < 0 || !h) return;
    BYTE buf[8192];
    ULONG ctx = 0, rl = 0;
    int total = 0;
    for (;;) {
        st = NtQueryDirectoryObject(h, buf, sizeof buf, FALSE, TRUE, &ctx, &rl);
        if (st < 0) { printf("  query=%08lx\n", (long)st); break; }
        ODI *odi = (ODI *)buf;
        while (odi->Name.Buffer) {
            if (wcsncmp(odi->Name.Buffer, L"wetype.", 7) == 0)
                printf("  MATCH: %ls (%ls)\n", odi->Name.Buffer,
                       odi->TypeName.Buffer ? odi->TypeName.Buffer : L"?");
            total++;
            odi++;
        }
        printf("  [%ls] this batch %ld entries (ctx=%lu st=%08lx)\n",
               path, (long)((ODI *)buf == odi ? 0 : 0), ctx, (long)st);
        break;   /* 只看第一批 */
    }
    printf("  total in first batch: %d\n", total);
    NtClose(h);
}

int main(void)
{
    dump_dir(L"\\??\\Pipe");
    dump_dir(L"\\Device\\NamedPipe");
    return 0;
}
