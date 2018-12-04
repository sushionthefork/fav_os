#include "fs_fat.h"
#include "../api/api.h"

// TODO Smazat
#include <iostream>
#include <string.h>
using namespace std;
///////////////

namespace kiv_fs_fat {
	// FAT entry status
	const uint32_t FAT_FREE = static_cast<uint32_t>(-2);
	const uint32_t FAT_RESERVED = static_cast<uint32_t>(-3);
	const uint32_t FAT_EOF = static_cast<uint32_t>(-4);

	const size_t MAX_DIR_ENTRIES = 21;
	const char *FAT_NAME = "fat";
	const TFAT_Dir_Entry root_dir_entry{ "\\" };


#pragma region IO Utils
	CFAT_Utils::CFAT_Utils(TSuperblock &sb, kiv_vfs::TDisk_Number disk_number)
		: mSb(sb), mDisk_number(disk_number)
	{
	}

	CFAT_Utils::CFAT_Utils(kiv_vfs::TDisk_Number disk_number)
		: mSb(TSuperblock{}), mDisk_number(disk_number)
	{
	}

	bool CFAT_Utils::Write_To_Disk(char *sectors, uint64_t first_sector, uint64_t num_of_sectors) {
		std::unique_lock<std::mutex> lock(mDisk_access_lock);

		kiv_hal::TRegisters regs;
		kiv_hal::TDisk_Address_Packet dap;

		dap.lba_index = first_sector;
		dap.count = num_of_sectors;
		dap.sectors = sectors;

		regs.rax.h = static_cast<decltype(regs.rax.h)>(kiv_hal::NDisk_IO::Write_Sectors);;
		regs.rdx.l = static_cast<decltype(regs.rdx.l)>(mDisk_number);
		regs.rdi.r = reinterpret_cast<decltype(regs.rdi.r)>(&dap);

		kiv_hal::Call_Interrupt_Handler(kiv_hal::NInterrupt::Disk_IO, regs);

		return (regs.flags.carry == 0);
	}

	bool CFAT_Utils::Read_From_Disk(char *buffer, uint64_t first_sector, uint64_t num_of_sectors) {
		std::unique_lock<std::mutex> lock(mDisk_access_lock);

		kiv_hal::TRegisters regs;
		kiv_hal::TDisk_Address_Packet dap;

		dap.lba_index = first_sector;
		dap.count = num_of_sectors;
		dap.sectors = buffer;

		regs.rax.h = static_cast<decltype(regs.rax.h)>(kiv_hal::NDisk_IO::Read_Sectors);;
		regs.rdx.l = static_cast<decltype(regs.rdx.l)>(mDisk_number);
		regs.rdi.r = reinterpret_cast<decltype(regs.rdi.r)>(&dap);

		kiv_hal::Call_Interrupt_Handler(kiv_hal::NInterrupt::Disk_IO, regs);

		return (regs.flags.carry == 0);
	}

	bool CFAT_Utils::Write_Clusters(char *clusters, uint64_t first_cluster, uint64_t num_of_clusters) {
		return Write_To_Disk(clusters, first_cluster * mSb.sectors_per_cluster, num_of_clusters * mSb.sectors_per_cluster);
	}

	bool CFAT_Utils::Read_Clusters(char *buffer, uint64_t first_cluster, uint64_t num_of_clusters) {
		return Read_From_Disk(buffer, first_cluster * mSb.sectors_per_cluster, num_of_clusters * mSb.sectors_per_cluster);
	}

	bool CFAT_Utils::Write_Data_Cluster(char *clusters, TFAT_Entry fat_entry) {
		return Write_Clusters(clusters, mSb.data_first_cluster + fat_entry, 1);
	}

	bool CFAT_Utils::Read_Data_Cluster(char *buffer, TFAT_Entry fat_entry) {
		return Read_Clusters(buffer, mSb.data_first_cluster + fat_entry, 1);
	}

	bool CFAT_Utils::Set_Fat_Entries_Value(std::vector<TFAT_Entry> &entries, TFAT_Entry value) {
		if (!entries.empty()) {
			std::map<TFAT_Entry, TFAT_Entry> map;
			for (auto it = entries.begin(); it != entries.end(); ++it) {
				map.insert(std::pair<TFAT_Entry, TFAT_Entry>(*it, value));
			}

			if (!Write_Fat_Entries(map)) {
				return false;
			}
		}

		return true;
	}

	bool CFAT_Utils::Get_Free_Fat_Entries(std::vector<TFAT_Entry> &entries, size_t number_of_entries) {
		size_t cluster_size = mSb.sectors_per_cluster * mSb.disk_params.bytes_per_sector;
		size_t entries_per_cluster = cluster_size / sizeof(TFAT_Entry);
		
		char *cluster_buffer = new char[cluster_size];

		size_t curr_cluster = mSb.fat_table_first_cluster;
		TFAT_Entry curr_entry = 0;

		TFAT_Entry entry;
		while (curr_entry < mSb.fat_table_number_of_entries) {

			// Read new cluster if needed
			if (curr_entry % entries_per_cluster == 0) {
				Read_Clusters(cluster_buffer, curr_cluster, 1);
				curr_cluster++;
			}

			// Load current FAT entry
			memcpy(&entry, cluster_buffer + (curr_entry % entries_per_cluster) * sizeof(TFAT_Entry), sizeof(TFAT_Entry));

			// Free entry found
			if (entry == FAT_FREE) {
				entries.push_back(curr_entry);

				// All requested entries found
				if (entries.size() == number_of_entries) {
					return Set_Fat_Entries_Value(entries, FAT_RESERVED);
				}

			}

			curr_entry++;
		}

		return false;
	}

