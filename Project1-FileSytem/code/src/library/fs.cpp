// fs.cpp: File System

#include "sfs/fs.h"

#include <algorithm>

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include <iostream>

// Debug file system -----------------------------------------------------------

void FileSystem::print_inode_info(Disk *disk, Inode *inode){        
    size_t i;
    Block indirectBlock;

    printf("    size: %d bytes\n", inode->Size);
     
    // print info of direct blocks
    printf("    direct blocks:");
    for(i = 0; i < POINTERS_PER_INODE; i++){
        if(inode->Direct[i]){
            printf(" %d", inode->Direct[i]);
        }
    }
    printf("\n");

    // print info of indirect blocks
    if(!inode->Indirect){
        return;
    }
    
    disk->read(inode->Indirect, indirectBlock.Data);
    printf("    indirect block: %d\n", inode->Indirect);
    printf("    indirect data blocks:");
    for(i = 0; i < POINTERS_PER_BLOCK; i++){
        if(indirectBlock.Pointers[i]){
            printf(" %d", indirectBlock.Pointers[i]);
        }
    }
    printf("\n");
}

void FileSystem::debug(Disk *disk) {
    size_t i, j;
    Block block, inodeBlock;
    Inode inode;

    // Read SuperBlock
    disk->read(0, block.Data);

    printf("SuperBlock:\n");
    printf("    magic number is %s\n", (block.Super.MagicNumber == MAGIC_NUMBER) ? "valid" : "invalid");
    printf("    %u blocks\n"         , block.Super.Blocks);
    printf("    %u inode blocks\n"   , block.Super.InodeBlocks);
    printf("    %u inodes\n"         , block.Super.Inodes);

    // Read Inode blocks
    for(i = 0; i < block.Super.InodeBlocks; i++){
        disk->read((i + 1), inodeBlock.Data);
        for(j = 0; j < INODES_PER_BLOCK; j++){
            inode = inodeBlock.Inodes[j];
            if(inode.Valid){
                printf("Inode %zu:\n", j + i * INODES_PER_BLOCK);
                print_inode_info(disk, &inode);
            }
        }
    }
}

// Format file system ----------------------------------------------------------

bool FileSystem::format(Disk *disk) {
    // if has been mounted
    if(disk->mounted())
        return false;

    Block superBlock = {0}, block;

    // Write superBlock
    size_t numOfBlocks = disk->size();

    size_t numOfInodeBlocks = (numOfBlocks-1) / 10 + 1;
    
    superBlock.Super.MagicNumber = MAGIC_NUMBER;
    superBlock.Super.Blocks = numOfBlocks;
    superBlock.Super.InodeBlocks = numOfInodeBlocks;
    superBlock.Super.Inodes = numOfInodeBlocks * INODES_PER_BLOCK;

    disk->write(0, superBlock.Data);

    // Clear all other blocks

    block = {0};

    for(size_t i = 1; i < numOfBlocks; i++){
        disk->write(i, block.Data);
    }
    return true;
}

// verify disk -----------------------------------------------------------

bool FileSystem::formatted(Disk *disk, Block superBlock){
    size_t numOfBlocks = disk->size();

    size_t numOfInodeBlocks = (numOfBlocks - 1)/10 + 1;
    
    if(superBlock.Super.MagicNumber != MAGIC_NUMBER){
        return false;
    }

    if(superBlock.Super.Blocks != numOfBlocks){
        return false;
    }

    if(superBlock.Super.InodeBlocks != numOfInodeBlocks){
        return false;
    }

    if(superBlock.Super.Inodes != numOfInodeBlocks * INODES_PER_BLOCK){
        return false;
    }

    return true;
}

// Mount file system -----------------------------------------------------------

bool FileSystem::mount(Disk *disk) {
    if(this->disk){
        return false;
    }
    
    // Read superBlock
    Block superBlock;
    disk->read(0, superBlock.Data);
    
    if(!this->formatted(disk, superBlock)){
        return false;
    }

    // Set device and mount
    this->disk = disk;
    disk->mount();

    // Copy metadata
    this->numOfBlocks = superBlock.Super.Blocks;
    this->numOfInodeBlocks = superBlock.Super.InodeBlocks;
    this->numOfInodes = superBlock.Super.Inodes;

    // Allocate free block bitmap
    Block inodeBlock, indirectBlock;
    Inode inode;
    
    this->freemap = new bool[this->numOfBlocks];
    for (size_t i = 0; i < this->numOfBlocks; i++){
        this->freemap[i] = true;
    }
    this->freemap[0] = false;
    for (size_t i = 0; i < this->numOfInodeBlocks; i++){
        this->freemap[i + 1] = false;
        disk->read(i+1, inodeBlock.Data);
        for (size_t j = 0; j < INODES_PER_BLOCK; j++){
            inode = inodeBlock.Inodes[j];
            if (inode.Valid){
                for(size_t k = 0; k < POINTERS_PER_INODE; k++){
                    if(inode.Direct[k]){
                        this->freemap[inode.Direct[k]] = false;
                    }
                }

                if(!inode.Indirect){
                    continue;
                }

                this->freemap[inode.Indirect] = false;
                disk->read(inode.Indirect, indirectBlock.Data);
                for(size_t k = 0; k < POINTERS_PER_BLOCK; k++){
                    if(indirectBlock.Pointers[k]){
                        this->freemap[indirectBlock.Pointers[k]] = false;
                    }
                }
            }
        }
    }
    
    return true;
}

