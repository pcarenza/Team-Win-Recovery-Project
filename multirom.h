#ifndef MULTIROM_H
#define MULTIROM_H

#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>
#include <stdlib.h>
#include <dirent.h>
#include <sys/wait.h>
#include <termios.h>
#include <string>

extern "C" {
#include "common.h"
#include "roots.h"
#include "tw_reboot.h"
#include "minuitwrp/minui.h"
#include "recovery_ui.h"
#include "ddftw.h"
#include "backstore.h"
#include "extra-functions.h"
#include "format.h"
}

#include "gui/objects.hpp"

class MultiROM
{
public:
	static int create_from_current();
	static int deactivate_backup(bool copy);
	static int erase_ask();
	static int erase();
	static int confirmed();
	static int activate();
	static int copy_modules();

private:
	static bool extract_ramdisk();
	static bool copy_folder(char *folder);
	static bool bg_system(char *cmd);
	
	static std::string m_confirm_action;
	
	// From mkbootimg/bootimg.h
#define BOOT_MAGIC "ANDROID!"
#define BOOT_MAGIC_SIZE 8
#define BOOT_NAME_SIZE 16
#define BOOT_ARGS_SIZE 512
	struct boot_img_hdr
	{
		unsigned char magic[BOOT_MAGIC_SIZE];

		unsigned kernel_size;  /* size in bytes */
		unsigned kernel_addr;  /* physical load addr */

		unsigned ramdisk_size; /* size in bytes */
		unsigned ramdisk_addr; /* physical load addr */

		unsigned second_size;  /* size in bytes */
		unsigned second_addr;  /* physical load addr */

		unsigned tags_addr;	/* physical addr for kernel tags */
		unsigned page_size;	/* flash page size we assume */
		unsigned unused[2];	/* future expansion: should be 0 */

		unsigned char name[BOOT_NAME_SIZE]; /* asciiz product name */

		unsigned char cmdline[BOOT_ARGS_SIZE];

		unsigned id[8]; /* timestamp / checksum / sha1 / etc */
	};
};

std::string MultiROM::m_confirm_action = "";

int MultiROM::create_from_current()
{
	DataManager::SetValue("multirom_msg", "Creating ROM...");
	gui_changePage("multirom_progress");
	DataManager::SetValue("ui_progress_frames", 100);
	DataManager::SetValue("ui_progress_portion", 0);

	static const int steps = 6;
	
	ui_print("Mounting DATA, SYSTEM & CACHE...\n");
	__system("mount /data");
	__system("mount /system");
	__system("mount /cache");

	DataManager::SetValue("ui_progress_portion", 1*(100/steps));

	__system("mkdir /sd-ext/multirom/rom");

	if(!extract_ramdisk())
		goto fail;
	DataManager::SetValue("ui_progress_portion", 2*(100/steps));

	if(!copy_folder("cache"))
		goto fail;
	DataManager::SetValue("ui_progress_portion", 3*(100/steps));
	
	if(!copy_folder("data"))
		goto fail;
	DataManager::SetValue("ui_progress_portion", 4*(100/steps));
	
	if(!copy_folder("system"))
		goto fail;
	DataManager::SetValue("ui_progress_portion", 5*(100/steps));

	DataManager::SetValue("ui_progress_portion", 100);

	ui_print("All done\n");

	DataManager::SetValue("multirom_done_title", "ROM successfuly created!");
	DataManager::SetValue("multirom_done_msg", "");
	return gui_changePage("multirom_done");
	
fail:
	__system("rm -r /sd-ext/multirom/rom && sync");
	DataManager::SetValue("multirom_done_title", "Failed to create ROM!");
	DataManager::SetValue("multirom_done_msg", "");
	return gui_changePage("multirom_done");
}

bool MultiROM::extract_ramdisk()
{
	ui_print("Dumping boot img...\n");
	if(__system("dump_image boot /tmp/boot.img") != 0)
	{
		ui_print("Could not dump boot.img!\n");
		return false;
	}

	FILE *boot_img = fopen("/tmp/boot.img", "r");
	FILE *ramdisk = fopen("/tmp/rd.cpio.gz", "w");
	if(!boot_img || !ramdisk)
	{
		ui_print("Could not open boot.img or ramdisk!\n");
		return false;
	}

	// load needed ints
	struct boot_img_hdr header;
	unsigned *start = &header.kernel_size;
	fseek(boot_img, BOOT_MAGIC_SIZE, SEEK_SET); 
	fread(start, 4, 8, boot_img);

	// get ramdisk offset
	unsigned int ramdisk_pos = (1 + ((header.kernel_size + header.page_size - 1) / header.page_size))*2048;
	ui_print("Ramdisk addr %u\nRamdisk size %u\n", ramdisk_pos, header.ramdisk_size);

	// get ramdisk!
	char *buffer = (char*) malloc(header.ramdisk_size);
	fseek(boot_img, ramdisk_pos, SEEK_SET);
	fread(buffer, 1, header.ramdisk_size, boot_img);
	fwrite(buffer, 1, header.ramdisk_size, ramdisk);
	fflush(ramdisk);
	fclose(boot_img);
	fclose(ramdisk);
	free(buffer);

	// extact it...
	ui_print("Extracting init files...\n");
	if(__system("mkdir -p /tmp/boot && cd /tmp/boot && gzip -d -c /tmp/rd.cpio.gz | cpio -i") != 0)
	{
		__system("rm -r /tmp/boot");
		ui_print("Failed to extract boot image!\n");
		return false;
	}

	// copy our files
	__system("mkdir /sd-ext/multirom/rom/boot");
	__system("cp /tmp/boot/*.rc /sd-ext/multirom/rom/boot/");
	__system("cp /tmp/boot/sbin/adbd /sd-ext/multirom/rom/boot/");
	__system("cp /tmp/boot/default.prop /sd-ext/multirom/rom/boot/");

	FILE *init_f = fopen("/tmp/boot/main_init", "r");
	if(init_f)
	{
		fclose(init_f);
		__system("cp /tmp/boot/main_init /sd-ext/multirom/rom/boot/init");
	}
	else __system("cp /tmp/boot/init /sd-ext/multirom/rom/boot/init");

	__system("rm /sd-ext/multirom/rom/boot/preinit.rc");

	sync();

	// and delete temp files
	__system("rm -r /tmp/boot");
	__system("rm /tmp/boot.img");
	__system("rm /tmp/rd.cpio.gz");
	return true;
}

