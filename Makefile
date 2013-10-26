#
#  Copyright (c) 2013 Alexander Wong <admin@alexander-wong.com>
#
#  This program is free software: you can redistribute it and/or modify
#  it under the terms of the GNU General Public License as published by
#  the Free Software Foundation, either version 3 of the License, or
#  (at your option) any later version.
#
#  This program is distributed in the hope that it will be useful,
#  but WITHOUT ANY WARRANTY; without even the implied warranty of
#  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
#  GNU General Public License for more details.
#
#  You should have received a copy of the GNU General Public License
#  along with this program.  If not, see <http://www.gnu.org/licenses/>.
#

# 'make all' to make server_f, server_p, and server_s.
# 'make server_f' to make server_f.
# 'make server_p' to make server_p.
# 'make server_s' to make server_s
# 'make clean' to clean all object files, executable byte code.

clean:
	-rm -f *.o all server_f server_p server_s core

all: server_f.c server_p.c server_s.c strlcpy.c
	gcc -c strlcpy.c
	gcc -o server_f server_f.c strlcpy.o 
	gcc -o server_p server_p.c strlcpy.o -lpthread 
	gcc -o server_s server_s.c strlcpy.o 

server_f: server_f.c
	gcc -c strlcpy.c
	gcc -o server_f server_f.c strlcpy.o 

server_p: server_p.c
	gcc -c strlcpy.c
	gcc -o server_p server_p.c strlcpy.o -lpthread 

server_s: server_s.c
	gcc -c strlcpy.c
	gcc -o server_s server_s.c strlcpy.o 