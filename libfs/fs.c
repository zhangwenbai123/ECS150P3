#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <stdbool.h>

#include "disk.h"
#include "fs.h"

#define FS_SIGNATURE_LEN 8
#define FAT_EOC 0xFFFF

//Super block data structure
typedef struct __attribute__ ((__packed__)) Superblock
{
        uint8_t signature[FS_SIGNATURE_LEN];
	uint16_t total_block_num;
	uint16_t root_dir_index;
	uint16_t data_start_index;
	uint16_t data_block_num;
	uint8_t fat_block_num;
	uint8_t padding[4079];
} Superblock;

//root directory data structure
typedef struct __attribute__ ((__packed__)) RootDirEntry
{
        uint8_t file_name[FS_FILENAME_LEN];
	uint32_t file_size;
	uint16_t data_index;
	uint8_t padding[10];
} RootDirEntry;

typedef struct OpenFileEntry
{
	uint32_t offset;
	int16_t rootdir_ptr;
} OpenFileEntry;




static Superblock superblock;
static uint16_t *FAT;
static RootDirEntry root_directory[FS_FILE_MAX_COUNT];
static OpenFileEntry open_file_table[FS_OPEN_MAX_COUNT];



int readFatBlocks(void)
{
	if (FAT != NULL)    //if FAT was not destroyed previously
		return -1;

	FAT = (uint16_t*)malloc(superblock.data_block_num*(sizeof(uint16_t)));

	if (FAT == NULL)   //FAT cannot be allocated.
		return -1;

	for (int i = 0; i < superblock.fat_block_num; ++i) {
		if (i == superblock.fat_block_num - 1) {
			if (superblock.data_block_num * 2 % BLOCK_SIZE != 0) {
				//If the last FAT is not fully used
				//need to set up a buffer for unmatched size.
				uint16_t buffer[BLOCK_SIZE / 2];
				if (block_read(i + 1, buffer))
					return -1;
				memcpy((&FAT[i * BLOCK_SIZE / 2]), buffer,
					superblock.data_block_num * 2 % BLOCK_SIZE);
			} else {
				//The last FAT is fully used.
				if (block_read(i + 1, (&FAT[i * BLOCK_SIZE / 2])))
					return -1;
			}
		} else {
			if (block_read(i + 1, (&FAT[i * BLOCK_SIZE / 2])))
				return -1;
		}
	}
	return 0;
}

int writebackFatBlocks(void)
{
	for (int i = 0; i < superblock.fat_block_num; ++i) {
		if (i == superblock.fat_block_num - 1) {
			if (superblock.data_block_num * 2 % BLOCK_SIZE != 0) {
				//If the last FAT is not fully used
				//need to set up a buffer for unmatched size.
				uint16_t buffer[BLOCK_SIZE / 2];
				memset(buffer, 0, BLOCK_SIZE);
				memcpy(buffer, (&FAT[i * BLOCK_SIZE / 2]),
					superblock.data_block_num * 2 % BLOCK_SIZE);
				if (block_write(i + 1, buffer))
					return -1;

			} else {
				//The last FAT is fully used.
				if (block_write(i + 1, (&FAT[i * BLOCK_SIZE / 2])))
					return -1;
			}
		} else {
			if (block_write(i + 1, (&FAT[i * BLOCK_SIZE / 2])))
				return -1;
		}
	}

	free(FAT);
	FAT = NULL;

	return 0;
}

void init_open_file_table(void)
{
	for (int i = 0; i < FS_OPEN_MAX_COUNT; ++i) {
		open_file_table[i].rootdir_ptr = -1;
	}
}

int fs_mount(const char *diskname)
{
	//Check if it can be opened, not already opened
	//and the first block can be read.
	if (block_disk_open(diskname) || block_read(0, &superblock))
		return -1;

	//Checking signature is correct
	if (!strcmp((char*)superblock.signature, "ECS150FS"))
		return -1;

	//Checking if block number matches
	if (block_disk_count() != superblock.total_block_num)
		return -1;

	//Reading the FAT blocks and root directory block.
	if (readFatBlocks() || block_read(superblock.root_dir_index, &root_directory))
		return -1;

	if (superblock.data_block_num + superblock.fat_block_num + 2
		!= superblock.total_block_num
		|| superblock.data_start_index != superblock.fat_block_num + 2
		|| superblock.root_dir_index != superblock.fat_block_num + 1)
		//Block number calculation count not match or index fault.
		return -1;

	init_open_file_table(); //initialize the data in open file table.

	return 0;

}

