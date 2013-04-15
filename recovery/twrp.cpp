/*
        Copyright 2013 bigbiff/Dees_Troy TeamWin
        This file is part of TWRP/TeamWin Recovery Project.

        TWRP is free software: you can redistribute it and/or modify
        it under the terms of the GNU General Public License as published by
        the Free Software Foundation, either version 3 of the License, or
        (at your option) any later version.

        TWRP is distributed in the hope that it will be useful,
        but WITHOUT ANY WARRANTY; without even the implied warranty of
        MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
        GNU General Public License for more details.

        You should have received a copy of the GNU General Public License
        along with TWRP.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "cutils/properties.h"
extern "C" {
#include "minadbd/adb.h"
#include "bootloader.h"
}

#ifdef ANDROID_RB_RESTART
#include "cutils/android_reboot.h"
#else
#include <sys/reboot.h>
#endif

extern "C" {
#include "gui/gui.h"
}
#include "twcommon.h"
#include "twrp-functions.hpp"
#include "data.hpp"
#include "partitions.hpp"
#include "openrecoveryscript.hpp"
#include "variables.h"

TWPartitionManager PartitionManager;
int Log_Offset;
int pause_for_battery_charge = 0;

static void Print_Prop(const char *key, const char *name, void *cookie) {
	printf("%s=%s\n", key, name);
}

static const char *COMMAND_FILE = "/cache/recovery/command";
static const int MAX_ARG_LENGTH = 4096;
static const int MAX_ARGS = 100;

// command line args come from, in decreasing precedence:
//   - the actual command line
//   - the bootloader control block (one per line, after "recovery")
//   - the contents of COMMAND_FILE (one per line)
void get_args(int *argc, char ***argv) {
	struct bootloader_message boot;
	memset(&boot, 0, sizeof(boot));
	TWFunc::get_bootloader_msg(&boot);  // this may fail, leaving a zeroed structure

	if (boot.command[0] != 0 && boot.command[0] != 255) {
        	LOGINFO("Boot command: %.*s\n", sizeof(boot.command), boot.command);
	}

	if (boot.status[0] != 0 && boot.status[0] != 255) {
        	LOGINFO("Boot status: %.*s\n", sizeof(boot.status), boot.status);
	}

	// --- if arguments weren't supplied, look in the bootloader control block
	if (*argc <= 1) {
        	boot.recovery[sizeof(boot.recovery) - 1] = '\0';  // Ensure termination
        	const char *arg = strtok(boot.recovery, "\n");
        	if (arg != NULL && !strcmp(arg, "recovery")) {
			*argv = (char **) malloc(sizeof(char *) * MAX_ARGS);
			(*argv)[0] = strdup(arg);
			for (*argc = 1; *argc < MAX_ARGS; ++*argc) {
				if ((arg = strtok(NULL, "\n")) == NULL) break;
				(*argv)[*argc] = strdup(arg);
			}
			LOGINFO("Got arguments from boot message\n");
        	} else if (boot.recovery[0] != 0 && boot.recovery[0] != 255) {
			LOGERR("Bad boot message\n\"%.20s\"\n", boot.recovery);
        	}
	}

	// --- if that doesn't work, try the command file
	if (*argc <= 1) {
		FILE *fp = fopen(COMMAND_FILE, "r");
		if (fp != NULL) {
			char *argv0 = (*argv)[0];
			*argv = (char **) malloc(sizeof(char *) * MAX_ARGS);
			(*argv)[0] = argv0;  // use the same program name

			char buf[MAX_ARG_LENGTH];
			for (*argc = 1; *argc < MAX_ARGS; ++*argc) {
				if (!fgets(buf, sizeof(buf), fp)) break;
				(*argv)[*argc] = strdup(strtok(buf, "\r\n"));  // Strip newline.
			}

			fflush(fp);
			if (ferror(fp)) LOGERR("Error in %s\n(%s)\n", COMMAND_FILE, strerror(errno));
				fclose(fp);
			LOGINFO("Got arguments from %s\n", COMMAND_FILE);
		}
	}

	// --> write the arguments we have back into the bootloader control block
	// always boot into recovery after this (until finish_recovery() is called)
	strlcpy(boot.command, "boot-recovery", sizeof(boot.command));
	strlcpy(boot.recovery, "recovery\n", sizeof(boot.recovery));
	int i;
	for (i = 1; i < *argc; ++i) {
        	strlcat(boot.recovery, (*argv)[i], sizeof(boot.recovery));
        	strlcat(boot.recovery, "\n", sizeof(boot.recovery));
	}
    	TWFunc::set_bootloader_msg(&boot);
}

int main(int argc, char **argv) {
	// Recovery needs to install world-readable files, so clear umask
	// set by init
	umask(0);

	Log_Offset = 0;

	// Set up temporary log file (/tmp/recovery.log)
	freopen(TMP_LOG_FILE, "a", stdout);
	setbuf(stdout, NULL);
	freopen(TMP_LOG_FILE, "a", stderr);
	setbuf(stderr, NULL);

	// Handle ADB sideload
	if (argc == 3 && strcmp(argv[1], "--adbd") == 0) {
		adb_main(argv[2]);
		return 0;
	}

	time_t StartupTime = time(NULL);
	printf("Starting Extended TWRP %s on %s", TW_VERSION_STR, ctime(&StartupTime));
	// Detect bootloader
	if (DataManager::Detect_BLDR() == 1) {
		printf("I:=> Detected bootloader: cLK\n");
		// If cLK detected, check cmdline for offmode-charging
		pause_for_battery_charge = DataManager::Pause_For_Battery_Charge();			
	} else if (DataManager::Detect_BLDR() == 2) {
		printf("I:=> Detected bootloader: MAGLDR\n");
	} else if (DataManager::Detect_BLDR() == 0) {
		printf("I:=> Detected bootloader: HARET\n");
	}

	// Load default values to set DataManager constants and handle ifdefs
	DataManager::SetDefaultValues();
	printf("Starting the UI...\n");
	gui_init();
	printf("=> Linking mtab\n");
	symlink("/proc/mounts", "/etc/mtab");
	printf("=> Processing recovery.fstab\n");
	if (!PartitionManager.Process_Fstab("/etc/recovery.fstab", 1)) {
		LOGERR("Failing out of recovery due to problem with recovery.fstab.\n");
		return -1;
	}
	DataManager::SetupTwrpFolder();
	// Load up all the resources
	gui_loadResources();
	PartitionManager.Mount_By_Path("/cache", true);

	string Zip_File, Reboot_Value, Restore_File, Partition_Cmd;
	bool Keep_Going = true, Cache_Wipe = false, Factory_Reset = false, Perform_Backup = false, Perform_Restore = false, Finish_SDCard_Partitioning = false;

	{
		get_args(&argc, &argv);

		int index, index2, len;
		char* argptr;
		char* ptr;
		printf("Startup Commands: ");
		for (index = 1; index < argc; index++) {
			argptr = argv[index];
			printf(" '%s'", argv[index]);
			len = strlen(argv[index]);
			if (*argptr == '-') {argptr++; len--;}
			if (*argptr == '-') {argptr++; len--;}
			if (*argptr == 'u') {
				ptr = argptr;
				index2 = 0;
				while (*ptr != '=' && *ptr != '\n')
					ptr++;
				if (*ptr) {
					Zip_File = ptr;
				} else
					LOGERR("argument error specifying zip file\n");
			} else if (*argptr == 'w') {
				if (len == 9)
					Factory_Reset = true;
				else if (len == 10)
					Cache_Wipe = true;
			} else if (*argptr == 'n') {
				Perform_Backup = true;
			} else if (*argptr == 's') {
				ptr = argptr;
				index2 = 0;
				while (*ptr != '=' && *ptr != '\n')
					ptr++;
				if (*ptr) {
					Reboot_Value = *ptr;
				}
			} else if (*argptr == 'r') {
				ptr = argptr;
				index2 = 0;
				while (*ptr != '=' && *ptr != '\n')
					ptr++;
				if (*ptr) {
					Perform_Restore = true;
					Restore_File = *ptr;
				}
			} else if (*argptr == 'p') {
				ptr = argptr;
				index2 = 0;
				while (*ptr != '=' && *ptr != '\n')
					ptr++;
				if (*ptr) {
					Finish_SDCard_Partitioning = true;
					Partition_Cmd = *ptr;
				}
			}
		}
		printf("\n");
	}

	if (Finish_SDCard_Partitioning && !Partition_Cmd.empty())
		PartitionManager.Format_SDCard(Partition_Cmd);

	char twrp_booted[PROPERTY_VALUE_MAX];
	property_get("ro.twrp.boot", twrp_booted, "0");
	if (strcmp(twrp_booted, "0") == 0) {
		property_list(Print_Prop, NULL);
		printf("\n");
		property_set("ro.twrp.boot", "1");
	}
	// Output partitions' details
	PartitionManager.Output_Partition_Logging();

#ifdef TW_INCLUDE_INJECTTWRP
	// Back up TWRP Ramdisk if needed:
	TWPartition* Boot = PartitionManager.Find_Partition_By_Path("/boot");
	string result;
	LOGINFO("Backing up TWRP ramdisk...\n");
	if (Boot == NULL || Boot->Current_File_System != "emmc")
		TWFunc::Exec_Cmd("injecttwrp --backup /tmp/backup_recovery_ramdisk.img", result);
	else {
		string injectcmd = "injecttwrp --backup /tmp/backup_recovery_ramdisk.img bd=" + Boot->Actual_Block_Device;
		TWFunc::Exec_Cmd(injectcmd, result);
	}
	LOGINFO("Backup of TWRP ramdisk done.\n");
#endif

	// Check for and run startup script if script exists
	TWFunc::check_and_run_script("/sbin/runatboot.sh", "boot");

	if (Perform_Backup) {
		DataManager::SetValue(TW_BACKUP_NAME, "(Current Date)");
		if (!OpenRecoveryScript::Insert_ORS_Command("backup BSDCAE\n"))
			Keep_Going = false;
	} else if (Perform_Restore && !Restore_File.empty()) {
		string ORSCommand = "restore " + Restore_File;
		if (!OpenRecoveryScript::Insert_ORS_Command(ORSCommand))
			Keep_Going = false;
	}
	if (Keep_Going && !Zip_File.empty()) {
		string ORSCommand = "install " + Zip_File;
		if (!OpenRecoveryScript::Insert_ORS_Command(ORSCommand))
			Keep_Going = false;
	}
	if (Keep_Going) {
		if (Factory_Reset) {
			if (!OpenRecoveryScript::Insert_ORS_Command("wipe data\n"))
				Keep_Going = false;
		} else if (Cache_Wipe) {
			if (!OpenRecoveryScript::Insert_ORS_Command("wipe cache\n"))
				Keep_Going = false;
		}
	}

	TWFunc::Update_Log_File();
	// Offer to decrypt if the device is encrypted
	if (DataManager::GetIntValue(TW_IS_ENCRYPTED) != 0) {
		LOGINFO("Is encrypted, do decrypt page first\n");
		if (gui_startPage("decrypt") != 0) {
			LOGERR("Failed to start decrypt GUI page.\n");
		}
	}

	// Read the settings file
	DataManager::ReadSettingsFile();
#if 0
	DataManager::DumpValues();
#endif
	// Run any outstanding OpenRecoveryScript
	if (DataManager::GetIntValue(TW_IS_ENCRYPTED) == 0 && (TWFunc::Path_Exists(SCRIPT_FILE_TMP) || TWFunc::Path_Exists(SCRIPT_FILE_CACHE))) {
		OpenRecoveryScript::Run_OpenRecoveryScript();
	}

	// Check for and run 3rd script if script exists
	TWFunc::check_and_run_script("/sbin/postrecoveryboot.sh", "postboot");

	// Launch the main GUI
	gui_start();

	// Check for su to see if the device is rooted or not
	if (PartitionManager.Mount_By_Path("/system", false)) {
		// Disable flashing of stock recovery
		if (TWFunc::Path_Exists("/system/recovery-from-boot.p")) {
			rename("/system/recovery-from-boot.p", "/system/recovery-from-boot.bak");
			gui_print("Renamed stock recovery file in /system to prevent\nthe stock ROM from replacing TWRP.\n");
		}
		if (DataManager::GetIntValue(TW_HANDLE_SU) != 0) {
			LOGINFO("Root checking started...\n");
			if (TWFunc::Path_Exists("/supersu/su") && !TWFunc::Path_Exists("/system/bin/su") && !TWFunc::Path_Exists("/system/xbin/su") && !TWFunc::Path_Exists("/system/bin/.ext/.su")) {
				// Device doesn't have su installed
				DataManager::SetValue("tw_busy", 1);
				if (gui_startPage("installsu") != 0) {
					LOGERR("Failed to start InstallSU GUI page.\n");
				}
			} else if (TWFunc::Check_su_Perms() > 0) {
				// su perms are set incorrectly
				DataManager::SetValue("tw_busy", 1);
				if (gui_startPage("fixsu") != 0) {
					LOGERR("Failed to start FixSU GUI page.\n");
				}
			}
			LOGINFO("Root checking completed.\n");
			sync();
		} else
			LOGINFO("Root checking skipped per user setting.\n");
		PartitionManager.UnMount_By_Path("/system", false);
	}

	// Reboot
	TWFunc::Update_Intent_File(Reboot_Value);
	TWFunc::Update_Log_File();
	gui_print("Rebooting...\n");
	string Reboot_Arg;
	DataManager::GetValue("tw_reboot_arg", Reboot_Arg);
	if (Reboot_Arg == "recovery")
		TWFunc::tw_reboot(rb_recovery);
	else if (Reboot_Arg == "hot")
		TWFunc::tw_reboot(rb_hot);
	else if (Reboot_Arg == "poweroff")
		TWFunc::tw_reboot(rb_poweroff);
	else if (Reboot_Arg == "bootloader")
		TWFunc::tw_reboot(rb_bootloader);
	else if (Reboot_Arg == "download")
		TWFunc::tw_reboot(rb_download);
	else if (Reboot_Arg == "sboot")
		TWFunc::tw_reboot(rb_sboot);
	else if (Reboot_Arg == "tboot")
		TWFunc::tw_reboot(rb_tboot);
	else if (Reboot_Arg == "vboot")
		TWFunc::tw_reboot(rb_vboot);
	else if (Reboot_Arg == "wboot")
		TWFunc::tw_reboot(rb_wboot);
	else if (Reboot_Arg == "xboot")
		TWFunc::tw_reboot(rb_xboot);
	else if (Reboot_Arg == "yboot")
		TWFunc::tw_reboot(rb_yboot);
	else if (Reboot_Arg == "zboot")
		TWFunc::tw_reboot(rb_zboot);
	else
		TWFunc::tw_reboot(rb_system);

#ifdef ANDROID_RB_RESTART
	android_reboot(ANDROID_RB_RESTART, 0, 0);
#else
	reboot(RB_AUTOBOOT);
#endif
    return 0;
}
