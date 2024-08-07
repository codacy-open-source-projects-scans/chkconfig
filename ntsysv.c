/* Copyright 1997-2007 Red Hat, Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 2,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA
 */
#include <alloca.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <libintl.h>
#include <locale.h>
#include <newt.h>
#include <popt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define _(String) gettext((String))
#define UNIT_FILE_MAX 256

#include "leveldb.h"

/* return 1 on cancel, 2 on error, 0 on success */
static int servicesWindow(struct service *services, int numServices, int levels,
                          int backButton) {
    newtComponent label, subform, ok, cancel;
    newtComponent *checkboxes, form, curr, blank;
    newtComponent sb = NULL;
    newtGrid grid, subgrid, buttons;
    char *states;
    int i, done = 0, update = 0, j;
    int width, height;
    struct newtExitStruct e;
    int count = 0;
    int last = 0;

    newtPushHelpLine(_("Press <F1> for more information on a service."));

    newtGetScreenSize(&width, &height);
    width = width > 80 ? width - 60 : 20;
    height = height > 25 ? height - 17 : 8;

    sb = newtVerticalScrollbar(-1, -1, height, NEWT_COLORSET_CHECKBOX,
                               NEWT_COLORSET_ACTCHECKBOX);

    subform = newtForm(sb, NULL, 0);
    newtFormSetBackground(subform, NEWT_COLORSET_CHECKBOX);
    newtFormSetHeight(subform, height);

    checkboxes = alloca(sizeof(*checkboxes) * numServices);
    states = alloca(sizeof(*states) * numServices);

    for (i = 0; i < numServices; i++, count++) {
        if (last != services[i].type) {
            newtFormAddComponent(
                subform,
                newtCompactButton(-1, count,
                                  services[i].type == TYPE_INIT_D
                                      ? "SysV initscripts"
                                      : services[i].type == TYPE_XINETD
                                            ? "xinetd services"
                                            : services[i].type == TYPE_SYSTEMD
                                                  ? "systemd services"
                                                  : "Unknown"));
            count++;
            last = services[i].type;
        }
        if (services[i].type == TYPE_XINETD) {
            checkboxes[i] =
                newtCheckbox(-1, count, services[i].name,
                             services[i].levels ? '*' : ' ', NULL, states + i);
        } else if (services[i].type == TYPE_SYSTEMD) {
            checkboxes[i] =
                newtCheckbox(-1, count, services[i].name,
                             services[i].enabled ? '*' : ' ', NULL, states + i);
        } else {
            checkboxes[i] =
                newtCheckbox(-1, count, services[i].name,
                             (services[i].currentLevels & levels) ? '*' : ' ',
                             NULL, states + i);
        }
        newtFormAddComponent(subform, checkboxes[i]);
    }

    newtFormSetWidth(subform, width);

    buttons = newtButtonBar(_("Ok"), &ok, backButton ? _("Back") : _("Cancel"),
                            &cancel, NULL);

    blank = newtForm(NULL, NULL, 0);
    newtFormSetWidth(blank, 2);
    newtFormSetHeight(blank, height);
    newtFormSetBackground(blank, NEWT_COLORSET_CHECKBOX);

    subgrid =
        newtGridHCloseStacked(NEWT_GRID_COMPONENT, subform, NEWT_GRID_COMPONENT,
                              blank, NEWT_GRID_COMPONENT, sb, NULL);

    label = newtTextboxReflowed(-1, -1, _("What services should be "
                                          "automatically started?"),
                                30, 0, 20, 0);
    grid = newtGridBasicWindow(label, subgrid, buttons);

    form = newtForm(NULL, NULL, 0);
    newtGridAddComponentsToForm(grid, form, 1);
    newtGridWrappedWindow(grid, _("Services"));
    newtGridFree(grid, 1);

    newtFormAddHotKey(form, NEWT_KEY_F1);

    while (!done) {
        newtFormRun(form, &e);

        if (e.reason == NEWT_EXIT_HOTKEY) {
            if (e.u.key == NEWT_KEY_F12) {
                done = 1;
                update = 1;
            } else {
                /* must be F1 */
                curr = newtFormGetCurrent(subform);
                for (i = 0; i < numServices; i++)
                    if (curr == checkboxes[i])
                        break;

                if (i < numServices && services[i].desc)
                    newtWinMessage(services[i].name, _("Ok"), services[i].desc);
            }
        } else if (e.u.co == ok || e.u.co == cancel) {
            done = 1;
            update = (e.u.co == ok);
        }
    }

    newtPopWindow();
    newtFormDestroy(form);

    if (!update)
        return 1;

    for (i = 0; i < numServices; i++) {
        if (services[i].type == TYPE_XINETD) {
            if ((services[i].enabled && states[i] != '*') ||
                (!services[i].enabled && states[i] == '*'))
                setXinetdService(services[i], states[i] == '*');
        } else if (services[i].type == TYPE_SYSTEMD) {
            char *cmd = NULL;
            int en = 0;
            if (services[i].enabled && states[i] != '*')
                en = 0;
            else if (!services[i].enabled && states[i] == '*')
                en = 1;
            else
                continue;
            asprintf(&cmd, "/usr/bin/systemctl %s %s >/dev/null 2>&1",
                     en ? "enable" : "disable", services[i].name);
            if (cmd == NULL)
                return 1;
            system(cmd);
            free(cmd);
        } else {
            for (j = 0; j < 7; j++) {
                if (levels & (1 << j))
                    doSetService(services[i], j, states[i] == '*');
            }
        }
    }

    return 0;
}

