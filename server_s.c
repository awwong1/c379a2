/*
 *  Copyright (c) 2013 Alexander Wong <admin@alexander-wong.com>
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

/*
 * Copyright (c) 2008 Bob Beck <beck@obtuse.com>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 */

/*
 * Simple server implementation using select to handle clients.
 *
 * compile with 'gcc -o server_s server_s.c'
 * or compile with 'make server_s'
 * or compile with 'make all'
 *
 * Run using format ./server_s 8000 /some/where/documents /some/where/logfile
 * where 8000 is the port number, 
 * /some/where/documents is the directory of html files, 
 * and /some/where/logfile is the directory for the log file
 */

#include <sys/param.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/wait.h>

#include <netinet/in.h>

#include <err.h>
#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>

/* Defined variables /**/
#define MAXCONN 512
#define BUF_SIZE 4096
#define LRG_LONG_INT (sizeof(long int))*8 + 1
#define STATE_UNUSED 0
#define STATE_READING 1
#define STATE_WRITING 2

struct connectiondata {
	FILE *logfile;          /* logfile file /**/
	struct sockaddr_in sa;  /* connection sockaddr /**/
	char getline[BUF_SIZE];	/* client GET line /**/
	char *ip;               /* value of the connection ip /**/
	char *buf;	        /* buffer to store characters for read/write /**/
	char *bp;	        /* buffer location pointer /**/
	int sd; 	        /* connection socket data /**/
	int state; 	        /* the state of the connection /**/
	int ok;		        /* request OK value /**/
	size_t slen;            /* the sockaddr length of the connection /**/
	size_t bs;	        /* total buffer size /**/
	size_t bl;	        /* total buffer left to read/write /**/
	size_t w;	        /* written bytes number /**/
};

/* Function prototypes /**/
struct connectiondata * get_free_conn();
struct connectiondata checklisten(int);
int  get_port(char *);
int  get_directory(char *, char *, char *);
int  get_next_line(char *, char *, int);
void closecon(struct connectiondata *, int);
void handlewrite(struct connectiondata *);
void handleread(struct connectiondata *);
void read_success(struct connectiondata *);
void set_write_content(struct connectiondata *, char *, int);
void write_OK_log(struct connectiondata *);
void write_to_log(char *, char *, struct connectiondata *);
void set_current_time(char *);
void write_BAD_REQUEST(struct connectiondata *, char *);
void write_FORBIDDEN(struct connectiondata *, char *);
void write_NOT_FOUND(struct connectiondata *, char *);
void write_INTERNAL_SERVER_ERROR(struct connectiondata *, char *);

/* Global variables /**/
struct connectiondata connections[MAXCONN];
char dir_documents[80];
char dir_logfile[80];

