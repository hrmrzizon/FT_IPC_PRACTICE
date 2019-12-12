CC = gcc
CFLAGS = -std=c11 -D_XOPEN_SOURCE=700 -pthread -lrt -g
LIBS = 
INCLUDES = -I ./ 
#TARGET = client_shm client_mp client_pipe server_shm server_mp server_pipe
TARGET = client_mp client_pipe server_mp server_pipe

# CLIENT_SHM_OBJ	= client_shm.c 	file_util.c
CLIENT_MP_OBJ   = client_mp.c	file_util.c
CLIENT_PIPE_OBJ = client_pipe.c	file_util.c
# SERVER_SHM_OBJ	= server_shm.c	file_util.c
SERVER_MP_OBJ	= server_mp.c	file_util.c
SERVER_PIPE_OBJ	= server_pipe.c	file_util.c

all: $(TARGET) 

%.o: %.c
	$(CC) -c $(CFLAGS) -o $@ $(INCLUDES) $<

client_mp: $(CLIENT_MP_OBJ)
	$(CC) $(CFLAGS) -o $@ $(CLIENT_MP_OBJ) $(INCLUDES)

server_mp: $(SERVER_MP_OBJ)
	$(CC) $(CFLAGS) -o $@ $(SERVER_MP_OBJ) $(INCLUDES)

mp: client_mp server_mp 

client_pipe: $(CLIENT_PIPE_OBJ)
	$(CC) $(CFLAGS) -o $@ $(CLIENT_PIPE_OBJ) $(INCLUDES)

server_pipe: $(SERVER_PIPE_OBJ)
	$(CC) $(CFLAGS) -o $@ $(SERVER_PIPE_OBJ) $(INCLUDES)

pipe: client_pipe server_pipe

# client_shm: $(CLIENT_SHM_OBJ)
# 	$(CC) $(CFLAGS) -o $@ $(CLIENT_SHM_OBJ) $(INCLUDES)

# server_shm: $(SERVER_SHM_OBJ)
# 	$(CC) $(CFLAGS) -o $@ $(SERVER_SHM_OBJ) $(INCLUDES)

# shm: client_shm server_shm

cleano:
	rm *.o

clean:
	rm $(TARGET)
	rm *.o