// initialize inode ----------------------------------------------------------------

void FileSystem::initialize_inode(Inode *inode){
    inode->Size = 0;

    for (size_t i = 0; i < POINTERS_PER_INODE; i++) {
        inode->Direct[i] = 0;
    }

    inode->Indirect  = 0;
}

// Create inode ----------------------------------------------------------------

ssize_t FileSystem::create() {
    Block inodeBlock;

    // Locate free inode in inode table
    for (size_t i = 0; i < this->numOfInodeBlocks; i++) {
        disk->read((i+1), inodeBlock.Data);
        for (size_t j = 0; j < INODES_PER_BLOCK; j++){
            if (!inodeBlock.Inodes[j].Valid){
                inodeBlock.Inodes[j].Valid = 1;
                initialize_inode(&inodeBlock.Inodes[j]);
                this->disk->write((i+1), inodeBlock.Data);
                return (i * INODES_PER_BLOCK + j);
            }
        }
    }

    return -1;
}

// Remove inode ----------------------------------------------------------------

bool FileSystem::remove(size_t inumber) {
     if (inumber >= this->numOfInodes){
        return false;
    }

    Block indirectBlock;
    Inode inode;
    size_t i;

    // Load inode information
    if(load_inode(this->disk, inumber, &inode) < 0){
        return false;
    }

    // Free direct blocks
    for (i = 0; i < POINTERS_PER_INODE; i++){
        if(inode.Direct[i]){
            this->freemap[inode.Direct[i]] = true;
            inode.Direct[i] = 0;
        }
    }

    // Free indirect blocks
    if (inode.Indirect){
        this->disk->read(inode.Indirect, indirectBlock.Data);    
        for (i = 0; i < POINTERS_PER_BLOCK; i++){
            if(indirectBlock.Pointers[i]){
                this->freemap[indirectBlock.Pointers[i]] = true;
                indirectBlock.Pointers[i] = 0;
            }
        }
        inode.Indirect = 0;
        this->freemap[inode.Indirect] = true;
    }

    // Clear inode in inode table
    inode.Valid = 0;
    inode.Size  = 0;

    return save_inode(this->disk, inumber, &inode);
}

// Inode stat ------------------------------------------------------------------

ssize_t FileSystem::stat(size_t inumber) {
    // Load inode information
    if (inumber >= this->numOfInodes){
        return -1;
    }

    Inode inode;
    return load_inode(this->disk, inumber, &inode);
}

// Read from inode -------------------------------------------------------------

ssize_t FileSystem::read(size_t inumber, char *data, size_t length, size_t offset) {
    if(inumber >= this->numOfInodes){
        return -1;
    }

    // Load inode information
    Inode inode;
    if(load_inode(this->disk, inumber, &inode) < 0 || offset >= inode.Size){
        return -1;
    }

    // Adjust length
    size_t rlength = std::min(length, (inode.Size-offset));
    
    size_t blockSize = Disk::BLOCK_SIZE;
    size_t startBlock = offset / blockSize;
    size_t startByte = offset % blockSize;

    // Read block and copy to data
    size_t size;
    size_t hasRead = 0;
    Block dataBlock;

    // Read from direct blocks
    size_t i = startBlock;
    while(hasRead < rlength && i < POINTERS_PER_INODE){
        if(!inode.Direct[i]){
            // continue;
            return hasRead;
        }
        this->disk->read(inode.Direct[i], dataBlock.Data);
        size = std::min((inode.Size - blockSize * i), blockSize)-startByte;
        size = std::min(size, rlength-hasRead);

        memcpy(data + hasRead, dataBlock.Data + startByte, size);

        hasRead += size;
        startByte = 0;
        i++;
    }

    // If has read length or EOF
    if(hasRead == rlength || !inode.Indirect){
        return hasRead;
    }

    // Read from indirect data blocks
    Block indirectBlock;
    this->disk->read(inode.Indirect, indirectBlock.Data);
    size_t j = i - 5;

    while(hasRead < rlength && j < POINTERS_PER_BLOCK){
        if(!indirectBlock.Pointers[j]){
            return hasRead;
        }
        this->disk->read(indirectBlock.Pointers[j], dataBlock.Data);
        size = std::min((inode.Size - blockSize * (POINTERS_PER_INODE + j)), blockSize) - startByte;
        size = std::min(size, rlength-hasRead);   
        memcpy(data + hasRead, dataBlock.Data + startByte, size);
        hasRead += size;
        startByte = 0;
        j++;
    }

    return hasRead;
}

// Write data to block --------------------------------------------------------------

