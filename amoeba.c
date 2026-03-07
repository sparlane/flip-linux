#include <sys/types.h>
#include <fcntl.h>
#include <errno.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

#include "host_os.h"
#include "amoeba.h"
#include "cmdreg.h"
#include "stderr.h"
#include "amparam.h"

#include <sys/socket.h>
#include <sys/un.h>
#include <sys/uio.h>

trpar	am_tp= { { {0, 0, 0}, {0, 0, 0} }, 300};

struct am_hdr {
	uint32_t type;
	uint32_t len;
} __attribute__((packed));

int _amoeba(int req)
{
    static int fd = -1;

	if (fd < 0)
    {
		struct sockaddr_un sa;
		sa.sun_family = AF_UNIX;
		strcpy(sa.sun_path, "/tmp/flip.sock");

	if ((fd = socket(AF_UNIX, SOCK_STREAM, 0)) < 0)
	{
		printf("Cannot create socket for amoeba driver: %s\n", strerror(errno));
	    return (errno == ENOENT || errno == ENODEV) ?
					RPC_NOTFOUND : RPC_TRYAGAIN;
	}
	if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0)
	{
		close(fd);
		fd = -1;
		printf("Cannot connect to amoeba driver: %s\n", strerror(errno));
	    return (errno == ENOENT || errno == ENODEV) ?
					RPC_NOTFOUND : RPC_TRYAGAIN;
	}
	fcntl(fd, F_SETFD, 1);
    }

	struct am_hdr *hdr = malloc (sizeof (struct am_hdr));
	hdr->type = req;
	switch (req) {
		case AM_TRANS:
			hdr->len = sizeof(header) + am_tp.tp_par[0].par_cnt;
			break;
		default:
			*(((int *)0)) = 0;
			break;
	}
	uint8 p[6];
	memcpy(p, &am_tp.tp_par[0].par_hdr->h_port, 6);
	/* Send request */
	struct iovec iov[3];
	iov[0].iov_base = hdr;
	iov[0].iov_len = sizeof(struct am_hdr);
	iov[1].iov_base = (req == AM_TRANS || req == AM_PUTREP) ? (void *)am_tp.tp_par[0].par_hdr : NULL;
	iov[1].iov_len = (req == AM_TRANS || req == AM_PUTREP) ? sizeof(header) : 0;
	iov[2].iov_base = (req == AM_TRANS || req == AM_PUTREP) ? (void *)am_tp.tp_par[0].par_buf : NULL;
	iov[2].iov_len = (req == AM_TRANS || req == AM_PUTREP) ? am_tp.tp_par[0].par_cnt : 0;

	int written = writev(fd, iov, (req == AM_TRANS || req == AM_PUTREP) ? (am_tp.tp_par[0].par_buf ? 3 : 2) : 1);
	/* Wait for reply */
	int n = recv(fd, hdr, sizeof(*hdr), 0);
	if (n < 0)
	{
		printf("Error receiving reply from amoeba driver: %s\n", strerror(errno));
	    return RPC_TRYAGAIN;
	}
	if (hdr->len >= sizeof (header))
	{
		n = recv(fd, am_tp.tp_par[1].par_hdr, sizeof(header), 0);
		if (n < 0)
		{
			printf("Error receiving header from amoeba driver: %s\n", strerror(errno));
		    return RPC_TRYAGAIN;
		}
		if (hdr->len > sizeof(header))
		{
			n = recv(fd, am_tp.tp_par[1].par_buf, hdr->len - sizeof(header), 0);
			if (n < 0)
			{
				printf("Error receiving buffer from amoeba driver: %s\n", strerror(errno));
				return RPC_TRYAGAIN;
			}
		}
	}
	free (hdr);
	return 0;
}