	bool CFAT_Utils::Write_Fat_Entries(std::map<TFAT_Entry, TFAT_Entry> &entries) {
		size_t cluster_size = mSb.sectors_per_cluster * mSb.disk_params.bytes_per_sector;
		char *cluster_buffer = new char[cluster_size];
		size_t entries_per_cluster = cluster_size / sizeof(TFAT_Entry);

		size_t cluster_loaded = static_cast<size_t>(-1);
		size_t cluster_needed;

		size_t order, order_in_cluster;
		TFAT_Entry value;
		for (auto it = entries.begin(); it != entries.end(); it++) {
			order = it->first;
			value = it->second;

			cluster_needed = (order / entries_per_cluster) + mSb.fat_table_first_cluster;

			// This FAT entry is not located in currently loaded cluster
			if (cluster_needed != cluster_loaded) {

				// Store current cluster
				if (cluster_loaded != static_cast<size_t>(-1)) {
					if (!Write_Clusters(cluster_buffer, cluster_loaded, 1)) {
						delete[] cluster_buffer;
						return false;
					}
				}

				// Load needed cluster
				if (!Read_Clusters(cluster_buffer, cluster_needed, 1)) {
					delete[] cluster_buffer;
					return false;
				}

				cluster_loaded = cluster_needed;
			}

			order_in_cluster = order % entries_per_cluster;

			// Store new FAT entry into a buffer
			memcpy(cluster_buffer + order_in_cluster * sizeof(TFAT_Entry), &value, sizeof(TFAT_Entry));
		}

		// Store current cluster
		if (cluster_loaded != static_cast<size_t>(-1)) {
			if (!Write_Clusters(cluster_buffer, cluster_loaded, 1)) {
				delete[] cluster_buffer;
				return false;
			}
		}

		delete[] cluster_buffer;
		return true;
	}

	bool CFAT_Utils::Get_File_Fat_Entries(TFAT_Entry first_entry, std::vector<TFAT_Entry> &entries) {
		size_t cluster_size = mSb.sectors_per_cluster * mSb.disk_params.bytes_per_sector;
		size_t entries_per_cluster = cluster_size / sizeof(TFAT_Entry);

		char *cluster_buffer = new char[cluster_size];

		size_t cluster_loaded = static_cast<size_t>(-1);
		size_t cluster_needed;

		size_t order_in_cluster;

		TFAT_Entry value = first_entry;
		while (value != FAT_EOF) {
			entries.push_back(value);

			cluster_needed = (value / entries_per_cluster) + mSb.fat_table_first_cluster;

			// FAT entry is not located in currently loaded cluster -> Load needed cluster
			if (cluster_needed != cluster_loaded) {
				if (!Read_Clusters(cluster_buffer, cluster_needed, 1)) {
					delete[] cluster_buffer;
					return false;
				}
				cluster_loaded = cluster_needed;
			}

			order_in_cluster = value % entries_per_cluster;

			// Load value
			memcpy(&value, cluster_buffer + order_in_cluster * sizeof(TFAT_Entry), sizeof(TFAT_Entry));
		}

		delete[] cluster_buffer;

		return true;
	}

	bool CFAT_Utils::Free_File_Fat_Entries(TFAT_Dir_Entry &entry) {
		std::vector<TFAT_Entry> entries;

		if (!Get_File_Fat_Entries(entry.start, entries)) {
			return false;
		}

		return Set_Fat_Entries_Value(entries, FAT_FREE);;
	}

	bool CFAT_Utils::Load_Directory(std::vector<TFAT_Dir_Entry> dirs_from_root, std::shared_ptr<IDirectory> &directory) {
		TFAT_Dir_Entry dir_entry = dirs_from_root.back();

		// Dir is root
		if (strcmp(dir_entry.name, root_dir_entry.name) == 0) {
			directory = mRoot;
		}
		else {
			kiv_vfs::TPath path;
			path.file = dir_entry.name;
			dirs_from_root.pop_back();
			directory = make_shared<CDirectory>(path, dir_entry, dirs_from_root, this);
		}

		return true;
	}

	void CFAT_Utils::Set_Superblock(TSuperblock sb) {
		mSb = sb;
	}

	void CFAT_Utils::Set_Root(std::shared_ptr<CRoot> &root) {
		mRoot = root;
	}

	TSuperblock &CFAT_Utils::Get_Superblock() {
		return mSb;
	}

	std::map<TFAT_Entry, TFAT_Entry> CFAT_Utils::Create_Fat_Entries_Chain(std::vector<TFAT_Entry> &entries) {
		std::map<TFAT_Entry, TFAT_Entry> entry_map;

		if (entries.size() != 0) {
			for (size_t i = 0; i < entries.size() - 1; i++) {
				entry_map.insert(std::pair<TFAT_Entry, TFAT_Entry>(entries.at(i), entries.at(i + 1)));
			}
			entry_map.insert(std::pair<TFAT_Entry, TFAT_Entry>(entries.back(), FAT_EOF));
		}

		return entry_map;
	}

#pragma endregion

#pragma region Abstract directory

