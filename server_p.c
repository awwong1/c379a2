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
 * Simple server implementation that services each client in a new
 * thread.
 * 
 * Compile using 'gcc -o server_p server_p.c'
 * or compile with 'make server_p'
 * or compile with 'make all'
 *
 * Run as ./server_p 8000 /some/where/documents /some/where/logfile
 * where 8000 is the port number, 
 * /some/where/documents is the directory of html files, and
 * /some/where/logfile is the directory of the log file
 */

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <netinet/in.h>

#include <err.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <limits.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <pthread.h>

/* Defined Variables /**/
#define BUF_SIZE 4096
#define NUM_THREADS 512
#define LRG_LONG_INT (sizeof(long int))*8 + 1

/* Function prototypes /**/
void * handle_client(void *);
int  get_port(char *);
int  read_client_request(int, char *);
int  get_directory(char *, char *, char *);
int  get_next_line(char *, char *, int);
int  write_to_client(int, char *);
int  write_OK(int, char *, FILE *, char *);
void write_to_log(char *, char *, char *, FILE *);
void set_current_time(char *);
void write_BAD_REQUEST(int, char *);
void write_FORBIDDEN(int, char *);
void write_NOT_FOUND(int, char *);
void write_INTERNAL_SERVER_ERROR(int, char *);

/* Global variables /**/
pthread_mutex_t lock;
char dir_documents[80];
char dir_logfile[80];

struct thread_data
{
	long tid;
	int clientsd;
	char clientip[80];
};

struct thread_data td[NUM_THREADS];

int main(int argc, char * argv[]) 
{
	struct sockaddr_in sockname, client;
	int clientlen, sd, rc;
	u_short port;
	pthread_t thread[NUM_THREADS];
  	pthread_attr_t attr;
	void *status;
	long t;
	
	if (daemon(1, 0) == -1)
		err(1, "daemon() failed");
	
        /* Check the arguments /**/
	if (argc != 4)
		err(1, "RUN AS: ./server_p PORT /dir/documents /dir/logfile");
	
	/* Send arguments to variables /**/
	port = get_port(argv[1]);
	strlcpy(dir_documents, argv[2], sizeof(dir_documents));
	strlcpy(dir_logfile, argv[3], sizeof(dir_logfile));

	/* Setup socket /**/
	memset(&sockname, 0, sizeof(sockname));
	sockname.sin_family = AF_INET;
	sockname.sin_port = htons(port);
	sockname.sin_addr.s_addr = htonl(INADDR_ANY);
	sd = socket(AF_INET, SOCK_STREAM, 0);
	if (sd == -1)
		err(1, "socket failed");
	if (bind(sd, (struct sockaddr *) &sockname, sizeof(sockname)) == -1)
		err(1, "bind failed");
	if (listen(sd,5) == -1)
		err(1, "listen failed");

	/* Initialize thread attributes /**/
	pthread_attr_init(&attr);
   	pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE);
	/* Initialize mutex lock /**/
	if (pthread_mutex_init(&lock, NULL) != 0)
		err(1, "mutex init failed");
	
	t = 0;
	/* Accept incoming client connections /**/
	while(1) 
	{
		int clientsd;
		char client_ip[INET_ADDRSTRLEN];

		if (t == NUM_THREADS)
			t = 0;

		/* Accept client connection /**/
		clientlen = sizeof(&client);
		clientsd = accept(sd, (struct sockaddr *)&client, &clientlen);
		if (clientsd == -1)
			err(1, "accept failed");

		/* Get client IP /**/
		inet_ntop(AF_INET,&(client.sin_addr), 
		    client_ip, INET_ADDRSTRLEN);
		/* Initialize thread data /**/
		td[t].tid = t;
		td[t].clientsd = clientsd;
		strlcpy(td[t].clientip, client_ip, sizeof(td[t].clientip));
		/* Create posix thread to handle client/**/
		rc = pthread_create(&thread[t], &attr, handle_client, 
		    (void *)&td[t]);
		if (rc)
			err(1, "unable to create thread");

		/* Wait for the thread /**/
		rc = pthread_join(thread[t], &status);
		if (rc)
			err(1, "failed to join thread");

		t++;
		close(clientsd);
	}
}

