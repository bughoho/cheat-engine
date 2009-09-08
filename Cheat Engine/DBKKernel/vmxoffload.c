/*
sets up all the needed data structures 
copies dbvm into physical memory
jumps into dbvm's os entry point

*/
#include "ntifs.h"
#include <windef.h>

#include "dbkfunc.h"
#include "vmxoffload.h"

unsigned char *vmm;

#pragma pack(2) 
struct
{
	WORD limit;
	DWORD base;
} NewGDTDescriptor;
#pragma pack()

typedef struct
{ //ok, everything uint64, I hate these incompatibilities with alignment between gcc and ms c
	UINT64		cpucount;
	UINT64		originalLME;
	UINT64		idtbase;
	UINT64		idtlimit;
	UINT64		gdtbase;
	UINT64		gdtlimit;
	UINT64		cr0;
	UINT64		cr2;
	UINT64		cr3;
	UINT64		cr4;
	UINT64		dr7;
	UINT64		rip;

	UINT64		rax;
	UINT64		rbx;
	UINT64		rcx;
	UINT64		rdx;
	UINT64		rsi;
	UINT64		rdi;
	UINT64		rbp;
	UINT64		rsp;
	UINT64		r8;
	UINT64		r9;
	UINT64		r10;
	UINT64		r11;
	UINT64		r12;
	UINT64		r13;
	UINT64		r14;
	UINT64		r15;

	UINT64		rflags;
	UINT64		cs;
	UINT64		ss;
	UINT64		ds;
	UINT64		es;
	UINT64		fs;
	UINT64		gs;
	UINT64		tr;
	UINT64		ldt;	
} OriginalState, *POriginalState;

unsigned char *enterVMM2;
POriginalState originalstate;

DWORD enterVMM2PA;

PVOID TemporaryPagingSetup;
DWORD TemporaryPagingSetupPA;
DWORD pagedirptrbasePA;
DWORD originalstatePA;

void ReturnFromvmxoffload(void);


_declspec( naked ) void enterVMM( void )
{

	__asm
	{
begin:
		xchg bx,bx //trigger bochs breakpoint

		//setup the GDT
		lgdt [ebx] //ebx is the 'virtual address' so just do that before disabling paging ok...

		//switch to identify mapped pagetable
		mov cr3,edx
		jmp short weee
weee:
		


		//now jump to the physical address (identity mapped to the same virtual address)
		mov eax,secondentry
	    sub eax,begin
		add eax,esi
		jmp eax

secondentry:

		

		//disable paging		
		mov eax,cr0
		and eax,0x7FFFFFFF
		mov cr0,eax
		//paging off
		jmp short weee2
weee2:

		

		//load paging for vmm (but don't apply yet, in nonpaged mode)
		mov cr3,ecx

		//enable PAE and PSE
		mov eax,0x30
		__emit 0x0f  //-|
		__emit 0x22  //-|-mov cr4,eax  (still WTF's me that visual studio doesn't know about cr4)
		__emit 0xe0  //-|


		mov ecx,0xc0000080 //enable efer_lme
		rdmsr
		or eax,0x100
		wrmsr

		//mov eax,cr0		
		//or eax,0x80000020 //re-enable pg (and ne to be sure)
		//edit, who cares, fuck the original state, it's my own state now
		mov eax,0x80000021
		mov cr0,eax

		mov eax,edi //tell dbvm it's an OS entry and a that location the start info is

		__emit 0xea  //-|
		__emit 0x00  //-|
		__emit 0x00  //-|
		__emit 0x40  //-|JMP FAR 0x50:0x00400000
		__emit 0x00  //-|
		__emit 0x50  //-|
		__emit 0x00  //-|

		__emit 0xce
		__emit 0xce
		__emit 0xce
		__emit 0xce
		__emit 0xce
		__emit 0xce
		__emit 0xce

	}
}

_declspec( naked ) void ReturnFromvmxoffload(void)
{
	//fix up the stack pointer and return
	__asm
	{
		cli 
		xchg bx,bx //bochs debug for now		
		ret 		
	}

		

}

