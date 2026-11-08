#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <arpa/inet.h>

#define PORT       "9000"
#define BACKLOG    10
#define CHUNK_SIZE 1024
#define DATA_FILE  "/var/tmp/aesdsocketdata"

static const char *ident = "aesdsocket";

/* Set by the signal handler, checked by the main loop. */
static volatile sig_atomic_t caught_signal = 0;

static void signal_handler(int signo)
{
    (void)signo;
    caught_signal = 1;
}

/* Write all len bytes to a file, retrying on partial writes. */
static int write_all(int fd, const char *buf, size_t len)
{
    while (len > 0) {
        ssize_t n = write(fd, buf, len);
        if (n == -1) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        buf += n;
        len -= (size_t)n;
    }
    return 0;
}

/* Send all len bytes to a socket, retrying on partial sends. */
static int send_all(int fd, const char *buf, size_t len)
{
    while (len > 0) {
        ssize_t n = send(fd, buf, len, MSG_NOSIGNAL);
        if (n == -1) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        buf += n;
        len -= (size_t)n;
    }
    return 0;
}

/* Append one complete packet (ending in '\n') to the data file. */
static void append_packet(const char *buf, size_t len)
{
    int fd = open(DATA_FILE, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd == -1) {
        syslog(LOG_ERR, "open %s failed: %m", DATA_FILE);
        return;
    }
    if (write_all(fd, buf, len) == -1)
        syslog(LOG_ERR, "write to %s failed: %m", DATA_FILE);
    close(fd);
}

/* Send the whole data file to the client, one chunk at a time. */
static int send_file(int client_fd)
{
    char buf[CHUNK_SIZE];
    int ret = 0;
    int fd = open(DATA_FILE, O_RDONLY);

    if (fd == -1) {
        syslog(LOG_ERR, "open %s for reading failed: %m", DATA_FILE);
        return -1;
    }

    for (;;) {
        ssize_t n = read(fd, buf, sizeof buf);
        if (n == 0)
            break;                      /* end of file */
        if (n == -1) {
            if (errno == EINTR)
                continue;
            syslog(LOG_ERR, "read %s failed: %m", DATA_FILE);
            ret = -1;
            break;
        }
        if (send_all(client_fd, buf, (size_t)n) == -1) {
            syslog(LOG_ERR, "send failed: %m");
            ret = -1;
            break;
        }
    }
    close(fd);
    return ret;
}

/* Receive from the client until it disconnects, one packet per newline. */
static void handle_client(int client_fd)
{
    char chunk[CHUNK_SIZE];
    char *packet = NULL;
    size_t packet_len = 0;
    int discarding = 0;

    for (;;) {
        ssize_t n = recv(client_fd, chunk, sizeof chunk, 0);
        if (n == 0)
            break;                      /* client closed the connection */
        if (n == -1) {
            if (errno == EINTR && !caught_signal)
                continue;
            if (errno != EINTR)
                syslog(LOG_ERR, "recv failed: %m");
            break;
        }

        /* A chunk may hold part of a packet, or several packets. */
        size_t offset = 0;
        while (offset < (size_t)n) {
            char *nl = memchr(chunk + offset, '\n', (size_t)n - offset);
            size_t take = nl ? (size_t)(nl - (chunk + offset)) + 1
                             : (size_t)n - offset;

            if (!discarding) {
                char *tmp = realloc(packet, packet_len + take);
                if (tmp == NULL) {
                    syslog(LOG_ERR, "realloc failed, discarding packet");
                    free(packet);
                    packet = NULL;
                    packet_len = 0;
                    discarding = 1;     /* skip bytes until the next '\n' */
                } else {
                    packet = tmp;
                    memcpy(packet + packet_len, chunk + offset, take);
                    packet_len += take;
                }
            }

            if (nl) {                   /* packet complete */
                if (!discarding)
                    append_packet(packet, packet_len);
                free(packet);
                packet = NULL;
                packet_len = 0;
                discarding = 0;

                if (send_file(client_fd) == -1)
                    return;             /* client gone; packet already freed */
            }
            offset += take;
        }
    }
    free(packet);   /* drop any unfinished packet; free(NULL) is safe */
}