int fs_umount(void)
{
	if (FAT == NULL)    //if disk was not mounted before.
		return -1;

	//write the root directory info and FAT back to disk.
	if (block_write(superblock.root_dir_index, &root_directory) || writebackFatBlocks())
		return -1;

	//close the disk
	if (block_disk_close())
		return -1;

	return 0;

}

int fs_info(void)
{
	uint16_t freeFat = 0;
	int freeDir = 0;

	printf("FS Info:\n");
	printf("total_blk_count=%d\n", superblock.total_block_num);
	printf("fat_blk_count=%d\n", superblock.fat_block_num);
	printf("rdir_blk=%d\n", superblock.root_dir_index);
	printf("data_blk=%d\n", superblock.data_start_index);
	printf("data_blk_count=%d\n", superblock.data_block_num);

	for (int i = 1; i < superblock.data_block_num; ++i) {
		if (FAT[i] == 0)
			freeFat += 1;
	}

	for (int i = 0; i < FS_FILE_MAX_COUNT; ++i) {
		if (root_directory[i].file_name[0] == '\0')
			freeDir += 1;
	}

	printf("fat_free_ratio=%d/%d\n", freeFat, superblock.data_block_num);
	printf("rdir_free_ratio=%d/%d\n", freeDir, FS_FILE_MAX_COUNT);

	return 0;
}

bool isFileNameValid(const char *filename)
{
	if (filename == NULL)
		return false;

	int fname_len = strlen(filename);

	if (fname_len > FS_FILENAME_LEN - 1 || fname_len == 0)
		return false;

	return true;
}

int fs_create(const char *filename)
{
	int first_free = -1;

	if (!isFileNameValid(filename))
		return -1;

	for (int i = 0; i < FS_FILE_MAX_COUNT; ++i) {
		if (root_directory[i].file_name[0] == '\0') {  //free entry
			if (first_free == -1)
				first_free = i;
		} else {
			if (strcmp((char*)root_directory[i].file_name, filename) == 0)
				//if the file name already exist in directory.
				return -1;
		}
	}

	if (first_free == -1)  //All directory entries are full.
		return -1;

	strcpy((char*)root_directory[first_free].file_name, filename);
	root_directory[first_free].file_size = 0;
	root_directory[first_free].data_index = FAT_EOC;

	return 0;
}

bool isFileOpen(int directoryIndex)
{
	for (int i = 0; i < FS_OPEN_MAX_COUNT; ++i) {
		if (open_file_table[i].rootdir_ptr == directoryIndex)
			return true;
	}
	return false;
}

int fs_delete(const char *filename)
{
	int i;
	uint16_t currentIndex, nextIndex;

	if (!isFileNameValid(filename))
		return -1;

	for (i = 0; i < FS_FILE_MAX_COUNT; ++i) {
		if (strcmp((char*)root_directory[i].file_name, filename) == 0)
			break;
	}

	//if the file not found or the file is open currently.
	if (i == FS_FILE_MAX_COUNT || isFileOpen(i))
		return -1;

	//free the FAT data.
	currentIndex = root_directory[i].data_index;
	root_directory[i].data_index = 0;
	while (currentIndex != FAT_EOC) {
		nextIndex = FAT[currentIndex];
		FAT[currentIndex] = 0;
		currentIndex = nextIndex;
	}
	FAT[currentIndex] = 0;

	//Set the filename to empty.
	root_directory[i].file_name[0] = '\0';

	root_directory[i].file_size = 0;

	return 0;
}

int fs_ls(void)
{
	if (FAT == NULL)    //if disk was not mounted before.
		return -1;

	printf("FS Ls:\n");

	for (int i = 0; i < FS_FILE_MAX_COUNT; ++i) {
		if (root_directory[i].file_name[0] != '\0') {
			printf("file: %s, size: %ld, data_blk: %d\n", root_directory[i].file_name,
				(long int)root_directory[i].file_size, root_directory[i].data_index);
		}
	}

	return 0;
}

