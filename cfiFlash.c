
#define NAME_MAX_LEN 128
// Device Commands:
#define FLASH_RESET 0xf0
#define READ_ID_CODES 0x90
#define READ_QUERY 0x98
#define WRITE_BUFFER_LOAD 0x25
#define WRITE_BYTE 0xA0
#define WRITE_BUFFER_CONFIRM 0x29

#define BLOCK_ERASE_START 0x80
#define BLOCK_ERASE_FINISH 0x30


// CFI Address Offsets
#define CFI_OFFSET_ADDR 0xAAA
#define CFI_MANUFACTURER (0x0<<1)
#define CFI_DEVICE (0x01<<1)
#define CFI_SIZE (0x27<<1)
#define CFI_BLOCKSIZE_L (0x2F<<1)
#define CFI_BLOCKSIZE_H (0x30<<1)
#define FLASH_OP_WRITE_BUFFER  0x01
#define FLASH_OP_PROGRAMMING   0x02
#define FLASH_OP_ERASE         0x04
#define TIMEOUT_MAX 0xffffffff

#define SUCCESS 1
#define FAILURE 0


static Address flash_base = 0;

static void flashWrite( Address addr, UINT2 data)
{
	*(volatile UINT2*) addr = data;
	//Put a memory barrier
}

static UINT2 flashRead( Address addr)
{
	UINT2 val = *(volatile UINT2*)addr;
	//Put a memory barrier
	return val;
}

static void flashUnLock(Address addr)
{
	flashWrite(addr+0xAAA, 0xAA ); /*Unlock cycle 1 */
	flashWrite(addr+0x554, 0x55 ); /*Unlock cycle 2 */
}

static UINT2 flashPollStatus(char operation_mode, unsigned long address, unsigned short data)
{
	unsigned UINT2 timeout = 0;
	volatile UINT2 ready; 
	while(1)
	{
		ready = *( (volatile unsigned short *) (address)) == data;
		if (ready)
			break;
		if(timeout++>TIMEOUT_MAX)
			break;
	}
  return 0;
}

/* Soft Reset */
UINT2 flashReset(void)
{
	flashUnLock(flash_base);
	flashWrite(flash_base, FLASH_RESET);
	return SUCCESS;
}

static void flashSetAutoMode( Address addr )
{
	flashWrite(addr+0xAAA, READ_ID_CODES );
}

static UINT2 flashGetManId( Address addr )
{
	flashUnLock(flash_base);
	flashSetAutoMode(flash_base);
	UINT2 res = flashRead(addr);
	flashReset();
	return res;
}	

static UINT2 flashReadQuery( Address addr )
{
	flashReset();
	//Add some delay(10);
	flashUnLock(flash_base);
	flashSetAutoMode(flash_base);
	flashWrite( flash_base + 0xAA, READ_QUERY );
	//Add some delay(10);
	UINT2  res = flashRead(addr);
	return res;
}

static void flashGetBlkSize(Address addr1, Address addr2, unsigned long *result)
{
	flashReset();
	//Add some delay(10);
	flashUnLock(flash_base);
	flashSetAutoMode(flash_base);
	flashWrite( flash_base + 0xAA, READ_QUERY );
	//Add some delay(10);
	// Block size is a two-byte register, and the actual
	// block size is z * 256 bytes.

	*result = flashRead(addr1);
	*result += 256*flashRead(addr2);
	*result *= 256;
}


UINT2 flashGetInfo( Address addr, UINT2 which, unsigned long *result )
{
	UINT2 err = SUCCESS;
	switch (which) {
	    case GET_FLASH_BASE:
		*result = flash_base;
		break;
	    case GET_MANUFACTURER:
			*result = flashGetManId(addr|CFI_MANUFACTURER);
			break;
	    case GET_DEVICE:
			*result = flashGetManId(addr|CFI_DEVICE) & 0x00ff;
			if (*result == 0x7E)
			{
				*result |=  (flashGetManId(addr|0x1C) & 0x00ff) << 8;
				//Add some delay(10);
				*result |= (flashGetManId(addr|0x1E) & 0x00ff) << 16;
			}
			break;
	    case GET_SIZE_BITS:
			*result = flashReadQuery(addr|CFI_SIZE);
			break;
	    case GET_BLOCK_SIZE:
			flashGetBlkSize(addr|CFI_BLOCKSIZE_L, addr|CFI_BLOCKSIZE_H, result);
			break;
	    default:
			err=FAILURE;
			break;
	}

	return err;
}


static Boolean flashWriteWord( Value addr, unsigned short data)
{
	flashUnLock(flash_base);
	flashWrite( flash_base+0xAAA, WRITE_BYTE );
	flashWrite( addr, data );
	if (flashPollStatus(FLASH_OP_WRITE_BUFFER, addr, data) != 0)
	{
		return FAILURE;
	}
	flashReset();

	return SUCCESS;
}

UINT2 flashEraseBlk( Value addr )
{
	UINT2 result=SUCCESS;
	flashReset();
	flashUnLock(flash_base);
	flashWrite( flash_base+0xAAA, BLOCK_ERASE_START );
	flashUnLock(flash_base);
	flashWrite( flash_base+addr, BLOCK_ERASE_FINISH );

	if (flashPollStatus(FLASH_OP_ERASE, flash_base+addr, 0xffff) != 0)
	{
		result = FAILURE;
	}
	flashReset();
	return result;
}

UINT2 flashWriteBuff( Value addr, char* buf, UINT2 len)
{
	UINT2 i = 0;
	volatile unsigned short *write_address = (volatile unsigned short *) (flash_base+addr);
	unsigned long end_address = flash_base+addr;
	volatile unsigned short *src = (volatile unsigned short *) buf;
	const UINT2 BufferSize = 32;
	while(len>0)
	{
		volatile UINT2 words = (len+1)>>1;
		volatile UINT2 block_bytes = words >= BufferSize ? BufferSize : words;
		end_address = end_address+(block_bytes*2);

		flashUnLock(flash_base+addr);
	
		flashWrite(flash_base+addr, WRITE_BUFFER_LOAD);
		flashWrite(flash_base+addr, block_bytes-1);
		for (i=0; i<block_bytes; i++)
		{
			*write_address++ = *src++;
		}
		flashWrite(flash_base+addr, WRITE_BUFFER_CONFIRM);
		//Add some delay if required
		
		if (flashPollStatus(FLASH_OP_WRITE_BUFFER, end_address-2, (unsigned short)(*(src-1))) != 0)
		{
			flashReset();/*Abort Programming */
			return FAILURE;
		}
		len -=block_bytes*2;
	}
	return SUCCESS;
}

UINT2 flashReadBuff( Value addr, char* buf, UINT2 len)
{
	memcpy( buf, (char*)flash_base+addr, len);
	return SUCCESS;
}


static UINT2 norFlashInit(DevTree_Node node, const char* match_name)
{
	static Address num_partitions;
	//Parse the dev tree node from dtb for flash virtual address
	while( part_node ) {
	///

	}
    return err;
}
