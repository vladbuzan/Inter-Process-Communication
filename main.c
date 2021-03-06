#include <stdlib.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <sys/mman.h> 
#include <stdbool.h>

#define BUFF_SIZE 1024

typedef struct __attribute__((__packed__)) section {
    char name[15];
    int type;
    int offset;
    int size;
} section_t;

typedef struct header {
    section_t* sections;
    short version;
    char noOfSections;
    short size;
    char magic[4];
} header_t;

header_t* parseSF(char* file_mem, unsigned int file_size)
{
    int currentPos = file_size - 4;
    header_t* ret = (header_t*)malloc(sizeof(header_t));

    strncpy(ret->magic, file_mem + currentPos, 4);
    currentPos -= 2;
    memcpy(&(ret->size), file_mem + currentPos, 2);
    currentPos = file_size - ret->size;
    memcpy(&(ret->version), file_mem + currentPos, 2);
    currentPos += 2;
    memcpy(&(ret->noOfSections), file_mem + currentPos, 1);
    currentPos ++;
    ret->sections = (section_t*)malloc(ret->noOfSections * sizeof(section_t));
    memcpy(ret->sections, file_mem + currentPos, ret->noOfSections * sizeof(section_t));
    return ret;
}


void sendMessage(int resp_pipe, const char* message, char length)
{
    write(resp_pipe, &length, 1);
    write(resp_pipe, message, length);
}

void checkSF(header_t* SF, int resp_pipe)
{
    bool isValid = true;

    if(strncmp(SF->magic, "e4yc", 4) != 0) isValid = false;
    if((SF->version < 97) || (SF->version > 160)) isValid = false;
    if((SF->noOfSections < 6) || (SF->noOfSections > 20)) isValid = false;

    for(int i = 0; i < SF->noOfSections; i++)
    {
        int sectType = SF->sections[i].type;
        if((sectType != 98) && (sectType != 91) && (sectType != 42) 
        && (sectType != 46) && (sectType != 97)) isValid = false;
    }
    if(!isValid)
    {
        printf("ERROR\ninvalid SF file\n");
        sendMessage(resp_pipe, "READ_FROM_FILE_SECTION", 22);
        sendMessage(resp_pipe, "ERROR", 5);
        free(SF->sections);
        free(SF);
        exit(5);
    }
}

void read_file_section(int resp_pipe, int req_pipe, char* file_mem, char* sh_mem, unsigned int file_size)
{
    unsigned int section_no, offset, no_of_bytes;
    read(req_pipe, &section_no, sizeof(unsigned int));
    read(req_pipe, &offset, sizeof(unsigned int));
    read(req_pipe, &no_of_bytes, sizeof(unsigned int));
    header_t* sf = parseSF(file_mem, file_size);
    checkSF(sf, resp_pipe);
    
    if((section_no > sf->noOfSections + 1) || (offset + no_of_bytes >= sf->sections[section_no - 1].size))
    {
        sendMessage(resp_pipe, "READ_FROM_FILE_SECTION", 22);
        sendMessage(resp_pipe, "ERROR", 5);
        free(sf->sections);
        free(sf);
        return;
    }

    memcpy(sh_mem, file_mem + sf->sections[section_no - 1].offset + offset, no_of_bytes);
    sendMessage(resp_pipe, "READ_FROM_FILE_SECTION", 22);
    sendMessage(resp_pipe, "SUCCESS", 7);
    free(sf->sections);
    free(sf);
}

void read_file_offset(int resp_pipe, int req_pipe, char* file_mem, char* sh_mem ,unsigned int file_size)
{
    unsigned int offset, no_of_bytes;

    read(req_pipe, &offset, sizeof(unsigned int));
    read(req_pipe, &no_of_bytes, sizeof(unsigned int));

    if((file_mem == NULL) || (sh_mem == NULL) || ((offset + no_of_bytes) >= file_size))
    {
        sendMessage(resp_pipe, "READ_FROM_FILE_OFFSET", 21);
        sendMessage(resp_pipe, "ERROR", 5);
        return;
    }

    memcpy(sh_mem, file_mem + offset, no_of_bytes);
    sendMessage(resp_pipe, "READ_FROM_FILE_OFFSET", 21);
    sendMessage(resp_pipe, "SUCCESS", 7);
}