/* Handle the client /**/
void * handle_client(void *thread_data)
{
	struct thread_data *t_data;
	FILE *file;
	FILE *logfile;
	char f[BUF_SIZE] = {0};
	char buffer[BUF_SIZE] = {0};
	char getline[BUF_SIZE] = {0};
	char GET_dir[BUF_SIZE] = {0};
	char curr_time[BUF_SIZE] = {0};
	char file_length_buf[LRG_LONG_INT] = {0};
	int total_written;
	char tw[LRG_LONG_INT] = {0};
	int read;

	t_data = (struct thread_data *) thread_data;

	/* Get time, read request /**/
	set_current_time(curr_time);
	read = read_client_request(t_data->clientsd, buffer);

	/* Open log file, handle file errors /**/
	logfile = fopen(dir_logfile, "a");
	if (logfile == NULL)
	{
		/* Log file doesn't exist /**/
		get_next_line(buffer, getline, 0);
		write_INTERNAL_SERVER_ERROR(t_data->clientsd, curr_time);
		pthread_exit((void*)t_data->tid);
		return;	
	}

	if (read == -1) 
	{
		/* Blank line failed /**/
		write_BAD_REQUEST(t_data->clientsd, curr_time);
		get_next_line(buffer, getline, 0);
		write_to_log(getline, "400 Bad Request", 
		    t_data->clientip, logfile);
		pthread_exit((void*)t_data->tid);
	}
	else if (read == -2)
	{
		/* Read file failed /**/
		write_INTERNAL_SERVER_ERROR(t_data->clientsd, curr_time);
		write_to_log(getline, "500 Internal Server Error",
		    t_data->clientip, logfile);
		pthread_exit((void*)t_data->tid);
	}	
		
	/* Retrieve directory of getline /**/
	if (get_directory(buffer, GET_dir, getline) == -1)
	{
		write_BAD_REQUEST(t_data->clientsd, curr_time);
		write_to_log(getline, "400 Bad Request", 
		    t_data->clientip, logfile);
		pthread_exit((void*)t_data->tid);
	}

	/* Get the requested file /**/
	memset(f, 0, sizeof(f));
	strlcpy(f, dir_documents, sizeof(f));
	strcat(f, GET_dir);
	errno = 0;
	file = fopen(f, "r");
	if (errno == EACCES) 
	{
		/* Forbidden /**/
		write_FORBIDDEN(t_data->clientsd, curr_time);
		write_to_log(getline, "403 Forbidden", 
		    t_data->clientip, logfile);
		pthread_exit((void*)t_data->tid);
	}
	if (file == NULL)
	{
		/* Not Found /**/
		write_NOT_FOUND(t_data->clientsd, curr_time);
		write_to_log(getline, "404 Not Found", 
		    t_data->clientip, logfile);
		pthread_exit((void*)t_data->tid);
	}

	/* Write the file to the client /**/
	fseek(file, 0L, SEEK_END);
	sprintf(file_length_buf, "%ld", ftell(file));
	fseek(file, 0L, SEEK_SET);
	total_written = write_OK(t_data->clientsd, curr_time, file, 
				 file_length_buf);
	sprintf(tw, "%d", total_written);
	strcat(tw, "/");
	strcat(tw, file_length_buf);
	memset(f, 0, sizeof(f));
	strlcpy(f, "200 OK ", sizeof(f));
	strcat(f, tw);
	write_to_log(getline, f, t_data->clientip, logfile);

	/* Close file and thread /**/
	fclose(file);
	pthread_exit((void*)t_data->tid);
}

/* Write to the log file /**/
void write_to_log(char *getline, char *completion, char *ip, FILE *logfile)
{
	/* prevent 2 or more threads from writing to the logfile at once /**/
	pthread_mutex_lock(&lock);

	char curr_time[BUF_SIZE] = {0};	
	set_current_time(curr_time);
	fprintf(logfile, "%s\t%s\t%s\t%s\n", curr_time, ip, getline, 
		completion);
	fclose(logfile);
	
	/* Unlock file /**/
	pthread_mutex_unlock(&lock);
}

/* Write to the client /**/
int write_to_client(int clientsd, char *message)
{
	ssize_t written, w;

	w = 0;
	written = 0;
	while (written < strlen(message)) {
		w = write(clientsd, message + written,
		    strlen(message) - written);
		if (w == -1) {
			if (errno != EINTR)
				return -1;
		}
		else
			written += w;
	}
	return written;
}

/* Read the request sent by the client /**/
int read_client_request(int clientsd, char * buffer)
{
	size_t maxread;
	ssize_t r, rc;
	int reading;

	r = -1; 
	rc = 0;
	maxread = BUF_SIZE - 1;
	reading = 1;

	while( (r != 0) && (rc < maxread) && reading ) 
	{
		int i;
		r = read(clientsd, buffer + rc, maxread - rc);

		for (i = 0; i < strlen(buffer); i++)
		{
			if (buffer[i] == '\n')
			{
				reading = 0;
				break;
			}
		}
		if (r == -1)
		{
			if (errno != EINTR)
				return -2;
		} else
			rc += r;
	}

	buffer[rc] = '\0';
	/* check for blank line /**/
	if (buffer[rc - 1] == '\n' && buffer[rc - 2] == '\n')
		return rc;
	else if (buffer[rc - 1] == '\n' && buffer[rc - 2] == '\r' 
	    && buffer[rc - 3] == '\n')
		return rc;
	else
		return -1;
}

