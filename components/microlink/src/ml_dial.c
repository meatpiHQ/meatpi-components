/**
 * @file ml_dial.c
 * @brief WiCAN (2026-11-13): a TCP connect with a bound, that a stop of the
 *        client cuts short.
 *
 * lwIP's blocking connect() ends when the SYN retries end, and the socket's
 * send timeout does not apply to it: about 18 s to a host that drops the
 * SYN, about a minute into an uplink that just went. The relay task and the
 * control task both dialled that way. A stop asked for meanwhile waited out
 * its 30 s join, kept the client and tried again (the ownership rule of the
 * same day makes that safe), so after a Wi-Fi blip the VPN was back in 66 to
 * 70 s instead of 14 to 17 (bench 2026-11-13: six of nine uplink losses
 * caught a relay dial in flight).
 *
 * Here the connect is non-blocking and waited for with select() in short
 * slices, each of which looks at the stop request first: the dial ends
 * within a slice of the stop, and after the caller's bound at the latest.
 * lwIP lets a socket in a non-blocking connect be closed (in a blocking one
 * it does not), which is what the caller does with a dial that failed.
 */
#include "microlink_internal.h"

#include <errno.h>
#include <fcntl.h>

#include "lwip/sockets.h"

#define ML_DIAL_SLICE_MS 200u

int ml_dial(microlink_t *ml, int sock, const struct sockaddr *addr,
            uint32_t addr_len, uint32_t timeout_ms)
{
#ifdef CONFIG_ML_ENABLE_CELLULAR
    if (ml_at_socket_is_at_fd(sock))
    {
        /* the modem's own connect (not built on WiCAN): as it always was */
        return ml_connect(sock, addr, (socklen_t)addr_len);
    }
#endif

    int flags = ml_fcntl(sock, F_GETFL, 0);

    if (flags < 0 || ml_fcntl(sock, F_SETFL, flags | O_NONBLOCK) < 0)
    {
        /* a socket that cannot be made non-blocking: as it always was */
        return ml_connect(sock, addr, (socklen_t)addr_len);
    }

    int rc = ml_connect(sock, addr, (socklen_t)addr_len);

    if (rc < 0 && errno == EINPROGRESS)
    {
        int result = ETIMEDOUT;

        for (uint32_t waited = 0; waited < timeout_ms; waited += ML_DIAL_SLICE_MS)
        {
            if (xEventGroupGetBits(ml->events) & ML_EVT_SHUTDOWN_REQUEST)
            {
                result = ECANCELED;
                break;
            }

            fd_set writable;
            fd_set failed;
            struct timeval slice = { .tv_sec = 0, .tv_usec = ML_DIAL_SLICE_MS * 1000 };

            FD_ZERO(&writable);
            FD_SET(sock, &writable);
            FD_ZERO(&failed);
            FD_SET(sock, &failed);

            int ready = ml_select_fds(sock + 1, NULL, &writable, &failed, &slice);

            if (ready < 0)
            {
                result = errno != 0 ? errno : EIO;
                break;
            }

            if (ready > 0)
            {
                /* connected, or refused: the socket's own word */
                int pending = 0;
                socklen_t len = sizeof(pending);

                if (getsockopt(sock, SOL_SOCKET, SO_ERROR, &pending, &len) < 0)
                {
                    pending = errno != 0 ? errno : EIO;
                }

                result = pending;
                break;
            }
        }

        rc = result == 0 ? 0 : -1;
        errno = result;
    }

    if (rc == 0)
    {
        /* blocking again: the callers' reads run on the socket's timeouts */
        (void)ml_fcntl(sock, F_SETFL, flags);
    }

    return rc;
}
