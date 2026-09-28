#define _GNU_SOURCE
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/*
 * Small ARM64 launcher. The actual recovery policy lives in the sibling
 * usb0-helper.sh script so it can be inspected and updated independently.
 * The sibling usb0-native binary provides the private low-level networking
 * implementation used by that script.
 */
int main(int argc, char **argv) {
    char path[PATH_MAX];
    if (!argv[0] || argv[0][0] != '/') {
        ssize_t n = readlink("/proc/self/exe", path, sizeof(path) - 1);
        if (n <= 0) { perror("readlink"); return 127; }
        path[n] = '\0';
    } else {
        snprintf(path, sizeof(path), "%s", argv[0]);
    }
    char *slash = strrchr(path, '/');
    if (!slash) { fprintf(stderr, "cannot locate helper script\n"); return 127; }
    strcpy(slash + 1, "usb0-helper.sh");

    char *args[64];
    int n = 0;
    args[n++] = "sh";
    args[n++] = path;
    for (int i = 1; i < argc && n < 63; ++i) args[n++] = argv[i];
    args[n] = NULL;
    execv("/system/bin/sh", args);
    perror("execv /system/bin/sh");
    return 127;
}