bool MultiROM::copy_folder(char *folder)
{
	ui_print("Copying folder /%s", folder);

	char cmd[100];
	sprintf(cmd, "mkdir /sd-ext/multirom/rom/%s", folder);
	__system(cmd);

	sprintf(cmd, "cp -r -p /%s/* /sd-ext/multirom/rom/%s/ && sync", folder, folder);
	if (!bg_system(cmd))
	{
		ui_print("Failed to copy /%s!\n", folder);
		return false;
	}
	return true;
}

bool MultiROM::bg_system(char *cmd)
{
	pid_t pid = fork();
	if (pid == 0)
	{
		char *args[] = { "/sbin/sh", "-c", cmd, "1>&2", NULL };
		execv("/sbin/sh", args);
		_exit(-1);
	}

	int status;
	uint8_t state = 0; 
	ui_print("|\n");
	while (waitpid(pid, &status, WNOHANG) == 0)
	{
		switch(state++)
		{
			case 4: state = 1; // fallthrough
			case 0: ui_print_overwrite("/"); break;
			case 1: ui_print_overwrite("-"); break;
			case 2: ui_print_overwrite("\\"); break;
			case 3: ui_print_overwrite("|"); break;
		}
		sleep(1);
	}
	ui_print("\n");

	if (!WIFEXITED(status) || (WEXITSTATUS(status) != 0))
		return false;
	return true;
}

int MultiROM::deactivate_backup(bool copy)
{
	DataManager::SetValue("multirom_msg", "Deactivating ROM");
	gui_changePage("multirom_progress");

	ui_print("Deactivating ROM...");

	// Just to make sure it exists
	__system("mkdir /sd-ext/multirom/backup");

	char cmd[512];
	time_t rawtime;
	struct tm *loctm;

	time (&rawtime);
	loctm = localtime(&rawtime);

	sprintf(cmd, "%s /sd-ext/multirom/rom /sd-ext/multirom/backup/rom_%u%02u%02u-%02u%02u && sync", copy ? "cp -r -p" : "mv",
			loctm->tm_year+1900, loctm->tm_mon+1, loctm->tm_mday, loctm->tm_hour, loctm->tm_min);
	
	if(__system(cmd) != 0)
		DataManager::SetValue("multirom_done_title", "Failed to deactivate ROM!");
	else
		DataManager::SetValue("multirom_done_title", "Successfuly deactivated ROM!");

	DataManager::SetValue("multirom_done_msg", "");
	return gui_changePage("multirom_done");
}

int MultiROM::erase_ask()
{
	DataManager::SetValue("multirom_confirm_title", "Erase current ROM?");
	DataManager::SetValue("multirom_confirm_msg", "");
	DataManager::SetValue("multirom_confirm_desc", "Swipe to erase.");

	m_confirm_action = "erase";

	return gui_changePage("multirom_swipe_confirm");
}

int MultiROM::erase()
{
	DataManager::SetValue("multirom_msg", "Erasing ROM");
	gui_changePage("multirom_progress");

	ui_print("Erasing ROM\n\n");
	if(!bg_system("rm -r /sd-ext/multirom/rom && sync"))
		DataManager::SetValue("multirom_done_title", "Failed to erase ROM!");
	else
		DataManager::SetValue("multirom_done_title", "Successfuly erased ROM!");

	DataManager::SetValue("multirom_done_msg", "");
	return gui_changePage("multirom_done");
}

int MultiROM::confirmed()
{
	if(m_confirm_action == "erase")
		return erase();
	return 0;
}

int MultiROM::activate()
{
	std::string backup = DataManager::GetStrValue("multirom_backup");

	if(backup == "")
		return 0;

	DataManager::SetValue("multirom_msg", "Activating ROM");
	gui_changePage("multirom_progress");
	
	ui_print("Activating backup\n\n");
	std::string cmd = "mv " + backup + " /sd-ext/multirom/rom && sync";
	
	if(!bg_system((char*)cmd.c_str()))
		DataManager::SetValue("multirom_done_title", "Failed to activate ROM!");
	else
		DataManager::SetValue("multirom_done_title", "Successfuly activated ROM!");

	DataManager::SetValue("multirom_done_msg", "");
	return gui_changePage("multirom_done");
}

int MultiROM::copy_modules()
{
	DataManager::SetValue("multirom_msg", "Copy kernel modules");
	gui_changePage("multirom_progress");
	
	ui_print("Mounting SYSTEM...\n");
	__system("mount /system");

	ui_print("Copying modules...\n");
	if(!bg_system("cp /system/lib/modules/* /sd-ext/multirom/rom/system/lib/modules/ && sync"))
		DataManager::SetValue("multirom_done_title", "Failed to copy kernel modules!");
	else
		DataManager::SetValue("multirom_done_title", "Successfuly copied modules!");

	DataManager::SetValue("multirom_done_msg", "");
	return gui_changePage("multirom_done");
}

#endif
