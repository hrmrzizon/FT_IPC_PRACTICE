/*
	client_pipe.c
	파이프를 사용한 파일 전송 클라이언트 소스입니다.
	사용의 전제는 다음과 같습니다.
		1. 서버 프로그램이 같은 경로에 존재함.
		2. 서버 프로그램이 클라이언트를 킬 시 반드시 켜져 있어야함.
		3. 파일은 단순히 이름으로 올라가기만 합니다. 
		4. 중복되는 이름은 서버/클라이언트에서 처리할 수 없습니다.
	
	위의 전제를 사용해 클라이언트는 서버에게 파일 전송을 요청하는 정보를 보낸 후,
	이를 처리하는 쓰레드를 생성합니다.
	송신단에서는 파이프의 크기제한을 고려하여 spinlock 이 필요하면 걸어줍니다.
	수신단에서는 간단하게 값을 받아옵니다.

	이러한 동작은 서버가 요청을 받아서 문제없이 처리한다는 가정하에 이루어집니다.
 */

#define _GNU_SOURCE

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ipc.h>
#include <sys/msg.h>
#include <fcntl.h>

#include <errno.h>
#include <unistd.h>
#include <pthread.h>
#include <signal.h>

#include <sys/ioctl.h>

#define SAFE_FREE(x) \
if(x) \
{ \
	free(x); \
	x = NULL; \
}
#define SAFE_FREE_PTR_ARRAY(x,len) \
if(x) \
{ \
	for(int i = 0; i < len; i++) \
		SAFE_FREE(x[i]); \
	SAFE_FREE(x); \
} 

#include "file_util.h"

void fatal(const char* msg)
{
	perror(msg);
	exit(1);
}

int upload_cnt;
char **upload_path;
int download_cnt;
char **download_path;
char *download_path_parent;

int interpreted_input_cleanup();
int interpret_input(int argc, char** argv, int* upload_cnt_ref, char*** upload_path_ref, int* download_cnt_ref, char*** download_path_ref, char** download_path_parent_ref);

pthread_t* threads;
int* result_flag;

// FIFO 변수 및 함수
#define REQ_FIFO_PERM 		0666
#define IO_FIFO_PERM		0666
#define MSG_BUFFER_SZ		2048

int fifo_cnt;
char** fifo_paths;

void cleanup_fifo()
{
	if (fifo_paths)
		for (int i = 0; i < fifo_cnt; i++)
			if (fifo_paths[i])
				unlink(fifo_paths[i]);

	SAFE_FREE_PTR_ARRAY(fifo_paths, fifo_cnt);
}

// 공유 자원을 전부 정리합니다.
// 해당 소스에서는 FIFO 를 정리합니다.
void signal_handler(int signal)
{
	cleanup_fifo();
	exit(1);
}
// FIFO 변수 및 함수

int download(char* filename, int idx);
int upload(char* filename, int idx);

void* file_task(void* pidx)
{
	int idx = *(int*)pidx;
	if (idx < upload_cnt)
		result_flag[idx] = upload(upload_path[idx], idx);
	else
		result_flag[idx] = download(download_path[idx-upload_cnt], idx);
	free(pidx);

	return NULL;
}