	IDirectory::IDirectory(CFAT_Utils *utils) 
		: mUtils(utils)
	{
	}

	kiv_os::NOS_Error IDirectory::Read(char *buffer, size_t buffer_size, size_t position, size_t &read) {
		std::unique_lock<std::recursive_mutex> lock(mFile_lock);

		read = 0;

		if (!Load()) {
			return kiv_os::NOS_Error::IO_Error;
		}
		// Buffer is not big enough even for one entry
		if (buffer_size < sizeof(kiv_os::TDir_Entry)) {
			return kiv_os::NOS_Error::Invalid_Argument;
		}

		kiv_os::TDir_Entry os_dir_entry;
		TFAT_Dir_Entry fat_dir_entry;

		size_t fat_entry_index;
		for (size_t i = 0; i < buffer_size; i += sizeof(kiv_os::TDir_Entry)) {
			fat_entry_index = (i + position) / sizeof(kiv_os::TDir_Entry);

			// All entries have been read
			if (fat_entry_index >= mEntries.size()) {
				break;
			}

			fat_dir_entry = mEntries.at(fat_entry_index);

			// Fat dir entry -> os dir entry
			strcpy_s(os_dir_entry.file_name, fat_dir_entry.name);
			os_dir_entry.file_attributes = static_cast<decltype(os_dir_entry.file_attributes)>(fat_dir_entry.attributes);

			// Store direntry into a buffer
			memcpy(buffer + i, &os_dir_entry, sizeof(kiv_os::TDir_Entry));

			read += sizeof(kiv_os::TDir_Entry);
		}

		return kiv_os::NOS_Error::Success;
	}

	bool IDirectory::Is_Empty() {
		std::unique_lock<std::recursive_mutex> lock(mFile_lock);

		if (!Load()) {
			return false;
		}

		return (mEntries.size() == 0);
	}

	std::shared_ptr<kiv_vfs::IFile> IDirectory::Create_File(const kiv_vfs::TPath path, kiv_os::NFile_Attributes attributes) {
		std::unique_lock<std::recursive_mutex> lock(mFile_lock);

		if (!Load()) {
			return false;
		}

		// Directory is full
		if (mEntries.size() == MAX_DIR_ENTRIES) {
			return nullptr;
		}

		std::vector<TFAT_Entry> entry;
		if (!mUtils->Get_Free_Fat_Entries(entry, 1)) {
			return nullptr;
		}

		// Create directory entry
		TFAT_Dir_Entry dir_entry;

		dir_entry.attributes = attributes;
		dir_entry.filesize = 0;
		strcpy_s(dir_entry.name, path.file.c_str()); // TODO unsafe maybe? (c_str() appends '\0')
		dir_entry.start = entry[0];

		mEntries.push_back(dir_entry);

		// Write directory entry to disk
		std::map<TFAT_Entry, TFAT_Entry> entry_map;
		entry_map.insert(std::pair<TFAT_Entry, TFAT_Entry>(entry[0], FAT_EOF));

		if (!mUtils->Write_Fat_Entries(entry_map)) {
			mUtils->Set_Fat_Entries_Value(entry, FAT_FREE);
			return nullptr;
		}

		mSize += sizeof(TFAT_Dir_Entry);

		if (!Save()) {
			mUtils->Set_Fat_Entries_Value(entry, FAT_FREE);
			return nullptr;
		}

		auto result = Make_File(path, dir_entry);
		if (!result) {
			mUtils->Set_Fat_Entries_Value(entry, FAT_FREE);
		}

		return result;
	}

	bool IDirectory::Remove_File(const kiv_vfs::TPath &path) {
		std::unique_lock<std::recursive_mutex> lock(mFile_lock);

		if (!Load()) {
			return false;
		}

		for (auto it = mEntries.begin(); it != mEntries.end(); ++it) {
			// File found
			if (it->name == path.file) {

				if (!mUtils->Free_File_Fat_Entries(*it)) {
					return false;
				}

				// This is the only dir entry -> just remove it
				if (mEntries.size() == 1) {
					mEntries.erase(it);
				}
				// Replace this entry with last one
				else {
					auto last_entry = mEntries.back();
					*it = last_entry;
					mEntries.pop_back();
				}

				mSize -= sizeof(TFAT_Dir_Entry);

				if (!Save()) {
					return false;
				}
				return true;
			}
		}

		return false;
	}

	bool IDirectory::Find(std::string filename, TFAT_Dir_Entry &entry) {
		std::unique_lock<std::recursive_mutex> lock(mFile_lock);

		if (!Load()) {
			return false;
		}
		for (auto it = mEntries.begin(); it != mEntries.end(); ++it) {
			if (it->name == filename) {
				entry = *it;
				return true;
			}
		}

		return false;
	}

	bool IDirectory::Change_Entry_Size(std::string filename, uint32_t filesize) {
		std::unique_lock<std::recursive_mutex> lock(mFile_lock);

		if (!Load()) {
			return false;
		}

		for (auto it = mEntries.begin(); it != mEntries.end(); ++it) {
			if (it->name == filename) {
				it->filesize = filesize;
				if (!Save()) {
					return false;
				}
				return true;
			}
		}

		return false;
	}