/* get the directory in the getline and check the rest of the header /**/
int get_directory(char *buffer, char *directory, char *getline)
{
	char line[BUF_SIZE] = {0};
	char get[BUF_SIZE] = {0};
	char http[BUF_SIZE] = {0};
	char from[BUF_SIZE] = {0};
	char useragent[BUF_SIZE] = {0};
	int position = 0;

	/* check for GET line /**/
	position = get_next_line(buffer, getline, position);

	if (position == -1)
		return -1;
	if ( (sscanf(getline, "%s %s %s", get, directory, http) != 3)
		|| (strcmp(get, "GET") != 0) 
		|| (strcmp(http, "HTTP/1.1") != 0) )
		return -1;

	/* Check for From line /**/
	position = get_next_line(buffer, line, position);

	if (position == -1)
		return -1;
	sscanf(line, "%s", from);
	if (strcmp(from, "From:") != 0 && strcmp(from, "Host:") != 0)
		return -1;

	/* Check for User Agent line /**/
	memset(line, 0, sizeof(line));
	get_next_line(buffer, line, position);

	sscanf(line, "%s", useragent);
	if (strcmp(useragent, "User-Agent:") != 0)
		return -1;

	return 1;
}

/* get the next line from a buffer starting at position i /**/
int get_next_line(char *buffer, char * line, int i)
{
	if (i >= strlen(buffer))
		return -1;

	int j = 0;
	while (i < strlen(buffer) && buffer[i] != '\n')
	{
		line[j] = buffer[i];
		j++;
		i++;
	}

	return i + 1;
}


/* Get the current time /**/
void set_current_time(char * t)
{
	time_t rawtime;
	struct tm * timeinfo;

	time(&rawtime);
	timeinfo = localtime(&rawtime);

	strftime(t, 80, "%a, %d %b %Y %X %Z", timeinfo);
}

/* Get the port, check validity /**/
int get_port(char * port)
{
	u_long p;
	char *ep;

	errno = 0;
	p = strtoul(port, &ep, 10);
	
	/* Port parameter is null or non numerical /**/
	if (*port == '\0' || *ep != '\0') 
		err(1, "port is not a number."); 
	/* Port is out of range /**/
	if ( (errno == ERANGE && p == ULONG_MAX) || (p > USHRT_MAX) )
		err(1, "port value out of range."); 
	return p;
}

/* Write a 400 Bad Request response to the client /**/
void write_BAD_REQUEST(int clientsd, char *curr_time) 
{
	char temp[BUF_SIZE] = {0};
	strcat(temp, "HTTP/1.1 400 Bad Request\nDate: ");
	strcat(temp, curr_time);
	strcat(temp, "\nContent-Type: text/html\nContent-Length: 107");
	strcat(temp, "\n\n<html><body>\n<h2>Malformed Request</h2>\n");
	strcat(temp, 
	    "Your browser sent a request I could not understand.\n");
	strcat(temp, "</body></html>");
	write_to_client(clientsd, temp);
}

/* Write a 403 Forbidden response to the client /**/
void write_FORBIDDEN(int clientsd, char *curr_time)
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
	write_to_client(clientsd, temp);
}

/* Write a 404 Not Found response to the client /**/
void write_NOT_FOUND(int clientsd, char *curr_time)
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
	write_to_client(clientsd, temp);
}

/* Write a 500 Internal Server Error to the client /**/
void write_INTERNAL_SERVER_ERROR(int clientsd, char *curr_time) 
{
	char temp[BUF_SIZE] = {0};
	strcat(temp, "HTTP/1.1 500 Internal Server Error\nDate: ");
	strcat(temp, curr_time);
	strcat(temp, "\nContent-Type: text/html\n");
	strcat(temp, "Content-Length: 131\n\n");
	strcat(temp, "<html><body>\n<h2>Oops. That didn't work");
	strcat(temp, "</h2>\nI had some sort of problem dealing with ");
	strcat(temp, "your request. Sorry, I'm lame.\n");
	strcat(temp, "</body></html>");
	write_to_client(clientsd, temp);
}

/* Write a 200 OK response to the client /**/
int write_OK(int clientsd, char *curr_time, FILE *file, char *file_length_buf)
{
	int total_written = 0;
	int written = 0;
	char buffer[1024] = {0};

	write_to_client(clientsd, "HTTP/1.1 200 OK\n");
	write_to_client(clientsd, "Date: ");
	write_to_client(clientsd, curr_time);
	write_to_client(clientsd, "\nContent-Type: text/html\n");
	write_to_client(clientsd, "Content-Length: ");
	write_to_client(clientsd, file_length_buf);
	write_to_client(clientsd, "\n\n");
	while (fgets(buffer, sizeof(buffer), file) != NULL)
	{
		written = write_to_client(clientsd, buffer);
		if (written == -1)
			return total_written;
		else 
			total_written += written;
	}
	return total_written;
}


