/* JAP - Just Another Portmapper
 * Copyright (C) 2005, 2006, 2012  Dmitry M. Shatrov
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 */

/* 2006.05.14 DMS - Fixed wrong wfds handling in proxy_thread
 *                  which caused infinite busy-looping after
 *                  connection to a remote server
 *
 * 2006.10.14 DMS - Removed "data lost" places, wrote complete send loops */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include <sys/types.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <pthread.h>

#define JAP_CONNECTION_TIMEOUT	5
#define JAP_BUFFER_SIZE 	15000

typedef struct {
    struct sockaddr_in remote_address;
    int client_socket;
} ProxyThreadArgs;

static int
set_address (const char *host_name, const char *service_name,
	     struct sockaddr_in *address)
{
    struct hostent *he;
    struct servent *se;
    char *endptr;
    short port;

    memset (address, 0, sizeof *address);
    address->sin_family = AF_INET;

    if (host_name != NULL) {
	if (!inet_aton (host_name, &address->sin_addr)) {
	    he = gethostbyname (host_name);
	    if (he == NULL)
		return -1;

	    address->sin_addr = *(struct in_addr*) he->h_addr;
	}
    } else
	address->sin_addr.s_addr = htonl (INADDR_ANY);

    port = strtol (service_name, &endptr, 0);
    if (*endptr == 0)
	address->sin_port = htons (port);
    else {
	se = getservbyname (service_name, "tcp");
	if (se == NULL)
	    return -1;

	address->sin_port = se->s_port;
    }

    return 0;
}

static void
print_usage (void)
{
    puts ("Usage: jap rhost rservice lservice [client]");
    puts ("   rhost    - IP address or host name of destination server");
    puts ("   rservice - destination port or service name");
    puts ("   lservice - local port or service name "
	  "to be mapped to host:port");
    puts ("   client   - if speceified, only this host will be served");
}

void* proxy_thread (ProxyThreadArgs *args)
{
    int     remote_socket;
    int     flags;
    char    remote_buf [JAP_BUFFER_SIZE];
    char    client_buf [JAP_BUFFER_SIZE];
    int     remote_len;
    int     client_len;
    int     remote_start;
    int     client_start;
    int     remote_shutdown;
    int     client_shutdown;
    fd_set  rfds, wfds, efds;
    struct timeval tv;
    int     optval;
    socklen_t optlen;
    int     retval;

    remote_socket = socket (AF_INET, SOCK_STREAM, 0);
    if (remote_socket == -1) {
	perror ("[proxy_thread] socket");
	goto _return_nosocket;
    }

    flags = fcntl (remote_socket, F_GETFL);
    if (flags == -1) {
	perror ("[proxy_thread] fcntl (F_GETFL)");
	goto _return;
    }

    if (fcntl (remote_socket, F_SETFL, flags | O_NONBLOCK)) {
	perror ("[proxy_thread] fcntl (F_SETFL)");
	goto _return;
    }

    flags = fcntl (args->client_socket, F_GETFL);
    if (flags == -1) {
	perror ("[proxy_thread] fcntl (F_GETFL, client_socket)");
	goto _return;
    }

    if (fcntl (args->client_socket, F_SETFL, flags | O_NONBLOCK)) {
	perror ("[proxy_thread] fcntl (F_SETFL, client_socket)");
	goto _return;
    }

    {
        int opt_val = 1;
        int const res = setsockopt (remote_socket, IPPROTO_TCP, TCP_NODELAY, &opt_val, sizeof (opt_val));
        if (res == -1)
            perror ("setsockopt failed (TCP_NODELAY, remote_socket)");
    }

    retval = connect (remote_socket,
		      (struct sockaddr*) &args->remote_address,
		      sizeof args->remote_address);
    if (retval && errno != EINPROGRESS) {
	perror ("[proxy_thread] connect");
	goto _return;
    }

    if (retval != 0) {
	FD_ZERO (&rfds);
	FD_SET (remote_socket, &rfds);
	wfds = rfds;
	efds = rfds;

	tv.tv_sec  = JAP_CONNECTION_TIMEOUT;
	tv.tv_usec = 0;

	retval = select (remote_socket + 1, &rfds, &wfds, &efds, &tv);
	if (retval == -1) {
	    perror ("[proxy_thread] select (connect)");
	    goto _return;
	} else
	if (retval == 0) {
	    fprintf (stderr, "[proxy_thread] connection timed out\n");
	    goto _return;
	}

	optlen = sizeof optval;
	if (getsockopt (remote_socket,
			SOL_SOCKET, SO_ERROR,
			&optval, &optlen))
	{
	    perror ("[proxy_thread] getsockopt");
	    goto _return;
	}

	if (optval) {
	    errno = optval;
	    perror ("[proxy_thread] connect (selected)");
	    goto _return;
	}
    }

    fprintf (stderr, "[proxy_thread] CONNECTED\n");

    remote_len = 0;
    client_len = 0;
    remote_shutdown = 0;
    client_shutdown = 0;

    while (1) {
	FD_ZERO (&rfds);
	FD_ZERO (&wfds);

	if (remote_len == 0 &&
	    client_len == 0 &&
	    remote_shutdown == 1 &&
	    client_shutdown == 1)
	    break;

	if (remote_len == 0 && !remote_shutdown) {
	    retval = recv (remote_socket, remote_buf, sizeof remote_buf, 0);
	    if (retval == 0) {
		remote_shutdown = 1;

		if (shutdown (args->client_socket, SHUT_WR)) {
		    perror ("[proxy_thread] shutdown (client_socket)");
		    goto _return;
		}

		/**/ goto _return; /**/
	    } else
	    if (retval == -1) {
		if (errno != EAGAIN) {
		    perror ("[proxy_thread] recv (remote_socket)");
		    goto _return;
		}

		FD_SET (remote_socket, &rfds);
	    } else {
		remote_len = retval;
		remote_start = 0;
		FD_SET (args->client_socket, &wfds);
	    }
	} else
	    FD_SET (args->client_socket, &wfds);

	if (client_len == 0 && !client_shutdown) {
	    retval = recv (args->client_socket,
			   client_buf, sizeof client_buf, 0);
	    if (retval == 0) {
		client_shutdown = 1;

		if (shutdown (remote_socket, SHUT_WR)) {
		    perror ("[proxy_thread] shutdown (remote_socket)");
		    goto _return;
		}

		/**/ goto _return; /**/
	    } else
	    if (retval == -1) {
		if (errno != EAGAIN) {
		    perror ("[proxy_thread] recv (client_socket)");
		    goto _return;
		}

		FD_SET (args->client_socket, &rfds);
	    } else {
		client_len = retval;
		client_start = 0;
		FD_SET (remote_socket, &wfds);
	    }
	} else
	    FD_SET (remote_socket, &wfds);

	retval = select ((remote_socket > args->client_socket ?
			  remote_socket : args->client_socket) + 1,
			 &rfds, &wfds, NULL, NULL);
	if (retval == -1) {
	    perror ("[proxy_thread] select");
	    goto _return;
	}

	if (FD_ISSET (remote_socket, &wfds)) {
	    ssize_t tosend,
		    sent_total,
		    sent_last;

	    tosend = client_len;
	    sent_total = client_start;
	    while (sent_total < tosend) {
		sent_last = send (remote_socket,
				  client_buf + sent_total,
				  client_len - sent_total,
				  MSG_NOSIGNAL);
		if (sent_last == -1) {
		    if (errno != EAGAIN) {
			perror ("[proxy_thread] send (client_socket)");
			goto _return;
		    }

		    break;
		}

		sent_total += sent_last;
		client_start += sent_last;
	    }

	    if (sent_total >= tosend)
		client_len = 0;
	}

	if (FD_ISSET (args->client_socket, &wfds)) {
	    ssize_t tosend,
		    sent_total,
		    sent_last;

	    tosend = remote_len;
	    sent_total = remote_start;
	    while (sent_total < tosend) {
		sent_last = send (args->client_socket,
				  remote_buf + sent_total,
				  remote_len - sent_total,
				  MSG_NOSIGNAL);
		if (sent_last == -1) {
		    if (errno != EAGAIN) {
			perror ("[proxy_thread] send (client_socket)");
			goto _return;
		    }

		    break;
		}

		sent_total += sent_last;
		remote_start += sent_last;
	    }

	    if (sent_total >= tosend)
		remote_len = 0;
	}
    }

_return:
    close (remote_socket);
_return_nosocket:
    close (args->client_socket);
    free (args);
    puts ("CLOSED");
    return 0;
}