	bool IDirectory::Get_Entry_Size(std::string filename, uint32_t &filesize) {
		std::unique_lock<std::recursive_mutex> lock(mFile_lock);

		if (!Load()) {
			return false;
		}

		for (auto it = mEntries.begin(); it != mEntries.end(); ++it) {
			if (it->name == filename) {
				filesize = it->filesize;
				return true;
			}
		}

		return false;
	}
#pragma endregion

#pragma region Subdirectory
	CDirectory::CDirectory(const kiv_vfs::TPath path, TFAT_Dir_Entry &dir_entry, std::vector<TFAT_Dir_Entry> dirs_to_parent, CFAT_Utils *utils)
		: IDirectory(utils), mDir_entry(dir_entry), mDirs_to_parent(dirs_to_parent)
	{
		mPath = path;
		mAttributes = dir_entry.attributes;
		mSize = dir_entry.filesize;
	}

	CDirectory::CDirectory(TFAT_Dir_Entry &dir_entry, CFAT_Utils *utils)
		: IDirectory(utils)
	{
		kiv_vfs::TPath path;
		CDirectory(path, dir_entry, std::vector<TFAT_Dir_Entry>{}, utils);
	}

	std::shared_ptr<kiv_vfs::IFile> CDirectory::Make_File(kiv_vfs::TPath path, TFAT_Dir_Entry entry) {
		std::vector<TFAT_Dir_Entry> dirs_to_this = { mDirs_to_parent };
		dirs_to_this.push_back(mDir_entry);

		if (entry.attributes == kiv_os::NFile_Attributes::Directory) {
			return std::make_shared<CDirectory>(path, entry, dirs_to_this, mUtils);
		}
		else {
			return std::make_shared<CFile>(path, entry, dirs_to_this, mUtils);
		}
	}

	bool CDirectory::Load() {
		mEntries.clear();

		// Load size
		std::shared_ptr<IDirectory> parent;
		if (!mUtils->Load_Directory(mDirs_to_parent, parent)) {
			return false;
		}
		parent->Get_Entry_Size(mPath.file, mSize);

		// Read data from disk
		char *buffer = new char[mUtils->Get_Superblock().sectors_per_cluster * mUtils->Get_Superblock().disk_params.bytes_per_sector];
		if (!mUtils->Read_Data_Cluster(buffer, mDir_entry.start)) {
			delete[] buffer;
			return false;
		}

		// Parse entries
		TFAT_Dir_Entry entry;
		size_t number_of_entries = mSize / sizeof(TFAT_Dir_Entry);

		for (size_t i = 0; i < number_of_entries; i++) {
			memcpy(&entry, buffer + i * sizeof(TFAT_Dir_Entry), sizeof(TFAT_Dir_Entry));
			mEntries.push_back(entry);
		}

		delete[] buffer;

		return true;
	}

	bool CDirectory::Save() {
		char *buffer = new char[mUtils->Get_Superblock().sectors_per_cluster * mUtils->Get_Superblock().disk_params.bytes_per_sector];

		// Save entries
		size_t address = 0;
		for (auto it = mEntries.begin(); it != mEntries.end(); ++it) {
			memcpy(buffer + address, &(*it), sizeof(TFAT_Dir_Entry));
			address += sizeof(TFAT_Dir_Entry);
		}

		TFAT_Dir_Entry entry;
		for (int i = 0; i < mEntries.size(); i++) {
			memcpy(&entry, buffer + i * sizeof(TFAT_Dir_Entry), sizeof(TFAT_Dir_Entry));
		}

		bool res = mUtils->Write_Data_Cluster(buffer, mDir_entry.start);
		delete[] buffer;

		// Save size of directory
		std::shared_ptr<IDirectory> parent;
		if (!mUtils->Load_Directory(mDirs_to_parent, parent)) {
			return nullptr;
		}
		parent->Change_Entry_Size(mPath.file, static_cast<uint32_t>(mEntries.size() * sizeof(TFAT_Dir_Entry)));

		return res;
	}

#pragma endregion

#pragma region Root
	CRoot::CRoot(CFAT_Utils *utils)
		: IDirectory(utils)
	{
		mAttributes = kiv_os::NFile_Attributes::Directory;
	}

	bool CRoot::Load() {
		std::unique_lock<std::recursive_mutex> lock(mFile_lock);

		mEntries.clear();
		char *buffer = new char[mUtils->Get_Superblock().sectors_per_cluster * mUtils->Get_Superblock().disk_params.bytes_per_sector];
		if (!mUtils->Read_Clusters(buffer, mUtils->Get_Superblock().root_cluster, 1)) {
			delete[] buffer;
			return false;
		}

		// Parse size of root
		memcpy(&mSize, buffer, sizeof(mSize));

		// Parse content of root
		TFAT_Dir_Entry entry;
		size_t number_of_entries = mSize / sizeof(TFAT_Dir_Entry);

		for (size_t i = 0; i < number_of_entries; i++) {
			memcpy(&entry, buffer + sizeof(mSize) + i * sizeof(TFAT_Dir_Entry), sizeof(TFAT_Dir_Entry));
			mEntries.push_back(entry);
		}

		delete[] buffer;
		return true;
	}