ssize_t FileSystem::write_data(Disk *disk, size_t blockNumber, char *data, size_t length, size_t offset){
    if(blockNumber >= disk->size()){
        return -1;
    }
    
    Block dataBlock;
    disk->read(blockNumber, dataBlock.Data);
    size_t wlength = std::min(length, Disk::BLOCK_SIZE-offset);
    memcpy(dataBlock.Data + offset, data, wlength);
    disk->write(blockNumber, dataBlock.Data);
    return wlength;
}

// Write to inode --------------------------------------------------------------

ssize_t FileSystem::write(size_t inumber, char *data, size_t length, size_t offset) {
    if(inumber >= this->numOfInodes){
        return -1;
    }

    // Load inode
    Inode inode;
    if(load_inode(this->disk, inumber, &inode) < 0){
        return -1;
    }

    // Write block and copy to data
    Block indirectBlock;
    indirectBlock.Pointers[0] = 0;
    size_t startBlock = offset / Disk::BLOCK_SIZE;
    size_t startByte = offset % Disk::BLOCK_SIZE;
    size_t i = startBlock;
    size_t j;
    size_t hasWrite = 0;
    ssize_t wlength;
    uint32_t freeBlock = 0;

    // Write to direct blocks
    for(; i < POINTERS_PER_INODE && hasWrite < length; i++){
        if(!inode.Direct[i]){
            if((freeBlock = find_free_block()) == 0){
                goto ret;
            }
        } else{
            freeBlock = inode.Direct[i];
        }
        wlength = write_data(this->disk, freeBlock, data + hasWrite, length - hasWrite, startByte);
        if(wlength == -1){
            goto ret;
        }
        inode.Direct[i] = freeBlock;
        this->freemap[freeBlock] = false;
        hasWrite += wlength;
        startByte = 0;
    }

    // If has not write length but inode does not have indirect block
    if(hasWrite < length && !inode.Indirect){
        uint32_t freeIndirect;
        if((freeIndirect = find_free_block()) == 0){
            goto ret;
        }
        this->freemap[freeIndirect] = false;
        // For the first indirect data block in the inode
        if((freeBlock = find_free_block()) == 0){
            // If no more free block for data, need return back the indirect block
            this->freemap[freeIndirect] = true;
            goto ret;
        }
        inode.Indirect = freeIndirect;
        init_data_block(this->disk, freeIndirect);
        this->freemap[freeIndirect] = false;
    }

    // Write to indirect data blocks
    this->disk->read(inode.Indirect, indirectBlock.Data);
    
    j = i - POINTERS_PER_INODE;
    for(; j < POINTERS_PER_BLOCK && hasWrite < length; j++){
        if(!indirectBlock.Pointers[j]){
            if((freeBlock = find_free_block()) == 0){
                goto ret;
            }
        } else {
            freeBlock = indirectBlock.Pointers[j];
        }
        wlength = write_data(this->disk, freeBlock, data + hasWrite, length - hasWrite, startByte);
        if(wlength == -1){
            goto ret;
        }
        indirectBlock.Pointers[j] = freeBlock;
        this->freemap[freeBlock] = false;
        hasWrite += wlength;
        startByte = 0; 
    }

    // Return: Write indirect block; Save inode; Return the length has Write
    ret:
        if(indirectBlock.Pointers[0]){
            this->disk->write(inode.Indirect, indirectBlock.Data);
        }
        inode.Size = std::max(inode.Size, (uint32_t)(offset+hasWrite));
        save_inode(this->disk, inumber, &inode);
        return hasWrite;
}

// Initialize data block --------------------------------------------------------------

void FileSystem::init_data_block(Disk *disk, size_t blockNumber){
    if(blockNumber > disk->size()){
        return;
    }

    Block block = {0};
    disk->write(blockNumber, block.Data);
}

// Find a free block --------------------------------------------------------------

size_t FileSystem::find_free_block(){
    for(size_t i = 0; i < this->numOfBlocks; i++){
        if(freemap[i]){
            return i;
        }
    }
    return 0;
}

// Load inode of inumber --------------------------------------------------------------

ssize_t FileSystem::load_inode(Disk *disk, size_t inumber, Inode *inode) {
    Block inodeBlock;

    disk->read(inumber/INODES_PER_BLOCK + 1, inodeBlock.Data);

    if(!inodeBlock.Inodes[inumber % INODES_PER_BLOCK].Valid){
        return -1;
    }

    *inode = inodeBlock.Inodes[inumber % INODES_PER_BLOCK];
    
    return inodeBlock.Inodes[inumber % INODES_PER_BLOCK].Size;
}   

// Save inode of inumber --------------------------------------------------------------

bool FileSystem::save_inode(Disk *disk, size_t inumber, Inode *inode){
    Block inodeBlock;
    disk->read(inumber/INODES_PER_BLOCK + 1, inodeBlock.Data);
    
    if(!inodeBlock.Inodes[inumber % INODES_PER_BLOCK].Valid){
        return false;
    }

    inodeBlock.Inodes[inumber % INODES_PER_BLOCK] = *inode;

    disk->write(inumber/INODES_PER_BLOCK + 1, inodeBlock.Data);
    
    return true;
} 
