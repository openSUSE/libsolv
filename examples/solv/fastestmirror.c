#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <netdb.h>
#include <poll.h>
#include <errno.h>

#include "util.h"

#include "fastestmirror.h"

void
findfastest(char **urls, int nurls)
{
  int i, j, port;
  int *socks, qc;
  struct pollfd *fds;
  char *p, *p2, *q;
  char portstr[16];
  struct addrinfo hints, *result;;

  fds = solv_calloc(nurls, sizeof(*fds));
  socks = solv_calloc(nurls, sizeof(*socks));
  for (i = 0; i < nurls; i++)
    {
      socks[i] = -1;
      p = strchr(urls[i], '/');
      if (!p)
	continue;
      if (p[1] != '/')
	continue;
      p += 2;
      q = strchr(p, '/');
      qc = 0;
      if (q)
	{
	  qc = *q;
	  *q = 0;
	}
      if ((p2 = strchr(p, '@')) != 0)
	p = p2 + 1;
      port = 80;
      if (!strncmp("https:", urls[i], 6))
	port = 443;
      else if (!strncmp("ftp:", urls[i], 4))
	port = 21;
      if ((p2 = strrchr(p, ':')) != 0)
	{
	  port = atoi(p2 + 1);
	  if (q)
	    *q = qc;
	  q = p2;
	  qc = *q;
	  *q = 0;
	}
      sprintf(portstr, "%d", port);
      memset(&hints, 0, sizeof(struct addrinfo));
      hints.ai_family = AF_UNSPEC;
      hints.ai_socktype = SOCK_STREAM;
      hints.ai_flags = AI_NUMERICSERV;
      result = 0;
      if (!getaddrinfo(p, portstr, &hints, &result))
	{
	  socks[i] = socket(result->ai_family, result->ai_socktype, result->ai_protocol);
	  if (socks[i] >= 0)
	    {
	      fcntl(socks[i], F_SETFL, O_NONBLOCK);
	      if (connect(socks[i], result->ai_addr, result->ai_addrlen) == -1)
		{
		  if (errno != EINPROGRESS)
		    {
		      close(socks[i]);
		      socks[i] = -1;
		    }
		}
	    }
	  freeaddrinfo(result);
	}
      if (q)
	*q = qc;
    }
  for (;;)
    {
      for (i = j = 0; i < nurls; i++)
	{
	  if (socks[i] < 0)
	    continue;
	  fds[j].fd = socks[i];
	  fds[j].events = POLLOUT;
	  j++;
	}
      if (j < 2)
	{
	  i = j - 1;
	  break;
	}
      if (poll(fds, j, 10000) <= 0)
	{
	  i = -1;	/* something is wrong */
	  break;
	}
      for (i = 0; i < j; i++)
	if ((fds[i].revents & POLLOUT) != 0)
	  {
	    int soe = 0;
	    socklen_t soel = sizeof(int);
	    if (getsockopt(fds[i].fd, SOL_SOCKET, SO_ERROR, &soe, &soel) == -1 || soe != 0)
	      {
	        /* connect failed, kill socket */
	        for (j = 0; j < nurls; j++)
		  if (socks[j] == fds[i].fd)
		    {
		      close(socks[j]);
		      socks[j] = -1;
		    }
		i = j + 1;
		break;
	      }
	    break;	/* horray! */
	  }
      if (i == j + 1)
	continue;
      if (i == j)
        i = -1;		/* something is wrong, no bit was set */
      break;
    }
  /* now i contains the fastest fd index */
  if (i >= 0)
    {
      for (j = 0; j < nurls; j++)
	if (socks[j] == fds[i].fd)
	  break;
      if (j != 0)
	{
	  char *url0 = urls[0];
	  urls[0] = urls[j];
	  urls[j] = url0;
	}
    }
  for (i = j = 0; i < nurls; i++)
    if (socks[i] >= 0)
      close(socks[i]);
  free(socks);
  free(fds);
}
