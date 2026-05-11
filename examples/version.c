/*
 * version.c -- minimal smoke test. Just calls eciVersion.
 * Build:  gcc version.c -o version -ldl
 * Run:    ./version /path/to/eci.so
 */

#include <dlfcn.h>
#include <stdio.h>

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <path-to-eci.so>\n", argv[0]);
        return 1;
    }
    void *h = dlopen(argv[1], RTLD_NOW);
    if (!h) {
        fprintf(stderr, "dlopen failed: %s\n", dlerror());
        return 1;
    }
    void (*eciVersion)(char *) = dlsym(h, "eciVersion");
    if (!eciVersion) {
        fprintf(stderr, "dlsym(eciVersion) failed: %s\n", dlerror());
        return 1;
    }
    char buf[64] = {0};
    eciVersion(buf);
    printf("eciVersion: '%s'\n", buf);
    return 0;
}