// 서버에서 FIFO로 데이터를 저장한 것을 받아와서 파일에 써줍니다.
int download(char* filename, int idx)
{
	char path_buffer[512];
	if (download_path_parent != NULL)
		sprintf(path_buffer, "%s/%s", download_path_parent, filename);
	else
		sprintf(path_buffer, "%s", filename);

	int fifo_fd = open(fifo_paths[idx], O_RDWR, IO_FIFO_PERM),
		make_fd = open(path_buffer, O_RDWR | O_CREAT, 0666);

	if (fifo_fd < 0)
	{
		unlink(path_buffer);
		unlink(fifo_paths[idx]);	
		return -1;
	}
	if (make_fd < 0)
	{
		close(fifo_fd);
		unlink(path_buffer);
		unlink(fifo_paths[idx]);	
		return -2;
	}

	char buffer[MSG_BUFFER_SZ];
	int read_len = 0, filesize = 0, accum = 0;

	if ((read_len = read(fifo_fd, buffer, 4)) < 0)
	{
		close(fifo_fd);
		close(make_fd);
		unlink(path_buffer);
		unlink(fifo_paths[idx]);
		return -3;
	}
	filesize = *((int*)buffer);

	while(1)
	{
		read_len = read(fifo_fd, buffer, MSG_BUFFER_SZ);
		if (!read_len) break;
		if (read_len < 0)
		{
			close(fifo_fd);
			close(make_fd);
			unlink(path_buffer);
			unlink(fifo_paths[idx]);
			return -3;
		}
		write(make_fd, buffer, read_len);
		accum += read_len;

		if (accum == filesize)
			break;
	}

	close(make_fd);
	close(fifo_fd);
	if (unlink(fifo_paths[idx]) < 0)
		return -5;

	return 1;
}

int get_max_fifo_size(int fifofd)
{
	return fcntl(fifofd, F_GETPIPE_SZ);
}

int get_used_fifo_size(int fifofd)
{
	int sz = 0;
	ioctl(fifofd, FIONREAD, &sz);
	return sz;
}

int get_remain_fifo_size(int fifofd)
{
	int sz = 0;
	ioctl(fifofd, FIONREAD, &sz);
	return fcntl(fifofd, F_GETPIPE_SZ) - sz;
}

// 파일에서 읽어서 FIFO 에 데이터를 넣어줍니다. 사용할 크기가 부족하면 spinlock 처럼 기다립니다.
int upload(char* filename, int idx)
{
	int file_fd = open(filename, O_RDONLY);
	int fifo_fd = open(fifo_paths[idx], O_RDWR);

	if (file_fd < 0)
	{
		unlink(fifo_paths[idx]);
		return -2;
	}
	if (fifo_fd < 0)
	{
		unlink(fifo_paths[idx]);
		close(file_fd);
		return -1;
	}

	char buffer[MSG_BUFFER_SZ];
	int sz = 0;
	int read_len = 0;
	while(1)
	{
		read_len = read(file_fd, buffer, MSG_BUFFER_SZ);
		if (!read_len) break;
		if (read_len < 0)
		{
			return -3;
		}

		while (get_remain_fifo_size(fifo_fd) < read_len);

		if (write(fifo_fd, buffer, read_len) < 0)
		{
			unlink(fifo_paths[idx]);
			return -4;
		}
	}

	close(file_fd);
	while (get_used_fifo_size(fifo_fd));
	close(fifo_fd);

	return 1;
}

char* flag_to_state(int flag)
{
	if (flag == 0)
		return "In progress..";
	else if(flag < 0)
	{
		switch(flag)
		{
			case -1:
				return "Fail to get fifo..";
			case -2:
				return "Fail to open file..";
			case -3:
				return "Fail to read..";
			case -4:
				return "Fail to write..";
			case -5:
				return "Fail to clear fifo..";
		}
		return "Fail to process file..";
	}
   	else if(flag > 0)
		return "Success!";
}

void print_current_state()
{
	for(int i = 0; i < upload_cnt; i++)
		printf("upload %2d:%s:%s\n", i, upload_path[i], flag_to_state(result_flag[i]));
	for(int i = 0; i < download_cnt; i++)
		printf("download %2d:%s:%s\n", i, download_path[i], flag_to_state(result_flag[i + upload_cnt]));
}

char* get_last_filename(char* directory)
{
	char* filename = directory, *temp;
	while(temp = strchr(filename, '/'))
		filename = temp+1;
	return filename;
}