int main(int argc,  char *argv[])
{
	/* Initialize local variables for first pass /**/
	struct sockaddr_in sockname;
	char *ep;
	fd_set *readable = NULL , *writable = NULL;
	int max = -1, omax;
	int sd;
	int i;
	u_short port;
	u_long p;
	
	if (daemon(1, 0) == -1)
		err(1, "daemon() failed");
	
	/* Check the arguments /**/
	if (argc != 4)
		err(1, "RUN AS: ./server_s PORT /dir/documents /dir/logfile");
	
	/* Send arguments to variables /**/
	port = get_port(argv[1]);
	strlcpy(dir_documents, argv[2], sizeof(dir_documents));
	strlcpy(dir_logfile, argv[3], sizeof(dir_logfile));

	/* Setup socket /**/
	memset(&sockname, 0, sizeof(sockname));
	sockname.sin_family = AF_INET;
	sockname.sin_port = htons(port);
	sockname.sin_addr.s_addr = htonl(INADDR_ANY);
	sd=socket(AF_INET,SOCK_STREAM,0);
	if ( sd == -1)
		err(1, "socket failed");
	if (bind(sd, (struct sockaddr *) &sockname, sizeof(sockname)) == -1)
		err(1, "bind failed");
	if (listen(sd,5) == -1)
		err(1, "listen failed");
	/* Setup all connection structs /**/
	for (i = 0; i < MAXCONN; i++)
		closecon(&connections[i], 1);
	
        /* Accept connections /**/
	while(1) 
	{
		int i;
		int maxfd = -1; /* max file descriptor set of value/**/
		omax = max;
		max = sd; /* the listen socket /**/
	
		/* Set max equal to the max socket descriptor /**/
		for (i = 0; i < MAXCONN; i++) {
			if (connections[i].sd > max)
				max = connections[i].sd;
		}
		if (max > omax) {
			/* Free old fd_sets /**/
			free(readable);
			free(writable);
			/* Allocate new fd_sets /**/
			readable = (fd_set *)calloc(howmany(max + 1, NFDBITS),
						    sizeof(fd_mask));
			writable = (fd_set *)calloc(howmany(max + 1, NFDBITS),
						    sizeof(fd_mask));
			if (readable == NULL || writable == NULL)
				err(1, "fd_sets out of memory");
			omax = max;
		}
		/* 0 out the fd_sets /**/
		memset(readable, 0, howmany(max+1, NFDBITS) *
		       sizeof(fd_mask));
		memset(writable, 0, howmany(max+1, NFDBITS) *
		       sizeof(fd_mask));
		
		/* Set corresponding bit in the readable set /**/
		FD_SET(sd, readable);
		if (maxfd < sd)
			maxfd = sd;

		/* 
		 * Iterate through connections, if connection state is
		 * reading, put in readable fd_set. If connection state
		 * is writing, put in the writable fd_set. 
		 /**/
		for (i = 0; i<MAXCONN; i++) {
			if (connections[i].state == STATE_WRITING) {
				FD_SET(connections[i].sd, writable);
				if (maxfd < connections[i].sd)
					maxfd = connections[i].sd;
			}
			if (connections[i].state == STATE_READING) {
				FD_SET(connections[i].sd, readable);
				if (maxfd < connections[i].sd)
					maxfd = connections[i].sd;
			}
		}

		/* 
		 * Call select with readable/writable fd_sets passed and
		 * use indicated sockets from select's return value
		 /**/
		i = select(maxfd + 1, readable, writable, NULL,NULL);
		if (i == -1  && errno != EINTR)
			err(1, "select failed");
		if (i > 0) {
			/* 
			 * Something to do. Check listen socket; 
			 * if true then accept new connection
			 /**/
			if (FD_ISSET(sd, readable)) {
				connections[i] = checklisten(sd);
			}
			/*
			 * now, iterate through all of our connections,
			 * check to see if they are readble or writable,
			 * and if so, do a read or write accordingly 
			 /**/
			for (i = 0; i<MAXCONN; i++) {
				if ((connections[i].state == STATE_READING) &&
				    FD_ISSET(connections[i].sd, readable))
					handleread(&connections[i]);
				if ((connections[i].state == STATE_WRITING) &&
				    FD_ISSET(connections[i].sd, writable))
					handlewrite(&connections[i]);
			}
		}
	}
}

/*
 * Check to see if the accept passed. If passed, get client IP
 * If free connections exist, set to state reading
 /**/
struct connectiondata checklisten(int sd)
{
	struct connectiondata *cp;
	struct sockaddr_in sa;
	int newsd;
	socklen_t slen;
	char ip[INET_ADDRSTRLEN];
	
	slen = sizeof(sa);
	newsd = accept(sd, (struct sockaddr *)&sa, &slen);
	if (newsd == -1)
		err(1, "accept failed");
	
	/* get IP of client /**/
	inet_ntop(AF_INET, &(sa.sin_addr), ip, INET_ADDRSTRLEN);
	
	cp = get_free_conn();
	if (cp == NULL) {
		/* No connections, close /**/
		close(newsd);
	} else {
		/* New Connection, set reading /**/
		memcpy(&cp->sa, &sa, sizeof(sa));
		cp->state = STATE_READING;
		cp->sd = newsd;
		cp->slen = slen;
		cp->ip = ip;
		cp->w = 0;
		cp->ok = 0;
	}

	return *cp;
}

/*
 * Handle connection to write to, assume is writable.
 * Change state to reading state after completed
 /**/
void handlewrite(struct connectiondata *cp)
{
	ssize_t i;
	
	/* We can safely do one write, due to check by select /**/
	i = write(cp->sd, cp->bp, cp->bl);
	if (i == -1) {
		if (errno != EAGAIN) {
			/* the write failed /**/
			if (cp->ok)
				write_OK_log(cp);
			cp->state = STATE_UNUSED;
			closecon(cp, 0);

		}		
		return;
	} else {
		cp->bp += i;  /* Increment counter placeholder /**/
		cp->bl -= i;  /* Decrement amount  left to write /**/
		cp->w += i;   /* Record written characters /**/
	}	
	if (cp->bl == 0) {
		if (cp->ok) 
			write_OK_log(cp);
		cp->state = STATE_UNUSED;
		closecon(cp, 0);
	}
}