int
main (int argc, char **argv)
{
    struct sockaddr_in local_address,
		       remote_address,
		       client_address,
		       restriction_address;
    int 	local_socket,
		client_socket;
    socklen_t	addrlen;
    int 	optval;
    pthread_t	thread;
    ProxyThreadArgs *thread_args;
    int 	restricted;

    if (argc < 4) {
	print_usage ();
	return -1;
    }

    if (argc > 4) {
	if (argc != 5) {
	    print_usage ();
	    return -1;
	}

	if (set_address (argv [4], "0", &restriction_address)) {
	    fprintf (stderr, "Bad host name\n");
	    return -1;
	}

	restricted = 1;
    } else
	restricted = 0;

    if (set_address (argv [1], argv [2], &remote_address)) {
	fprintf (stderr, "Bad destination\n");
	return -1;
    }

    local_socket = socket (AF_INET, SOCK_STREAM, 0);
    if (local_socket == -1) {
	perror ("socket");
	return -1;
    }

    optval = 1;
    if (setsockopt (local_socket,
		    SOL_SOCKET, SO_REUSEADDR,
		    &optval, sizeof optval))
    {
	perror ("setsockopt");
    }

    if (set_address (NULL, argv [3], &local_address)) {
	fprintf (stderr, "Bad local service/port\n");
	return -1;
    }

    if (bind (local_socket,
	      (struct sockaddr*) &local_address,
	      sizeof local_address))
    {
	perror ("bind");
	return -1;
    }

    if (listen (local_socket, SOMAXCONN)) {
	perror ("listen");
	return -1;
    }

    while (1) {
	addrlen = sizeof client_address;
	client_socket = accept (local_socket,
				(struct sockaddr*) &client_address,
				&addrlen);
	if (client_socket == -1) {
	    perror ("accept");
	    return -1;
	}

	fprintf (stderr, "ACCEPTED\n");

        {
            int opt_val = 1;
            int const res = setsockopt (client_socket, IPPROTO_TCP, TCP_NODELAY, &opt_val, sizeof (opt_val));
            if (res == -1)
                perror ("setsockopt failed (TCP_NODELAY, client_socket)");
        }

	if (!restricted ||
	    (restricted &&
	     client_address.sin_addr.s_addr ==
		restriction_address.sin_addr.s_addr))
	{
	    thread_args = (ProxyThreadArgs*) malloc (sizeof (ProxyThreadArgs));
	    memcpy (&thread_args->remote_address,
		    &remote_address, sizeof remote_address);
	    thread_args->client_socket = client_socket;

	    if (pthread_create (&thread, NULL,
				(void* (*)(void*)) proxy_thread,
				thread_args))
		perror ("pthread_create");
	    else 
		pthread_detach (thread);
	} else {
	    close (client_socket);
	    fprintf (stderr, "RESTRICTED\n");
	}
    }

    return 0;
}