int main(int argc, char** argv)
{
	if (argc < 3)
	{
		puts("usage: client_pipe ([upload|download] [filepath|filepath,..] )*");
		return 1;
	}

	signal(SIGINT, signal_handler);
	signal(SIGABRT, signal_handler);
	signal(SIGHUP, signal_handler);
	signal(SIGKILL, signal_handler);

	// 파일 경로 처리
	interpret_input(argc, argv, &upload_cnt, &upload_path, &download_cnt, &download_path, &download_path_parent);

	int cnt = fifo_cnt = upload_cnt + download_cnt;
	char buffer[MSG_BUFFER_SZ];

	result_flag = (int*)malloc(cnt * sizeof(int));
	memset(result_flag, 0, sizeof(int) * cnt);

	// FIFO 경로 할당 및 설정
	fifo_paths = (char**)malloc(fifo_cnt * sizeof(char*));
	for (int i = 0; i < fifo_cnt; i++)
	{
		sprintf(buffer, "./fifo/%d_%d", getpid(), i);
		fifo_paths[i] = (char*)malloc((strlen(buffer)+1) * sizeof(char));
		strcpy(fifo_paths[i], buffer);
		fifo_paths[i][strlen(buffer)] = '\0';
	}

	if (cnt > 0)
	{
		// 서버에서 요청 FIFO 이 생성된 전제하에 단순히 열기만 합니다.
		int rqfifo_id = open("./fifo/requests", O_RDWR, REQ_FIFO_PERM);
		if (rqfifo_id < 0)
		{
			perror("cannot open request fifo..");
			goto cleanup;
		}

		printf("GET FIFO: ./fifo/requests:%d\n", rqfifo_id);
	
		int write_count = 0;
		char* temp = buffer;	
		// CLI 레벨에서 들어온 데이터에 따라서 FIFO 와 여러 것들을 초기화합니다.
		for (int i = 0; i < cnt; i++)
		{
			char* filename;
			if (i < upload_cnt)
				filename = upload_path[i];
			else
				filename = download_path[i-upload_cnt];
			filename = get_last_filename(filename);

			// FIFO 생성
			if (mkfifo(fifo_paths[i], IO_FIFO_PERM) < 0)
			{
				perror("cannot make I/O fifo..");
				goto cleanup;
			}

			int filesize = 0;
			struct stat st;
			stat(filename, &st);
			filesize = st.st_size;

			// request message <- 1/0: upload/download, filesize, file name, fifo path
			int buffer_string_count = sprintf(temp, "%d %d %s %s\n", i < upload_cnt, filesize, filename, fifo_paths[i]);
			temp = temp + buffer_string_count;
			write_count += buffer_string_count;
		}

		// 버퍼에 저장된 모든 요청 정보를 한꺼번에 보냅니다.
		if (write(rqfifo_id, buffer, write_count) < 0)
		{
			perror("fail to send request message..");
			goto cleanup;
		}

		threads = (pthread_t*)malloc(cnt * sizeof(pthread_t));
		for (int i = 0; i < cnt; i++)
		{
			int *pi = (int*)malloc(sizeof(int));
			*pi = i;
			pthread_create(threads + i, NULL, file_task, pi);
		}

		// 처리 할 때까지 상태 출력하며 대기
		while(1)
		{
			int check = 1;
			for (int i = 0; i < cnt; i++)
				if (result_flag[i] == 0)
					check = 0;
				
			system("clear");
			print_current_state();

			if (check)	break;
			else		sleep(1);
		}

		system("clear");
		// 처리 끝 난 후 출력
		for (int i = 0; i < cnt; i++)
		{
			char* filename;

			if (i < upload_cnt)
				filename = upload_path[i];
			else
				filename = download_path[i-upload_cnt];
			
			printf("%d. %4s, %4s, %4s\n", 
					i, 
					(i < upload_cnt? "upload  ": "download"), 
					filename, 
					result_flag[i] == 1? "success!": "fail..");
		}

		close(rqfifo_id);
	}

cleanup:
	SAFE_FREE(result_flag);
	SAFE_FREE(threads);
	
	cleanup_fifo();
	interpreted_input_cleanup();

	return 0;
}

