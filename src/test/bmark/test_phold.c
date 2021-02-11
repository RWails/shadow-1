/*
 * The Shadow Simulator
 * See LICENSE for licensing information
 */

#include <arpa/inet.h>
#include <errno.h>
#include <glib.h>
#include <math.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/timerfd.h>
#include <sys/types.h>
#include <unistd.h>

#if 1 /* #ifdef DEBUG */
#define PHOLD_MAGIC 0xABBABAAB
#define PHOLD_ASSERT(obj) g_assert(obj && (obj->magic == PHOLD_MAGIC))
#else
#define PHOLD_MAGIC 0
#define PHOLD_ASSERT(obj)
#endif

#define phold_error(...)     _phold_log(G_LOG_LEVEL_ERROR, __FILE__, __LINE__, __FUNCTION__, __VA_ARGS__)
#define phold_critical(...)  _phold_log(G_LOG_LEVEL_CRITICAL, __FILE__, __LINE__, __FUNCTION__, __VA_ARGS__)
#define phold_warning(...)   _phold_log(G_LOG_LEVEL_WARNING, __FILE__, __LINE__, __FUNCTION__, __VA_ARGS__)
#define phold_message(...)   _phold_log(G_LOG_LEVEL_MESSAGE, __FILE__, __LINE__, __FUNCTION__, __VA_ARGS__)
#define phold_info(...)      _phold_log(G_LOG_LEVEL_INFO, __FILE__, __LINE__, __FUNCTION__, __VA_ARGS__)
#define phold_debug(...)     _phold_log(G_LOG_LEVEL_DEBUG, __FILE__, __LINE__, __FUNCTION__, __VA_ARGS__)

#define PHOLD_LISTEN_PORT 8998

// It is assumed that hosts in the experiment are assigned IP addresses
// sequentially starting at this address.
enum { BASE_IP_ADDR = 184549376 };

typedef struct _PHold PHold;
struct _PHold {
    GString* basename;
    guint64 quantity;
    guint64 msgload;
    guint64 cpuload;
    guint64 size;

    guint64 num_peers;

    GString* hostname;
    gint listend;
    gint epolld_in;
    gint timerd;

    guint8* sendbuf; // len is size from above

    guint64 num_msgs_sent;
    guint64 num_msgs_sent_tot;
    guint64 num_bytes_sent;
    guint64 num_bytes_sent_tot;
    guint64 num_msgs_recv;
    guint64 num_msgs_recv_tot;
    guint64 num_bytes_recv;
    guint64 num_bytes_recv_tot;

    guint magic;
};

GLogLevelFlags pholdLogFilterLevel = G_LOG_LEVEL_INFO;

static const gchar* _phold_logLevelToString(GLogLevelFlags logLevel) {
    switch (logLevel) {
        case G_LOG_LEVEL_ERROR:
            return "error";
        case G_LOG_LEVEL_CRITICAL:
            return "critical";
        case G_LOG_LEVEL_WARNING:
            return "warning";
        case G_LOG_LEVEL_MESSAGE:
            return "message";
        case G_LOG_LEVEL_INFO:
            return "info";
        case G_LOG_LEVEL_DEBUG:
            return "debug";
        default:
            return "default";
    }
}

/* our test code only relies on a log function, so let's supply that implementation here */
static void _phold_log(GLogLevelFlags level, const gchar* fileName, const gint lineNum, const gchar* functionName, const gchar* format, ...) {
    if(level > pholdLogFilterLevel) {
        return;
    }

    va_list vargs;
    va_start(vargs, format);

    gchar* fileStr = fileName ? g_path_get_basename(fileName) : g_strdup("n/a");
    const gchar* functionStr = functionName ? functionName : "n/a";

    GDateTime* dt = g_date_time_new_now_local();
    GString* newformat = g_string_new(NULL);

    g_string_append_printf(newformat, "%04i-%02i-%02i %02i:%02i:%02i %"G_GINT64_FORMAT".%06i [%s] [%s:%i] [%s] %s",
            g_date_time_get_year(dt), g_date_time_get_month(dt), g_date_time_get_day_of_month(dt),
            g_date_time_get_hour(dt), g_date_time_get_minute(dt), g_date_time_get_second(dt),
            g_date_time_to_unix(dt), g_date_time_get_microsecond(dt),
            _phold_logLevelToString(level), fileStr, lineNum, functionName, format);

    gchar* message = g_strdup_vprintf(newformat->str, vargs);
    g_print("%s\n", message);
    g_free(message);

    g_string_free(newformat, TRUE);
    g_date_time_unref(dt);
    g_free(fileStr);

    va_end(vargs);
}

