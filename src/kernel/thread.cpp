#include <memory>
#include <algorithm>

#include "thread.h"
#include "process.h"
#include "common.h"
#include "..\api\api.h"
#include "kernel.h"

namespace kiv_thread {

#pragma endregion
		
		CThread_Manager * CThread_Manager::instance = NULL;

		CThread_Manager::CThread_Manager() {
		}

		void CThread_Manager::Destroy() {
			delete instance;
		}

		CThread_Manager & CThread_Manager::Get_Instance() {

			if (instance == NULL) {
				instance = new CThread_Manager();
			}

			return *instance;

		}

		// Vyvoti vlakno pro proces
		bool CThread_Manager::Create_Thread(const size_t pid, kiv_hal::TRegisters& context) {

			kiv_hal::TRegisters regs;
			regs.rax.x = context.rbx.e >> 16;
			regs.rbx.x = context.rbx.e && 0xFFFF;

			const char* func_name = (char*)(context.rdx.r);

			std::shared_ptr<kiv_process::TProcess_Control_Block> pcb = kiv_process::CProcess_Manager::Get_Instance().process_table[pid];

			kiv_os::TThread_Proc func = (kiv_os::TThread_Proc) GetProcAddress(User_Programs, func_name);

			if (!func) {
				return false;
			}

			std::shared_ptr<TThread_Control_Block> tcb = std::make_shared<TThread_Control_Block>();

			// uzamknuti tabulky procesu tzn. i tabulky vlaken
			// mozna by slo udelat efektivneji nez zamykat celou tabulku
			std::unique_lock<std::mutex> lock(kiv_process::CProcess_Manager::ptable);
			{

				std::unique_lock<std::mutex> tm_lock(maps_lock);
				{
					tcb->thread = std::thread(func, regs);

					tcb->pcb = pcb;
					tcb->state = NThread_State::RUNNING;
					tcb->tid = Hash_Thread_Id(tcb->thread.get_id());

					//return handle to parent process
					context.rax.r = tcb->tid;

					std::shared_ptr<TThread_Control_Block> ptr = tcb;
					thread_map.emplace(tcb->tid, tcb);
				}
				tm_lock.unlock();



				tcb->terminate_handler = nullptr;


				pcb->thread_table.push_back(tcb);
			}
			lock.unlock();

			return true;
		}

		//Vytvori vlakno pro jiz existujici proces
		bool  CThread_Manager::Create_Thread(kiv_hal::TRegisters& context) {

			std::shared_ptr<TThread_Control_Block> tcb;

			if (Get_Thread_Control_Block(Hash_Thread_Id(std::this_thread::get_id()), &tcb)) {
				Create_Thread(tcb->pcb->pid, context);
			}
			else {
				return false;
			}

			return true;
		}

		//Funkce je volana po skonceni vlakna/procesu
		bool CThread_Manager::Thread_Exit(kiv_hal::TRegisters& context) {

			std::shared_ptr<TThread_Control_Block> tcb;
			std::unique_lock<std::mutex> plock(kiv_process::CProcess_Manager::ptable);
			{

				if (Get_Thread_Control_Block(Hash_Thread_Id(std::this_thread::get_id()), &tcb)) {
					tcb->state = NThread_State::TERMINATED;
				}
				else {
					plock.unlock();
					return false;
				}

				//Signalizace ukonceni procesu tem kdo na to cekaji
				for (auto const & tid : tcb->waiting_threads) {
					
					std::shared_ptr<TThread_Control_Block> tcb;
					if (Get_Thread_Control_Block(tid, &tcb) == true) {
						if (tcb->wait_semaphore != nullptr) {
							tcb->wait_semaphore->Signal();
						}
					}
				}
				tcb->waiting_threads.clear();

			}
			plock.unlock();

			return true;
		}

		// Prida vlaknu/procesu hendler na funkci, ktera ho ukonci
		bool CThread_Manager::Add_Terminate_Handler(const kiv_hal::TRegisters& context) {

			std::shared_ptr<TThread_Control_Block> tcb;

			if (Get_Thread_Control_Block(Hash_Thread_Id(std::this_thread::get_id()), &tcb) == false) {
				return false;
			}

			// Pokud je rdx.r == 0 potom se ulozi do terminat_handler (stejne uz by tam mela byt)
			tcb->terminate_handler = reinterpret_cast<kiv_os::TThread_Proc>(context.rdx.r); 

			return true;

		}

