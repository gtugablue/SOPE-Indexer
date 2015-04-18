#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <dirent.h>
#include <string.h>
#include <stdlib.h>
#include <limits.h>

#define MAX_DIR_SIZE 254
#define MAX_BUF_SIZE 2048
#define MAX_WORD_SIZE 50
#define END_WORD_CHAR ':'
#define TEMP_FOLDER "temp"					// temporary folder where files generated by sw are placed
#define UNSORTED_INDEX_NAME "us_index.txt"	// unsorted index, to be removed
#define SORTED_REPEAT_INDEX "sr_index.txt"	// sorted index, with repeated lines
#define INDEX_NAME "index.txt"				// sorted index, result

#define PIPE_READ 0
#define PIPE_WRITE 1

// Function declarations
int setup_dir(DIR **dir);
int concatenate_files_to_index(char** filenames, int filenumber, int pipefd[]);
void prep_filenames(char** filenames, int filenumber);
int* sort_index(int pipefd[], int newpipe[]);
int remove_repeated_lines(int pipefd[]);

int main(int argc, char *argv[])
{
	if (argc != 1)
	{
		printf("\nThis program takes no arguments\n\n");
		return 1;
	}

	DIR* dir = NULL;
	if(setup_dir(&dir) != 0) return 1; // Set dir string

	struct dirent *ent;
	char** filenames = malloc(0);
	int filenumber = 0;

	ent = readdir(dir);
	while(ent != NULL)
	{
		if(ent->d_type == DT_REG) // all files in TEMP_FOLDER must be the temporary indexes created by sw
		{
			char** temp = (char**)malloc(++filenumber*sizeof(char*));
			memcpy(temp, filenames, (filenumber-1)*sizeof(char*));
			filenames[filenumber-1] = ent->d_name;
		}
		ent = readdir(dir);
	}
	// at this point, filenames contains the list of filenames to be read from DIR


	prep_filenames(filenames, filenumber);

	int pipefd[2];
	if(pipe(pipefd) != 0) return 1;

	if(concatenate_files_to_index(filenames, filenumber, pipefd) != 0) return 1;

	int newpipe[2];
	if(pipe(newpipe) != 0) return 1;

	if(sort_index(pipefd, newpipe) == NULL) return 1;												// this function must close pipefd write end, so now it is unavailable

	close(newpipe[PIPE_WRITE]);

	if(remove_repeated_lines(newpipe)!= 0) return 1;

	return 0;
}

int setup_dir(DIR **dir)
{
	char *dir_name;

	dir_name = (char*)malloc(MAX_DIR_SIZE);
	dir_name = getcwd(dir_name, MAX_DIR_SIZE);

	if(dir_name == NULL) return 1;	// Error getting current dir

	int dir_size = strlen(dir_name);
	int temp_folder_size = strlen(TEMP_FOLDER);

	if(dir_size+2+temp_folder_size > MAX_DIR_SIZE) return 1;	// Complete path is too big

	dir_name[dir_size] = '/';
	int i;
	for(i = 0; i < temp_folder_size; i++)
	{
		dir_name[dir_size+1+i] = TEMP_FOLDER[i];
	}

	dir_size = strlen(dir_name);
	dir_name[dir_size++] = '/';
	dir_name[dir_size++] = '\0';

	*dir = opendir(dir_name);

	if(*dir == NULL) return -1;

	return 0;
}

int concatenate_files_to_index(char** filenames, int filenumber, int pipefd[])
{
	char** cmd = (char**)malloc((filenumber+2)*sizeof(char*));
	cmd[0] = "cat";
	cmd[filenumber+1] = NULL;

	int i;
	for(i = 0; i < filenumber; i++)
	{
		cmd[i+1] = filenames[i];
	}

	if (fork() == 0)
	{
		close(pipefd[PIPE_READ]);
		dup2(pipefd[PIPE_WRITE], STDOUT_FILENO);  // send standard output to the pipe
		execvp(cmd[0], cmd);
		exit(1);
	}
	else
	{
		int status = 0;
		wait(&status);
		if(status != 0) return 1;
	}

	return 0;
}

