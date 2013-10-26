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
 * Simple server implementation that forks a child process to service each
 * request it gets from the network.
 *
 * Compile using 'gcc -o server_f server_f.c'
 * or compile with 'make server_f'
 * or compile with 'make all'
 *
 * Run as ./server_f 8000 /some/where/documents /some/where/logfile
 * where 8000 is the port number, 
 * /some/where/documents is the directory of html files, and
 * /some/where/logfile is the directory of the log file
 */

#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <netinet/in.h>

#include <err.h>
#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* Defined Variables /**/
#define BUF_SIZE 4096

/* Function Prototypes /**/
static void handle_child(int);
void handle_client(int, char *);
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

/* Global variables (file directories) /**/
char dir_documents[80];
char dir_logfile[80];

int main(int argc, char * argv[]) 
{
	struct sockaddr_in sockname, client;
	struct sigaction sa;
	int clientlen, sigdata;
	u_short port;
	pid_t pid;

	if (daemon(1, 0) == -1)
		err(1, "daemon() failed");

	/* Check if there are 4 arguments passed /**/
	if (argc != 4)
		err(1, "RUN AS: ./server_f 8000 /dir/documents/ /dir/logfile");
	
	/* Handler for child processes /**/
	sa.sa_handler = handle_child;
	sigemptyset(&sa.sa_mask);
	sa.sa_flags = SA_RESTART;
	if (sigaction(SIGCHLD, &sa, NULL) == -1)
		err(1, "sigaction failed");

	/* set arguments to variables /**/
	port = get_port(argv[1]);	
	strlcpy(dir_documents, argv[2], sizeof(dir_documents));
	strlcpy(dir_logfile, argv[3], sizeof(dir_logfile));

	/* Set up the socket /**/
	memset(&sockname, 0, sizeof(sockname));
	sockname.sin_family = AF_INET;
	sockname.sin_port = htons(port);
	sockname.sin_addr.s_addr = htonl(INADDR_ANY);
	sigdata = socket(AF_INET, SOCK_STREAM, 0);
	if (sigdata == -1)
		err(1, "socket failed");
	if (bind(sigdata, (struct sockaddr *) &sockname, sizeof(sockname)) 
	    == -1)
		err(1, "bind failed");
	if (listen(sigdata,5) == -1)
		err(1, "listen failed");

	/* Start listening for connections /**/
	while(1) 
	{
		int clientsd;
		clientlen = sizeof(&client);
		clientsd = accept(sigdata, (struct sockaddr *)&client, 
				  &clientlen);
		if (clientsd == -1)
			err(1, "accept failed");
		pid = fork();
		if (pid == -1)
			err(1, "fork failed");
		if (pid == 0)
		{
			char client_ip[INET_ADDRSTRLEN];
			inet_ntop(AF_INET,&(client.sin_addr), 
			    client_ip, INET_ADDRSTRLEN);
			handle_client(clientsd, client_ip);
			exit(0);
		}
		close(clientsd);
	}
}


/* Handle zombie child processes /**/
static void handle_child(int signum) 
{
	waitpid(WAIT_ANY, NULL, WNOHANG);	
}