/* Connection has readable data,. If newline, change to writing state /**/
void handleread(struct connectiondata *cp)
{
	ssize_t i;
	
	if (cp->bl < 10) {
		char *tmp;
		tmp = realloc(cp->buf, (cp->bs + BUF_SIZE) * sizeof(char));
		if (tmp == NULL) {
			/* we're out of memory /**/
			closecon(cp, 0);
			return;
		}
		cp->buf = tmp;
		cp->bs += BUF_SIZE;
		cp->bl += BUF_SIZE;
		cp->bp = cp->buf + (cp->bs - cp->bl);
	}
	
        /* We can safely do one read, due to check by select /**/
	i = read(cp->sd, cp->bp, cp->bl);
	if (i == 0) {
		write_to_log("", "500 Internal Server Error", cp);
		closecon(cp, 0);
		return;
	}
	if (i == -1) {
		if (errno != EAGAIN) {
			/* read failed /**/
			char curr_time[BUF_SIZE] = {0};
			set_current_time(curr_time);
			write_INTERNAL_SERVER_ERROR(cp, curr_time);
			cp->state = STATE_WRITING;
			cp->bl = cp->bp - cp->buf;
			cp->bp = cp->buf;
			write_to_log("", "500 Internal Server Error", cp);
		}
		/*
		 * note if EAGAIN, we just return, and let our caller
		 * decide to call us again when socket is readable
		 /**/
		return;
	}
	/*
	 * ok we really got something read. change where we're
	 * pointing
	 /**/
	cp->bp += i;
	cp->bl -= i;

	char * cur;
	/* check if we read atleast the first line /**/
	for (cur = cp->buf; cur < cp->bp; cur++)
	{
		if ( *cur == '\n')
		{
			char getline[BUF_SIZE] = {0};
			char curr_time[BUF_SIZE] = {0};
			set_current_time(curr_time);

			/* open log file /**/
			cp->logfile = fopen(dir_logfile, "a");
			if (cp->logfile == NULL)
			{
				get_next_line(cp->buf, getline, 0);
				write_INTERNAL_SERVER_ERROR(cp, curr_time);
			}
			else 
			{
				/* missing blank line /**/
				if ( (*((cp->bp)-2) != '\n' || 
				    *((cp->bp)-1) != '\n')
				    &&
				    (*((cp->bp)-2) != '\r' ||
				    *((cp->bp)-1) != '\n' ||
				    *((cp->bp)-3) != '\n' ) )
				{
					get_next_line(cp->buf, getline, 0);
					write_BAD_REQUEST(cp, curr_time);
					write_to_log(getline, 
					    "400 Bad Request", cp);
				}
				else 
					read_success(cp);
			}
			
			cp->state = STATE_WRITING;
			cp->bl = cp->bp - cp->buf;
			cp->bp = cp->buf;	
			return;
		}
	}
}

/* Handle sucessful read /**/
void read_success(struct connectiondata *cp)
{
	FILE *file;
	char GET_dir[BUF_SIZE] = {0};
	char curr_time[BUF_SIZE] = {0};
	char dir[BUF_SIZE] = {0};
	char temp[BUF_SIZE] = {0};	
	char *buff;
	int buf_len = 0;
	int total_len = 0;
	char file_length_buf[LRG_LONG_INT];

	/* get current time /**/
	set_current_time(curr_time);

	/* Retrieve GET line's directory /**/
	if (get_directory(cp->buf, GET_dir, cp->getline) == -1)
	{
		write_BAD_REQUEST(cp, curr_time);
		write_to_log(cp->getline, "400 Bad Request", cp);
	}
	else
	{
		/* get the requested file /**/
		strlcpy(dir, dir_documents, sizeof(dir));
		strcat(dir, GET_dir);
		errno = 0;
		file = fopen(dir, "r");
		if (errno == EACCES)
		{
			/* file non-readable /**/
			write_FORBIDDEN(cp, curr_time);
			write_to_log(cp->getline, "403 Forbidden", cp);
			return;
		}
		if (file == NULL)
		{
			/* file not found /**/
			write_NOT_FOUND(cp, curr_time);
			write_to_log(cp->getline, "404 Not Found", cp);
			return;
		}

		/* Get length of the file /**/
		fseek(file, 0L, SEEK_END);
		sprintf(file_length_buf, "%ld", ftell(file));
		fseek(file, 0L, SEEK_SET);

		/* OK request /**/
		buff = malloc(BUF_SIZE * sizeof(char));
		if (buff == NULL)
		{
			write_INTERNAL_SERVER_ERROR(cp, curr_time);
			write_to_log(cp->getline, "500 Internal Server Error",
			    cp);
			return;
		}
		memset(buff, 0, BUF_SIZE);
		buf_len = BUF_SIZE * sizeof(char);

		strcat(buff, "HTTP/1.1 200 OK\nDate: ");
		strcat(buff, curr_time);
		strcat(buff, "\nContent-Type: text/html\nContent-Length: ");
		strcat(buff, file_length_buf);
		strcat(buff, "\n\n");
		while (fgets(temp, sizeof(temp), file) != NULL)
		{
			total_len += strlen(temp);
			if (buf_len - total_len < BUF_SIZE)
			{
				buff = realloc(buff, buf_len * 2);
				if (buff == NULL)
				{
					write_INTERNAL_SERVER_ERROR(cp, 
					    curr_time);
					write_to_log(cp->getline, 
					    "500 Internal Server Error",
					     cp);
					return;
				}
				buf_len *= 2;
			}
			strcat(buff, temp);
		}
		
		cp->ok = 1;
		set_write_content(cp, buff, strlen(buff));	
		free(buff);
		fclose(file);
	}
}

