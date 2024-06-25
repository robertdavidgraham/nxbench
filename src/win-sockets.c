int iso_forbids_empty_file_winsock;
#ifdef _WIN32
#include "win-sockets.h"

int
sock_nonblocking(socket_t fd) {
    unsigned long flags = 1;
    ioctlsocket(fd, FIONBIO, &flags);
    return 0;
}


#define MAX_ERROR_MESSAGE_LENGTH 256

const char* sock_strerror(int errorCode) {
    static char errorMessage[MAX_ERROR_MESSAGE_LENGTH];
    DWORD result;

    result = FormatMessageA(
        FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        NULL,
        errorCode,
        MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        errorMessage,
        MAX_ERROR_MESSAGE_LENGTH,
        NULL
    );

    if (result == 0) {
        strcpy(errorMessage, "Failed to retrieve error message");
    }
    else {
        // Remove newline characters at the end of the message
        char* newline;
        while ((newline = strrchr(errorMessage, '\r')) != NULL ||
            (newline = strrchr(errorMessage, '\n')) != NULL) {
            *newline = '\0';
        }
    }

    return errorMessage;
}

#endif
