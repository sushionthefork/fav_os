#pragma once

#include "kernel.h"
#include "io.h"
#include <Windows.h>

#include <iostream>
#include "common.h"
#include "process.h"
#include "vfs.h"
#include "fs_stdio.h"
#include "fs_fat.h"
#include "fs_proc.h"

#include <iostream>

HMODULE User_Programs;


void Initialize_Kernel() {
	User_Programs = LoadLibraryW(L"user.dll");

	/*
	 * Registering all known file systems crucial for kernel
	 */
	kiv_vfs::CVirtual_File_System::Get_Instance().Register_File_System(new kiv_fs_stdio::CFile_System());
	kiv_vfs::CVirtual_File_System::Get_Instance().Register_File_System(new kiv_fs_fat::CFile_System());
	kiv_vfs::CVirtual_File_System::Get_Instance().Register_File_System(new kiv_fs_proc::CFile_System());

	/*
	 * Mounting registered
	 */
	kiv_vfs::CVirtual_File_System::Get_Instance().Mount_File_System("stdio", "stdio");
	kiv_vfs::CVirtual_File_System::Get_Instance().Mount_File_System("fat", "C", 0x81); // TODO Change disk
	kiv_vfs::CVirtual_File_System::Get_Instance().Mount_File_System("fs_proc", "proc");
}

void Shutdown_Kernel() {

	kiv_process::CProcess_Manager::Destroy();
	kiv_thread::CThread_Manager::Destroy();
	kiv_vfs::CVirtual_File_System::Destroy();

	FreeLibrary(User_Programs);
}

void __stdcall Sys_Call(kiv_hal::TRegisters &regs) {

	switch (static_cast<kiv_os::NOS_Service_Major>(regs.rax.h)) {
		case kiv_os::NOS_Service_Major::File_System:		
			Handle_IO(regs);
			break;

		case kiv_os::NOS_Service_Major::Process:
			kiv_process::Handle_Process(regs);
			break;
	}

}


void __stdcall Bootstrap_Loader(kiv_hal::TRegisters &context) {
	Initialize_Kernel();
	kiv_hal::Set_Interrupt_Handler(kiv_os::System_Int_Number, Sys_Call);
	/*
	//v ramci ukazky jeste vypiseme dostupne disky
	kiv_hal::TRegisters regs;
	for (regs.rdx.l = 0; ; regs.rdx.l++) {
		kiv_hal::TDrive_Parameters params;		
		regs.rax.h = static_cast<uint8_t>(kiv_hal::NDisk_IO::Drive_Parameters);;
		regs.rdi.r = reinterpret_cast<decltype(regs.rdi.r)>(&params);
		kiv_hal::Call_Interrupt_Handler(kiv_hal::NInterrupt::Disk_IO, regs);
			
		if (!regs.flags.carry) {
			auto print_str = [](const char* str) {
				kiv_hal::TRegisters regs;
				regs.rax.l = static_cast<uint8_t>(kiv_os::NOS_File_System::Write_File);
				regs.rdi.r = reinterpret_cast<decltype(regs.rdi.r)>(str);
				regs.rcx.r = strlen(str);
				Handle_IO(regs);
			};

			const char dec_2_hex[16] = { L'0', L'1', L'2', L'3', L'4', L'5', L'6', L'7', L'8', L'9', L'A', L'B', L'C', L'D', L'E', L'F' };
			char hexa[3];
			hexa[0] = dec_2_hex[regs.rdx.l >> 4];
			hexa[1] = dec_2_hex[regs.rdx.l & 0xf];
			hexa[2] = 0;

			print_str("Nalezen disk: 0x");
			print_str(hexa);
			print_str("\n");

		}

		if (regs.rdx.l == 255) break;
	}

	*/

	const char* prog_name = "shell";

	// shell start
	{
		kiv_hal::TRegisters sregs;

		sregs.rbx.e = 0;
		sregs.rbx.e = (sregs.rbx.e << 16) | 1;

		sregs.rax.h = static_cast<uint8_t>(kiv_os::NOS_Service_Major::Process);
		sregs.rax.l = static_cast<uint8_t>(kiv_os::NOS_Process::Clone);
		sregs.rcx.r = static_cast<uint8_t>(kiv_os::NClone::Create_Process);

		sregs.rdx.r = reinterpret_cast<decltype(sregs.rdx.r)>(prog_name);
		sregs.rdi.r = reinterpret_cast<decltype(sregs.rdx.r)>(nullptr);

		Sys_Call(sregs);

		kiv_hal::TRegisters wregs;
		wregs.rax.h = static_cast<uint8_t>(kiv_os::NOS_Service_Major::Process);
		wregs.rax.l = static_cast<uint8_t>(kiv_os::NOS_Process::Wait_For);

		size_t handles[] = { sregs.rax.r };
		
		wregs.rdx.r = reinterpret_cast<size_t>(handles);
		wregs.rcx.r = 1;
		
		Sys_Call(wregs);

		kiv_hal::TRegisters regs;
		regs.rax.h = static_cast<uint8_t>(kiv_os::NOS_Service_Major::Process);
		regs.rax.l = static_cast<uint8_t>(kiv_os::NOS_Process::Read_Exit_Code);

		regs.rdx.r = sregs.rax.r;

		Sys_Call(regs);
		
	}

	//system shutdown
	{
		kiv_hal::TRegisters sregs;

		sregs.rax.h = static_cast<uint8_t>(kiv_os::NOS_Service_Major::Process);
		sregs.rax.l = static_cast<uint8_t>(kiv_os::NOS_Process::Shutdown);

		Sys_Call(sregs);
	}
}


void Set_Error(const bool failed, kiv_hal::TRegisters &regs) {
	if (failed) {
		regs.flags.carry = true;
		regs.rax.r = GetLastError();
	}
	else
		regs.flags.carry = false;
}