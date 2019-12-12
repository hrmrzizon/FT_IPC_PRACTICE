/*
	server_pipe.c
	파이프를 사용한 파일 전송 서버 소스입니다.
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
#include <sys/ioctl.h>
#include <fcntl.h>
#include <sys/ipc.h>
#include <sys/msg.h>

#include <errno.h>
#include <unistd.h>
#include <pthread.h>
#include <signal.h>

#include "file_util.h"

#define REQ_MPQ_PERM 		0666
#define IO_MPQ_PERM			0666
#define MSG_BUFFER_SZ		2048	

void signal_handler(int signal)
{
	unlink("./fifo/requests");
	exit(1);
}

void fatal(const char* msg)
{
	perror(msg);
	exit(1);
}

typedef struct file_request
{
	int is_uploaded;
	int filesize;
	char* filename;
	// FIFO 파일 경로
	char* fifopath;
} file_req;

int receive_upload(file_req* pr, char* buffer);
int send_download(file_req* pr, char* buffer);

void* file_task(void* p)
{
	char buffer[MSG_BUFFER_SZ];
	file_req* preq = (file_req*)p;
	int result = preq->is_uploaded? receive_upload(preq, buffer): send_download(preq, buffer);

	if (result < 0)
	{
		switch(result)
		{
			case -1:
				printf(">> file_task: file(%s) cannot open..\n", preq->filename);
				break;
			case -2:
				printf(">> file_task: fifo(%s) cannot open..\n", preq->fifopath);
				break;
			default:
				printf(">> file_task: unknown error(%d)\n", result);
				break;
		}
	}

	free(preq->filename);
	free(preq->fifopath);
	free(preq);

	return NULL;
}

// 업로드/ 클라이언트에서 보낸 FIFO 데이터를 FILE에 넣어줍니다.
int receive_upload(file_req* pr, char* buffer)
{
	struct timespec tstart, tend;
	printf(">> receive_upload(fs=%d,name=\"%s\",fifo=\"%s\") start!\n", pr->filesize, pr->filename, pr->fifopath);

	sprintf(buffer, "./file/%s", pr->filename);
	int nwfd = open(buffer, O_WRONLY | O_CREAT);
	int fifo = open(pr->fifopath, O_RDWR);

	if (nwfd < 0)
		return -1;
	if (pipe < 0)
		return -2;

	int loop_cnt = 0;
	long accum_time = 0;
	int read_len = 0, accum = 0;
	while(1)
	{
		clock_gettime(CLOCK_REALTIME, &tstart);
		read_len = read(fifo, buffer, MSG_BUFFER_SZ);
		clock_gettime(CLOCK_REALTIME, &tend);

		if (tend.tv_nsec - tstart.tv_nsec > 0)
			accum_time += tend.tv_nsec - tstart.tv_nsec;

		if (!read_len) break;
		if (read_len < 0)
		{
			close(fifo);
			unlink(pr->fifopath);
			return -3;
		}
		write(nwfd, buffer, read_len);
		accum += read_len;

		if (accum == pr->filesize)
			break;
	}

	close(nwfd);
	
	close(fifo);
	unlink(pr->fifopath);

	printf(">> receive_upload(fs=%d,name=\"%s\",fifo=\"%s\") end(%ld)!\n", pr->filesize, pr->filename, pr->fifopath, accum_time);
	return 0;
}

int get_max_fifo_size(int fifofd)
{
	return fcntl(fifofd, F_GETPIPE_SZ);
}

int get_remain_fifo_size(int fifofd)
{
	int sz = 0;
	ioctl(fifofd, FIONREAD, &sz);
	return fcntl(fifofd, F_GETPIPE_SZ) - sz;
}

// 다운로드/ 클라이언트가 요청한 파일을 FIFO에 넣어줍니다. 
// 여기서도 스핀락으로 파이프 크기에 따라 조절합니다.
int send_download(file_req* pr, char* buffer)
{
	struct timespec tstart, tend;

	printf(">> send_download(fs=%d,name=\"%s\",fifo=\"%s\") start!\n", pr->filesize, pr->filename, pr->fifopath);

	sprintf(buffer, "./file/%s", pr->filename);
	int odfd = open(buffer, O_RDONLY);
	int fifo = open(pr->fifopath, O_RDWR);

	struct stat st;
	stat(buffer, &st);
	pr->filesize = st.st_size;
	
	printf(">> send_download(fs=%d,name=\"%s\",fifo=\"%s\") update fs\n", pr->filesize, pr->filename, pr->fifopath);

	if (odfd < 0)
		return -1;
	if (fifo < 0)
		return -2;

	*(int*)buffer = pr->filesize;

	if (write(fifo, buffer, 4) < 0)
	{
		close(fifo);
		return -3;
	}

	int loop_cnt = 0;
	long accum_time = 0;
	int read_len = 0, accum = 0, idx = 0;
	while(1)
	{
		read_len = read(odfd, buffer, MSG_BUFFER_SZ);
		if (!read_len) break;
		accum += read_len;

		while(1)
			if (get_remain_fifo_size(fifo) >= read_len)
				break;


		clock_gettime(CLOCK_REALTIME, &tstart);
		if (write(fifo, buffer, read_len) < 0)
		{
			close(fifo);
			return -4;
		}
		clock_gettime(CLOCK_REALTIME, &tend);

		if (tend.tv_nsec - tstart.tv_nsec > 0)
			accum_time += tend.tv_nsec - tstart.tv_nsec;
	}
	
	close(odfd);

	printf(">> send_download(fs=%d,name=\"%s\",fifo=\"%s\") on idle\n", pr->filesize, pr->filename, pr->fifopath);

	while(0)
	{
		if (get_max_fifo_size(fifo) == 0)
			break;
	}

	close(fifo);

 	printf(">> send_download(fs=%d,name=\"%s\",key=\"%s\") end(%ld)!\n", pr->filesize, pr->filename, pr->fifopath, accum_time);

	return 0;
}

// 메인쓰레드에서 수행되는 함수로, 
// 요청 FIFO 를 만들고, 이에 들어오는 모든 데이터를 읽어
// 정리하고, 쓰레드를 할당해줍니다.
void read_request()
{
	int rqid = 0, e;
	if ((e = mkfifo("./fifo/requests", 0666)) < 0)
		if (e != EEXIST)
			fatal("Fail to make request fifo.. ");
	if ((rqid = open("./fifo/requests", O_RDWR, 0666)) < 0)
		fatal("Fail to open request fifo.. ");

	int value, filesize, ipc_key;
	char filename[512], path[512], buffer[MSG_BUFFER_SZ];

	int read_count = 0,
		scan_count = 0;

	do
	{
		read_count = read(rqid, buffer, MSG_BUFFER_SZ);

		if (read_count < 0)
		{
			fatal("Fail to msgrcv from request.. ");
			return;
		}

		char* temp = buffer;
		do
		{
			temp[read_count] = '\0';
			scan_count = sscanf(temp, "%d %d %s %s\n", &value, &filesize, filename, path);

			if (scan_count != 4) break;

			int filename_len = strlen(filename), pipepath_len = strlen(path);
			file_req* req = (file_req*)malloc(sizeof(file_req));
			req->is_uploaded = value;
			req->filesize = filesize;
		
			req->filename = (char*)malloc(sizeof(char)*(filename_len+1));
			strcpy(req->filename, filename);
			req->filename[filename_len] = '\0';

			req->fifopath = (char*)malloc(sizeof(char)*(pipepath_len+1));
			strcpy(req->fifopath, path);
			req->fifopath[pipepath_len] = '\0';

			pthread_t pid;
			pthread_create(&pid, NULL, file_task, req);

			for (int i = 0; i < strlen(temp); i++)
			{
				if (temp[i] == '\n')
				{
					temp += i;
					break;
				}
			}

			if (*temp == '\n')
			{
				temp++;
				continue;
			}

			break;
		}
		while(1);
	}
	while(1);

}

int main()
{
	signal(SIGINT, signal_handler);
	signal(SIGABRT, signal_handler);
	signal(SIGHUP, signal_handler);
	signal(SIGKILL, signal_handler);

	if (!is_dir("./fifo"))
		system("mkdir ./fifo");
	if (!is_dir("./file"))
		system("mkdir ./file");

	read_request();

	return 0;
}