static double _phold_get_uniform_double() {
    // return a uniform value between 0 and 1
    return ((double)random()) / ((double)RAND_MAX);
}

static double _phold_generate_normal_deviate() {
    // Box-Muller method
    double u = _phold_get_uniform_double();
    double v = _phold_get_uniform_double();
    double x = sqrt(-2 * log(u)) * cos(2 * G_PI * v);
    //double y = sqrt(-2 * log(u)) * sin(2 * M_PI * v);
    return x;
}

static double _phold_generate_normal(double location, double scale) {
    // location is mu, mean of normal distribution
    // scale is sigma, sqrt of the variance of normal distribution
    double z = _phold_generate_normal_deviate();
    return location + (scale * z);
}

static double _phold_generate_exponential(double rate) {
    // inverse transform sampling
    double u = _phold_get_uniform_double();
    return -log(u)/rate;
}

static in_addr_t _phold_chooseNode(PHold* phold) {
    PHOLD_ASSERT(phold);

    double r = _phold_get_uniform_double();

    double cumulative = 0.0;
    double normWeight = 1.0 / ((double)phold->num_peers);

    for (gint64 i = 0; i < phold->num_peers; i++) {
        cumulative += normWeight;
        if(cumulative >= r) {
            return i;
        }
    }

    return -1;
}

static int _phold_sendToNode(PHold* phold, gint64 peerIndex, in_port_t port, void* msg,
                             size_t msg_len) {
    int result = 0;

    /* create a new socket */
    gint socketd = socket(AF_INET, (SOCK_DGRAM | SOCK_NONBLOCK), 0);

    /* get node address for this message */
    struct sockaddr_in node = {0};
    node.sin_family = AF_INET;
    node.sin_addr.s_addr = htonl(1 + BASE_IP_ADDR + peerIndex);
    node.sin_port = port;
    socklen_t len = sizeof(struct sockaddr_in);

    /* send the message to the node */
    ssize_t b = sendto(socketd, msg, msg_len, 0, (struct sockaddr*)(&node), len);
    if (b > 0) {
        phold->num_msgs_sent++;
        phold->num_msgs_sent_tot++;
        phold->num_bytes_sent += b;
        phold->num_bytes_sent_tot += b;
        phold_debug("host '%s' sent %i byte%s to host '%s%" G_GINT64_FORMAT "'",
                    phold->hostname->str, (gint)b, b == 1 ? "" : "s", phold->basename,
                    peerIndex + 1);
        result = TRUE;
    } else if (b < 0) {
        phold_warning("sendto(): returned %i host '%s' errno %i: %s", (gint)b, phold->hostname->str,
                      errno, g_strerror(errno));
        result = FALSE;
    }

    /* close socket */
    close(socketd);

    return result;
}

static void _phold_sendNewMessage(PHold* phold) {
    PHOLD_ASSERT(phold);

    /* pick a node */
    gint64 peerIndex = _phold_chooseNode(phold);

    if (peerIndex >= 0) {
        in_port_t port = (in_port_t)htons(PHOLD_LISTEN_PORT);

        _phold_sendToNode(phold, peerIndex, port, phold->sendbuf, phold->size);
    } else {
        phold_warning("Unable to choose valid peer index");
    }
}

static void _phold_bootstrapMessages(PHold* phold) {
    phold_info("sending %" G_GUINT64_FORMAT " messages to bootstrap", phold->msgload);
    for (guint64 i = 0; i < phold->msgload; i++) {
        _phold_sendNewMessage(phold);
    }
}

