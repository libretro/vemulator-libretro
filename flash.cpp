/*
    VeMUlator - A Dreamcast Visual Memory Unit emulator for libretro
    Copyright (C) 2018  Mahmoud Jaoune

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <https://www.gnu.org/licenses/>.
*/

#include "flash.h"

/* Forward declarations */
extern "C" {
   RFILE* rfopen(const char *path, const char *mode);
   int rfclose(RFILE* stream);
   int64_t rfseek(RFILE* stream, int64_t offset, int origin);
   int rfputc(int character, RFILE * stream);
}

VE_VMS_FLASH::VE_VMS_FLASH(VE_VMS_RAM *_ram)
{
	userData      = new byte[0x19000];
	directory     = new byte[0x1A00];
	FAT           = new byte[0x200];
	rootBlock     = new byte[0x200];
	data          = new byte[0x20000];
	
	// The destructor closes this when set; only a writable .bin sets it.
	flashWriter   = NULL;
	IsRealFlash   = true;
	IsSaveEnabled = true;
	
	ram           = _ram;
}

VE_VMS_FLASH::~VE_VMS_FLASH()
{
	if(flashWriter)
		rfclose(flashWriter);
   flashWriter = NULL;
}

///Loads raw VMS data to be easily accessed.
//romType 0: Memory Dump (.bin)
//romType 1: VMS file
//romType 2: DCI file
void VE_VMS_FLASH::loadROM(byte *d, size_t buffSize, int romType, const char *fileName, bool enableSave)
{
   size_t i, i2, j, k, c;
   byte *romData  = new byte[0x20000];
   size_t romSize = buffSize;

   if(romType == 2)
      romSize -= 32;

   if(romType == 2)
   {
      for(i = 32, i2 = 0; i < romSize; i += 4, i2 += 4) 
         for(j = 3, k = 0; j >= 0; j--, k++)
            romData[i2 + k] = d[i + j] & 0xFF;
   }
   else 
   {
      for (i = 0; i < romSize; i++)
         romData[i] = d[i] & 0xFF;
   }

   //If VMS or DCI, create a bogus flash memory to contain it in.
   if(romType == 1 || romType == 2)
   {
      int rootPtr = 255*512;
      int FATPtr  = 254*512;
      int dirPtr  = 253*512;
      int sz      = (romSize+511) >> 9;
      int i       = 0;

      IsRealFlash = false;

      //FAT init
      for(; i < 256*2; i += 2)
      {
         romData[FATPtr + i]     = 0xFC;
         romData[FATPtr + i + 1] = 0xFF;
      }

      for(i = 0; i < sz; i++)
      {
         romData[FATPtr + 2*i]     = i+1;
         romData[FATPtr + (2*i)+1] = 0;
      }

      if((--i) >= 0)
      {
         romData[FATPtr + 2*i]     = 0xFA;
         romData[FATPtr + (2*i)+1] = 0xFF;
      }
      romData[FATPtr + 254*2]      = 0xFA;
      romData[FATPtr + (254*2)+1]  = 0xFF;
      romData[FATPtr + 255*2]      = 0xFA;
      romData[FATPtr + (255*2)+1]  = 0xFF;

      for(i = 253; i > 241; --i)
      {
         romData[FATPtr + 2*i]     = i-1;
         romData[FATPtr + (2*i)+1] = 0;
      }
      romData[FATPtr + 241*2]       = 0xFA;
      romData[FATPtr + (241*2) + 1] = 0xFA;

      //Dir init
      romData[dirPtr] = 0xCC;
      //Fill bogus name (Spaces)
      for(i = 4; i < 12; i++)
         romData[dirPtr + i] = ' ';

      romData[dirPtr + 0x18] = sz & 0xFF;
      romData[dirPtr + 0x19] = sz >> 8;
      romData[dirPtr + 0x1A] = 1;

      for(i = 0; i < 16; i++)
         romData[rootPtr + i] = 0x55;

      romData[rootPtr + 0x10] = 1;

      for(i = 0; i < 8; i++)
         romData[rootPtr + 0x30 + i] = romData[dirPtr+0x10 + i];

      romData[rootPtr+ 0x44]  = 255;
      romData[rootPtr + 0x46] = 254;
      romData[rootPtr + 0x48] = 1;
      romData[rootPtr + 0x4A] = 253;
      romData[rootPtr + 0x4C] = 13;
      romData[rootPtr + 0x50] = 200;
   }

   if(romType == 0) 
   {
      //Loading userData (200 blocks, blocks are ordered descending)
      for (i = 0, c = 0; i < 200; ++i) 
      {
         for (j = 0; j < 512; j++)
         {
            userData[c] = romData[(i * 512) + j];
            c++;
         }
      }

      //Loading directory (13 blocks)
      for (i = 253, c = 0; i >= 241; --i) 
      {
         for (j = 0; j < 512; j++)
         {
            directory[c] = romData[(i * 512) + j];
            c++;
         }
      }

      //Loading FAT and rootBlock
      for (j = 0; j < 512; j++)
         FAT[j] = romData[(0x1FC00) + j];  //0x1FC00 being 254 x 512

      for (j = 0; j < 512; j++)
         rootBlock[j] = romData[(0x1FE00) + j];  //0x1FE00 being 255 x 512
   }

   //We also need data as a whole
   for(j = 0; j < romSize; j++)
      data[j] = romData[j];

   IsSaveEnabled = enableSave;

   if(IsSaveEnabled && romType == 0)
      flashWriter = rfopen(fileName, "r+b");
}