static int serviceNameCmp(const void *a, const void *b) {
    const struct service *first = a;
    const struct service *second = b;

    if (first->type != second->type) {
        return first->type - second->type;
    }

    return strcmp(first->name, second->name);
}

int getSystemdServices(struct service **servicesPtr, int *numServicesPtr) {
    FILE *sys = NULL;
    char service[UNIT_FILE_MAX + 1];
    int i;
    int r = 0;
    int c = ' ';
    struct service *services = NULL;
    struct service *p = NULL;
    int numServices = 0;
    int numServicesAlloced = 10;

    numServicesAlloced = 10;
    services = malloc(sizeof(*services) * numServicesAlloced);
    if (services == NULL)
        return -ENOMEM;

    sys = popen("systemctl list-unit-files --no-legend --no-pager", "r");
    if (!sys) {
        r = -1;
        goto finish;
    }

    while (c != EOF) {
        int enabled;
        char *suffix;

        for (i = 0; (c = fgetc(sys)) != EOF && c != ' ' && i < UNIT_FILE_MAX;
             i++)
            service[i] = c;

        if (i == UNIT_FILE_MAX) {
            while ((c = fgetc(sys)) != '\n' && c != EOF)
                ;
            continue;
        }

        service[i] = '\0';
        if (c == EOF)
            break;

        while ((c = fgetc(sys)) == ' ')
            ;
        if (c == EOF)
            break;

        enabled = c == 'e' ? 1 : c == 'd' ? 0 : -1;
        while ((c = fgetc(sys)) != '\n' && c != EOF)
            ;

        if (enabled == -1)
            continue;

        if (strrchr(service, '@'))
            continue;

        suffix = strrchr(service, '.');
        if (suffix == NULL)
            continue;
        suffix++;
        if (strcmp(suffix, "service") && strcmp(suffix, "socket"))
            continue;

        if (numServices == numServicesAlloced) {
            numServicesAlloced += 10;
            p = realloc(services, numServicesAlloced * sizeof(*services));
            if (p == NULL) {
                r = -ENOMEM;
                goto finish;
            }
            services = p;
        }

        bzero(services + numServices, sizeof(struct service));

        services[numServices].name = strdup(service);
        if (services[numServices].name == NULL) {
            r = -ENOMEM;
            goto finish;
        }

        readSystemdUnitProperty(service, "Description",
                                &services[numServices].desc);

        services[numServices].enabled = enabled;
        services[numServices].type = TYPE_SYSTEMD;
        numServices++;
    }

    p = realloc(*servicesPtr,
                (*numServicesPtr + numServices) * sizeof(struct service));
    if (p == NULL) {
        r = -ENOMEM;
        goto finish;
    }
    *servicesPtr = p;
    memcpy(*servicesPtr + *numServicesPtr, services,
           numServices * sizeof(struct service));
    *numServicesPtr = *numServicesPtr + numServices;

finish:
    if (services)
        free(services);

    if (sys)
        pclose(sys);

    return r;
}