		void CThread_Manager::Wait_For(kiv_hal::TRegisters& context) {

			const size_t * tids = reinterpret_cast<size_t *>(context.rdx.r);
			const size_t tids_count = context.rcx.r;

			std::unique_lock<std::mutex> lock(maps_lock);
			{
				for (int i = 0; i < tids_count; i++) {

					auto result = thread_map.find(tids[i]);

					if (result == thread_map.end()) {
						//TODO raise some error??
						context.rax.r = -1;
						lock.unlock();
						return;
					}
					else if (result->second->state == NThread_State::TERMINATED) {
						context.rax.r = tids[i];
						lock.unlock();
						return;
					}
				}
			}
			lock.unlock();

			context.rax.r = Wait(tids, tids_count);

		}

		size_t CThread_Manager::Wait(const size_t * tids, const size_t tids_count) {

			const size_t my_tid = Hash_Thread_Id(std::this_thread::get_id());
			std::shared_ptr<TThread_Control_Block> tcb;

			//TODO potencial error
			if (Get_Thread_Control_Block(my_tid, &tcb) == false) {
				return 0;
			}

			tcb->wait_semaphore = new Semaphore(0);

			for (int i = 0; i < tids_count; i++) {
				Add_Event(tids[i], my_tid);
				i++;
			}
			tcb->wait_semaphore->Wait();

			//TODO erase others form waiting quees
			size_t terminated = 0;
			for (int i = 0; i < tids_count; i++) {
				bool result = Check_Event(tids[i], my_tid);
				if (result) {
					terminated = tids[i];
				}
			}

			delete tcb->wait_semaphore;
			tcb->wait_semaphore = nullptr;

			return terminated;
		}

		void CThread_Manager::Add_Event(const size_t tid, const size_t my_tid) {
			
			std::shared_ptr<TThread_Control_Block> tcb;

			if (Get_Thread_Control_Block(tid, &tcb) == false) {
				return;
			}

			std::unique_lock<std::mutex> e_lock(tcb->waiting_lock);
			{
				tcb->waiting_threads.push_back(my_tid);
			}
			e_lock.unlock();

		}

		bool CThread_Manager::Check_Event(const size_t tid, const size_t my_tid) {

			std::shared_ptr<TThread_Control_Block> tcb;
			// Thread removed -> thread terminated
			if (Get_Thread_Control_Block(tid, &tcb) == false) {
				return true;
			}

			std::unique_lock<std::mutex> e_lock(tcb->waiting_lock);
			{
				tcb->waiting_threads.erase(std::remove(tcb->waiting_threads.begin(), tcb->waiting_threads.end(), my_tid), tcb->waiting_threads.end());
			}
			e_lock.unlock();

			if (tcb->state == NThread_State::TERMINATED) {
				return true;
			}

			return false;

		}

		bool CThread_Manager::Read_Exit_Code(kiv_hal::TRegisters &context) {

			return Read_Exit_Code(context.rdx.r, context.rcx.x);

		}

		bool CThread_Manager::Read_Exit_Code(const size_t handle, uint16_t &exit_code) {

			std::shared_ptr<TThread_Control_Block> tcb;
			NThread_State terminated = NThread_State::RUNNING;

			if (Get_Thread_Control_Block(handle, &tcb)) {
				std::unique_lock<std::mutex> lock(maps_lock);
				{
					terminated = tcb->state;
					if (terminated == NThread_State::TERMINATED) {
						exit_code = tcb->exit_code;
						thread_map.erase(tcb->tid);
					}
					else {
						lock.unlock();
						return false;
					}
				}
				lock.unlock();
			}
			else {
				return false;
			}

			if (terminated == NThread_State::TERMINATED) {
				kiv_process::CProcess_Manager::Get_Instance().Check_Process_State(tcb->pcb);
			}

			return true;

		}

		bool CThread_Manager::Get_Thread_Control_Block(const size_t &tid, std::shared_ptr<TThread_Control_Block> *tcb) {

			std::unique_lock<std::mutex> lock(maps_lock);
			{
				auto result = thread_map.find(tid);
				if (result == thread_map.end()) {
					lock.unlock();
					return false;
				}
				else {
					*tcb = result->second;
				}
			}
			lock.unlock();

			return true;
		}

}