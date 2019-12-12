/*
	server_mp.c
	메세지 패싱을 사용한 파일 전송 서버 소스입니다.
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

// MESSAGE PASSING 에 대한 정의들
#define REQ_MP_KEY 			60050
#define REQ_MPQ_PERM 		0666

#define IO_MP_KEY_BASE		60051
#define IO_MPQ_PERM			0666

#define MSG_BUFFER_SZ		2048

struct msg_buf
{
	long mtype;
	char message[MSG_BUFFER_SZ];
};
// MESSAGE PASSING 에 대한 정의들

void signal_handler(int signal)
{
	struct msqid_ds msqstat;
	int msgq = msgget(REQ_MP_KEY, REQ_MPQ_PERM); 
	msgctl(msgq, IPC_RMID, &msqstat);
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
	// IPC KEY
	int mp_ipc_key;
} file_req;

int receive_upload(file_req* pr);
int send_download(file_req* pr);

void* file_task(void* p)
{
	file_req* preq = (file_req*)p;
	int result = preq->is_uploaded? receive_upload(preq): send_download(preq);

	if (result < 0)
	{
		switch(result)
		{
			case -1:
				printf(">> file_task: file(%s) cannot open..\n", preq->filename);
				break;
			case -2:
				printf(">> file_task: ipc_key(%d) cannot open..\n", preq->mp_ipc_key);
				break;
			default:
				printf(">> file_task: unknown error(%d)\n", result);
				break;
		}
	}

	free(preq->filename);
	free(preq);

	return NULL;
}

// 업로드/ 클라이언트에서 보낸 MESSAGE QUEUE 에 있던 데이터를 FILE에 넣어줍니다.
int receive_upload(file_req* pr)
{
	struct timespec tstart, tend;

	printf(">> receive_upload(fs=%d,name=\"%s\",key=%d) start!\n", pr->filesize, pr->filename, pr->mp_ipc_key);

	struct msg_buf buffer;
	buffer.mtype = 0;
	sprintf(buffer.message, "./file/%s", pr->filename);
	int newfile = open(buffer.message, O_WRONLY | O_CREAT);
	int msgq_id = msgget(pr->mp_ipc_key, IO_MPQ_PERM);

	if (newfile < 0)
		return -1;
	if (pipe < 0)
		return -2;

	int loop_cnt = 0;
	long accum_time = 0;
	int read_len = 0, accum = 0;
	while(1)
	{
		clock_gettime(CLOCK_REALTIME, &tstart);
		read_len = msgrcv(msgq_id, &buffer, MSG_BUFFER_SZ, 0, MSG_NOERROR);
		clock_gettime(CLOCK_REALTIME, &tend);

		if (tend.tv_nsec - tstart.tv_nsec > 0)
			accum_time += tend.tv_nsec - tstart.tv_nsec;

		if (!read_len) break;
		if (read_len < 0)
		{
			struct msqid_ds msqstat;
			msgctl(msgq_id, IPC_RMID, &msqstat);
			return -3;
		}
		write(newfile, buffer.message, read_len);
		accum += read_len;

		if (accum == pr->filesize)
			break;
	}

	close(newfile);

	struct msqid_ds msqstat;
	msgctl(msgq_id, IPC_RMID, &msqstat);

	printf(">> receive_upload(fs=%d,name=\"%s\",key=%d) end(%ld)!\n", pr->filesize, pr->filename, pr->mp_ipc_key, tend.tv_nsec - tstart.tv_nsec);
	return 0;
}

// 다운로드/ 클라이언트가 요청한 파일을 MESSAGE QUEUE에 넣어줍니다. 
// 여기서도 스핀락으로 파이프 크기에 따라 조절합니다.
int send_download(file_req* pr)
{
	struct timespec tstart, tend;

	printf(">> send_download(fs=%d,name=\"%s\",key=%d) start!\n", pr->filesize, pr->filename, pr->mp_ipc_key);

	struct msqid_ds msqstat;
	struct msg_buf buffer;
	sprintf(buffer.message, "./file/%s", pr->filename);
	int oldfile = open(buffer.message, O_RDONLY);
	int msgq_id = msgget(pr->mp_ipc_key, IO_MPQ_PERM);

	struct stat st;
	stat(buffer.message, &st);
	pr->filesize = st.st_size;
	
	printf(">> send_download(fs=%d,name=\"%s\",key=%d) update fs\n", pr->filesize, pr->filename, pr->mp_ipc_key);

	if (oldfile < 0)
		return -1;
	if (msgq_id < 0)
		return -2;

	buffer.mtype = 1;
	*(int*)buffer.message = pr->filesize;

	if (msgsnd(msgq_id, &buffer, 4, 0) < 0)
	{
		struct msqid_ds msqstat;
		msgctl(msgq_id, IPC_RMID, &msqstat);
		return -3;
	}

	int loop_cnt = 0;
	long accum_time = 0;
	int read_len = 0, accum = 0, idx = 0;
	while(1)
	{
		buffer.mtype = buffer.mtype + 1;

		read_len = read(oldfile, buffer.message, MSG_BUFFER_SZ);
		if (!read_len) break;
		accum += read_len;

		while(1)
		{
			// 여유 공간이 있는지 확인
			msgctl(msgq_id, IPC_STAT, &msqstat);
			if (msqstat.msg_qbytes - msqstat.__msg_cbytes > MSG_BUFFER_SZ)
				break;
		}

		clock_gettime(CLOCK_REALTIME, &tstart);
		if (msgsnd(msgq_id, &buffer, read_len, 0) < 0)
		{
			msgctl(msgq_id, IPC_RMID, &msqstat);
			return -4;
		}
		clock_gettime(CLOCK_REALTIME, &tend);
		
		if (tend.tv_nsec - tstart.tv_nsec > 0)
			accum_time += tend.tv_nsec - tstart.tv_nsec;
	}
	
	close(oldfile);

	printf(">> send_download(fs=%d,name=\"%s\",key=%d) on idle\n", pr->filesize, pr->filename, pr->mp_ipc_key);

	while(1)
	{
		if (msgctl(msgq_id, IPC_STAT, &msqstat) < 0)
			return -3;
		if (msqstat.__msg_cbytes == 0)
			break;
	}

	printf(">> send_download(fs=%d,name=\"%s\",key=%d) end(%ld)!\n", pr->filesize, pr->filename, pr->mp_ipc_key, accum_time);

	return 0;
}

// 메인쓰레드에서 수행되는 함수로, 
// 요청 MESSAGE QUEUE  를 만들고, 이에 들어오는 모든 데이터를 읽어
// 정리하고, 쓰레드를 할당해줍니다.
void read_request(int rqid)
{
	struct msg_buf buffer;
	buffer.mtype = 1;
	int value, filesize, ipc_key;
	char filename[512], path[512];

	int read_count = 0,
		scan_count = 0;

	do
	{
		scan_count = 0;
		read_count = msgrcv(rqid, &buffer, MSG_BUFFER_SZ, 0, MSG_NOERROR);

		if (read_count < 0)
		{
			fatal("Fail to msgrcv from request.. ");
			return;
		}

		char* temp = buffer.message;
		do
		{
			temp[read_count] = '\0';
			scan_count = sscanf(temp, "%d %d %s %d\n", &value, &filesize, filename, &ipc_key);

			if (scan_count != 4) break;

			int filename_len = strlen(filename), pipepath_len = strlen(path);
			file_req* req = (file_req*)malloc(sizeof(file_req));
			req->is_uploaded = value;
			req->filesize = filesize;
		
			req->filename = (char*)malloc(sizeof(char)*(filename_len+1));
			strcpy(req->filename, filename);
			req->filename[filename_len] = '\0';

			req->mp_ipc_key = ipc_key;

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

	if (!is_dir("./file"))
		system("mkdir ./file");

	int rqid = 0;
	if ((rqid = msgget(REQ_MP_KEY, REQ_MPQ_PERM | IPC_CREAT)) < 0)
		fatal("Fail to get request mq.. ");

	printf("GEN MSG Q: %x:%d\n", REQ_MP_KEY, rqid);

	while(1)
		read_request(rqid);

	struct msqid_ds msqstat;
	msgctl(rqid, IPC_RMID, &msqstat);

	return 0;
}