	bool CRoot::Save() {
		char *buffer = new char[mUtils->Get_Superblock().sectors_per_cluster * mUtils->Get_Superblock().disk_params.bytes_per_sector];

		bool result;

		{
			std::unique_lock<std::recursive_mutex> lock(mFile_lock);

			// Save size of root
			memcpy(buffer, &mSize, sizeof(mSize));

			// Save entries (start after size of the root)
			size_t address = sizeof(mSize);
			for (auto it = mEntries.begin(); it != mEntries.end(); ++it) {
				memcpy(buffer + address, &(*it), sizeof(TFAT_Dir_Entry));
				address += sizeof(TFAT_Dir_Entry);
			}

			result = mUtils->Write_Clusters(buffer, mUtils->Get_Superblock().root_cluster, 1);
		}

		delete[] buffer;

		return result;
	}

	std::shared_ptr<kiv_vfs::IFile> CRoot::Make_File(kiv_vfs::TPath path, TFAT_Dir_Entry entry) {
		std::vector<TFAT_Dir_Entry> dirs_to_this = { root_dir_entry };

		if (entry.attributes == kiv_os::NFile_Attributes::Directory) {
			return std::make_shared<CDirectory>(path, entry, dirs_to_this, mUtils);
		}
		else {
			return std::make_shared<CFile>(path, entry, dirs_to_this, mUtils);
		}
	}
#pragma endregion

#pragma region File
	CFile::CFile(const kiv_vfs::TPath path, TFAT_Dir_Entry &dir_entry, std::vector<TFAT_Dir_Entry> dirs_to_parent, CFAT_Utils *utils)
		: mUtils(utils), mDirs_to_parent(dirs_to_parent)
	{
		mPath = path;
		mAttributes = dir_entry.attributes;
		mSize = dir_entry.filesize;
		if (!mUtils->Get_File_Fat_Entries(dir_entry.start, mFat_entries)) {
			// TODO Handle error
		}
	}

	kiv_os::NOS_Error CFile::Write(const char *buffer, size_t buffer_size, size_t position,size_t &written) {
		std::unique_lock<std::recursive_mutex> lock(mFile_lock);

		written = 0;

		if (buffer_size == 0) {
			return kiv_os::NOS_Error::Invalid_Argument;
		}

		size_t bytes_to_write = buffer_size;

		size_t cluster_size = mUtils->Get_Superblock().sectors_per_cluster * mUtils->Get_Superblock().disk_params.bytes_per_sector;
		size_t first_cluster = position / cluster_size;
		size_t last_byte = position + bytes_to_write;
		size_t last_cluster;
		if (last_byte == 0) {
			last_cluster = 0;
		}
		else {
			last_cluster = (last_byte % cluster_size == 0)
				? ((last_byte / cluster_size) - 1)
				: ((last_byte / cluster_size));
		}
		size_t clusters_needed = last_cluster + 1;

		// Need new clusters
		std::vector<TFAT_Entry> new_entries;
		if (mFat_entries.size() < clusters_needed) {
			// Get free entries
			size_t num_of_new_entries = clusters_needed - mFat_entries.size();
			if (!mUtils->Get_Free_Fat_Entries(new_entries, num_of_new_entries)) {
				return kiv_os::NOS_Error::Not_Enough_Disk_Space;
			}

			// Create new vector of entries (current entries + new entries). Current entries will be set to vector this later.
			std::vector<TFAT_Entry> tmp_entries = mFat_entries;
			tmp_entries.insert(tmp_entries.end(), new_entries.begin(), new_entries.end());

			// Write new entries
			auto entry_map = mUtils->Create_Fat_Entries_Chain(tmp_entries);
			if (!mUtils->Write_Fat_Entries(entry_map)) {
				mUtils->Set_Fat_Entries_Value(new_entries, FAT_FREE);
				return kiv_os::NOS_Error::IO_Error;
			}

			mFat_entries = tmp_entries;
		}

		// Write to clusters
		char *cluster = new char[cluster_size];
		size_t bytes_to_write_in_cluster;
		for (size_t i = first_cluster; i <= last_cluster; i++) {
			if (!mUtils->Read_Data_Cluster(cluster, mFat_entries.at(i))) {
				delete[] cluster;
				return kiv_os::NOS_Error::IO_Error;
			}

			// The position has to be taken into consideration in the first cluster
			if (i == first_cluster) {
				bytes_to_write_in_cluster = (bytes_to_write > (cluster_size * (i + 1) - position))
					? (cluster_size * (i + 1) - position)
					: bytes_to_write;
				memcpy(cluster + (position - cluster_size * i), buffer, bytes_to_write_in_cluster);
			}
			else {
				bytes_to_write_in_cluster = ((bytes_to_write - written) > cluster_size)
					? cluster_size
					: (bytes_to_write - written);
				memcpy(cluster, buffer + written, bytes_to_write_in_cluster);
			}

			if (!mUtils->Write_Data_Cluster(cluster, mFat_entries.at(i))) {
				delete[] cluster;
				written = 0;
				return kiv_os::NOS_Error::IO_Error;
			}
			written += bytes_to_write_in_cluster;
		}

		// Change filesize if needed
		if (position + bytes_to_write > mSize) {
			mSize = static_cast<uint32_t>(position + bytes_to_write);
			std::shared_ptr<IDirectory> parent;
			if (!mUtils->Load_Directory(mDirs_to_parent, parent)) {
				written = 0;
				return kiv_os::NOS_Error::IO_Error;
			}
			parent->Change_Entry_Size(mPath.file, mSize);
		}

		delete[] cluster;
		return kiv_os::NOS_Error::Success;
	}

