#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/select.h>
#include <arpa/inet.h>

#include "common.h"

#define FDS_READ 0
#define FDS_WRITE 1
#define FDS_ERROR 2
#define BUFSIZE (0x1000)

struct session
{
	int fd, sent, eof;
	uint8_t prot;
	uint32_t trans_id;
	char *rbuf, *wbuf;
	unsigned int rbsize, wbsize;
	char last;
	struct list_elem elem;
};

void session_append(struct session *session, void *buffer, unsigned int size)
{
	if(session->wbuf)
	{
		session->wbuf = realloc(session->wbuf, session->wbsize + size);
		memcpy(session->wbuf + session->wbsize, buffer, size);
		session->wbsize += size;
	}
	else
	{
		session->wbuf = malloc(size);
		memcpy(session->wbuf, buffer, size);
		session->wbsize = size;
	}
}

void session_flush(struct session *session, void *buffer, unsigned int size)
{
	free(session->rbuf);
	if(size)
	{
		session->rbuf = malloc(size);
		memcpy(session->rbuf, buffer, size);
		session->rbsize = size;
	}
	else
	{
		session->rbuf = NULL;
		session->rbsize = 0;
	}
}

struct session *session_close(struct session *session)
{
	struct session *prev;
	if(close(session->fd) < 0)
	{
		perror("close");
	}
	prev = list_entry(list_prev(&session->elem), struct session, elem);
	if(session->rbuf)
	{
		free(session->rbuf);
	}
	if(session->wbuf)
	{
		free(session->wbuf);
	}
	list_remove(&session->elem);
	free(session);
	return prev;
}

struct session *serve_accept(struct session *session, fd_set *set)
{
	struct neghdr neg;
	session->prot = 0;
	session->trans_id = rand();
	session->rbuf = session->wbuf = NULL;
	session->rbsize = session->wbsize = 0;
	session->sent = session->eof = 0;

	neg.op = 0;
	neg.proto = session->prot;
	neg.checksum = 0;
	neg.trans_id = session->trans_id;
	neg.checksum = ~checksum(&neg, sizeof(struct neghdr));

	session_append(session, &neg, sizeof(struct neghdr));
	FD_SET(session->fd, &set[FDS_WRITE]);
	FD_SET(session->fd, &set[FDS_ERROR]);

	return session;
}

struct session *serve_read(struct session *session, fd_set *set)
{
	int i, j, len;
	unsigned int size;
	char *memory;
	char buffer[BUFSIZE];
	struct neghdr *neg;
	if((len = recv(session->fd, buffer, BUFSIZE, 0)) < 0)
	{
		perror("recv");
		return session_close(session);
	}
	if(len)
	{
		if(session->rbuf)
		{
			session->rbuf = realloc(session->rbuf, session->rbsize + len);
		}
		else
		{
			session->rbuf = malloc(len);
		}
		memcpy(session->rbuf + session->rbsize, buffer, len);
		session->rbsize += len;
	}

	if(session->prot == 0)
	{
		if(len == 0)
		{
			fprintf(stderr, "client fault: socket\n");
			return session_close(session);
		}

		if(sizeof(struct neghdr) <= session->rbsize)
		{
			neg = (struct neghdr *)session->rbuf;
			if(neg->op != 1 || (neg->proto != 1 && neg->proto != 2) || neg->trans_id != session->trans_id)
			{
				fprintf(stderr, "client fault: protocol\n");
				return session_close(session);
			}
			session->prot = neg->proto;
			if(checksum(neg, sizeof(struct neghdr)) != 0xffff)
			{
				fprintf(stderr, "client fault: checksum\n");
				return session_close(session);
			}
			session_flush(session, buffer + len - (session->rbsize - sizeof(struct neghdr)), session->rbsize - sizeof(struct neghdr));
		}
	}
	if(session->prot == 1)
	{
		for(i = 0; i < session->rbsize; i++)
		{
			if(session->rbuf[i] == '\\')
			{
				if(i + 1 < session->rbsize)
				{
					i++;
					if(session->rbuf[i] == '\\')
					{
						if(session->sent && session->last == '\\')
						{
							continue;
						}
						session_append(session, "\\\\", 2);
						session->sent = 1;
						session->last = '\\';
					}
					else if(session->rbuf[i] == '0')
					{
						session_append(session, "\\0", 2);
					}
					else
					{
						fprintf(stderr, "client fault: escape\n");
						return session_close(session);
					}
				}
				else
				{
					break;
				}
			}
			else
			{
				if(session->sent && session->last == session->rbuf[i])
				{
					continue;
				}
				session_append(session, &session->rbuf[i], 1);
				session->sent = 1;
				session->last = session->rbuf[i];
			}
		}
		session_flush(session, buffer + len - (session->rbsize - i), session->rbsize - i);
	}
	else if(session->prot == 2)
	{
		if(4 <= session->rbsize)
		{
			size = ntohl(*(uint32_t *)session->rbuf);
			if(4 + size <= session->rbsize)
			{
				memory = (char *)malloc(size);
				for(i = j = 4; i < 4 + size; i++)
				{
					if(session->sent && session->last == session->rbuf[i])
					{
						continue;
					}
					session->sent = 1;
					memory[j++] = session->last = session->rbuf[i];
				}
				*(uint32_t *)memory = htonl(j - 4);
				session_append(session, memory, j);
				free(memory);
				session_flush(session, buffer + len - (session->rbsize - (4 + size)), session->rbsize - (4 + size));
			}
		}
	}
	if(len == 0)
	{
		if(session->rbuf)
		{
			fprintf(stderr, "client fault: socket\n");
			return session_close(session);
		}
		if(session->wbuf == NULL)
		{
			return session_close(session);
		}
		session->eof = 1;
	}
	else
	{
		FD_SET(session->fd, &set[FDS_READ]);
	}
	if(session->wbuf)
	{
		FD_SET(session->fd, &set[FDS_WRITE]);
	}
	FD_SET(session->fd, &set[FDS_ERROR]);
	
