#include <sys/stat.h>

int is_dir(const char* filepath)
{
	struct stat st;
	stat(filepath, &st);
	return S_ISDIR(st.st_mode);
}

int is_fifo(const char* filepath)
{
	struct stat st;
	stat(filepath, &st);
	return S_ISFIFO(st.st_mode);
}

int is_file(const char* filepath)
{
	struct stat st;
	stat(filepath, &st);
	return S_ISREG(st.st_mode);
}