	kiv_os::NOS_Error CFile::Read(char *buffer, size_t buffer_size, size_t position, size_t &read) {
		std::unique_lock<std::recursive_mutex> lock(mFile_lock);

		read = 0;

		if (buffer_size == 0) {
			return kiv_os::NOS_Error::Invalid_Argument;
		}

		// Get number of bytes to read (whole buffer or rest of the file)
		size_t bytes_to_read = (position + buffer_size < mSize) 
			? buffer_size 
			: (mSize - position);

		size_t cluster_size = mUtils->Get_Superblock().sectors_per_cluster * mUtils->Get_Superblock().disk_params.bytes_per_sector;
		size_t first_cluster = position / cluster_size;
		size_t last_byte = position + bytes_to_read;
		size_t last_cluster;
		if (last_byte == 0) {
			last_cluster = 0;
		}
		else {
			last_cluster = (last_byte % cluster_size == 0)
				? (last_byte / cluster_size) - 1
				: ((last_byte / cluster_size));
		}

		// Read from clusters
		char *cluster = new char[cluster_size];
		size_t bytes_to_read_in_cluster;
		for (size_t i = first_cluster; i <= last_cluster; i++) {
			if (!mUtils->Read_Data_Cluster(cluster, mFat_entries.at(i))) {
				delete[] cluster;
				read = 0;
				return kiv_os::NOS_Error::IO_Error;
			}

			// The position has to be taken into consideration in the first cluster
			if (i == first_cluster) {
				bytes_to_read_in_cluster = (bytes_to_read > (cluster_size * (i + 1) - position))
					? (cluster_size * (i + 1) - position)
					: bytes_to_read;
				memcpy(buffer, cluster + (position - (cluster_size * i)), bytes_to_read_in_cluster);
			} 
			else {
				bytes_to_read_in_cluster = ((bytes_to_read - read) > cluster_size)
					? cluster_size
					: (bytes_to_read - read);

				memcpy(buffer + read, cluster, bytes_to_read_in_cluster);
			}
			read += bytes_to_read_in_cluster;
		}

		delete[] cluster;
		return kiv_os::NOS_Error::Success;
	}

	kiv_os::NOS_Error CFile::Resize(size_t size) {
		std::unique_lock<std::recursive_mutex> lock(mFile_lock);

		// Nothing to do
		if (size == mSize) {
			return kiv_os::NOS_Error::Success;
		}

		TSuperblock sb = mUtils->Get_Superblock();
		size_t bytes_per_cluster = sb.sectors_per_cluster * sb.disk_params.bytes_per_sector;
		size_t clusters_needed = ((size % bytes_per_cluster) == 0)
			? (size / bytes_per_cluster)
			: ((size / bytes_per_cluster) + 1);
		size_t clusters_allocated = mFat_entries.size();

		// Downsize
		if (size < mSize) {

			// Need to free clusters that became unused
			if (clusters_needed != clusters_allocated) {
				size_t clusters_to_free = clusters_allocated - clusters_needed;

				// Remove last N entries and free them
				std::vector<TFAT_Entry> entries_to_free;
				for (int i = 0; i < clusters_to_free; i++) {
					entries_to_free.push_back(mFat_entries.back());
					mFat_entries.pop_back();
				}
				mUtils->Set_Fat_Entries_Value(entries_to_free, FAT_FREE);

				// Modify last entry
				mUtils->Set_Fat_Entries_Value(std::vector<TFAT_Entry>{mFat_entries.back()}, FAT_EOF);
			}

		}

		// Upsize
		else {

			// Need to allocate new clusters
			if (clusters_needed != clusters_allocated) {
				size_t clusters_to_allocate = clusters_needed - clusters_allocated;

				std::vector<TFAT_Entry> allocated_entries;
				if (!mUtils->Get_Free_Fat_Entries(allocated_entries, clusters_to_allocate)) {
					return kiv_os::NOS_Error::Not_Enough_Disk_Space;
				}

				// Create new vector of entries (current entries + new entries). Current entries will be set to vector this later.
				std::vector<TFAT_Entry> tmp_entries = mFat_entries;
				tmp_entries.insert(tmp_entries.end(), allocated_entries.begin(), allocated_entries.end());

				auto entry_map = mUtils->Create_Fat_Entries_Chain(tmp_entries);
				if (!mUtils->Write_Fat_Entries(entry_map)) {
					mUtils->Set_Fat_Entries_Value(allocated_entries, FAT_FREE);
					return kiv_os::NOS_Error::IO_Error;
				}

				mFat_entries = tmp_entries;
			}

		}

		// Change filesize
		mSize = static_cast<uint32_t>(size);
		std::shared_ptr<IDirectory> parent;
		if (!mUtils->Load_Directory(mDirs_to_parent, parent)) {
			return kiv_os::NOS_Error::IO_Error;
		}
		parent->Change_Entry_Size(mPath.file, mSize);

		return kiv_os::NOS_Error::Success;
	}

	bool CFile::Is_Available_For_Write() {
		std::unique_lock<std::recursive_mutex> lock(mFile_lock);

		return (mWrite_count == 0);
	}