int fs_open(const char *filename)
{
	int i, r;

	if (!isFileNameValid(filename))
		return -1;

	for (i = 0; i < FS_OPEN_MAX_COUNT; ++i) {
		if (open_file_table[i].rootdir_ptr == -1)
			break;
	}

	if (i == FS_OPEN_MAX_COUNT)
		return -1;    //No more spaces in open file table.

	for (r = 0; r < FS_FILE_MAX_COUNT; ++r) {
		if (strcmp((char*)root_directory[r].file_name, filename) == 0)
			break;
	}

	if (r == FS_FILE_MAX_COUNT)
		return -1;      //file not found

	open_file_table[i].rootdir_ptr = r;
	open_file_table[i].offset = 0;

	return 0;
}

int fs_close(int fd)
{
	if (fd >= FS_OPEN_MAX_COUNT || open_file_table[fd].rootdir_ptr == -1)
		return -1;   //out of bound or not currently open

	open_file_table[fd].rootdir_ptr = -1;

	return 0;
}

int fs_stat(int fd)
{
	if (fd >= FS_OPEN_MAX_COUNT || open_file_table[fd].rootdir_ptr == -1)
		return -1;   //out of bound or not currently open

	return (int)root_directory[open_file_table[fd].rootdir_ptr].file_size;
}

int fs_lseek(int fd, size_t offset)
{
	if (fd >= FS_OPEN_MAX_COUNT || open_file_table[fd].rootdir_ptr == -1)
		return -1;   //out of bound or not currently open

	if (fs_stat(fd) > fs_stat(fd))
		return -1;   //offset exceeds file size.

	open_file_table[fd].offset = offset;

	return 0;
}

//return the block index that the offset is pointing to.
//it also saves the index prior to current in prev_index for modification purposes.
int go_to_offset(int fd, int16_t* prev_index)
{
	uint16_t currentIndex = root_directory[open_file_table[fd].rootdir_ptr].data_index;
	*prev_index = -1;

	for (int i = 0; i < (int)(open_file_table[fd].offset / BLOCK_SIZE); ++i) {
		*prev_index = currentIndex;
		currentIndex = FAT[currentIndex];
	}

	return currentIndex;
}

size_t min(size_t a, size_t b)
{
	if (a < b)
		return a;
	return b;
}

size_t max(size_t a, size_t b)
{
	if (a > b)
		return a;
	return b;
}

//returns the next free fat.
//returns -1 if no free fat avaliable.
int next_free_fat(void)
{
	for (int i = 1; i < superblock.data_block_num; ++i) {
		if (FAT[i] == 0)
			return i;
	}
	return -1;
}