/* Fork into the background: the parent exits, the child carries on. */
static int daemonize(void)
{
    pid_t pid = fork();
    if (pid == -1) {
        syslog(LOG_ERR, "fork failed: %m");
        return -1;
    }
    if (pid > 0)
        exit(EXIT_SUCCESS);             /* parent */

    if (setsid() == -1) {
        syslog(LOG_ERR, "setsid failed: %m");
        return -1;
    }
    if (chdir("/") == -1) {
        syslog(LOG_ERR, "chdir failed: %m");
        return -1;
    }

    int devnull = open("/dev/null", O_RDWR);
    if (devnull != -1) {
        dup2(devnull, STDIN_FILENO);
        dup2(devnull, STDOUT_FILENO);
        dup2(devnull, STDERR_FILENO);
        if (devnull > STDERR_FILENO)
            close(devnull);
    }
    return 0;
}

int main(int argc, char *argv[])
{
    int status;
    int opt;
    int daemon_mode = 0;
    int sockfd = -1;
    int new_fd = -1;
    int ret = -1;
    int yes = 1;
    struct addrinfo hints;
    struct addrinfo *res = NULL;
    struct sockaddr_storage their_addr;
    struct sockaddr_in *client;
    struct sigaction sa;
    socklen_t addr_size;
    char ip_str[INET_ADDRSTRLEN];

    openlog(ident, LOG_PID, LOG_DAEMON);

    while ((opt = getopt(argc, argv, "d")) != -1) {
        if (opt == 'd') {
            daemon_mode = 1;
        } else {
            syslog(LOG_ERR, "usage: %s [-d]", argv[0]);
            goto cleanup;
        }
    }

    /* No SA_RESTART: a blocked accept() must return EINTR on a signal. */
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    if (sigaction(SIGINT, &sa, NULL) == -1 ||
        sigaction(SIGTERM, &sa, NULL) == -1) {
        syslog(LOG_ERR, "sigaction failed: %m");
        goto cleanup;
    }

    memset(&hints, 0, sizeof hints);
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags    = AI_PASSIVE;

    status = getaddrinfo(NULL, PORT, &hints, &res);
    if (status != 0) {
        syslog(LOG_ERR, "getaddrinfo failed: %s", gai_strerror(status));
        goto cleanup;
    }

    sockfd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (sockfd == -1) {
        syslog(LOG_ERR, "socket failed: %m");
        goto cleanup;
    }

    if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes) == -1) {
        syslog(LOG_ERR, "setsockopt failed: %m");
        goto cleanup;
    }

    if (bind(sockfd, res->ai_addr, res->ai_addrlen) == -1) {
        syslog(LOG_ERR, "bind failed: %m");
        goto cleanup;
    }

    freeaddrinfo(res);
    res = NULL;

    /* Daemonize only after bind succeeded, so bind errors reach the caller. */
    if (daemon_mode && daemonize() == -1)
        goto cleanup;

    if (listen(sockfd, BACKLOG) == -1) {
        syslog(LOG_ERR, "listen failed: %m");
        goto cleanup;
    }

    while (!caught_signal) {
        addr_size = sizeof their_addr;
        new_fd = accept(sockfd, (struct sockaddr *)&their_addr, &addr_size);
        if (new_fd == -1) {
            if (errno != EINTR)
                syslog(LOG_ERR, "accept failed: %m");
            continue;                   /* loop condition checks the flag */
        }

        client = (struct sockaddr_in *)&their_addr;
        if (inet_ntop(AF_INET, &client->sin_addr, ip_str, sizeof ip_str) == NULL)
            strcpy(ip_str, "unknown");
        syslog(LOG_INFO, "Accepted connection from %s", ip_str);

        handle_client(new_fd);

        close(new_fd);
        new_fd = -1;
        syslog(LOG_INFO, "Closed connection from %s", ip_str);
    }

    syslog(LOG_INFO, "Caught signal, exiting");
    if (remove(DATA_FILE) == -1 && errno != ENOENT)
        syslog(LOG_ERR, "remove %s failed: %m", DATA_FILE);
    ret = 0;

cleanup:
    if (new_fd != -1)
        close(new_fd);
    if (sockfd != -1)
        close(sockfd);
    if (res != NULL)
        freeaddrinfo(res);
    closelog();
    return ret;
}