	size_t CFile::Get_Size() {
		std::unique_lock<std::recursive_mutex> lock(mFile_lock);

		return mSize;
	}
#pragma endregion

#pragma region Mount
	CMount::CMount(std::string label, kiv_vfs::TDisk_Number disk_number) {
		mLabel = label;
		mDisk_Number = disk_number;
		mUtils = new CFAT_Utils(disk_number);

		kiv_hal::TDrive_Parameters disk_params;
		if (!Load_Disk_Params(disk_params)) {
			cout << "FAT - Couldn't load disk params" << endl;
			return;
			// TODO Handle error
		}

		if (!Load_Superblock(disk_params)) { 
			cout << "FAT - LOAD SUPERBLOCK ERR" << endl;
			return;
			// TODO Handle error
		}

		// Check if disk is formatted
		if (!Chech_Superblock()) { 
			cout << "FAT - NOT FORMATTED" << endl;
			if (!Format_Disk(disk_params)) { 
				cout << "FAT - FORMAT ERR" << endl;
				return;
				// TODO Handle error
			}
		}

		root = std::make_shared<CRoot>(mUtils); // TODO Handle error

		mUtils->Set_Superblock(mSuperblock);
		mUtils->Set_Root(root);
	}

	CMount::~CMount() {
		delete mUtils;
	}

	kiv_os::NOS_Error CMount::Open_File(const kiv_vfs::TPath &path, kiv_os::NFile_Attributes attributes, std::shared_ptr<kiv_vfs::IFile> &file) {
		std::vector<TFAT_Dir_Entry> entries_from_root{ root_dir_entry };
		TFAT_Dir_Entry entry;
		std::shared_ptr<IDirectory> directory;

		// Open root
		if (path.file.length() == 0) {
			file = root;
			return kiv_os::NOS_Error::Success;
		}

		// File is directly in the root
		if (path.path.empty()) {
			if (!root->Find(path.file, entry)) {
				return kiv_os::NOS_Error::File_Not_Found;
			}
			file = root->Make_File(path, entry);
			return kiv_os::NOS_Error::Success;
		}

		// Find parent
		for (int i = 0; i < path.path.size(); i++) {
			mUtils->Load_Directory(entries_from_root, directory);
			// Directory does not exist
			if (!directory->Find(path.path[i], entry)) {
				return kiv_os::NOS_Error::File_Not_Found;
			}
			entries_from_root.push_back(entry);
		}

		mUtils->Load_Directory(entries_from_root, directory);

		// File does not exists
		if (!directory->Find(path.file, entry)) {
			return kiv_os::NOS_Error::File_Not_Found;
		}

		file = directory->Make_File(path, entry);
		return kiv_os::NOS_Error::Success;
	}
	
	kiv_os::NOS_Error CMount::Create_File(const kiv_vfs::TPath &path, kiv_os::NFile_Attributes attributes, std::shared_ptr<kiv_vfs::IFile> &file) {

		// TODO check filename?
		if (path.file.length() == 0) {
			return kiv_os::NOS_Error::File_Not_Found;
		}

		// Create file directly in the root
		if (path.path.empty()) {
			if (root->Find(path.file, TFAT_Dir_Entry{})) {
				Delete_File(path);
			}
			file = root->Create_File(path, attributes);
			return kiv_os::NOS_Error::Success;
		}

		std::vector<TFAT_Dir_Entry> entries_from_root { root_dir_entry };
		TFAT_Dir_Entry entry;
		std::shared_ptr<IDirectory> directory;
		kiv_vfs::TPath tmp_path;

		// Find parent
		for (int i = 0; i < path.path.size(); i++) {
			mUtils->Load_Directory(entries_from_root, directory);
			// Directory not exists -> Create it
			if (!directory->Find(path.path[i], entry)) {
				tmp_path.file = path.path[i];
				if (!directory->Create_File(tmp_path, kiv_os::NFile_Attributes::Directory)) {
					return kiv_os::NOS_Error::Not_Enough_Disk_Space;
				}
				directory->Find(path.path[i], entry);
			}

			tmp_path.file = path.path[i];
			entries_from_root.push_back(entry);
		}

		// File already exists -> Remove it
		mUtils->Load_Directory(entries_from_root, directory);
		if (directory->Find(path.file, entry)) {
			Delete_File(path);
		}
		file = directory->Create_File(path, attributes);
		return kiv_os::NOS_Error::Success;
	}

	kiv_os::NOS_Error CMount::Delete_File(const kiv_vfs::TPath &path) {
		kiv_vfs::TPath parent_path;

		// Get path of file's parent
		parent_path.mount = path.mount;
		parent_path.path = path.path;

		// File is not in the root
		if (parent_path.path.size() != 0) {
			parent_path.file = parent_path.path.back();
			parent_path.path.pop_back();
		}

		// Open file's parent
		std::shared_ptr<kiv_vfs::IFile> parent_file;
		kiv_os::NOS_Error open_result = Open_File(parent_path, (kiv_os::NFile_Attributes)(0), parent_file);
		if (open_result != kiv_os::NOS_Error::Success || !parent_file->Is_Directory()) {
			return kiv_os::NOS_Error::File_Not_Found;
		}

		// Remove file from the parent
		std::shared_ptr<IDirectory> parent_dir = std::dynamic_pointer_cast<IDirectory>(parent_file);
		return parent_dir->Remove_File(path) 
			? kiv_os::NOS_Error::Success 
			: kiv_os::NOS_Error::File_Not_Found;
	}