int fs_write(int fd, void *buf, size_t count)
{
	int block_to_be_wrt;   //How many blocks need to perform write based on count
	size_t bytesWritten = 0;
	size_t frontPadding, endPadding;
	uint16_t currentIndex, start_offset;
	uint32_t fileSize;
	int16_t prev_index;

	if (fd >= FS_OPEN_MAX_COUNT || open_file_table[fd].rootdir_ptr == -1)
		return -1;   //out of bound or not currently open

	fileSize = root_directory[open_file_table[fd].rootdir_ptr].file_size;
	start_offset = superblock.data_start_index; //The offset for the first data block in disk.

	//Initialize paddings before first read
	frontPadding = open_file_table[fd].offset % BLOCK_SIZE;
	endPadding = 0;

	//Go to the block with offset.
	currentIndex = go_to_offset(fd, &prev_index);

	block_to_be_wrt = (frontPadding + count) / BLOCK_SIZE;
	if ((frontPadding + count) % BLOCK_SIZE != 0)
		//The leftover data will need another block with end paddings.
		block_to_be_wrt += 1;

	if (count == 0)   //No bytes need to be write.
		block_to_be_wrt = 0;

	//Loop writes all the blocks
	for (int i = 0; i < block_to_be_wrt; ++i) {
		//First block corner cases
		if (i == 0) {
			//if the file is empty or at the file end.
			if (currentIndex == FAT_EOC && prev_index == -1) {  //File empty
				currentIndex = next_free_fat();
				root_directory[open_file_table[fd].rootdir_ptr].data_index = currentIndex;
				FAT[currentIndex] = FAT_EOC;
			}
		}

		//If it reaches the end of the file.
		if (currentIndex == FAT_EOC) {
			currentIndex = next_free_fat();
			FAT[prev_index] = currentIndex;
			FAT[currentIndex] = FAT_EOC;
		}

		//Last block, need calculate end padding.
		if (i == block_to_be_wrt - 1)
			endPadding = block_to_be_wrt * BLOCK_SIZE - (frontPadding + count);

		//Determine large or small operation predicated on paddings
		if (frontPadding == 0 && endPadding == 0) {  //Entire operation, no paddings
			block_write(currentIndex + start_offset, &(((uint8_t*)buf)[bytesWritten]));
		} else {  //Small operation, needs bounce buffer.
			uint8_t bounce_buffer[BLOCK_SIZE];
			block_read(currentIndex + start_offset, bounce_buffer);
			memcpy(bounce_buffer+frontPadding, &(((uint8_t*)buf)[bytesWritten]),
					BLOCK_SIZE - frontPadding - endPadding);

			//write back bounce buffer after modified.
			block_write(currentIndex + start_offset, bounce_buffer);
		}
		prev_index = currentIndex;
		currentIndex = FAT[currentIndex];
		bytesWritten += (BLOCK_SIZE - frontPadding - endPadding);

		if (i == 0)
			frontPadding = 0;   //No need of front padding after first block.
	}

	if (bytesWritten != count)   //Count not match
		return -1;
	open_file_table[fd].offset += bytesWritten;
	root_directory[open_file_table[fd].rootdir_ptr].file_size = max(fileSize,
									open_file_table[fd].offset);
	return bytesWritten;
}

int fs_read(int fd, void *buf, size_t count)
{
	size_t bytesRead = 0;
	size_t frontPadding, endPadding;
	int block_to_be_read;   //How many blocks need to perform read based on count
	uint16_t currentIndex, start_offset;
	uint32_t fileSize, offset;
	int16_t prev_index;    //To save the previous index before current FAT index.
				//not used for read, but needed for function arguments.

	if (fd >= FS_OPEN_MAX_COUNT || open_file_table[fd].rootdir_ptr == -1)
		return -1;   //out of bound or not currently open

	offset = open_file_table[fd].offset;
	fileSize = root_directory[open_file_table[fd].rootdir_ptr].file_size;
	start_offset = superblock.data_start_index; //The offset for the first data block in disk.

	//Initialize paddings before first read
	frontPadding = open_file_table[fd].offset % BLOCK_SIZE;
	endPadding = 0;

	//Go to the block with offset.
	currentIndex = go_to_offset(fd, &prev_index);

	//update the count if it reaches file end first.
	count = min(count, fileSize - offset);
	block_to_be_read = (frontPadding + count) / BLOCK_SIZE;
	if ((frontPadding + count) % BLOCK_SIZE != 0)
		//The leftover data will need another block with end paddings.
		block_to_be_read += 1;

	if (count == 0)   //No bytes need to be read.
		block_to_be_read = 0;

	//Loop reads all the blocks and saves it in buffer.
	for (int i = 0; i < block_to_be_read; ++i) {
		//Last block, need calculate end padding.
		if (i == block_to_be_read - 1)
			endPadding = block_to_be_read * BLOCK_SIZE - (frontPadding + count);

		//Determine large or small operation predicated on paddings
		if (frontPadding == 0 && endPadding == 0) {  //Entire operation, no paddings
			block_read(currentIndex + start_offset, &(((uint8_t*)buf)[bytesRead]));
		} else {  //Small operation, needs bounce buffer.
			uint8_t bounce_buffer[BLOCK_SIZE];
			block_read(currentIndex + start_offset, bounce_buffer);

			memcpy(&(((uint8_t*)buf)[bytesRead]), bounce_buffer+frontPadding,
					BLOCK_SIZE - frontPadding - endPadding);
		}

		currentIndex = FAT[currentIndex];
		bytesRead += (BLOCK_SIZE - frontPadding - endPadding);

		if (i == 0)
			frontPadding = 0;   //No need of front padding after first block.
	}

	if (bytesRead != count)   //Count not match
		return -1;
	open_file_table[fd].offset += bytesRead;

	return bytesRead;
}