/* Fill connection structure /**/
void set_write_content(struct connectiondata *cp, char * content, int length)
{
	char *tmp;
	tmp = realloc(cp->buf, length * sizeof(char));
	if (tmp == NULL) 
	{
		char curr_time[BUF_SIZE] = {0};
		set_current_time(curr_time);
		write_INTERNAL_SERVER_ERROR(cp, curr_time);
		write_to_log(cp->getline, "500 Internal Server Error", cp);
		return;
	}
	cp->buf = tmp;
	memset(cp->buf, 0, length);
	strlcpy(cp->buf, content, length);
	strcat(content, "\0");
	cp->bp = cp->buf + length;
	cp->bs = length * sizeof(char);
}

/* Make a free connection /**/
struct connectiondata * get_free_conn()
{
	int i;
	for (i = 0; i < MAXCONN; i++) {
		if (connections[i].state == STATE_UNUSED)
			return(&connections[i]);
	}
	return(NULL);
}

/* Close or initialize a connection /**/
void closecon (struct connectiondata *cp, int initflag)
{
	if (!initflag) {
		if (cp->sd != -1)
			close(cp->sd);
		free(cp->buf);
	}
	memset(cp, 0, sizeof(struct connectiondata));
	cp->buf = NULL; 
	cp->sd = -1;
}

/* Get the directory /**/
int get_directory(char *buffer, char *directory, char *getline)
{
	char line[BUF_SIZE] = {0};
	char get[BUF_SIZE] = {0};
	char http[BUF_SIZE] = {0};
	char from[BUF_SIZE] = {0};
	char useragent[BUF_SIZE] = {0};
	int  position = 0;

	/* check for GET line /**/
	position = get_next_line(buffer, getline, position);
	/* Check if the position is valid /**/
	if (position == -1)
		return -1;
	/* Check if it is a proper request /**/
	if ( (sscanf(getline, "%s %s %s", get, directory, http) != 3)
		|| (strcmp(get, "GET") != 0) 
		|| (strcmp(http, "HTTP/1.1") != 0) )
		return -1;

	/* Check the from line /**/
	position = get_next_line(buffer, line, position);
	/* Check if the position is correct /**/
	if (position == -1)
		return -1;
	/* Grab host data string /**/
	sscanf(line, "%s", from);
	if (strcmp(from, "From:") != 0 && strcmp(from, "Host:") != 0)
		return -1;

	/* Check for User Agent line /**/
	memset(line, 0, sizeof(line));
	get_next_line(buffer, line, position);
	/* Grab useragent string /**/
	sscanf(line, "%s", useragent);
	if (strcmp(useragent, "User-Agent:") != 0)
		return -1;
	return 1;
}
/* Get the next line starting from position i from the buffer /**/
int get_next_line(char *buffer, char * line, int i)
{
	/* Check out of position /**/
	if (i >= strlen(buffer))
		return -1;
	/* Iterate through buffer, fill buffer /**/
	int j = 0;
	while (i < strlen(buffer) && buffer[i] != '\n')
	{
		line[j] = buffer[i];
		j++;
		i++;
	}
	i++;
	return i;
}

/* Get the current local time /**/
void set_current_time(char * t)
{
	time_t rawtime;
	struct tm * timeinfo;
	time(&rawtime);
	timeinfo = localtime(&rawtime);
	/* Store into t /**/
	strftime(t, 80, "%a, %d %b %Y %X %Z", timeinfo);
}