void vmxoffload(PCWSTR dbvmimgpath)
{
	//save entry state for easy exit in ReturnFromvmxoffload
	EFLAGS eflags;

	int i;
	PHYSICAL_ADDRESS minPA, maxPA,bam;
	GDT gdt;
	IDT idt;
	
	
	//allocate 4MB of contigues physical memory
	minPA.QuadPart=0x00400000; //at least start from 4MB
	maxPA.QuadPart=0xfffff000;
	bam.QuadPart=0x00400000; //4 mb boundaries

	
	//vmm=MmAllocateContiguousMemory(4*1024*1024, maxPA);
	vmm=MmAllocateContiguousMemorySpecifyCache(4*1024*1024, minPA, maxPA, bam, MmCached);
	if (vmm)
	{	
		HANDLE dbvmimghandle;
		UNICODE_STRING filename;
		IO_STATUS_BLOCK statusblock;
		OBJECT_ATTRIBUTES oa;
		NTSTATUS OpenedFile;

		DbgPrint("Allocated 4MB at virtual address %p (physical address %x)\n",vmm,MmGetPhysicalAddress(vmm));
		RtlZeroMemory(vmm,4*1024*1024);

		DbgPrint("Initializing filename\n");			
		DbgPrint("original=%S\n", dbvmimgpath);
		RtlInitUnicodeString(&filename, dbvmimgpath);

		DbgPrint("after=%S\n", filename.Buffer);
		
		
		//Load the .img file
		InitializeObjectAttributes(&oa, &filename, 0, NULL, NULL);
		OpenedFile=ZwCreateFile(&dbvmimghandle,SYNCHRONIZE|STANDARD_RIGHTS_READ , &oa, &statusblock, NULL, FILE_SYNCHRONOUS_IO_NONALERT| FILE_ATTRIBUTE_NORMAL, 0, FILE_OPEN, 0, NULL, 0);
		if (OpenedFile==STATUS_SUCCESS)
		{
			WORD startsector;
			LARGE_INTEGER byteoffset;
			FILE_STANDARD_INFORMATION fsi;
			NTSTATUS ReadFile;
			DbgPrint("Opened the file. Handle=%x\n", dbvmimghandle);

			DbgPrint("Getting filesize\n");

			ZwQueryInformationFile(dbvmimghandle, &statusblock, &fsi, sizeof(fsi),  FileStandardInformation);
			DbgPrint("fsi.EndOfFile=%d",fsi.EndOfFile);

			if (fsi.EndOfFile.QuadPart>4*1024*1024)
			{
				DbgPrint("File bigger than 4MB. Retard detected\n");
				return;
			}


			byteoffset.QuadPart=0x8; //offset containing sectornumber of the vmm location
			ReadFile=ZwReadFile(dbvmimghandle, NULL, NULL, NULL, &statusblock, &startsector, 2, &byteoffset, NULL);
			DbgPrint("ReadFile=%x\n",ReadFile);

			DbgPrint("statusblock.Status=%x (read %d)\n",statusblock.Status,statusblock.Information);
			if (ReadFile==STATUS_PENDING)
			{		
				
				if (ZwWaitForSingleObject(dbvmimghandle, FALSE, NULL)==STATUS_SUCCESS)
				{
					DbgPrint("Wait was a success\n");
				}
				else DbgPrint("Wait was a failure\n");
				
			}

			DbgPrint("statusblock.Status=%x (read %d)\n",statusblock.Status,statusblock.Information);

			if (statusblock.Status==STATUS_SUCCESS)
			{
				DWORD vmmsize=fsi.EndOfFile.LowPart-(startsector*512);
							

				


				//now read the VMM				
				DbgPrint("The startsector=%d (that's offset %d)\n",startsector,startsector*512);

				byteoffset.QuadPart=startsector*512; 
				ReadFile=ZwReadFile(dbvmimghandle, NULL, NULL, NULL, &statusblock, vmm, vmmsize, &byteoffset, NULL);
				if (ReadFile==STATUS_PENDING)
					ZwWaitForSingleObject(dbvmimghandle, FALSE, NULL);

				vmmsize=(vmmsize+4096) & 0xfffff000; //adjust the size internally to a page boundary (sure, there's some mem loss, but it's predicted, dbvm assumes first 10 pages are scratch pages)

				if (statusblock.Status==STATUS_SUCCESS )
				{
					//basic paging setup for the vmm, will get expanded by the vmm itself
				
					UINT64		*GDTBase=(UINT64 *)((DWORD)vmm+vmmsize+4096);
					DWORD		pagedirptrbase=(DWORD)vmm+vmmsize+2*4096;					
					PUINT64		PageMapLevel4=(PUINT64)pagedirptrbase;
					PUINT64		PageDirPtr=(PUINT64)(pagedirptrbase+4096);
					PUINT64		PageDir=(PUINT64)(pagedirptrbase+4096+4096);

					pagedirptrbasePA=MmGetPhysicalAddress((PVOID)pagedirptrbase).LowPart;
					
					DbgPrint("sizeof(PageDirPtr[0])=%d",sizeof(PageDirPtr[0]));
				
					//blame MS for making this hard to read
					DbgPrint("Setting up initial paging table for vmm\n");

					PageMapLevel4[0]=MmGetPhysicalAddress(PageDirPtr).LowPart;					
					((PPDPTE_PAE)(&PageMapLevel4[0]))->P=1;
					((PPDPTE_PAE)(&PageMapLevel4[0]))->RW=1;
					

					PageDirPtr[0]=MmGetPhysicalAddress(PageDir).LowPart;
					((PPDPTE_PAE)(&PageDirPtr[0]))->P=1;
					((PPDPTE_PAE)(&PageDirPtr[0]))->RW=1;
					

					PageDir[0]=0;
					((PPDE2MB_PAE)(&PageDir[0]))->P=1;
					((PPDE2MB_PAE)(&PageDir[0]))->RW=1;
					((PPDE2MB_PAE)(&PageDir[0]))->PS=1; //2MB


					PageDir[1]=0;
					((PPDE2MB_PAE)(&PageDir[1]))->P=1;
					((PPDE2MB_PAE)(&PageDir[1]))->RW=1;
					((PPDE2MB_PAE)(&PageDir[1]))->PS=1; //2MB

					PageDir[2]=MmGetPhysicalAddress(vmm).LowPart;
					((PPDE2MB_PAE)(&PageDir[2]))->P=1;
					((PPDE2MB_PAE)(&PageDir[2]))->RW=1;
					((PPDE2MB_PAE)(&PageDir[2]))->PS=1; //2MB


					PageDir[3]=MmGetPhysicalAddress(vmm+0x200000).LowPart; 
					((PPDE2MB_PAE)(&PageDir[3]))->P=1;
					((PPDE2MB_PAE)(&PageDir[3]))->RW=1;
					((PPDE2MB_PAE)(&PageDir[3]))->PS=1; //2MB




					//setup GDT
					GDTBase[0 ]=0;						//0 :
					GDTBase[1 ]=0x00cf92000000ffffULL;	//8 : 32-bit data
					GDTBase[2 ]=0x00cf96000000ffffULL;	//16: test, stack, failed, unused
					GDTBase[3 ]=0x00cf9b000000ffffULL;	//24: 32-bit code
					GDTBase[4 ]=0x00009a000000ffffULL;	//32: 16-bit code
					GDTBase[5 ]=0x000092000000ffffULL;	//40: 16-bit data
					GDTBase[6 ]=0x00009a030000ffffULL;	//48: 16-bit code, starting at 0x30000
					GDTBase[7 ]=0;						//56: 32-bit task	
					GDTBase[8 ]=0;						//64: 64-bit task
					GDTBase[9 ]=0;						//72:  ^   ^   ^
					GDTBase[10]=0x00a09a0000000000ULL;	//80: 64-bit code
					GDTBase[11]=0;						//88:  ^   ^   ^
					GDTBase[12]=0;						//96: 64-bit tss descriptor (2)
					GDTBase[13]=0;						//104: ^   ^   ^


					NewGDTDescriptor.limit=0x6f; //111
					NewGDTDescriptor.base=0x00400000+vmmsize+4096;

					
					DbgPrint("Before enterVMM2 alloc: minPA=%x, maxPA=%x, bam=%x\n",minPA.LowPart, maxPA.LowPart, bam.LowPart);


					
					maxPA.QuadPart=0x003fffffULL; //allocate below 00400000
					enterVMM2=MmAllocateContiguousMemory(4096,maxPA);
					if (enterVMM2)
					{
						unsigned char *original=(unsigned char *)enterVMM;
						RtlZeroMemory(enterVMM2,4096);
						DbgPrint("enterVMM is located at %p (%x)\n", enterVMM, MmGetPhysicalAddress(enterVMM).LowPart);
						DbgPrint("enterVMM2 is located at %p (%x)\n", enterVMM2, MmGetPhysicalAddress(enterVMM2).LowPart);


						DbgPrint("Copying function till end\n");
						//copy memory
						
						i=0;
						while ((i<4096) && ((original[i]!=0xce) || (original[i+1]!=0xce) || (original[i+2]!=0xce) || (original[i+3]!=0xce) || (original[i+4]!=0xce)))
							i++;

						DbgPrint("size is %d",i);

						RtlCopyMemory(enterVMM2,original, i);
						DbgPrint("Copy done\n");
					}
					else
					{
						DbgPrint("Failure allocating enterVMM2\n");
						return;
					}

					


					//now create a paging setup where enterVMM2 is identity mapped AND mapped at the current virtual address, needed to be able to go down to nonpaged mode
					//easiest way, make every 2MB page point to enterVMM2 (which is why it's set to a 2MB boundary)
				
					
					
					//bam.QuadPart=4096;	 (fails on win2k)
					
					TemporaryPagingSetup=ExAllocatePool(NonPagedPool, 4096*3);
					if (TemporaryPagingSetup==NULL)
					{
						DbgPrint("TemporaryPagingSetup==NULL!!! minPA=%x, maxPA=%x, bam=%x\n",minPA.LowPart, maxPA.LowPart, bam.LowPart);
						return;
					}

					RtlZeroMemory(TemporaryPagingSetup,4096*3);
					DbgPrint("TemporaryPagingSetup is located at %p (%x)\n", TemporaryPagingSetup, MmGetPhysicalAddress(TemporaryPagingSetup).LowPart);


					TemporaryPagingSetupPA=MmGetPhysicalAddress(TemporaryPagingSetup).LowPart;

					DbgPrint("Setting up temporary paging setup\n");
					
					if (PTESize==8) //PAE paging
					{
						PUINT64	PageDirPtr=(PUINT64)TemporaryPagingSetup;						
						PUINT64	PageDir=(PUINT64)((DWORD)TemporaryPagingSetup+4096);
						PUINT64	PageTable=(PUINT64)((DWORD)TemporaryPagingSetup+2*4096);

						DbgPrint("PAE paging\n");
						for (i=0; i<512; i++)
						{
							PageDirPtr[i]=MmGetPhysicalAddress(PageDir).LowPart;
							((PPDPTE_PAE)(&PageDirPtr[i]))->P=1;
							//((PPDPTE_PAE)(&PageDirPtr[i]))->RW=1;


							PageDir[i]=MmGetPhysicalAddress(PageTable).LowPart;
							((PPDE_PAE)(&PageDir[i]))->P=1;
							//((PPDE_PAE)(&PageDir[i]))->RW=1;							
							((PPDE_PAE)(&PageDir[i]))->PS=0; //4KB

							PageTable[i]=MmGetPhysicalAddress(enterVMM2).LowPart;
							((PPTE_PAE)(&PageTable[i]))->P=1;
							//((PPTE_PAE)(&PageTable[i]))->RW=1;					

						}

					}	
					else
					{
						PDWORD PageDir=(PDWORD)TemporaryPagingSetup;
						PDWORD PageTable=(PDWORD)((DWORD)TemporaryPagingSetup+4096);
						DbgPrint("Normal paging\n");
						for (i=0; i<1024; i++)
						{
							PageDir[i]=MmGetPhysicalAddress(PageTable).LowPart;
							((PPDE)(&PageDir[i]))->P=1;
							((PPDE)(&PageDir[i]))->RW=1;							
							((PPDE)(&PageDir[i]))->PS=0; //4KB

							PageTable[i]=MmGetPhysicalAddress(enterVMM2).LowPart;
							((PPTE)(&PageTable[i]))->P=1;
							((PPTE)(&PageTable[i]))->RW=1;	
						}

					}

					DbgPrint("Temp paging has been setup\n");

					enterVMM2PA=MmGetPhysicalAddress(enterVMM2).LowPart;
					
					minPA.QuadPart=0;
					maxPA.QuadPart=0xfffff000; //keep it under the 4GB range (parameter passed can only be 32 bits)
					bam.QuadPart=0;
					originalstate=MmAllocateContiguousMemorySpecifyCache(((sizeof(OriginalState)>4096) ? sizeof(OriginalState) : 4096), minPA, maxPA, bam, MmCached);
					RtlZeroMemory(originalstate, sizeof(OriginalState));
					originalstatePA=MmGetPhysicalAddress(originalstate).LowPart;
					DbgPrint("enterVMM2PA=%x\n",enterVMM2PA);

					DbgPrint("Storing original state\n");

					
					originalstate->cpucount=getCpuCount();
					originalstate->originalLME=(int)(((DWORD)(readMSR(0xc0000080)) >> 8) & 1);
					originalstate->cr0=getCR0();
					originalstate->cr2=getCR2();
					originalstate->cr3=getCR3();
					originalstate->cr4=getCR4();
					originalstate->ss=getSS();
					originalstate->cs=getCS();
					originalstate->ds=getDS();
					originalstate->es=getES();
					originalstate->fs=getFS();
					originalstate->gs=getGS();
					originalstate->ldt=GetLDT();
					originalstate->tr=GetTR();

					originalstate->dr7=getDR7();
					
					GetGDT(&gdt);									
					originalstate->gdtbase=(ULONG_PTR)gdt.vector;
					originalstate->gdtlimit=gdt.wLimit;

					GetIDT(&idt);
					originalstate->idtbase=(ULONG_PTR)idt.vector;
					originalstate->idtlimit=idt.wLimit;



					eflags=getEflags();
					originalstate->rflags=*(PDWORD)&eflags;

			
					{
						ULONG vmmentryeip;
						
						__asm
						{
							lea eax,[entervmmexit]
							mov vmmentryeip,eax 
						}	
						originalstate->rip=(UINT64)vmmentryeip;
					}


					originalstate->rsp=getESP();
					originalstate->rbp=getEBP();
					originalstate->rax=getEAX();
					originalstate->rbx=getEBX();
					originalstate->rcx=getECX();
					originalstate->rdx=getEDX();
					originalstate->rsi=getESI();
					originalstate->rdi=getEDI();

					
					DbgPrint("Calling entervmm2\n");


					__asm{
						cli //goodbye interrupts
						xchg bx,bx
						
						lea ebx,NewGDTDescriptor
						mov ecx,pagedirptrbasePA
						mov edx,TemporaryPagingSetupPA //for the mov cr3,ecx
						mov esi,enterVMM2PA
						mov edi,originalstatePA
						call [enterVMM2]
						

entervmmexit:
						cli
						nop
						nop
						nop						
						nop
						nop
						nop						
					}

					return;



					//DbgPrint("After enterVMM2. Should be in VM mode now\n");

					
				}

			}
			


			ZwClose(dbvmimghandle);
		}
		else
		{
			DbgPrint("Failure opening the file. Status=%x\n",OpenedFile);

		
		}
		//fill in some specific memory regions

	}
	else
	{
		DbgPrint("Failure allocating the required 4MB\n");
	}
	
}