	return session;
}

struct session *serve_write(struct session *session, fd_set *set)
{
	int len;
	char *buffer;
	struct session *prev;
	if((len = send(session->fd, session->wbuf, session->wbsize, 0)) < 0)
	{
		perror("send");
		return session_close(session);
	}
	if(len < session->wbsize)
	{
		buffer = malloc(session->wbsize - len);
		memcpy(buffer, session->wbuf + len, session->wbsize - len);
		free(session->wbuf);
		session->wbuf = buffer;
		session->wbsize -= len;
		FD_SET(session->fd, &set[FDS_WRITE]);
	}
	else
	{
		free(session->wbuf);
		session->wbuf = NULL;
		session->wbsize = 0;
		if(session->eof)
		{
			return session_close(session);
		}
		FD_SET(session->fd, &set[FDS_READ]);
	}
	FD_SET(session->fd, &set[FDS_ERROR]);

	return session;
}

struct session *serve_error(struct session *session, fd_set *set)
{
	struct session *prev;
	printf("%3d: error\n", session->fd);
	fprintf(stderr, "client fault: socket\n");
	return session_close(session);
}

int main(int argc, char *argv[])
{
	int sock, nfds, fd;
	uint16_t port;
	char c;
	char *argv_port = NULL;
	struct sockaddr_in addr;
	struct list sessions;
	struct list_elem *it;
	struct session *now;
	fd_set set[3], buf[3];
	while((c = getopt(argc, argv, "p:")) != -1)
	{
		switch(c)
		{
			case 'p':
				argv_port = optarg;
				port = strtoul(argv_port, NULL, 10);
				break;
		}
	}
	if(argv_port == NULL)
	{
		fprintf(stderr, "usage: %s -p port\n", argv[0]);
		exit(1);
	}

	if((sock = socket(AF_INET, SOCK_STREAM, 0)) < 0)
	{
		perror("socket");
		exit(1);
	}
	memset(&addr, 0, sizeof(struct sockaddr_in));
	addr.sin_family = AF_INET;
	addr.sin_port = htons(port);
	addr.sin_addr.s_addr = INADDR_ANY;
	if(bind(sock, (const struct sockaddr *)&addr, sizeof(struct sockaddr_in)) < 0)
	{
		perror("bind");
		exit(1);
	}
	if(listen(sock, 5) < 0)
	{
		perror("listen");
		exit(1);
	}
	srand((unsigned int)time(NULL));
	list_init(&sessions);
	FD_ZERO(&set[FDS_READ]);
	FD_ZERO(&set[FDS_WRITE]);
	FD_ZERO(&set[FDS_ERROR]);
	nfds = sock + 1;
	for(; ; )
	{
		FD_SET(sock, &set[FDS_READ]);
		FD_SET(sock, &set[FDS_ERROR]);
		if(select(nfds, &set[FDS_READ], &set[FDS_WRITE], &set[FDS_ERROR], NULL) < 0)
		{
			perror("select");
			exit(1);
		}
		FD_ZERO(&buf[FDS_READ]);
		FD_ZERO(&buf[FDS_WRITE]);
		FD_ZERO(&buf[FDS_ERROR]);
		if(FD_ISSET(sock, &set[FDS_ERROR]))
		{
			printf("%3d: error\n", sock);
			exit(1);
		}
		if(FD_ISSET(sock, &set[FDS_READ]))
		{
			if((fd = accept(sock, NULL, NULL)) < 0)
			{
				perror("accept");
			}
			else
			{
				now = (struct session *)malloc(sizeof(struct session));
				now->fd = fd;
				if(nfds <= fd)
				{
					nfds = fd + 1;
				}
				list_insert(&sessions, &now->elem);
				serve_accept(now, buf);
			}
		}
		for(it = list_begin(&sessions); it != list_end(&sessions); it = list_next(it))
		{
			now = list_entry(it, struct session, elem);
			if(FD_ISSET(now->fd, &set[FDS_ERROR]))
			{
				now = serve_error(now, buf);
			}
			if(it == &now->elem && FD_ISSET(now->fd, &set[FDS_WRITE]))
			{
				now = serve_write(now, buf);
			}
			if(it == &now->elem && FD_ISSET(now->fd, &set[FDS_READ]))
			{
				now = serve_read(now, buf);
			}
			it = &now->elem;
		}
		FD_COPY(&buf[FDS_READ], &set[FDS_READ]);
		FD_COPY(&buf[FDS_WRITE], &set[FDS_WRITE]);
		FD_COPY(&buf[FDS_ERROR], &set[FDS_ERROR]);
	}
	return 0;
}
