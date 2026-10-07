#ifndef _U_FS_H_
#define _U_FS_H_

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>

static inline int readfile(const char* path, void** outdata, size_t* outsize) {
	assert(outdata);
	assert(outsize);

	FILE* handle = fopen(path, "r");
	if (!handle) {
		return errno;
	}

	fseek(handle, 0, SEEK_END);
	size_t filesize = ftell(handle);
	fseek(handle, 0, SEEK_SET);

	void* filedata = malloc(filesize);
	if (!filedata) {
		abort();
	}

	int res = 0;
	if (fread(filedata, 1, filesize, handle) != filesize) {
		free(filedata);
		res = errno;
	}

	fclose(handle);

	*outdata = filedata;
	*outsize = filesize;
	return res;
}

static inline int writefile(
    const char* path, const void* data, size_t datasize
) {
	assert(data);
	assert(datasize);

	FILE* handle = fopen(path, "w");
	if (!handle) {
		return errno;
	}

	int res = 0;

	size_t byteswritten = fwrite(data, 1, datasize, handle);
	if (byteswritten != datasize) {
		res = errno;
	}

	fclose(handle);
	return res;
}
#endif	// _U_FS_H_