// 파라미터 정보 정리
int interpreted_input_cleanup()
{
	SAFE_FREE_PTR_ARRAY(upload_path, upload_cnt);
	SAFE_FREE_PTR_ARRAY(download_path, download_cnt);
	SAFE_FREE(download_path_parent);

	return 0;
}


// 업로드 / 다운로드에 따라서 인자들을 원하는 메모리 레이아웃으로 매핑
int interpret_input(int argc, char** argv, int* upload_cnt_ref, char*** upload_path_ref, int* download_cnt_ref, char*** download_path_ref, char** download_path_parent_ref)
{
	char buffer[256];
	int state = 0;
	for (int i = 1; i < argc; i++)
	{
		char* item = argv[i];
		switch(state)
		{
			case 0:
				if (strcmp(argv[i], "upload") == 0)
					state = 1;
				else if (strcmp(argv[i], "download") == 0)
					state = 2;
				else if (strcmp(argv[i], "dpath") == 0)
					state = 3;
				else
				{
					fprintf(stderr, "Argument%d:%s is not behaviour..", i, argv[i]);
					exit(1);
				}
				break;
			case 1:
				{
					strcpy(buffer, item);
					buffer[strlen(item)] = '\0';
					
					char* token = strtok(buffer, ",");
					int fd;

					while(token != NULL)
					{
						if (*upload_path_ref == NULL)
						{
							*upload_path_ref = (char**)malloc(sizeof(char*));
							(*upload_path_ref)[0] = (char*)malloc(sizeof(char)*(strlen(token)+1));
							strcpy((*upload_path_ref[0]), token);
							(*upload_path_ref)[0][strlen(token)] = '\0';
							*upload_cnt_ref = 1;
						}
						else
						{
							*upload_path_ref = (char**)realloc(*upload_path_ref, sizeof(char*)*++(*upload_cnt_ref));
							(*upload_path_ref)[*upload_cnt_ref-1] = (char*)malloc(sizeof(char)*(strlen(token)+1));
							strcpy((*upload_path_ref)[*upload_cnt_ref-1], token);
							(*upload_path_ref)[*upload_cnt_ref-1][strlen(token)] = '\0';
						}
						token = strtok(NULL, ",");
					}
					state = 0;
				}
				break;
			case 2:
				{
					strcpy(buffer, item);
					buffer[strlen(item)] = '\0';
					
					char* token = strtok(buffer, ",");
					int fd;

					while(token != NULL)
					{
						if (*download_path_ref == NULL)
						{
							*download_path_ref = (char**)malloc(sizeof(char*));
							(*download_path_ref)[0] = (char*)malloc(sizeof(char)*(strlen(token)+1));
							strcpy((*download_path_ref[0]), token);
							(*download_path_ref)[0][strlen(token)] = '\0';
							*download_cnt_ref = 1;
						}
						else
						{
							*download_path_ref = (char**)realloc(*download_path_ref, sizeof(char*)*++(*download_cnt_ref));
							(*download_path_ref)[*download_cnt_ref-1] = (char*)malloc(sizeof(char)*(strlen(token)+1));
							strcpy((*download_path_ref)[*download_cnt_ref-1], token);
							(*download_path_ref)[*download_cnt_ref-1][strlen(token)] = '\0';
						}
						token = strtok(NULL, ",");
					}
					state = 0;
				}
				break;
			case 3:
				{
					int len = strlen(argv[i]);
					*download_path_parent_ref = (char*)malloc(sizeof(char)*(len+1));
					strcpy(*download_path_parent_ref, argv[i]);
					(*download_path_parent_ref)[strlen(*download_path_parent_ref)] = '\0';
					state = 0;
				}
				break;
		}

	}

	return 0;
}
