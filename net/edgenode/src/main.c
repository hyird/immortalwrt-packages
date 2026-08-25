#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>

#include <ev.h>

#include "edge_config.h"
#include "edge_modem.h"
#include "edge_ws.h"

static bool reload_requested;

static void signal_received(struct ev_loop *loop, ev_signal *watcher, int events) {
    (void)events;
    reload_requested = watcher->signum == SIGHUP;
    ev_break(loop, EVBREAK_ALL);
}

int main(int argc, char **argv) {
    if (argc == 5 && strcmp(argv[1], "--initialize-modem") == 0)
        return edge_modem_initialize(argv[2], argv[3], argv[4]);
    if (argc == 6 && strcmp(argv[1], "--monitor-modem") == 0) {
        char *end = NULL;
        const unsigned long interval = strtoul(argv[5], &end, 10);
        if (end == argv[5] || *end != '\0' || interval == 0U || interval > 3600U)
            return EXIT_FAILURE;
        return edge_modem_monitor(argv[2], argv[3], argv[4], (unsigned)interval);
    }
    if (argc != 1) {
        fprintf(stderr,
                "usage: %s [--initialize-modem PORT STATUS WAN | "
                "--monitor-modem PORT STATUS WAN INTERVAL]\n",
                argv[0]);
        return EXIT_FAILURE;
    }

    openlog("edgenode", LOG_PID, LOG_DAEMON);
    struct ev_loop *loop = EV_DEFAULT;
    if (loop == NULL) {
        syslog(LOG_ERR, "cannot create event loop");
        return EXIT_FAILURE;
    }

    ev_signal terminate;
    ev_signal interrupt;
    ev_signal reload;
    ev_signal_init(&terminate, signal_received, SIGTERM);
    ev_signal_init(&interrupt, signal_received, SIGINT);
    ev_signal_init(&reload, signal_received, SIGHUP);
    ev_signal_start(loop, &terminate);
    ev_signal_start(loop, &interrupt);
    ev_signal_start(loop, &reload);

    edge_ws_app *app = calloc(1U, sizeof(*app));
    if (app == NULL) {
        syslog(LOG_ERR, "cannot allocate edge runtime");
        return EXIT_FAILURE;
    }

    int exit_code = EXIT_SUCCESS;
    do {
        reload_requested = false;
        edge_app_config config;
        char error[256];
        if (!edge_config_load(&config, error, sizeof(error))) {
            syslog(LOG_ERR, "configuration rejected: %s", error);
            exit_code = EXIT_FAILURE;
            break;
        }
        if (!edge_ws_app_init(app, loop, &config)) {
            syslog(LOG_ERR, "cannot initialize platform sessions");
            exit_code = EXIT_FAILURE;
            break;
        }
        syslog(LOG_INFO, "starting IMEI %s with %u platform session(s)", config.imei,
               (unsigned)config.platform_count);
        edge_ws_app_start(app);
        ev_run(loop, 0);
        edge_ws_app_stop(app);
        memset(app, 0, sizeof(*app));
    } while (reload_requested);

    ev_signal_stop(loop, &reload);
    ev_signal_stop(loop, &interrupt);
    ev_signal_stop(loop, &terminate);
    free(app);
    closelog();
    return exit_code;
}