char* map_file(int req_pipe, int resp_pipe, unsigned int* file_size)
{
    int fd;
    unsigned char size;
    char* path;
    char* ret;

    read(req_pipe, &size, 1);
    path = (char*)malloc(size + 1);
    read(req_pipe, path, size);
    path[size] = '\0';
    fd = open(path, O_RDONLY);
    free(path);

    if (fd < 0) 
    {
        sendMessage(resp_pipe, "MAP_FILE", 8);
        sendMessage(resp_pipe, "ERROR", 5);
        free(path);
        return NULL;
    }

    *file_size = lseek(fd, 0, SEEK_END);
    lseek(fd, 0, SEEK_SET);
    ftruncate(fd, *file_size);
    ret = (char*)mmap(NULL, *file_size, PROT_READ, MAP_PRIVATE, fd, 0);
    if(ret == MAP_FAILED)
    {
        sendMessage(resp_pipe, "MAP_FILE", 8);
        sendMessage(resp_pipe, "ERROR", 5);
        close(fd);
        free(path);
        return NULL;
    }
    else
    {
        sendMessage(resp_pipe, "MAP_FILE", 8);
        sendMessage(resp_pipe, "SUCCESS", 7);
        free(path);
        close(fd);
    }
    
    return ret;
}

char* create_shm(int req_pipe, int resp_pipe, unsigned int* mem_size)
{
    int fd;
    char* ret;

    read(req_pipe, mem_size, sizeof(unsigned int));
    fd = shm_open("/FnHhly1", O_CREAT | O_RDWR, 0644);

    if(fd < 0) return NULL;
    if(ftruncate(fd, (off_t)(*mem_size)) < 0) return NULL;
    ret = (char*)mmap(NULL, *mem_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    sendMessage(resp_pipe, "CREATE_SHM", 10);

    if(ret == MAP_FAILED)
    {
        sendMessage(resp_pipe, "ERROR", 5);
        return NULL;
    }
    else
    {
        sendMessage(resp_pipe, "SUCCESS", 7);
    }
    
    return ret;
}
void read_logical(int resp_pipe, int req_pipe, char* file_mem, char* sh_mem, unsigned int file_size)
{
    unsigned int logical_offset, no_of_bytes;
    int current_index = 0, sf_file_no, actual_offset, prev_index;
    header_t* SF = parseSF(file_mem, file_size);
    
    checkSF(SF, resp_pipe);
    read(req_pipe, &logical_offset, sizeof(unsigned int));
    read(req_pipe, &no_of_bytes, sizeof(unsigned int));
    //logical_map = (int*)malloc(SF->noOfSections * sizeof(int));
    prev_index = 0;

    for(int i = 1; i < SF->noOfSections; i++)
    {
        current_index += 1024;
        while(SF->sections[i - 1].size > current_index - prev_index) current_index += 1024;
        if (current_index > logical_offset) {
            sf_file_no = i - 1;
            break;
        }
        prev_index = current_index;
    }

    actual_offset = logical_offset - prev_index;
    printf("%d\n", actual_offset);
    if(SF->sections[sf_file_no].size < actual_offset + no_of_bytes)
    {
        sendMessage(resp_pipe, "READ_FROM_LOGICAL_SPACE_OFFSET", 30);
        sendMessage(resp_pipe, "ERROR", 5);
        free(SF->sections);
        free(SF);
    }

    memcpy(sh_mem, file_mem + (SF->sections[sf_file_no].offset + actual_offset), no_of_bytes);
    sendMessage(resp_pipe, "READ_FROM_LOGICAL_SPACE_OFFSET", 30);
    sendMessage(resp_pipe, "SUCCESS", 7);
    free(SF->sections);
    free(SF);
}
void write_shm(int req_pipe, int resp_pipe, unsigned int mem_size, char* sh_mem)
{
    unsigned int offset, value;
    
    read(req_pipe, &offset, sizeof(unsigned int));
    read(req_pipe, &value, sizeof(unsigned int));

    if((offset + 4 > mem_size) || (sh_mem == NULL)) 
    {
        sendMessage(resp_pipe, "WRITE_TO_SHM", 12);
        sendMessage(resp_pipe, "ERROR", 5);
    }
    else
    {
        memcpy(sh_mem + offset, (char*)&value, sizeof(unsigned int));
        sendMessage(resp_pipe, "WRITE_TO_SHM", 12);
        sendMessage(resp_pipe, "SUCCESS", 7);
    }
    
}

void ping(int resp_pipe)
{
    unsigned int nr = 37667;
    
    sendMessage(resp_pipe, "PING", 4);
    sendMessage(resp_pipe, "PONG", 4);
    write(resp_pipe, &nr, sizeof(unsigned int));
}

int parseRequest(char *request, int req_pipe) 
{
    char reqLength;
    
    if(read(req_pipe, &reqLength, 1) < 0) return -1;

    if(read(req_pipe, request, reqLength * sizeof(char)) < 0)
    {
        return -1;
    }

    return reqLength;
}

int main(int argc, char** argv) 
{
    int resp_pipe, req_pipe;
    char request[BUFF_SIZE];
    int requestSize;
    unsigned int sh_mem_size, file_mem_size;
    char* sh_mem = NULL;
    char* file_mem = NULL;

    remove("RESP_PIPE_37667");

    if (mkfifo("RESP_PIPE_37667", 0640) != 0)
    {
        printf("ERROR\ncannot create the response pipe\n");
        exit(1);
    }

    req_pipe = open("REQ_PIPE_37667", O_RDONLY);

    if(req_pipe < 0) 
    {
        printf("ERROR\ncannot open the request pipe\n");
        exit(1);
    }

  
    resp_pipe = open("RESP_PIPE_37667", O_WRONLY);

    if (resp_pipe < 0)
    {
        printf("ERROR\ncannot open the response pipe\n");
        exit(1);
    }

    sendMessage(resp_pipe, "CONNECT", 7);

    printf("SUCCESS\n");

    while(1) 
    {
        requestSize = parseRequest(request, req_pipe);
        
        if(requestSize < 0)
        {
            printf("ERROR\ncouldn't parse request\n");
            exit(2);
        }

        if(strncmp("PING", request, 4) == 0)
        {
            ping(resp_pipe);
        } 
        
        if(strncmp("CREATE_SHM", request, 10) == 0)
        {
            sh_mem = create_shm(req_pipe, resp_pipe, &sh_mem_size);
            if(sh_mem == NULL) exit(3);
        }

        if(strncmp("WRITE_TO_SHM", request, 12) == 0)
        {
            write_shm(req_pipe, resp_pipe, sh_mem_size, sh_mem);
        }

        if(strncmp("MAP_FILE", request, 8) == 0)
        {
            file_mem = map_file(req_pipe, resp_pipe, &file_mem_size);
            if (file_mem == NULL) exit(4);
        } 

        if(strncmp("READ_FROM_FILE_OFFSET", request, 21) == 0)
        {
            read_file_offset(resp_pipe, req_pipe, file_mem, sh_mem, file_mem_size);
        }

        if(strncmp("READ_FROM_FILE_SECTION", request, 22) == 0)
        {
            read_file_section(resp_pipe, req_pipe, file_mem, sh_mem, file_mem_size);
        }

        if(strncmp("READ_FROM_LOGICAL_SPACE_OFFSET", request, 30) == 0)
        {
            read_logical(resp_pipe, req_pipe, file_mem, sh_mem, file_mem_size);
        }

        if(strncmp("EXIT", request, 4) == 0) 
        {
            close(req_pipe);
            close(resp_pipe);
            remove("RESP_PIPE_37667");
            return 0;
        }
        
    }
    if(file_mem != NULL) munmap(file_mem, file_mem_size);
    if(sh_mem != NULL) munmap(sh_mem, sh_mem_size);
    close(req_pipe);
    close(resp_pipe);
    remove("RESP_PIPE_37667");

    return 0;

}