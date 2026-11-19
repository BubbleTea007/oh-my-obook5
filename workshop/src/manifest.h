#ifndef WORKSHOP_MANIFEST_H
#define WORKSHOP_MANIFEST_H

#define WORKSHOP_MAX_APPS 32

typedef struct {
    char id[64];
    char dir[512];
    char name[128];
    char exec[256];
    char args[128];
    char error[256];
    int special; /* 1 = return desktop */
} workshop_app_t;

/*
 * Scan <dir>/<app>/app.conf and load app metadata.
 * Missing or unreadable app.conf is ignored.
 * Returns the number of loaded apps.
 */
int manifest_load(const char *dir, workshop_app_t *apps, int max_apps);

#endif