///Returns raw VMS data and its size
size_t VE_VMS_FLASH::getROM(byte *out)
{
   int i, j, c;
	//Loading userData (200 blocks, blocks are ordered descending)
	for(i = 0, c = 0; i < 200; ++i)
	{
		for(j = 0; j < 512; j++)
      {
         out[(i*512) + j] = userData[c];
         c++;
      }
	}

	//Loading directory (13 blocks)
	for(i = 253, c = 0; i >= 241; --i)
	{
		for(j = 0; j < 512; j++)
      {
			out[(i*512) + j] = directory[c];
			c++;
		}
	}

	//Loading FAT and rootBlock
	for(j = 0; j < 512; j++) 
	{
		out[(0x1FC00) + j] = FAT[j];  //0x1FC00 being 254 x 512

		// for(int j = 0; j < 512; j++)
		out[(0x1FE00) + j] = rootBlock[j];  //0x1FE00 being 255 x 512
	}

   return 0;
}

///Returns data
size_t VE_VMS_FLASH::getData(byte *out)
{	
   size_t i;
   for(i = 0; i < 0x20000; ++i, out[i] = data[i]);

   return 0x20000;
}


//Operations
///Returns byte at address. (No banking)
byte VE_VMS_FLASH::getByte(size_t address)
{
	return data[address] & 0xFF;
}

///Returns byte at address (With banking)
byte VE_VMS_FLASH::readByte(size_t address)
{
	if((ram->readByte(0x154) & 1) == 1)
      address += 0x10000;

	return data[address] & 0xFF;
}

//Returns int16 at address (Little-endian)
int VE_VMS_FLASH::getWord(size_t address)
{
	int word = 0;
	if(address < 0x19000)
	{
		word = (userData[address+1] << 16) | (userData[address] & 0xFF);
		return word;
	}
	else if(address >= 0x19000 && address < 0x1E200)
		return 0;   //This area of ROM is not used, why request it?
	else if(address >= 0x1E200 && address < 0x1FC00)
	{
		word = (directory[address - 0x1E200 + 1] << 16) | (directory[address - 0x1E200] & 0xFF);
		return word;
	}
	else if(address >= 0x1FC00 && address < 0x1FE00) 
	{
		word = (FAT[address - 0x1FC00 + 1] << 16) | (FAT[address - 0x1FC00] & 0xFF);
		return word;
	}
	else if(address >= 0x1FE00 && address < 0x20000) 
	{
		word = (rootBlock[address - 0x1FE00 + 1] << 16) | (rootBlock[address - 0x1FE00] & 0xFF);
		return word;
	}
	else return 0;
}

///Writes int to address
void VE_VMS_FLASH::writeByte(size_t address, byte d)
{
	if((ram->readByte(0x154) & 2) != 0) return;   //An EXT similar register but for Flash

	if((ram->readByte(0x154) & 1) == 1) address += 0x10000;

	data[address] = d & 0xFF;

	//If playing a flashrom, save changes in real time
	if(IsRealFlash && IsSaveEnabled) 
	{
		rfseek(flashWriter, address, SEEK_SET);
		rfputc(d, flashWriter);
	}
}

//Writes int to raw address
void VE_VMS_FLASH::writeByte_RAW(size_t address, byte d)
{
	data[address] = d & 0xFF;

	//If playing a flashrom, save changes in real time
	if(IsRealFlash && IsSaveEnabled) 
	{
		rfseek(flashWriter, address, SEEK_SET);
		rfputc(d, flashWriter);
	}
}