/* Get the port, check validity /**/
int get_port(char * port)
{
	u_long p;
	char *ep;

	errno = 0;
	p = strtoul(port, &ep, 10);

	/* invalid parameter /**/
	if (*port == '\0' || *ep != '\0') 
		err(1, "port is not a number."); 
	/* Port is out of range /**/
	if ( (errno == ERANGE && p == ULONG_MAX) || (p > USHRT_MAX) )
		err(1, "port value out of range."); 
	return p;
}

/* Write to the log file /**/
void write_to_log(char *getline, char *completion, struct connectiondata *cp)
{
	char curr_time[BUF_SIZE] = {0};	
	set_current_time(curr_time);
	fprintf(cp->logfile, "%s\t%s\t%s\t%s\n", curr_time, 
	    cp->ip, getline, completion);
	fclose(cp->logfile);
}

/* Write OK message to log /**/
void write_OK_log(struct connectiondata *cp)
{
	char log_msg[BUF_SIZE] = {0};
	char buff[BUF_SIZE] = {0};
	int header_count = 0;
	char *curr;

	for (curr = cp->buf; *curr != '\n' || *(curr+1) != '\n'; curr++)
		header_count++;

	header_count += 2;

	strcat(log_msg, "200 OK ");
	sprintf(buff, "%ld", cp->w - header_count);
	strcat(log_msg, buff);
	strcat(log_msg, "/");
	sprintf(buff, "%ld", cp->bs - header_count);
	strcat(log_msg, buff);
	write_to_log(cp->getline, log_msg, cp);	
}

/* Write a Bad Request Error to the client through connectiondata /**/
void write_BAD_REQUEST(struct connectiondata *cp, char *curr_time)
{
	char temp[BUF_SIZE] = {0};
	strcat(temp, "HTTP/1.1 400 Bad Request\nDate: ");
	strcat(temp, curr_time);
	strcat(temp, "\nContent-Type: text/html\nContent-Length: 107");
	strcat(temp, "\n\n<html><body>\n<h2>Malformed Request</h2>\n");
	strcat(temp, 
	    "Your browser sent a request I could not understand.\n");
	strcat(temp, "</body></html>");	  
	set_write_content(cp, temp, strlen(temp));
}

/* Write a Forbidden response to the client through connectiondata /**/
void write_FORBIDDEN(struct connectiondata *cp, char *curr_time)
{
	char temp[BUF_SIZE] = {0};
	strcat(temp, "HTTP/1.1 403 Forbidden\nDate: ");
	strcat(temp, curr_time);
	strcat(temp, "\nContent-Type: text/html\n");
	strcat(temp, "Content-Length: 130\n\n");
	strcat(temp, "<html><body>\n<h2>Permission Denied");
	strcat(temp, "</h2>\nYou asked for a document you ");
	strcat(temp, "are not permitted to see. It sucks to ");
	strcat(temp, "be you.\n</body></html>");	  
	set_write_content(cp, temp, strlen(temp));
}

/* Write a Not Found Error to the client through connectiondata /**/
void write_NOT_FOUND(struct connectiondata *cp, char *curr_time)
{
	char temp[BUF_SIZE] = {0};
	strcat(temp, "HTTP/1.1 404 Not Found\nDate: ");
	strcat(temp, curr_time);
	strcat(temp, "\nContent-Type: text/html\n");
	strcat(temp, "Content-Length: 117\n\n");
	strcat(temp, "<html><body>\n<h2>Document not found");
	strcat(temp, "</h2>\nYou asked for a document ");
	strcat(temp, "that doesn't exist. That is so sad.\n");
	strcat(temp, "</body></html>");
	set_write_content(cp, temp, strlen(temp));
}

/* Write an Internal Server Error to the client rhough connectiondata /**/
void write_INTERNAL_SERVER_ERROR(struct connectiondata *cp, char *curr_time)
{
	char temp[BUF_SIZE] = {0};
	strcat(temp, "HTTP/1.1 500 Internal Server Error\n");
	strcat(temp, "Date: ");
	strcat(temp, curr_time);
	strcat(temp, "\nContent-Type: text/html\n");
	strcat(temp, "Content-Length: 131\n\n");
	strcat(temp, "<html><body>\n<h2>Oops. That didn't ");
	strcat(temp, "work</h2>\nI had some sort of problem ");
	strcat(temp, "dealing with your request. Sorry, ");
	strcat(temp, "I'm lame.\n</body></html>");
	set_write_content(cp, temp, strlen(temp));
}