/* Handle the client /**/
void handle_client(int clientsd, char *client_ip)
{
	FILE *file;
	FILE *logfile;
	char buffer[BUF_SIZE] = {0};
	char filebuf[BUF_SIZE] = {0};
	char getline[BUF_SIZE] = {0};
	char getdirc[BUF_SIZE] = {0};
	char curr_time[BUF_SIZE] = {0};
	char file_length_buf[BUF_SIZE] = {0};
	char total_writtenbuf[BUF_SIZE] = {0};
	int  total_written;
	int  read;

	/* Get time, read request /**/
	set_current_time(curr_time);
	read = read_client_request(clientsd, buffer);

	/* Open log file, handle file errors /**/
	logfile = fopen(dir_logfile, "a");
	if (logfile == NULL)
	{
		/* Log file doesn't exist /**/
		get_next_line(buffer, getline, 0);
		write_INTERNAL_SERVER_ERROR(clientsd, curr_time);
		return;	
	}

	if (read == -1) {
		/* Blank line failed /**/
		write_BAD_REQUEST(clientsd, curr_time);
		get_next_line(buffer, getline, 0);
		write_to_log(getline, "400 Bad Request", client_ip, logfile);
		return;
	} else if (read == -2) {
		/* Read file failed /**/
		write_INTERNAL_SERVER_ERROR(clientsd, curr_time);
		write_to_log(getline, "500 Internal Server Error", 
		    client_ip, logfile);
		return;
	}
	/* Retrieve directory of getline /**/
	if (get_directory(buffer, getdirc, getline) == -1)
	{
		write_BAD_REQUEST(clientsd, curr_time);
		write_to_log(getline, "400 Bad Request", client_ip, logfile);
		return;
	}

	/* Get the requested file /**/
	memset(filebuf, 0, sizeof(filebuf));
	strlcpy(filebuf, dir_documents, sizeof(filebuf));
	strcat(filebuf, getdirc);
	errno = 0;
	file = fopen(filebuf, "r");
	if (errno == EACCES) 
	{
		/* Forbidden /**/
		write_FORBIDDEN(clientsd, curr_time);
		write_to_log(getline, "403 Forbidden", client_ip, logfile);
		return;		
	}
	if (file == NULL)
	{
		/* Not Found /**/
		write_NOT_FOUND(clientsd, curr_time);
		write_to_log(getline, "404 Not Found", client_ip, logfile);
		return;
	}

	/* Write the file to the client /**/
	fseek(file, 0L, SEEK_END);
	sprintf(file_length_buf, "%ld", ftell(file));
	fseek(file, 0L, SEEK_SET);
	total_written = write_OK(clientsd, curr_time, file, file_length_buf);
	sprintf(total_writtenbuf, "%d", total_written);
	strcat(total_writtenbuf, "/");
	strcat(total_writtenbuf, file_length_buf);

	memset(filebuf, 0, sizeof(filebuf));
	strlcpy(filebuf, "200 OK ", sizeof(filebuf));
	strcat(filebuf, total_writtenbuf);
	write_to_log(getline, filebuf, client_ip, logfile);

	/* Close file /**/
	fclose(file);
}

/* Read the request sent by the client /**/
int read_client_request(int clientsd, char * buffer)
{
	size_t maxread;
	ssize_t reader, readerc;
	int reading;

	reader = -1; 
	readerc = 0;
	maxread = BUF_SIZE - 1;
	reading = 1;

	while( (reader != 0) && (readerc < maxread) && reading ) 
	{
		int i;
		reader = read(clientsd, buffer + readerc, maxread - readerc);

		for (i = 0; i < strlen(buffer); i++)
		{
			if (buffer[i] == '\n')
			{
				reading = 0;
				break;
			}
		}
		if (reader == -1)
		{
			if (errno != EINTR)
				return -2;
		} else
			readerc += reader;
	}

	buffer[readerc] = '\0';
	if (buffer[readerc - 1] == '\n' && buffer[readerc - 2] == '\n') {
		return readerc;
	} else if (buffer[readerc - 1] == '\n' && buffer[readerc - 2] == '\r' 
		   && buffer[readerc - 3] == '\n') {
		return readerc;
	} else {
		return -1;
	}
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
		i++;
		j++;
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

	/* Port parameter is null or non numerical /**/
	if (*port == '\0' || *ep != '\0') 
		err(1, "Port is non numerical."); 
	/* Port is out of range /**/
	if ( (errno == ERANGE && p == ULONG_MAX) || (p > USHRT_MAX) )
		err(1, "Port value is out of range."); 
	return p;
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

/* Write to the log file /**/
void write_to_log(char *getline, char *completion, char *ip, FILE *logfile)
{
	char curr_time[BUF_SIZE] = {0};	
	set_current_time(curr_time);
	fprintf(logfile, "%s\t%s\t%s\t%s\n", curr_time, 
	    ip, getline, completion);
	fclose(logfile);
}

/* Write a 200 OK response to the client /**/
int write_OK(int clientsd, char *curr_time, FILE *file, char *file_length_buf)
{
	int total_written = 0;
	int written = 0;
	char buffer[BUF_SIZE] = {0};

	write_to_client(clientsd, "HTTP/1.1 200 OK\n");
	write_to_client(clientsd, "Date: ");
	write_to_client(clientsd, curr_time);
	write_to_client(clientsd, "\nContent-Type: text/html\n");
	write_to_client(clientsd, "Content-Length: ");
	write_to_client(clientsd, file_length_buf);
	write_to_client(clientsd, "\n\n");
	/* Return the file to the client /**/
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