///Writes int16 to address (Little-endian)
void VE_VMS_FLASH::writeWord(size_t address, byte d)
{
	if(address < 0x19000) 
	{
		userData[address] = (int)(d & 0xFF);
		userData[address + 1] = (int)((d >> 16) & 0xFF);
	}
	else if(address >= 0x19000 && address < 0x1E200)
		return;   //This area of ROM is not used, why request it?
	else if(address >= 0x1E200 && address < 0x1FC00) 
	{
		directory[address - 0x1E200] = (int)(d & 0xFF);
		directory[address - 0x1E200 + 1] = (int)((d >> 16) & 0xFF);
	}
	else if(address >= 0x1FC00 && address < 0x1FE00) 
	{
		FAT[address - 0x1FC00] = (int)(d & 0xFF);
		FAT[address - 0x1FC00 + 1] = (int)((d >> 16) & 0xFF);
	}
	else if(address >= 0x1FE00 && address < 0x20000) 
	{
		rootBlock[address - 0x1FE00] = (int)(d & 0xFF);
		rootBlock[address - 0x1FE00 + 1] = (int)((d >> 16) & 0xFF);
	}
}

///Get block data (512)
void VE_VMS_FLASH::readBlock(int blockNumber, byte *out)
{
	for(int j = 0; j < 512; j++)
		out[j] = getByte((blockNumber * 512) + j);
}
///Write data to block (512)
void VE_VMS_FLASH::writeBlock(int blockNumber, byte *in)
{
	for(int j = 0; j < 512; j++)
		writeByte((blockNumber * 512 + j), in[j]);
}

///Checks if card is corrupt
bool VE_VMS_FLASH::IsCorrupt()
{
	for(int i = 0; i < 0x0F; ++i)
		if(rootBlock[i] != 0x55)
			return true;

	return false;
}

///Counts files in directory
int VE_VMS_FLASH::countFiles()
{
	int count = 0;

	//Read entries in dictionary (Each entry is 32-bytes long)
	for(int i = 0x1E200; i < 0x1FC00; i += 32)
	{
		byte d = getByte(i);

		//If first int in entry is not 0x00, it means that the file is either data or a game
		if(d != 0x00)
			++count;
	}

	return count;
}

//File operations
///Get file info from directory depending on its index
VE_VMS_FLASH_FILE VE_VMS_FLASH::getFileAt(int index)
{
   int fileSize, fileHeader;
   int i;
	int pos            = index * 32; //Each entry is 32-bits
	VMS_FILE_TYPE type = VE_VMS_FLASH_FILE::getType(directory[pos + 0]);
	int startBlock     = directory[pos + 1] | (directory[pos + 2] << 16);
	byte *nameArray    = new byte[12];
	char *fileName     = (char *)malloc(12);
	
	for(i = 0; i < 12; ++i)
	{
		int c = directory[pos + 4 + i];
		if((char)c == ' ')
         break;
		nameArray[i] = (byte)c;
		if(c == '\0')
         break;
	}

   //Filenames in VMS are UTF-8 encoded
	for(i = 0; i < 12; i++, fileName[i] = nameArray[i]);  

	fileSize   = directory[pos + 0x18] | (directory[pos + 0x19] << 16);
	fileHeader = directory[pos + 0x1A] | (directory[pos + 0x1B] << 16);

	VE_VMS_FLASH_FILE file(index, type, startBlock, fileName, fileSize, fileHeader);

	return file;
}

///Get file info from directory depending on its name
VE_VMS_FLASH_FILE VE_VMS_FLASH::getFile(const char *name)
{
   int i;
   VE_VMS_FLASH_FILE file;
   int fileCount = countFiles();

   for(i = 0; i < fileCount; ++i)
   {
      file = getFileAt(i);

      if(strcmp(file.getFileName(), name))
         break;
   }

   return file;
}

///Counts number of mini-games found in ROM
int VE_VMS_FLASH::countGames()
{
   int i;
	int fileCount = countFiles();
	int gameCount = 0;

	for(i = 0; i < fileCount; i++)
		if(getFileAt(i).getType() == GAME)
			gameCount++;

	return gameCount;
}


//FAT operations
///Get data of file from FAT FS
size_t VE_VMS_FLASH::getFileData(VE_VMS_FLASH_FILE fileinfo, byte *out)
{
	byte startBlock = fileinfo.getStartBlock();
	byte blockCount = fileinfo.getFileSize();

	out = new byte[blockCount * 512];

	for(int i = startBlock * 2, e = startBlock, w = 0; i < 512; i+=2, e++)
	{   //e is for entry
		byte FATEntry = FAT[i] | (FAT[i+1] << 16);   //Little-endian

		if(FATEntry != 0xFFFC)
      {
         byte *block = new byte[512];
         readBlock(e, block);    //Block is allocated to our file

         //Write read block to 'data'
         for(int j = 0; j < 512; ++j)
            data[(w * 512) + j] = block[j];

         ++w;    //Indicates number of blocks written

         if(FATEntry == 0xFFFA) break;
      }
	}

	return blockCount * 512;
}