void prep_filenames(char** filenames, int filenumber)
{
	int temp = strlen(TEMP_FOLDER);
	char *dir_str = malloc((1+temp)*sizeof(char));
	strcpy(dir_str, TEMP_FOLDER);
	dir_str[temp] = '/';		// dir_str becomes TEMP_FOLDED/, to be added to filenames for use in cat
	temp = strlen(dir_str);

	int i;
	for(i = 0; i < filenumber; i++)
	{
		int filename_length = strlen(filenames[i]);
		char *new_str = malloc((filename_length+temp)*sizeof(char));
		strcpy(new_str, dir_str);
		strcat((new_str+strlen(new_str)), filenames[i]);
		filenames[i] = new_str;
	}

	free(dir_str);/////////////////////
}

int* sort_index(int pipefd[], int newpipe[])
{
	char **cmd = malloc(4);
	cmd[0] = "sort";
	cmd[1] = "-V";
	cmd[2] = "-f";
	cmd[3] = NULL;

	int temppipe[2];
	if(pipe(temppipe) != 0) return NULL;

	int pid = fork();
	int status = 0;
	if(pid == 0)
	{
		close(temppipe[PIPE_READ]);
		close(pipefd[PIPE_WRITE]);
		dup2(temppipe[PIPE_WRITE], STDOUT_FILENO);
		dup2(pipefd[PIPE_READ], STDIN_FILENO);
		execvp(cmd[0], cmd);
		exit(1);
	}

	close(temppipe[PIPE_WRITE]);
	close(pipefd[PIPE_WRITE]);		// must be done to ensure sort terminates
	wait(&status);
	if(status != 0) return NULL;

	char buffer[MAX_BUF_SIZE+1];
	int nbytes = 0;
	while((nbytes = read(temppipe[PIPE_READ], buffer, MAX_BUF_SIZE)) > 0)
	{
		buffer[nbytes] = '\0';
		write(newpipe[PIPE_WRITE], buffer, strlen(buffer));
	}

	close(pipefd[PIPE_READ]);
	close(temppipe[PIPE_READ]);

	return newpipe;
}

int remove_repeated_lines(int pipefd[])
{
	FILE *repeat_file = fdopen(pipefd[PIPE_READ], "rb");
	int index_fd = open(INDEX_NAME, O_WRONLY | O_CREAT, S_IRUSR | S_IWUSR | S_IXUSR | S_IRGRP | S_IROTH);

	if(!repeat_file) return 1;
	if(index_fd < 0) return 1;

	char *header = "INDEX";
	write(index_fd, header, strlen(header)*sizeof(char));

	char *str = (char *)malloc(MAX_WORD_SIZE*sizeof(char));
	char *prev_word = (char *)malloc(MAX_WORD_SIZE*sizeof(char));

	int last_read_word = 0;		// 0 - false, 1 - true

	while(fscanf(repeat_file, "%s", str) != EOF)
	{
		int len = strlen(str);

		if(str[len-1] == END_WORD_CHAR)			// WORD
		{
			if(strcmp(str, prev_word) == 0)		// Last word was the same
			{
				last_read_word = 0;
			}
			else								// New word
			{
				write(index_fd, "\n\n", 2*sizeof(char));
				write(index_fd, str, len*sizeof(char));
				write(index_fd, " ", sizeof(char));

				strcpy(prev_word, str);
				last_read_word = 1;
			}
		}
		else								// FILE-LINE match
		{
			if(last_read_word == 0)
				write(index_fd, ", ", 2*sizeof(char));

			write(index_fd, str, len*sizeof(char));
			last_read_word = 0;
		}
	}

	if (write(index_fd, "\n", 1) != 1) return 1; // Add ending newline

	close(index_fd);
	fclose(repeat_file);
	close(pipefd[PIPE_READ]);

	return 0;
}