	bool CMount::Load_Superblock(kiv_hal::TDrive_Parameters &params) {
		char *buff = new char[params.bytes_per_sector];

		bool result = mUtils->Read_From_Disk(buff, 0, 1);
		if (result) {
			mSuperblock = *reinterpret_cast<TSuperblock *>(buff);
		}

		delete[] buff;

		return result;
	}

	bool CMount::Chech_Superblock() {
		return (strcmp(FAT_NAME, mSuperblock.name) == 0);
	}

	bool CMount::Format_Disk(kiv_hal::TDrive_Parameters &params) {
		cout << "FAT - Formating" << endl;

		size_t sectors_per_cluster = 1;
		size_t cluster_size = sectors_per_cluster * params.bytes_per_sector;
		size_t disk_size = params.absolute_number_of_sectors * params.bytes_per_sector;
		size_t available_space = disk_size - (2 * cluster_size); // Disk size - superblock cluster - root cluster
		size_t num_of_fat_entries = available_space / (sizeof(TFAT_Dir_Entry) + cluster_size);
		num_of_fat_entries -= ((num_of_fat_entries * sizeof(TFAT_Entry)) % (cluster_size)) / sizeof(TFAT_Entry);
		size_t num_of_fat_entries_clusters = (num_of_fat_entries * sizeof(TFAT_Entry)) / cluster_size;

		// Set up superblock
		strcpy_s(mSuperblock.name, FAT_NAME);
		mSuperblock.disk_params = params;
		mSuperblock.fat_table_first_cluster = 1;
		mSuperblock.sectors_per_cluster = sectors_per_cluster;
		mSuperblock.fat_table_number_of_entries = num_of_fat_entries;
		mSuperblock.root_cluster = 1 + num_of_fat_entries_clusters;
		mSuperblock.data_first_cluster = mSuperblock.root_cluster + 1; 

		mUtils->Set_Superblock(mSuperblock);

		// Write superblock to the first sector
		char *superblock_sector = reinterpret_cast<char *>(&mSuperblock);
		if (!mUtils->Write_To_Disk(superblock_sector, 0, 1)) {
			return false;
		}

		// Init FAT table
		if (!Init_Fat_Table()) {
			cout << "FAT - INIT FAT TABLE ERROR" << endl;
			return false;
		}

		if (!Init_Root()) {
			cout << "FAT - INIT ROOT" << endl;
			return false;
		}

		cout << "FAT - Formating DONE" << endl;

		return true;
	}

	bool CMount::Load_Disk_Params(kiv_hal::TDrive_Parameters &params) {
		kiv_hal::TRegisters regs;

		regs.rax.h = static_cast<decltype(regs.rax.h)>(kiv_hal::NDisk_IO::Drive_Parameters);;
		regs.rdx.l = static_cast<decltype(regs.rdx.l)>(mDisk_Number);
		regs.rdi.r = reinterpret_cast<decltype(regs.rdi.r)>(&params);
		kiv_hal::Call_Interrupt_Handler(kiv_hal::NInterrupt::Disk_IO, regs);

		return (regs.flags.carry == 0);
	}

	bool CMount::Init_Fat_Table() {
		size_t entry_size = sizeof(TFAT_Entry);
		size_t cluster_size = mSuperblock.sectors_per_cluster * mSuperblock.disk_params.bytes_per_sector;
		size_t entries_per_cluster = cluster_size / entry_size;
		size_t clusters_needed = ((mSuperblock.fat_table_number_of_entries % entries_per_cluster) == 0)
			? (mSuperblock.fat_table_number_of_entries / entries_per_cluster)
			: ((mSuperblock.fat_table_number_of_entries / entries_per_cluster) + 1);

		char *buffer = new char[clusters_needed * cluster_size];

		// Init FAT entries (ensure that one entry isn't spreaded over two clusters)
		TFAT_Entry entry = FAT_FREE;
		char *free_entry_casted = reinterpret_cast<char *>(&entry);

		size_t curr_cluster = 0;
		size_t cluster_address, cluster_offset, address;

		for (size_t i = 0; i < mSuperblock.fat_table_number_of_entries; i++) {
			if ((i / entries_per_cluster != 0) && (i % entries_per_cluster == 0)) {
				curr_cluster++;
			}

			cluster_address = curr_cluster * cluster_size;
			cluster_offset = ((i * entry_size) - (curr_cluster * cluster_size));

			address = cluster_address + cluster_offset;
			memcpy(buffer + address, free_entry_casted, entry_size);
		}

		// Write FAT table
		bool write_result = mUtils->Write_Clusters(buffer, mSuperblock.fat_table_first_cluster, clusters_needed);

		delete[] buffer;

		return write_result;
	}

	bool CMount::Init_Root() {
		char *buffer = new char[mSuperblock.sectors_per_cluster * mSuperblock.disk_params.bytes_per_sector];

		uint32_t size = 0;
		memcpy(buffer, &size, sizeof(size));

		bool result = mUtils->Write_Clusters(buffer, mSuperblock.root_cluster, 1);

		delete[] buffer;
		return result;
	}

#pragma endregion

#pragma region Filesystem
	CFile_System::CFile_System() {
		mName = FAT_NAME;
	}

	kiv_vfs::IMounted_File_System *CFile_System::Create_Mount(const std::string label, const kiv_vfs::TDisk_Number disk_number) {
		return new CMount(label, disk_number);
	}
#pragma endregion
}