static int getServices(struct service **servicesPtr, int *numServicesPtr,
                       int backButton, int honorHide) {
    DIR *dir;
    struct dirent *ent;
    struct stat sb;
    char fn[1024];
    struct service *services;
    int numServices = 0;
    int numServicesAlloced, rc;
    int err = 0;
    int systemd = systemdActive();

    numServicesAlloced = 10;
    services = malloc(sizeof(*services) * numServicesAlloced);

    if (!(dir = opendir(RUNLEVELS "/init.d"))) {
        fprintf(stderr, "failed to open " RUNLEVELS "/init.d: %s\n",
                strerror(errno));
        free(services);
        return 2;
    }

    while ((ent = readdir(dir))) {
        if (strchr(ent->d_name, '~') || strchr(ent->d_name, ',') ||
            (ent->d_name[0] == '.'))
            continue;

        if (systemd && isOverriddenBySystemd(ent->d_name))
            continue;
        sprintf(fn, RUNLEVELS "/init.d/%s", ent->d_name);
        if (stat(fn, &sb)) {
            fprintf(stderr, _("error reading info for service %s: %s\n"),
                    ent->d_name, strerror(errno));
            continue;
        }
        if (!S_ISREG(sb.st_mode))
            continue;

        if (numServices == numServicesAlloced) {
            numServicesAlloced += 10;
            services =
                realloc(services, numServicesAlloced * sizeof(*services));
        }

        rc = readServiceInfo(ent->d_name, TYPE_INIT_D, services + numServices,
                             honorHide);

        if (!rc) {
            int i;

            rc = -2;
            for (i = 0; i < 7; i++) {
                if (isConfigured(ent->d_name, i, NULL, NULL)) {
                    rc = 0;
                    break;
                }
            }
        }

        if (rc == -1) {
            fprintf(stderr, _("error reading info for service %s: %s\n"),
                    ent->d_name, strerror(errno));
            continue;
        } else if (!rc)
            numServices++;
    }

    closedir(dir);

    if (!stat("/usr/sbin/xinetd", &sb)) {
        if (!(dir = opendir(XINETDDIR))) {
            fprintf(stderr, "failed to open " XINETDDIR ": %s\n",
                    strerror(errno));
            freeServices(services, numServices);
            return 2;
        }

        while ((ent = readdir(dir))) {
            if (strchr(ent->d_name, '~') || strchr(ent->d_name, ',') ||
                strchr(ent->d_name, '.'))
                continue;

            sprintf(fn, "%s/%s", XINETDDIR, ent->d_name);
            if (stat(fn, &sb)) {
                err = errno;
                continue;
            }
            if (!S_ISREG(sb.st_mode))
                continue;

            if (numServices == numServicesAlloced) {
                numServicesAlloced += 10;
                services =
                    realloc(services, numServicesAlloced * sizeof(*services));
            }

            rc = readXinetdServiceInfo(ent->d_name, services + numServices);

            if (rc == -1) {
                fprintf(stderr, _("error reading info for service %s: %s\n"),
                        ent->d_name, strerror(errno));
                closedir(dir);
                freeServices(services, numServices);
                return 2;
            } else if (!rc)
                numServices++;
        }

        if (err) {
            fprintf(stderr, _("error reading from directory %s: %s\n"),
                    XINETDDIR, strerror(err));
            freeServices(services, numServices);
            return 1;
        }

        closedir(dir);
    }

    getSystemdServices(&services, &numServices);

    qsort(services, numServices, sizeof(*services), serviceNameCmp);

    *servicesPtr = services;
    *numServicesPtr = numServices;

    return 0;
}

int main(int argc, const char **argv) {
    struct service *services;
    int numServices;
    int levels = -1;
    char *levelsStr = NULL;
    char *progName;
    poptContext optCon;
    int rc, backButton = 0, hide = 0;
    struct poptOption optionsTable[] = {
        {"back", '\0', 0, &backButton, 0},
        {"level", '\0', POPT_ARG_STRING, &levelsStr, 0},
        {"hide", '\0', 0, &hide, 0},
        {0, 0, 0, 0, 0}};

    setlocale(LC_ALL, "");
    bindtextdomain("chkconfig", "/usr/share/locale");
    textdomain("chkconfig");

    progName = (char *)argv[0];

    if (getuid() != 0) {
        fprintf(stderr, _("You must be root to run %s.\n"), progName);
        exit(1);
    }

    optCon = poptGetContext(progName, argc, argv, optionsTable, 0);
    poptReadDefaultConfig(optCon, 1);

    if ((rc = poptGetNextOpt(optCon)) < -1) {
        fprintf(stderr, "%s: %s\n",
                poptBadOption(optCon, POPT_BADOPTION_NOALIAS),
                poptStrerror(rc));
        exit(1);
    }

    if (levelsStr) {
        levels = parseLevels(levelsStr, 0);
        if (levels == -1) {
            fprintf(stderr, _("bad argument to --levels\n"));
            exit(2);
        }
    }

    if (getServices(&services, &numServices, backButton, hide))
        return 1;
    if (!numServices) {
        fprintf(stderr, _("No services may be managed by ntsysv!\n"));
        return 2;
    }

    newtInit();
    newtCls();

    newtPushHelpLine(NULL);
    newtDrawRootText(0, 0, "ntsysv " VERSION " - (C) 2000-2001 Red Hat, Inc. ");

    if (levels == -1)
        levels = parseLevels("2345", 0);

    rc = servicesWindow(services, numServices, levels, backButton);

    newtFinished();

    return rc;
}
