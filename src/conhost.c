
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>

int main(void) {
    HANDLE hIn = CreateFileW(L"CONIN$", GENERIC_READ|GENERIC_WRITE,
                             FILE_SHARE_READ|FILE_SHARE_WRITE,
                             NULL, OPEN_EXISTING, 0, NULL);

    fprintf(stderr, "hIn=%p gle=%lu\n", hIn, GetLastError());

    DWORD mode = 0xAAAAAAAA;
    BOOL ok = GetConsoleMode(hIn, &mode);

    fprintf(stderr, "ok=%d gle=%lu mode=0x%08lx\n",
            (int)ok, GetLastError(), (unsigned long)mode);
}