static gint _phold_startListening(PHold* phold) {
    PHOLD_ASSERT(phold);

    /* create the socket and get a socket descriptor */
    phold->listend = socket(AF_INET, (SOCK_DGRAM | SOCK_NONBLOCK), 0);
    if (phold->timerd < 0) {
        phold_warning("Unable to create listener socket, error %i: %s", errno, strerror(errno));
        return -1;
    }

    phold_info("opened listener at socket %i", phold->listend);

    /* setup the socket address info, client has outgoing connection to server */
    struct sockaddr_in bindAddr;
    memset(&bindAddr, 0, sizeof(bindAddr));
    bindAddr.sin_family = AF_INET;
    bindAddr.sin_addr.s_addr = htonl(INADDR_ANY);
    bindAddr.sin_port = htons(PHOLD_LISTEN_PORT);

    /* bind the socket to the server port */
    gint result = bind(phold->listend, (struct sockaddr *) &bindAddr, sizeof(bindAddr));
    if (result < 0) {
        phold_warning("Unable to bind listener socket, error %i: %s", errno, strerror(errno));
        return -1;
    }

    return 0;
}

static gint _phold_startHeartbeatTimer(PHold* phold) {
    PHOLD_ASSERT(phold);

    phold->timerd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK);
    if (phold->timerd < 0) {
        phold_warning("Unable to create heartbeat timer, error %i: %s", errno, strerror(errno));
        return -1;
    }

    phold_info("opened timer at timerfd %i", phold->timerd);

    struct itimerspec heartbeat = {.it_value.tv_sec = 1, .it_interval.tv_sec = 1};
    int result = timerfd_settime(phold->timerd, 0, &heartbeat, NULL);
    if (result < 0) {
        phold_warning(
            "Unable to set timeout on heartbeat timer, error %i: %s", errno, strerror(errno));
        return -1;
    }

    return 0;
}

static inline void _phold_logHeartbeatMessage(PHold* phold) {
    phold_info("%s: "
               "heartbeat: "
               "msgs_sent=%" G_GUINT64_FORMAT " msgs_recv=%" G_GUINT64_FORMAT " "
               "tot_msgs_sent=%" G_GUINT64_FORMAT " tot_msgs_recv=%" G_GUINT64_FORMAT " "
               "bytes_sent=%" G_GUINT64_FORMAT " bytes_recv=%" G_GUINT64_FORMAT " "
               "tot_bytes_sent=%" G_GUINT64_FORMAT " tot_bytes_recv=%" G_GUINT64_FORMAT,
               phold->hostname->str, phold->num_msgs_sent, phold->num_msgs_recv,
               phold->num_msgs_sent_tot, phold->num_msgs_recv_tot, phold->num_bytes_sent,
               phold->num_bytes_recv, phold->num_bytes_sent_tot, phold->num_bytes_recv_tot);
    phold->num_msgs_recv = 0;
    phold->num_msgs_sent = 0;
    phold->num_bytes_recv = 0;
    phold->num_bytes_sent = 0;
}

static void _phold_generateCPULoad(PHold* phold) {
    PHOLD_ASSERT(phold);
    // this is volatile to prevent the compiler from optimizing out the loop
    guint64 volatile result = 0;
    for (guint64 i = 0; i < phold->cpuload; i++) {
        result = i;
    }
}

