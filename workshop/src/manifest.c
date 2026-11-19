#include "manifest.h"

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *trim(char *s)
{
    while (*s && isspace((unsigned char)*s)) s++;
    char *end = s + strlen(s);
    while (end > s && isspace((unsigned char)end[-1])) end--;
    *end = '\0';
    return s;
}

static void set_defaults(workshop_app_t *app, const char *id, const char *dir)
{
    memset(app, 0, sizeof(*app));
    snprintf(app->id, sizeof(app->id), "%s", id);
    snprintf(app->dir, sizeof(app->dir), "%s", dir);
    snprintf(app->name, sizeof(app->name), "%s", id);
}

static void read_one(const char *path, workshop_app_t *app)
{
    FILE *f = fopen(path, "r");
    if (!f) return;

    char line[512];
    while (fgets(line, sizeof(line), f)) {
        char *s = trim(line);
        if (!*s || *s == '#' || *s == ';') continue;
        char *eq = strchr(s, '=');
        if (!eq) continue;
        *eq = '\0';
        char *key = trim(s);
        char *value = trim(eq + 1);

        if (!strcmp(key, "name")) {
            snprintf(app->name, sizeof(app->name), "%s", value);
        } else if (!strcmp(key, "exec")) {
            snprintf(app->exec, sizeof(app->exec), "%s", value);
        } else if (!strcmp(key, "args")) {
            snprintf(app->args, sizeof(app->args), "%s", value);
        } else if (!strcmp(key, "error")) {
            snprintf(app->error, sizeof(app->error), "%s", value);
        } else if (!strcmp(key, "special")) {
            app->special = (!strcmp(value, "return_desktop") || atoi(value) != 0) ? 1 : 0;
        }
    }
    fclose(f);
}

static int cmp_app(const void *a, const void *b)
{
    const workshop_app_t *aa = (const workshop_app_t *)a;
    const workshop_app_t *bb = (const workshop_app_t *)b;
    return strcmp(aa->name, bb->name);
}

int manifest_load(const char *dir, workshop_app_t *apps, int max_apps)
{
    DIR *d = opendir(dir);
    if (!d) return 0;

    int count = 0;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL && count < max_apps) {
        if (ent->d_name[0] == '.') continue;

        char app_dir[512];
        char path[512];
        snprintf(app_dir, sizeof(app_dir), "%s/%s", dir, ent->d_name);
        snprintf(path, sizeof(path), "%s/app.conf", app_dir);

        FILE *probe = fopen(path, "r");
        if (!probe) continue;
        fclose(probe);

        workshop_app_t app;
        set_defaults(&app, ent->d_name, app_dir);
        read_one(path, &app);
        apps[count++] = app;
    }
    closedir(d);

    qsort(apps, (size_t)count, sizeof(apps[0]), cmp_app);
    return count;
}