static void _phold_wait_and_process_events(PHold* phold) {
    PHOLD_ASSERT(phold);

    guint8* buffer = g_new(guint8, phold->size);

    /* storage for collecting events from our epoll descriptor */
    struct epoll_event epevs[10];
    memset(epevs, 0, 10*sizeof(struct epoll_event));

    /* collect and process all events that are ready.
     * this is a blocking call, it will block until we have packets coming in on listend */
    gint nfds = epoll_wait(phold->epolld_in, epevs, 10, -1);
    for (gint i = 0; i < nfds; i++) {
        gint fd = epevs[i].data.fd;
        if (fd == phold->timerd) {
            _phold_logHeartbeatMessage(phold);
            /* Read the timer buf so that its not readable again until the next
             * interval. */
            uint64_t num_expirations = 0;
            read(phold->timerd, &num_expirations, sizeof(num_expirations));
            continue;
        }

        while(TRUE) {
            struct sockaddr_in addrbuf;
            memset(&addrbuf, 0, sizeof(struct sockaddr_in));
            socklen_t addrlen = sizeof(struct sockaddr_in);

            gssize nBytes = recvfrom(
                phold->listend, &buffer[0], phold->size, 0, (struct sockaddr*)(&addrbuf), &addrlen);

            if(nBytes <= 0) {
                break;
            }
            buffer[nBytes] = 0x0;
            phold->num_msgs_recv++;
            phold->num_msgs_recv_tot++;
            phold->num_bytes_recv += nBytes;
            phold->num_bytes_recv_tot += nBytes;

            in_addr_t ip = addrbuf.sin_addr.s_addr;
            char netbuf[INET_ADDRSTRLEN+1];
            memset(netbuf, 0, INET_ADDRSTRLEN+1);
            const char* netresult = inet_ntop(AF_INET, &ip, netbuf, INET_ADDRSTRLEN);

            phold_debug("got new message of %lu bytes from peer at %s", (unsigned int) nBytes, netbuf);

            // generate configured amount of cpu load
            _phold_generateCPULoad(phold);

            // send another message to maintain configured msgload
            _phold_sendNewMessage(phold);
        }
    }

    g_free(buffer);
}

static gint _phold_addToEpoll(PHold* phold, gint fd) {
    /* setup the events we will watch for */
    struct epoll_event ev;
    ev.events = EPOLLIN; // watch for readability
    ev.data.fd = fd;

    /* start watching fd */
    gint result = epoll_ctl(phold->epolld_in, EPOLL_CTL_ADD, fd, &ev);
    if (result < 0) {
        phold_warning("Unable to add fd %i to epoll, error %i: %s", fd, errno, strerror(errno));
        return -1;
    }
    return 0;
}

static int _phold_run(PHold* phold) {
    PHOLD_ASSERT(phold);

    /* create an epoll so we can wait for IO events */
    phold->epolld_in = epoll_create(1);
    if (phold->epolld_in < 0) {
        phold_warning("Unable to create epoll, error %i: %s", errno, strerror(errno));
        return EXIT_FAILURE;
    }
    phold_info("opened epoll %i", phold->epolld_in);

    if (_phold_startHeartbeatTimer(phold) || _phold_startListening(phold)) {
        return EXIT_FAILURE;
    }

    if (_phold_addToEpoll(phold, phold->listend) || _phold_addToEpoll(phold, phold->timerd)) {
        return EXIT_FAILURE;
    }

    phold_info("listening on fd %i, heartbeat timer on fd %i", phold->listend, phold->timerd);

    _phold_bootstrapMessages(phold);

    /* main loop - wait for events from the descriptors */
    struct epoll_event events[100];
    int nReadyFDs;
    phold_info("entering main loop to watch descriptors");

    while (1) {
        /* wait for some events */
        _phold_wait_and_process_events(phold);
    }

    phold_info("finished main loop, cleaning up");

    return EXIT_SUCCESS;
}

static gboolean _phold_parseOptions(PHold* phold, gint argc, gchar* argv[]) {
    PHOLD_ASSERT(phold);

    /* loglevel: one of 'debug' or 'info'
     * basename: name of the test nodes in shadow, without the integer suffix
     * quantity: number of test nodes running in the experiment with the same basename as this one
     * msgload: number of messages to generate when the simulation starts
     * cpuload: number of iterations of a CPU busy loop to run whenever a message is received
     */

    const gchar* usage =
        "loglevel=STR basename=STR quantity=INT msgload=INT cpuload=INT";

    gchar myname[128];
    g_assert(gethostname(&myname[0], 128) == 0);

    int num_params_found = 0;
#define ARGC_PEER 7

    if(argc == ARGC_PEER && argv != NULL) {
        /* argv[0] is the program name */
        for(gint i = 1; i < ARGC_PEER; i++) {
            gchar* token = argv[i];
            gchar** config = g_strsplit(token, (const gchar*) "=", 2);

            if(!g_ascii_strcasecmp(config[0], "loglevel")) {
                if(!g_ascii_strcasecmp(config[1], "debug")) {
                    pholdLogFilterLevel = G_LOG_LEVEL_DEBUG;
                } else {
                    pholdLogFilterLevel = G_LOG_LEVEL_INFO;
                }
                num_params_found++;
            } else if(!g_ascii_strcasecmp(config[0], "basename")) {
                phold->basename = g_string_new(config[1]);
                num_params_found++;
            } else if(!g_ascii_strcasecmp(config[0], "quantity")) {
                phold->quantity = g_ascii_strtoull(config[1], NULL, 10);
                phold->num_peers = phold->quantity;
                num_params_found++;
            } else if (!g_ascii_strcasecmp(config[0], "msgload")) {
                phold->msgload = g_ascii_strtoull(config[1], NULL, 10);
                num_params_found++;
            } else if (!g_ascii_strcasecmp(config[0], "cpuload")) {
                phold->cpuload = g_ascii_strtoull(config[1], NULL, 10);
                num_params_found++;
            } else if (!g_ascii_strcasecmp(config[0], "size")) {
                phold->size = g_ascii_strtoull(config[1], NULL, 10);
                num_params_found++;
            } else {
                phold_warning("skipping unknown config option %s=%s", config[0], config[1]);
            }

            g_strfreev(config);
        }
    }

    if (phold->basename != NULL && num_params_found == ARGC_PEER - 1) {
        phold->hostname = g_string_new(&myname[0]);

        phold->sendbuf = g_new0(guint8, phold->size);
        memset(phold->sendbuf, 666, phold->size);

        phold_info("successfully parsed options for %s: "
                   "basename=%s quantity=%" G_GUINT64_FORMAT " msgload=%" G_GUINT64_FORMAT
                   " cpuload=%" G_GUINT64_FORMAT " size=%" G_GUINT64_FORMAT,
                   &myname[0], phold->basename->str, phold->quantity, phold->msgload,
                   phold->cpuload, phold->size);

        return TRUE;
    } else {
        phold_error("invalid argv string for node %s", &myname[0]);
        phold_info("USAGE: %s", usage);

        return FALSE;
    }
}

static void _phold_free(PHold* phold) {
    PHOLD_ASSERT(phold);

    _phold_logHeartbeatMessage(phold);
    if(phold->listend > 0) {
        close(phold->listend);
    }
    if(phold->epolld_in > 0) {
        close(phold->epolld_in);
    }

    if(phold->hostname) {
        g_string_free(phold->hostname, TRUE);
    }
    if(phold->basename) {
        g_string_free(phold->basename, TRUE);
    }
    if (phold->sendbuf) {
        g_free(phold->sendbuf);
    }

    phold->magic = 0;
    g_free(phold);
}

static PHold* phold_new(gint argc, gchar* argv[]) {
    PHold* phold = g_new0(PHold, 1);
    phold->magic = PHOLD_MAGIC;

    if(!_phold_parseOptions(phold, argc, argv)) {
        _phold_free(phold);
        return NULL;
    }

    return phold;
}

/* program execution starts here */
int phold_main(int argc, char *argv[]) {
    pholdLogFilterLevel = G_LOG_LEVEL_INFO;

    /* get our hostname for logging */
    gchar hostname[128];
    memset(hostname, 0, 128);
    gethostname(hostname, 128);

    /* default to info level log until we make it configurable */
    phold_info("Initializing phold test on host %s process id %i", hostname, (gint)getpid());

    /* create the new state according to user inputs */
    PHold* phold = phold_new(argc, argv);
    if (!phold) {
        phold_error("Error initializing new instance");
        return -1;
    }

    int result = 0;
    if(phold) {
        result = _phold_run(phold);
    } else {
        phold_error("neither generator or peer are active");
        result = EXIT_FAILURE;
    }

    if(phold) {
        _phold_free(phold);
    }

    return result;
}
