/* query_sysenv.c
 *
 * <copyright>
 * Copyright (C) 2014-2017 Sanford Rockowitz <rockowitz@minsoft.com>
 *
 * Licensed under the GNU General Public License Version 2
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 * </endcopyright>
 */

#include <config.h>

#define _GNU_SOURCE 1       // for function group_member

/** \cond */
#include <assert.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <glib-2.0/glib.h>         // glib-2.0/ to avoid bogus eclipse error
#include <grp.h>
#include <limits.h>
// #include <libosinfo-1.0/osinfo/osinfo.h>
// #include <libudev.h>        // not yet used
#include <linux/hiddev.h>
#include <linux/limits.h>
#include <pwd.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>
#ifdef USE_X11
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/extensions/Xrandr.h>
#endif

#include "util/data_structures.h"
#include "util/device_id_util.h"
#include "util/edid.h"
#include "util/file_util.h"
#include "util/report_util.h"
#include "util/string_util.h"
#include "util/subprocess_util.h"
#include "util/sysfs_util.h"
#ifdef PROBE_USING_SYSTEMD
#include "util/systemd_util.h"
#endif
#ifdef USE_X11
#include "util/x11_util.h"
#endif
#include "util/udev_i2c_util.h"
#include "util/udev_usb_util.h"
#include "util/udev_util.h"
/** \endcond */

#include "base/build_info.h"
#include "base/core.h"
#include "base/ddc_errno.h"
#include "base/linux_errno.h"

#include "i2c/i2c_bus_core.h"

#include "ddc/ddc_packet_io.h"

#include "adl/adl_shim.h"

#include "query_drm_sysenv.h"
#include "query_sysenv_xref.h"

#include "query_sysenv.h"


/** Perform redundant checks as cross-verification */
bool redundant_i2c_device_identification_checks = true;



// Linked list of driver names
// struct driver_name_node;
struct driver_name_node {
   char * driver_name;
   struct driver_name_node * next;
};

// Forward reference
static void free_driver_name_list(struct driver_name_node * driver_list);


// Collects information relevant to later tests
typedef struct {
   char * architecture;
   char * distributor_id;
   bool   is_raspbian;
   bool   is_arm;
   Byte_Value_Array i2c_device_numbers;
   struct driver_name_node * driver_list;
} Env_Accumulator;


void free_env_accumulator(Env_Accumulator * accum) {
   if (accum) {
      free(accum->architecture);
      free(accum->distributor_id);
      if (accum->i2c_device_numbers)
         bva_free(accum->i2c_device_numbers);
      if (accum->driver_list)
         free_driver_name_list(accum->driver_list);
      free(accum);
   }
}


static char * known_video_driver_modules[] = {
      "amdgpu",
      "fbdev",
      "fglrx",
      "fturbo",
      "i915",
      "mgag200",
      "nvidia",
      "nouveau",
      "radeon",
      "vboxvideo",
      "vc4",
      NULL
};

static char * prefix_matches[] = {
      "amdgpu",
      "drm",
      "i2c",
      "video",
      "vc4",
      NULL
};

static char * other_driver_modules[] = {
      "drm",
  //  "eeprom",       // not really interesting
      "i2c_algo_bit",
      "i2c_dev",
      "i2c_piix4",
      NULL
};


//
// Get list of /dev/i2c devices
//
// There are too many ways of doing this throughout the code.
// Consolidate them here.  (IN PROGRESS)
//

Byte_Value_Array get_i2c_devices_by_existence_test() {
   Byte_Value_Array bva = bva_create();
   for (int busno=0; busno < I2C_BUS_MAX; busno++) {
      if (i2c_device_exists(busno)) {
         // if (!is_ignorable_i2c_device(busno))
         bva_append(bva, busno);
      }
   }
   return bva;
}


Byte_Value_Array get_i2c_devices_by_ls() {
   Byte_Value_Array bva = bva_create();

   int ival;

   // returns array of I2C bus numbers in string form, sorted in numeric order
   GPtrArray * busnums = execute_shell_cmd_collect("ls /dev/i2c* | cut -c 10- | sort -n");

   if (!busnums) {
      rpt_vstring(1, "No I2C buses found");
      goto bye;
   }
   if (busnums->len > 0) {
      bool isint = str_to_int(g_ptr_array_index(busnums,0), &ival);
      if (!isint) {
         rpt_vstring(1, "Apparently no I2C buses");
         goto bye;
      }
   }
   for (int ndx = 0; ndx < busnums->len; ndx++) {
      char * sval = g_ptr_array_index(busnums, ndx);
      bool isint = str_to_int(sval, &ival);
      if (!isint) {
         rpt_vstring(1, "Parsing error.  Invalid I2C bus number: %s", sval);
      }
      else {
         bva_append(bva, ival);
         // is_smbus_device_using_sysfs(ival);
      }
   }
bye:
   if (busnums)
      g_ptr_array_free(busnums, true);

   return bva;
}


/** Consolidated function to identify I2C devices.
 *
 *  \return #ByteValueArray of bus numbers for detected I2C devices
 */
Byte_Value_Array identify_i2c_devices() {

   Byte_Value_Array i2c_device_numbers_result = NULL;   // result

   Byte_Value_Array bva1 = NULL;
   Byte_Value_Array bva2 = NULL;
   Byte_Value_Array bva3 = NULL;

   bva1 = get_i2c_devices_by_existence_test();
   if (redundant_i2c_device_identification_checks) {
      bva2 = get_i2c_devices_by_ls();
      bva3 = get_i2c_device_numbers_using_udev(/* include_smbus= */ true);

      assert(bva_sorted_eq(bva1,bva2));
      assert(bva_sorted_eq(bva1,bva3));
   }

   i2c_device_numbers_result = bva1;
   if (redundant_i2c_device_identification_checks) {
      bva_free(bva2);
      bva_free(bva3);
   }
   // DBGMSG("Identified %d I2C devices", bva_length(bva1));
   return i2c_device_numbers_result;
}


//
// Utilities
//

static ushort h2ushort(char * hval) {
   bool debug = false;
   int ct;
   ushort ival;
   ct = sscanf(hval, "%hx", &ival);
   assert(ct == 1);
   if (debug)
      DBGMSG("hhhh = |%s|, returning 0x%04x", hval, ival);
   return ival;
}


static void report_file_first_line(char * fn, char * title, int depth) {
   int d1 = depth+1;
   if (title)
      rpt_title(title, depth);
   else
      rpt_vstring(depth, "%s:", fn);

   char * s = file_get_first_line(fn, true /* verbose */);
   if (s)
      rpt_title(s, d1);
   else
      rpt_vstring(d1, "Unable to read %s", fn);
}


static bool show_one_file(char * dir_name, char * simple_fn, bool verbose, int depth) {
   bool result = false;
   char fqfn[PATH_MAX+2];
   strcpy(fqfn,dir_name);
   if (!str_ends_with(dir_name, "/"))
      strcat(fqfn,"/");
   assert(strlen(fqfn) + strlen(simple_fn) <= PATH_MAX);   // for Coverity
   strncat(fqfn,simple_fn, sizeof(fqfn)-(strlen(fqfn)+1));  // use strncat to make Coverity happy
   if (regular_file_exists(fqfn)) {
      rpt_vstring(depth, "%s:", fqfn);
      rpt_file_contents(fqfn, /*verbose=*/true, depth+1);
      result = true;
   }
   else if (verbose)
      rpt_vstring(depth, "File not found: %s", fqfn);
   return result;
}


/** Gets the sysfs name of an I2C device,
 *  i.e. the value of /sys/bus/in2c/devices/i2c-n/name
 *
 *  \param  busno   I2C bus number
 *  \return newly allocated string containing attribute value,
 *          NULL if not found
 *
 *  \remark
 *  Caller is responsible for freeing returned value
 */
static char * get_i2c_device_sysfs_name(int busno) {
   char workbuf[50];
   snprintf(workbuf, 50, "/sys/bus/i2c/devices/i2c-%d/name", busno);
   char * name = file_get_first_line(workbuf, /*verbose */ false);
   // DBGMSG("busno=%d, returning: %s", busno, bool_repr(result));
   return name;
}

#ifdef UNUSED
static bool is_smbus_device_using_sysfs(int busno) {
#ifdef OLD
   char workbuf[50];
   snprintf(workbuf, 50, "/sys/bus/i2c/devices/i2c-%d/name", busno);
   char * name = file_get_first_line(workbuf, /*verbose */ false);
#endif
   char * name = get_i2c_device_sysfs_name(busno);

   bool result = false;
   if (name && str_starts_with(name, "SMBus"))
      result = true;
   free(name);
   // DBGMSG("busno=%d, returning: %s", busno, bool_repr(result));
   return result;
}
#endif

/** Checks if an I2C bus cannot be a DDC/CI connected monitor
 *  and therefore can be ignored, e.g. if it is an SMBus device.
 *
 *  \param  busno  I2C bus number
 *  \return true if ignorable, false if not
 *
 *  \remark
 *  This function avoids unnecessary calls to i2cdetect, which can be
 *  slow for SMBus devices and fills the system logs with errors
 */
static bool is_ignorable_i2c_device(int busno) {
   bool result = false;
   char * name = get_i2c_device_sysfs_name(busno);
   if (name) {
      if (str_starts_with(name, "SMBus"))
         result = true;
      else if (streq(name, "soc:i2cdsi"))     // Raspberry Pi
         result = true;
      free(name);
   }
   return result;
}


// Functions to query and free the linked list of driver names.
// The list is created by executing function query_card_and_driver_using_sysfs(),
// which is grouped with the sysfs functions.

/** Frees the driver name list created by query_card_and_driver_using_sysfs()
 *
 * \param driver_list     pointer to head of linked list of driver names
 */
static void free_driver_name_list(struct driver_name_node * driver_list) {
   // Free the driver list
   struct driver_name_node * cur_node = driver_list;
   while (cur_node) {
      struct driver_name_node * next_node = cur_node->next;
      free(cur_node);
      cur_node = next_node;
   }
}


/** Checks the list of detected drivers to see if AMD's proprietary
 * driver fglrx is the only driver.
 *
 * \param  driver_list     linked list of driver names
 * \return true if fglrx is the only driver, false otherwise
 */
bool only_fglrx(struct driver_name_node * driver_list) {
   int driverct = 0;
   bool fglrx_seen = false;
   struct driver_name_node * curnode = driver_list;
   while (curnode) {
      driverct++;
      if (str_starts_with(curnode->driver_name, "fglrx"))
         fglrx_seen = true;
      curnode = curnode->next;
   }
   bool result = (driverct == 1 && fglrx_seen);
   // DBGMSG("driverct = %d, returning %d", driverct, result);
   return result;
}


/** Checks the list of detected drivers to see if the proprietary
 *  AMD and Nvidia drivers are the only ones.
 *
 * \param  driver list        linked list of driver names
 * \return true  if both nvidia and fglrx are present and there are no other drivers,
 *         false otherwise
 */
static bool only_nvidia_or_fglrx(struct driver_name_node * driver_list) {
   int driverct = 0;
   bool other_driver_seen = false;
   struct driver_name_node * curnode = driver_list;
   while (curnode) {
      driverct++;
      if (!str_starts_with(curnode->driver_name, "fglrx") &&
          !streq(curnode->driver_name, "nvidia")
         )
      {
         other_driver_seen = true;
      }
      curnode = curnode->next;
   }
   bool result = (!other_driver_seen && driverct > 0);
   // DBGMSG("driverct = %d, returning %d", driverct, result);
   return result;
}


/** Checks if any driver name in the list of detected drivers starts with
 * the specified string.
 *
 *  \param  driver list     linked list of driver names
 *  \parar  driver_prefix   driver name prefix
 *  \return true if found, false if not
 */
static bool found_driver(struct driver_name_node * driver_list, char * driver_prefix) {
   bool found = false;
   struct driver_name_node * curnode = driver_list;
   while (curnode) {
      if ( str_starts_with(curnode->driver_name, driver_prefix) ) {
         found = true;
         break;
      }
      curnode = curnode->next;
   }
   // DBGMSG("driver_name=%s, returning %d", driver_prefix, found);
   return found;
}


//
// dmidecode related functions
//

// from dmidecode.c
static const char *dmi_chassis_type(Byte code)
{
   /* 7.4.1 */
   static const char *type[] = {
      "Other", /* 0x01 */
      "Unknown",
      "Desktop",
      "Low Profile Desktop",
      "Pizza Box",
      "Mini Tower",
      "Tower",
      "Portable",
      "Laptop",
      "Notebook",
      "Hand Held",
      "Docking Station",
      "All In One",
      "Sub Notebook",
      "Space-saving",
      "Lunch Box",
      "Main Server Chassis", /* CIM_Chassis.ChassisPackageType says "Main System Chassis" */
      "Expansion Chassis",
      "Sub Chassis",
      "Bus Expansion Chassis",
      "Peripheral Chassis",
      "RAID Chassis",
      "Rack Mount Chassis",
      "Sealed-case PC",
      "Multi-system",
      "CompactPCI",
      "AdvancedTCA",
      "Blade",
      "Blade Enclosing",
      "Tablet",
      "Convertible",
      "Detachable",
      "IoT Gateway",
      "Embedded PC",
      "Mini PC",
      "Stick PC" /* 0x24 */
   };

   code &= 0x7F; /* bits 6:0 are chassis type, 7th bit is the lock bit */

   if (code >= 0x01 && code <= 0x24)
      return type[code - 0x01];
   return NULL;
}


#ifdef UNUSED_UGLY
void report_dmidecode_string(char * s, int depth) {
   char cmd[100];
   strcpy(cmd, "dmidecode -s ");
   strcat(cmd, s);
   rpt_vstring(depth, "%s:", s);
   execute_shell_cmd_rpt(cmd, depth+1);
}


void report_dmicode_group(char * header, int depth) {
   char cmd[100];
   snprintf(cmd, 100, "dmidecode | grep '%s' -A2", header);
   // DBGMSG("cmd: |%s|", cmd);
   GPtrArray * lines = execute_shell_cmd_collect(cmd);
   if (lines) {
      for (int ndx = 0; ndx < lines->len; ndx++) {
         char * s = g_ptr_array_index(lines, ndx);
         rpt_title(s, depth);
      }
      g_ptr_array_free(lines,true);
   }
   else
      rpt_vstring(depth, "Command failed: %s", cmd);
}
#endif


/** Reports DMI information for the system.
 */
static void query_dmidecode() {

#ifdef NO
   // leave in for testing
   rpt_nl();
   if (test_command_executability("dmidecode") == 0) {
      rpt_vstring(0, "System information from dmidecode:");

#ifdef NO_UGLY
      report_dmidecode_string("baseboard-manufacturer", 1);
      report_dmidecode_string("baseboard-product-name", 1);
      report_dmidecode_string("system-manufacturer", 1);
      report_dmidecode_string("system-product-name", 1);
      report_dmidecode_string("chassis-manufacturer", 1);
      report_dmidecode_string("chassis-type", 1);
#endif

      report_dmicode_group("Base Board Info", 1);
      report_dmicode_group("System Info", 1);
      report_dmicode_group("Chassis Info", 1);
   }
   else
      rpt_vstring(0, "dmidecode command unavailable");
#endif

   char * sysdir = "/sys/class/dmi/id";
   // better way, doesn't require privileged dmidecode
   // rpt_nl();
   rpt_title("DMI Information from /sys/class/dmi/id:", 0);

   char * dv = "(Unavailable)";
   char buf[100];
   int bufsz = 100;

   //    verbpse
   rpt_vstring(1, "%-25s %s","Motherboard vendor:",       read_sysfs_attr_w_default_r(sysdir, "board_vendor",  dv, buf, bufsz, false));
   rpt_vstring(1, "%-25s %s","Motherboard product name:", read_sysfs_attr_w_default_r(sysdir, "board_name",    dv, buf, bufsz, false));
   rpt_vstring(1, "%-25s %s","System vendor:",            read_sysfs_attr_w_default_r(sysdir, "sys_vendor",    dv, buf, bufsz, false));
   rpt_vstring(1, "%-25s %s","System product name:",      read_sysfs_attr_w_default_r(sysdir, "product_name",  dv, buf, bufsz, false));
   rpt_vstring(1, "%-25s %s","Chassis vendor:",           read_sysfs_attr_w_default_r(sysdir, "chassis_vendor",dv, buf, bufsz, false));

   char * chassis_type_s = read_sysfs_attr(sysdir, "chassis_type", /*verbose=*/ true);
   char * chassis_desc = dv;
   char workbuf[100];
   if (chassis_type_s) {
      int chassis_type_i = atoi(chassis_type_s);   // TODO: use something safer?
      const char * chassis_type_name = dmi_chassis_type(chassis_type_i);
      if (chassis_type_name)
         snprintf(workbuf, 100, "%s - %s", chassis_type_s, chassis_type_name);
      else
         snprintf(workbuf, 100, "%s - Unrecognized value", chassis_type_s);
      chassis_desc = workbuf;
   }
   rpt_vstring(1, "%-25s %s", "Chassis type:", chassis_desc);

   // Note: The alternative of calling execute_shell_cmd_collect() with the following
   // command fails if executing from a non-privileged account, which lacks permissions
   // for /dev/mem or /sys/firmware/dmi/tables/smbios_entry_point
   // char * cmd =    "dmidecode | grep \"['Base Board Info'|'Chassis Info'|'System Info']\" -A2";
   // GPtrArray * lines = execute_shell_cmd_collect(cmd);

}


/** Compile time and runtime checks of endianness.
 *
 *  \param depth logical indentation depth
 */
static void report_endian(int depth) {
   int d1 = depth+1;
   rpt_title("Byte order checks:", depth);

   bool is_bigendian = (*(uint16_t *)"\0\xff" < 0x100);
   rpt_vstring(d1, "Is big endian (local test):       %s", bool_repr(is_bigendian));

   rpt_vstring(d1, "WORDS_BIGENDIAN macro (autoconf): "
#ifdef WORDS_BIGENDIAN
         "defined"
#else
         "not defined"
#endif
         );
   rpt_vstring(d1, "__BYTE_ORDER__ macro (gcc):       "
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
         "__ORDER_LITTLE_ENDIAN__"
#elif __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
         "__ORDER_BIG_ENDIAN__"
#elif __BYTE_ORDER__ == __ORDER_PDP_ENDIAN__
         "__ORDER_PDP_ENDIAN__"
#else
         "unexpected value"
#endif
         );

#ifdef REDUNDANT
   __u32 i = 1;
   bool is_bigendian2 =  ( (*(char*)&i) == 0 );
   rpt_vstring(d1, "Is big endian (runtime test): %s", bool_repr(is_bigendian2));
#endif
}


//
// Higher level functions
//

/** Reports basic system information
 *
 * \param  accum  pointer to struct in which information is returned
 */
static void query_base_env(Env_Accumulator * accum) {
   rpt_vstring(0, "ddcutil version: %s", BUILD_VERSION);
   rpt_nl();

   report_file_first_line("/proc/version", NULL, 0);

   char * expected_architectures[] = {"x86_64", "i386", "armv7l", NULL};
   char * architecture   = execute_shell_cmd_one_line_result("arch");      // alt: use uname -m
   char * distributor_id = execute_shell_cmd_one_line_result("lsb_release -s -i");  // e.g. Ubuntu, Raspbian
   char * release        = execute_shell_cmd_one_line_result("lsb_release -s -r");
   rpt_nl();
   rpt_vstring(0, "Architecture:     %s", architecture);
   rpt_vstring(0, "Distributor id:   %s", distributor_id);
   rpt_vstring(0, "Release:          %s", release);

   if ( ntsa_find(expected_architectures, architecture) >= 0) {
      rpt_vstring(0, "Found a known architecture");
   }
   else {
      rpt_vstring(0, "Unexpected architecture %s.  Please report.", architecture);
   }

   accum->architecture   = architecture;
   accum->distributor_id = distributor_id;
   accum->is_raspbian    = streq(accum->distributor_id, "Raspbian");
   accum->is_arm         = str_starts_with(accum->architecture, "arm");
   free(release);

#ifdef REDUNDANT
   rpt_nl();
   rpt_vstring(0,"/etc/os-release...");
   bool ok = execute_shell_cmd_rpt("grep PRETTY_NAME /etc/os-release", 1 /* depth */);
   if (!ok)
      rpt_vstring(1,"Unable to read PRETTY_NAME from /etc/os-release");
#endif

   rpt_nl();
   report_file_first_line("/proc/cmdline", NULL, 0);

   if (get_output_level() >= DDCA_OL_VERBOSE) {
      rpt_nl();
      rpt_vstring(0,"Processor information as reported by lscpu:");
        bool ok = execute_shell_cmd_rpt("lscpu", 1);
        if (!ok) {   // lscpu should always be there, but just in case:
           rpt_vstring(1, "Command lscpu not found");
           rpt_nl();
           rpt_title("Processor information from /proc/cpuinfo:", 0);
           execute_shell_cmd_rpt( "cat /proc/cpuinfo | grep vendor_id | uniq", 1);
           execute_shell_cmd_rpt( "cat /proc/cpuinfo | grep \"cpu family\" | uniq", 1);
           execute_shell_cmd_rpt( "cat /proc/cpuinfo | grep \"model[[:space:]][[:space:]]\" | uniq",  1);   //  "model"
           execute_shell_cmd_rpt( "cat /proc/cpuinfo | grep \"model name\" | uniq",  1);   // "model name"
       }

       rpt_nl();
        if (accum->is_arm) {
           rpt_vstring(0, "Skipping dmidecode checks on architecture %s.", accum->architecture);
        }
        else {
           query_dmidecode();
        }

      rpt_nl();
      report_endian(0);
   }

}


/** Scans /proc/modules for information on loaded drivers of interest
 */
static int query_proc_modules_for_video() {
   bool debug = false;
   DBGMSF(debug, "Starting.");

   int rc = 0;

   GPtrArray * garray = g_ptr_array_sized_new(300);

   rpt_vstring(0,"Scanning /proc/modules for driver environment...");
   int ct = file_getlines("/proc/modules", garray, true);
   if (ct < 0)
      rc = ct;
   else {
      int ndx = 0;
      for (ndx=0; ndx<garray->len; ndx++) {
         char * curline = g_ptr_array_index(garray, ndx);
         char mod_name[32];
         int  mod_size;
         int  mod_instance_ct;
         char mod_dependencies[500];
         char mod_load_state[10];     // one of: Live Loading Unloading
         char mod_addr[30];
         int piece_ct = sscanf(curline, "%s %d %d %s %s %s",
                               mod_name,
                               &mod_size,
                               &mod_instance_ct,
                               mod_dependencies,
                               mod_load_state,
                               mod_addr);
         if (piece_ct != 6) {
            DBGMSG("Unexpected error parsing /proc/modules.  sscanf returned %d", piece_ct);
         }
         if (streq(mod_name, "drm") ) {
            rpt_vstring(0,"   Loaded drm module depends on: %s", mod_dependencies);
         }
         else if (streq(mod_name, "video") ) {
            rpt_vstring(0,"   Loaded video module depends on: %s", mod_dependencies);
         }
         else if (exactly_matches_any(mod_name, known_video_driver_modules) >= 0 ) {
            rpt_vstring(0,"   Found video driver module: %s", mod_name);
         }
         else if ( starts_with_any(mod_name, prefix_matches) >= 0 ) {
            rpt_vstring(0,"   Found other loaded module: %s", mod_name);
         }
      }
   }

   DBGMSF(debug, "Done.");
   return rc;
}


/** Reports nvidia proprietary driver information by examining
 *  /proc/driver/nvidia.
 */
static bool query_proc_driver_nvidia() {
   bool debug = false;
   bool result = false;
   char * dn = "/proc/driver/nvidia/";
   if ( directory_exists(dn) ) {
      rpt_vstring(0,"Examining /proc/driver/nvidia:");
      result = true;
      show_one_file(dn, "version",  debug, 1);
      show_one_file(dn, "registry", debug, 1);
      show_one_file(dn, "params",   debug, 1);
      char * dn_gpus = "/proc/driver/nvidia/gpus/";
      if (directory_exists(dn_gpus)) {
         DIR * dp = opendir(dn_gpus);
         struct dirent * ep;

         while ( (ep = readdir(dp)) ) {
            if ( !streq(ep->d_name,".") && !streq(ep->d_name, "..") ) {
               rpt_vstring(1, "PCI bus id: %s", ep->d_name);
               char dirbuf[400];
               strcpy(dirbuf, dn_gpus);
               strcat(dirbuf, ep->d_name);
               // printf("Reading directory: %s\n", dirbuf);
               // DIR * dp2 = opendir(dirbuf);
               // if (dp2) {
               //    struct dirent * ep2;
               //    printf("GPU: %s\n", ep->d_name);
               //    while ( (ep2 = readdir(dp2)) ) {
               //       if ( !streq(ep2->d_name,".") && !streq(ep2->d_name, "..") ) {
               //          puts(ep2->d_name);
               //       }
               //    }
               //    closedir(dp2);
               // }
               if ( directory_exists(dirbuf)) {
                  show_one_file(dirbuf, "information", debug, 1);
                  show_one_file(dirbuf, "registry",    debug, 1);
               }
            }
         }

         closedir(dp);
      }
   }
   else {
       DBGMSF(debug, "Nvidia driver directory %s not found\n", dn);
   }
   return result;
}


// Auxiliary function for raw_scan_i2c_devices()
bool is_i2c_device_rw(int busno) {
   bool debug = false;
   DBGMSF(debug, "Starting. busno=%d", busno);

   bool result = true;

   char fnbuf[PATH_MAX];
   snprintf(fnbuf, sizeof(fnbuf), "/dev/i2c-%d", busno);

   int rc;
   int errsv;
   DBGMSF(debug, "Calling access() for %s", fnbuf);
   rc = access(fnbuf, R_OK|W_OK);
   if (rc < 0) {
      errsv = errno;
      rpt_vstring(0,"Device %s is not readable and writable.  Error = %s",
             fnbuf, linux_errno_desc(errsv) );
      result = false;
   }

   DBGMSF(debug, "Returning: %s", bool_repr(result));
   return result;
}



// Auxiliary function for raw_scan_i2c_devices()
// adapted from ddc_vcp_tests

Public_Status_Code try_single_getvcp_call(
      int           fh,
      unsigned char vcp_feature_code,
      int           depth)
{
   bool debug = false;
   DBGMSF(debug, "Starting. vcp_feature_code=0x%02x", vcp_feature_code );

   int ndx;
   Status_Errno rc = 0;

   // extra sleep time does not help P2411

#ifdef NO
   usleep(50000);   // doesn't help
   // usleep(50000);
   // write seems to be necessary to reset monitor state
   unsigned char zeroByte = 0x00;  // 0x00;
   rc = write(fh, &zeroByte, 1);
   if (rc < 0) {
      rpt_vstring(0,"(%s) Bus reset failed. rc=%d, errno=%d. ", __func__, rc, errno );
      return -1;
   }
#endif
   // without this or 0 byte write, read() sometimes returns all 0 on P2411H
   usleep(50000);
   // usleep(50000);

   unsigned char ddc_cmd_bytes[] = {
      0x6e,              // address 0x37, shifted left 1 bit
      0x51,              // source address
      0x02 | 0x80,       // number of DDC data bytes, with high bit set
      0x01,              // DDC Get Feature Command
      vcp_feature_code,  //
      0x00,              // checksum, to be set
   };

   // calculate checksum by XORing bytes 0..4
   ddc_cmd_bytes[5] = ddc_cmd_bytes[0];
   for (ndx=1; ndx < 5; ndx++)
      ddc_cmd_bytes[5] ^= ddc_cmd_bytes[ndx];    // calculate checksum

   int writect = sizeof(ddc_cmd_bytes)-1;
   rc = write(fh, ddc_cmd_bytes+1, writect);
   if (rc < 0) {
      int errsv = errno;
      DBGMSF(debug, "write() failed, errno=%s", linux_errno_desc(errsv));
      rc = -errsv;
      goto bye;
   }
   if (rc != writect) {
      DBGMSF(debug, "write() returned %d, expected %d", rc, writect );
      rc = DDCRC_BAD_BYTECT;
      goto bye;
   }
   usleep(50000);
   // usleep(50000);

   unsigned char ddc_response_bytes[12];
   int readct = sizeof(ddc_response_bytes)-1;

   rc = read(fh, ddc_response_bytes+1, readct);
   if (rc < 0) {
      // printf("(%s) read() returned %d, errno=%d.\n", __func__, rc, errno );
      int errsv = errno;
      DBGMSF(debug, "read() failed, errno=%s", linux_errno_desc(errsv));
      rc = -errsv;
      goto bye;
   }

   char * hs = hexstring(ddc_response_bytes+1, rc);
   rpt_vstring(depth, "read() returned %s", hs );
   free(hs);

   if (rc != readct) {
      DBGMSF(debug, "read() returned %d, should be %d", rc, readct );
      rc = DDCRC_BAD_BYTECT;
      goto bye;
   }

   // printf("(%s) read() returned %s\n", __func__, hexstring(ddc_response_bytes+1, readct) );
   if (debug) {
      char * hs = hexstring(ddc_response_bytes+1, readct);
      DBGMSF(debug, "read() returned %s", hs );
      free(hs);
      // hex_dump(ddc_response_bytes,1+rc);
   }

   if ( all_bytes_zero( ddc_response_bytes+1, readct) ) {
      DBGMSF(debug, "All bytes zero");
      rc = DDCRC_READ_ALL_ZERO;
      goto bye;
   }

   int ddc_data_length = ddc_response_bytes[2] & 0x7f;
   // some monitors return a DDC null response to indicate an invalid request:
   if (ddc_response_bytes[1] == 0x6e &&
       ddc_data_length == 0          &&
       ddc_response_bytes[3] == 0xbe)     // 0xbe == checksum
   {
      DBGMSF(debug, "Received DDC null response");
      rc = DDCRC_NULL_RESPONSE;
      goto bye;
   }

   if (ddc_response_bytes[1] != 0x6e) {
      // assert(ddc_response_bytes[1] == 0x6e);
      DBGMSF(debug, "Invalid address byte in response, expected 06e, actual 0x%02x",
                    ddc_response_bytes[1] );
      rc = DDCRC_INVALID_DATA;
      goto bye;
   }

   if (ddc_data_length != 8) {
      DBGMSF(debug, "Invalid query VCP response length: %d", ddc_data_length );
      rc = DDCRC_BAD_BYTECT;
      goto bye;
   }

   if (ddc_response_bytes[3] != 0x02) {       // get feature response
      DBGMSF(debug, "Expected 0x02 in feature response field, actual value 0x%02x",
                    ddc_response_bytes[3] );
      rc = DDCRC_INVALID_DATA;
      goto bye;
   }

   ddc_response_bytes[0] = 0x50;   // for calculating DDC checksum
   // checksum0 = xor_bytes(ddc_response_bytes, sizeof(ddc_response_bytes)-1);
   unsigned char calculated_checksum = ddc_response_bytes[0];
   for (ndx=1; ndx < 11; ndx++)
      calculated_checksum ^= ddc_response_bytes[ndx];
   // printf("(%s) checksum0=0x%02x, calculated_checksum=0x%02x\n", __func__, checksum0, calculated_checksum );
   if (ddc_response_bytes[11] != calculated_checksum) {
      DBGMSF(debug, "Unexpected checksum.  actual=0x%02x, calculated=0x%02x",
             ddc_response_bytes[11], calculated_checksum );
      rc = DDCRC_CHECKSUM;
      goto bye;
   }

   if (ddc_response_bytes[4] == 0x00) {         // valid VCP code
      // The interpretation for most VCP codes:
      int max_val = (ddc_response_bytes[7] << 8) + ddc_response_bytes[8];
      int cur_val = (ddc_response_bytes[9] << 8) + ddc_response_bytes[10];
      DBGMSF(debug, "cur_val = %d, max_val = %d", cur_val, max_val );
      rc = 0;
   }
   else if (ddc_response_bytes[4] == 0x01) {    // unsupported VCP code
      DBGMSF(debug, "Unsupported VCP code: 0x%02x", vcp_feature_code);
      rc = DDCRC_REPORTED_UNSUPPORTED;
   }
   else {
      DBGMSF(debug, "Unexpected value in supported VCP code field: 0x%02x  ",
                    ddc_response_bytes[4] );
      rc = DDCRC_INVALID_DATA;
   }

bye:
   DBGMSF(debug, "Returning: %s",  psc_desc(rc));
   return rc;
}


/** Checks each I2C device.
 *
 * This function largely uses direct coding to probe the I2C buses.
 * Allows for trying to read x37 even if X50 fails, and provides
 * clearer diagnostic messages than relying entirely on normal code
 * path.
 */
void raw_scan_i2c_devices() {
   bool debug = false;
   DBGMSF(debug, "Starting");

   int depth = 0;
   int d1 = depth+1;
   int d2 = depth+2;
   Parsed_Edid * edid = NULL;

   rpt_nl();
   rpt_title("Performing basic scan of I2C devices using local sysenv functions...",depth);

   Buffer * buf0 = buffer_new(1000, __func__);
   int  busct = 0;
   Public_Status_Code psc;
   Status_Errno rc;
   bool saved_i2c_force_slave_addr_flag = i2c_force_slave_addr_flag;

   for (int busno=0; busno < I2C_BUS_MAX; busno++) {
      if (i2c_device_exists(busno)) {
         busct++;
         rpt_nl();
         rpt_vstring(d1, "Examining device /dev/i2c-%d...", busno);

         if (is_ignorable_i2c_device(busno))
            continue;

         if (!is_i2c_device_rw(busno))   // issues message if not RW
            continue;

         int fd = i2c_open_bus(busno, CALLOPT_ERR_MSG);
         if (fd < 0)
            continue;

         // DBGMSG("Calling i2c_get_functionality_flags_by_fd()");
         unsigned long functionality = i2c_get_functionality_flags_by_fd(fd);
         // DBGMSG("i2c_get_functionality_flags_by_fd() returned %ul", functionality);
         i2c_report_functionality_flags(functionality, 90, d2);

         //  Base_Status_Errno rc = i2c_set_addr(fd, 0x50, CALLOPT_ERR_MSG);
         // TODO save force slave addr setting, set it for duration of call - do it outside loop
         psc = i2c_get_raw_edid_by_fd(fd, buf0);
         if (psc != 0) {
            rpt_vstring(d2, "Unable to read EDID, psc=%s", psc_desc(psc));
         }
         else {
            rpt_vstring(d2, "Raw EDID:");
            rpt_hex_dump(buf0->bytes, buf0->len, d2);
            edid = create_parsed_edid(buf0->bytes);
            if (edid)
              report_parsed_edid_base(
                    edid,
                    true,     // verbose
                    false,    // show_edid
                    d2);
            else
               rpt_vstring(d2, "Unable to parse EDID");

            Device_Id_Xref * xref = device_xref_get(buf0->bytes);
            xref->i2c_busno = busno;
         }

         rpt_nl();
         rpt_vstring(d2, "Trying simple VCP read of feature 0x10...");
         rc = i2c_set_addr(fd, 0x37, CALLOPT_ERR_MSG);
         if (rc == 0) {
            int maxtries = 3;
            psc = -1;
            for (int tryctr=0; tryctr<maxtries && psc < 0; tryctr++) {
               psc = try_single_getvcp_call(fd, 0x10, d2);
               if (psc == 0 || psc == DDCRC_NULL_RESPONSE || psc == DDCRC_REPORTED_UNSUPPORTED) {
                  switch (psc) {
                  case 0:
                     rpt_vstring(d2, "Attempt %d to read feature succeeded.", tryctr+1);
                     break;
                  case DDCRC_REPORTED_UNSUPPORTED:
                     rpt_vstring(d2, "Attempt %d to read feature returned DDCRC_REPORTED_UNSUPPORTED", tryctr+1);
                     psc = 0;
                     break;
                  case DDCRC_NULL_RESPONSE:
                     rpt_vstring(d2, "Attempt %d to read feature returned DDCRC_NULL_RESPONSE", tryctr+1);
                     break;
                  }
                  break;
               }
               if (get_modulation(psc) == RR_ERRNO) {    // also RR_ADL?
                  rpt_vstring(d2, "Attempt %d to read feature returned hard error: %s", tryctr+1, psc_desc(psc));
                  break;
               }
               rpt_vstring(d2, "Attempt %d to read feature failed. status = %s.  %s",
                             tryctr+1, psc_desc(psc), (tryctr < maxtries-1) ? "Retrying..." : "");
            }
            if (psc == 0)
               rpt_vstring(d2, "DDC communication succeeded");
            else {
               rpt_vstring(d2, "DDC communication failed.");
               if (edid)
                  rpt_vstring(d2, "Is DDC/CI enabled in the monitor's on-screen display?");
            }
         }

         if (edid) {
            free_parsed_edid(edid);
            edid = NULL;
         }
         i2c_close_bus(fd, busno, CALLOPT_ERR_MSG);
      }
   }

   if (busct == 0) {
      rpt_vstring(d2, "No /dev/i2c-* devices found\n");
   }

   i2c_force_slave_addr_flag = saved_i2c_force_slave_addr_flag;
   buffer_free(buf0, __func__);

   DBGMSF(debug, "Done" );
}


/** Checks on the existence and accessibility of /dev/i2c devices.
 *
 *  \param driver_list singly linked list of names of video drivers previously detected
 *
 * Checks that user has RW access to all /dev/i2c devices.
 * Checks if group i2c exists and whether the current user is a member.
 * Checks for references to i2c in /etc/udev/makedev.d
 *
 * If the only driver in driver_list is fglrx, the tests are
 * skipped (or if verbose output, purely informational).
 *
 * TODO: ignore i2c smbus devices
 */
static void check_i2c_devices(struct driver_name_node * driver_list) {
   bool debug = false;
   // int rc;
   // char username[32+1];       // per man useradd, max username length is 32
   char *uname = NULL;
   // bool have_i2c_devices = false;

   rpt_vstring(0,"Checking /dev/i2c-* devices...");
   DDCA_Output_Level output_level = get_output_level();

   bool just_fglrx = only_fglrx(driver_list);
   if (just_fglrx){
      rpt_nl();
      rpt_vstring(0,"Apparently using only the AMD proprietary driver fglrx.");
      rpt_vstring(0,"Devices /dev/i2c-* are not required.");
      if (output_level < DDCA_OL_VERBOSE)
         return;
      rpt_vstring(0, "/dev/i2c device detail is purely informational.");
   }

   rpt_nl();
   rpt_multiline(0,
          "Unless the system is using the AMD proprietary driver fglrx, devices /dev/i2c-*",
          "must exist and the logged on user must have read/write permission for those",
          "devices (or at least those devices associated with monitors).",
          "Typically, this access is enabled by:",
          "  - setting the group for /dev/i2c-* to i2c",
          "  - setting group RW permissions for /dev/i2c-*",
          "  - making the current user a member of group i2c",
          "Alternatively, this could be enabled by just giving everyone RW permission",
          "The following tests probe for these conditions.",
          NULL
         );

   rpt_nl();
   rpt_vstring(0,"Checking for /dev/i2c-* devices...");
   execute_shell_cmd_rpt("ls -l /dev/i2c-*", 1);

#ifdef OLD
   rc = getlogin_r(username, sizeof(username));
   printf("(%s) getlogin_r() returned %d, strlen(username)=%zd\n", __func__,
          rc, strlen(username));
   if (rc == 0)
      printf("(%s) username = |%s|\n", __func__, username);
   // printf("\nLogged on user:  %s\n", username);
   printf("(%s) getlogin() returned |%s|\n", __func__, getlogin());
   char * cmd = "echo $LOGNAME";
   printf("(%s) executing command: %s\n", __func__, cmd);
   bool ok = execute_shell_cmd_rpt(cmd, 0);
   printf("(%s) execute_shell_cmd() returned %s\n", __func__, bool_repr(ok));

#endif
   uid_t uid = getuid();
   // uid_t euid = geteuid();
   // printf("(%s) uid=%u, euid=%u\n", __func__, uid, euid);
   struct passwd *  pwd = getpwuid(uid);
   rpt_nl();
   rpt_vstring(0,"Current user: %s (%u)\n", pwd->pw_name, uid);
   uname = strdup(pwd->pw_name);

   bool all_i2c_rw = false;
   int busct = i2c_device_count();   // simple count, no side effects, consider replacing with local code
   if (busct == 0 && !just_fglrx) {
      rpt_vstring(0,"WARNING: No /dev/i2c-* devices found");
   }
   else {
      all_i2c_rw = true;
      int  busno;
      char fnbuf[20];

      for (busno=0; busno < 32; busno++) {
         if (i2c_device_exists(busno)) {
            snprintf(fnbuf, sizeof(fnbuf), "/dev/i2c-%d", busno);
            int rc;
            int errsv;
            DBGMSF(debug, "Calling access() for %s", fnbuf);
            rc = access(fnbuf, R_OK|W_OK);
            if (rc < 0) {
               errsv = errno;
               rpt_vstring(0,"Device %s is not readable and writable.  Error = %s",
                      fnbuf, linux_errno_desc(errsv) );
               all_i2c_rw = false;
            }
         }
      }

      if (!all_i2c_rw) {
         rpt_vstring(0,"WARNING: Current user (%s) does not have RW access to all /dev/i2c-* devices.",
 //               username);
                uname);
      }
      else
         rpt_vstring(0,"Current user (%s) has RW access to all /dev/i2c-* devices.",
               // username);
               uname);
   }

   if (!all_i2c_rw || output_level >= DDCA_OL_VERBOSE) {
      rpt_nl();
      rpt_vstring(0,"Checking for group i2c...");
      // replaced by C code
      // execute_shell_cmd("grep i2c /etc/group", 1);

      bool group_i2c_exists = false;   // avoid special value in gid_i2c
      // gid_t gid_i2c;
      struct group * pgi2c = getgrnam("i2c");
      if (pgi2c) {
         rpt_vstring(0,"   Group i2c exists");
         group_i2c_exists = true;
         // gid_i2c = pgi2c->gr_gid;
         // DBGMSG("getgrnam returned gid=%d for group i2c", gid_i2c);
         // DBGMSG("getgrnam() reports members for group i2c: %s", *pgi2c->gr_mem);
         int ndx=0;
         char * curname;
         bool found_curuser = false;
         while ( (curname = pgi2c->gr_mem[ndx]) ) {
            rtrim_in_place(curname);
            // DBGMSG("member_names[%d] = |%s|", ndx, curname);
            if (streq(curname, uname /* username */)) {
               found_curuser = true;
            }
            ndx++;
         }
         if (found_curuser) {
            rpt_vstring(0,"   Current user %s is a member of group i2c", uname  /* username */);
         }
         else {
            rpt_vstring(0,"   WARNING: Current user %s is NOT a member of group i2c", uname /*username*/);
         }
      }
      if (!group_i2c_exists) {
         rpt_vstring(0,"   Group i2c does not exist");
      }
      free(uname);
   #ifdef BAD
      // getgroups, getgrouplist returning nonsense
      else {
         uid_t uid = geteuid();
         gid_t gid = getegid();
         struct passwd * pw = getpwuid(uid);
         printf("Effective uid %d: %s\n", uid, pw->pw_name);
         char * uname = strdup(pw->pw_name);
         struct group * pguser = getgrgid(gid);
         printf("Effective gid %d: %s\n", gid, pguser->gr_name);
         if (group_member(gid_i2c)) {
            printf("User %s (%d) is a member of group i2c (%d)\n", uname, uid, gid_i2c);
         }
         else {
            printf("WARNING: User %s (%d) is a not member of group i2c (%d)\n", uname, uid, gid_i2c);
         }

         size_t supp_group_ct = getgroups(0,NULL);
         gid_t * glist = calloc(supp_group_ct, sizeof(gid_t));
         int rc = getgroups(supp_group_ct, glist);
         int errsv = errno;
         DBGMSF(debug, "getgroups() returned %d", rc);
         if (rc < 0) {
            DBGMSF(debug, "getgroups() returned %d", rc);

         }
         else {
            DBGMSG("Found %d supplementary group ids", rc);
            int ndx;
            for (ndx=0; ndx<rc; ndx++) {
               DBGMSG("Supplementary group id: %d", *glist+ndx);
            }

         }

         int supp_group_ct2 = 100;
         glist = calloc(supp_group_ct2, sizeof(gid_t));
         DBGMSG("Calling getgrouplist for user %s", uname);
         rc = getgrouplist(uname, gid, glist, &supp_group_ct2);
         errsv = errno;
         DBGMSG("getgrouplist returned %d, supp_group_ct=%d", rc, supp_group_ct2);
         if (rc < 0) {
            DBGMSF(debug, "getgrouplist() returned %d", rc);
         }
         else {
            DBGMSG("getgrouplist found %d supplementary group ids", rc);
            int ndx;
            for (ndx=0; ndx<rc; ndx++) {
               DBGMSG("Supplementary group id: %d", *glist+ndx);
            }
         }
      }
   #endif

      rpt_nl();
      rpt_vstring(0,"Looking for udev nodes files that reference i2c:");
      execute_shell_cmd_rpt("grep -H i2c /etc/udev/makedev.d/*", 1);
      rpt_nl();
      rpt_vstring(0,"Looking for udev rules files that reference i2c:");
      execute_shell_cmd_rpt("grep -H i2c "
                        "/lib/udev/rules.d/*rules "
                        "/run/udev/rules.d/*rules "
                        "/etc/udev/rules.d/*rules", 1 );
   }
}


/* Checks if a module is built in to the kernel.
 *
 * Arguments:
 *   module_name    simple module name, as it appears in the file system, e.g. i2c-dev
 *
 * Returns:         true/false
 */
static bool is_module_builtin(char * module_name) {
   bool debug = false;
   bool result = false;

   struct utsname utsbuf;
   int rc = uname(&utsbuf);
   assert(rc == 0);
   // DBGMSG("uname() returned release: %s", &utsbuf.release);

   // works, but simpler to use uname() that doesn't require free(osrelease)
   // char * osrelease = file_get_first_line("/proc/sys/kernel/osrelease", true /* verbose */);
   // assert(streq(utsbuf.release, osrelease));

   char modules_builtin_fn[100];
   snprintf(modules_builtin_fn, 100, "/lib/modules/%s/modules.builtin", utsbuf.release);
   // free(osrelease);

   char cmdbuf[200];

   snprintf(cmdbuf, 200, "grep -H %s.ko %s", module_name, modules_builtin_fn);
   // DBGMSG("cmdbuf = |%s|", cmdbuf);

   GPtrArray * response = execute_shell_cmd_collect(cmdbuf);
   // internal rc =  0 if found, 256 if not found
   // returns 0 lines if not found
   // NULL response if command error

   // DBGMSG("execute_shell_cmd_collect() returned %d lines", response->len);
   // for (int ndx = 0; ndx < response->len; ndx++) {
   //    puts(g_ptr_array_index(response, ndx));
   // }

   result = (response && response->len > 0);
   g_ptr_array_free(response, true);

   DBGMSF(debug, "module_name = %s, returning %s", module_name, bool_repr(result));
   return result;
}



/* Checks if module i2c_dev is required and if so whether it is loaded.
 * Reports the result.
 *
 * Arguments:
 *    video_driver_list  list of video drivers
 *
 * Returns:              nothing
 */
static void check_i2c_dev_module(struct driver_name_node * video_driver_list) {
   rpt_vstring(0,"Checking for module i2c_dev...");

   DDCA_Output_Level output_level = get_output_level();

   bool module_required = !only_nvidia_or_fglrx(video_driver_list);
   if (!module_required) {
      rpt_vstring(0,"Using only proprietary nvidia or fglrx driver. Module i2c_dev not required.");
      if (output_level < DDCA_OL_VERBOSE)
         return;
      rpt_vstring(0,"Remaining i2c_dev detail is purely informational.");
   }

   bool is_builtin = is_module_builtin("i2c-dev");
   rpt_vstring(0,"   Module %-16s is %sbuilt into kernel", "i2c_dev", (is_builtin) ? "" : "NOT ");
   if (is_builtin) {
      if (output_level < DDCA_OL_VERBOSE)
         return;
      if (module_required)  // no need for duplicate message
         rpt_vstring(0,"Remaining i2c_dev detail is purely informational.");
   }

   bool is_loaded = is_module_loaded_using_sysfs("i2c_dev");
      // DBGMSF(debug, "is_loaded=%d", is_loaded);
   if (!is_builtin)
      rpt_vstring(0,"   Module %-16s is %sloaded", "i2c_dev", (is_loaded) ? "" : "NOT ");

   if ( (!is_loaded && !is_builtin) || output_level >= DDCA_OL_VERBOSE) {
      rpt_nl();
      rpt_vstring(0,"Check that kernel module i2c_dev is being loaded by examining files where this would be specified...");
      execute_shell_cmd_rpt("grep -H i2c[-_]dev "
                        "/etc/modules "
                        "/etc/modules-load.d/*conf "
                        "/run/modules-load.d/*conf "
                        "/usr/lib/modules-load.d/*conf "
                        , 1);
      rpt_nl();
      rpt_vstring(0,"Check for any references to i2c_dev in /etc/modprobe.d ...");
      execute_shell_cmd_rpt("grep -H i2c[-_]dev "
                        "/etc/modprobe.d/*conf "
                        "/run/modprobe.d/*conf "
                        , 1);
   }
}


#ifdef OLD

/* Checks for installed packages i2c-tools and libi2c-dev
 */
static void query_packages() {
   rpt_multiline(0,
         "ddcutil requiries package i2c-tools.  Use both dpkg and rpm to look for it.",
          "While we're at it, check for package libi2c-dev which is used for building",
          "ddcutil.",
          NULL
         );

   bool ok;
   // n. apt show produces warning msg that format of output may change.
   // better to use dpkg
   rpt_nl();
   rpt_vstring(0,"Using dpkg to look for package i2c-tools...");
   ok = execute_shell_cmd_rpt("dpkg --status i2c-tools", 1);
   if (!ok)
      rpt_vstring(0,"dpkg command not found");
   else {
      execute_shell_cmd_rpt("dpkg --listfiles i2c-tools", 1);
   }

   rpt_nl();
   rpt_vstring(0,"Using dpkg to look for package libi2c-dev...");
   ok = execute_shell_cmd_rpt("dpkg --status libi2c-dev", 1);
   if (!ok)
      rpt_vstring(0,"dpkg command not found");
   else {
      execute_shell_cmd_rpt("dpkg --listfiles libi2c-dev", 1);
   }

   rpt_nl();
   rpt_vstring(0,"Using rpm to look for package i2c-tools...");
   ok = execute_shell_cmd_rpt("rpm -q -l --scripts i2c-tools", 1);
   if (!ok)
      rpt_vstring(0,"rpm command not found");
}
#endif

static bool query_card_and_driver_using_lspci() {
   // DBGMSG("Starting");
   bool ok = true;
   FILE * fp;

   rpt_vstring(0,"Using lspci to examine driver environment...");
   fp = popen("lspci", "r");
   if (!fp) {
      // int errsv = errno;
      rpt_vstring(0,"Unable to execute command lspci: %s", strerror(errno));

      printf("lspci command unavailable\n");       // why doesn't this print?
      printf("lspci command really unavailable\n");  // or this?
      ok = false;
   }
   else {
      char * a_line = NULL;
      size_t len = 0;
      ssize_t read;
      char pci_addr[15];
      // char device_title[100];
      char device_name[300];
      while ( (read=getline(&a_line, &len, fp)) != -1) {
         if (strlen(a_line) > 0)
            a_line[strlen(a_line)-1] = '\0';
         // UGLY UGLY - WHY DOESN'T SCANF WORK ???
         // DBGMSG("lspci line: |%s|", a_line);
#ifdef SCAN_FAILS
         // doesn't find ':'
         // char * pattern = "%s %s:%s";
         char * pattern = "%[^' '],%[^':'], %s";
         int ct = sscanf(a_line, pattern, pci_addr, device_title, device_name);

         DBGMSG("ct=%d, t_read=%ld, pci_addr=%s, device_title=%s", ct, len, pci_addr, device_title);
         if (ct == 3) {
            if ( str_starts_with("VGA", device_title) ) {
               printf("Video controller: %s\n", device_name);
            }
         }
#endif
         int ct = sscanf(a_line, "%s %s", pci_addr, device_name);
         // DBGMSG("ct=%d, t_read=%ld, pci_addr=%s, device_name=%s", ct, len, pci_addr, device_name);
         if (ct == 2) {
            if ( str_starts_with("VGA", device_name) ) {
               // printf("Video controller 0: %s\n", device_name);
               char * colonpos = strchr(a_line + strlen(pci_addr), ':');
               if (colonpos)
                  rpt_vstring(0,"Video controller: %s", colonpos+1);
               else
                  rpt_vstring(0,"colon not found");
            }
         }
      }
      pclose(fp);
   }
   return ok;
}


// Two ways to get the hex device identifiers.  Both are ugly.
// Reading modalias requires extracting values from a single string.
// Reading individual ids from individual attributes is simpler,
// but note the lack of error checking.
// Pick your poison.

typedef struct {
   ushort   vendor_id;
   ushort   device_id;
   ushort   subdevice_id;    // subsystem device id
   ushort   subvendor_id;    // subsystem vendor id
} Device_Ids;

/** Reads the device identifiers from directory
 *  /sys/bus/pci/devices/nnnn:nn:nn.n/ using the individual vendor, device,
 *  subsystem, and subsystem_device attributes.
 *
 *  \param cur_dir_name  directory name
 *  \return struct containing the ids
 *
 *  \remark
 *  Note the struct itself is returned on the stack, not a pointer to a struct.
 *  There is nothing to free.
 */
Device_Ids read_device_ids1(char * cur_dir_name) {
   Device_Ids result = {0};

   rpt_vstring(0, "Reading device ids from individual attribute files...");
   // printf("vendor: %s\n", read_sysfs_attr(cur_dir_name, "vendor", true));
   // printf("device: %s\n", read_sysfs_attr(cur_dir_name, "device", true));
   // printf("subsystem_device: %s\n", read_sysfs_attr(cur_dir_name, "subsystem_device", true));
   // printf("subsystem_vendor: %s\n", read_sysfs_attr(cur_dir_name, "subsystem_vendor", true));

   char * vendor_id        = read_sysfs_attr_w_default(cur_dir_name, "vendor",           "0x00", true);
   char * device_id        = read_sysfs_attr_w_default(cur_dir_name, "device",           "0x00", true);
   char * subsystem_device = read_sysfs_attr_w_default(cur_dir_name, "subsystem_device", "0x00", true);
   char * subsystem_vendor = read_sysfs_attr_w_default(cur_dir_name, "subsystem_vendor", "0x00", true);

   result.vendor_id    = h2ushort(vendor_id);
   result.device_id    = h2ushort(device_id);
   result.subvendor_id = h2ushort(subsystem_vendor);
   result.subdevice_id = h2ushort(subsystem_device);

   free(vendor_id);
   free(device_id);
   free(subsystem_device);
   free(subsystem_vendor);

   return result;
}


/** Reads the device identifiers from directory
 *  /sys/bus/pci/devices/nnnn:nn:nn.n/ by reading and parsing the modalias
 *  attribute.
 *
 *  \param cur_dir_name  directory name
 *  \return struct containing the ids
 *
 *  \remark
 *  Note the struct itself is returned on the stack, not a pointer to a struct.
 *  There is nothing to free.
 */
Device_Ids read_device_ids2(char * cur_dir_name) {
   Device_Ids result = {0};

   // TODO: Reimplement using proper parsing.  See kernel file file2alias.c
   // See also:
   //  http://people.skolelinux.org/pere/blog/Modalias_strings___a_practical_way_to_map__stuff__to_hardware.html

   rpt_vstring(0, "Reading device ids by parsing modalias attribute...");
   char * modalias = read_sysfs_attr(cur_dir_name, "modalias", true);
               // printf("modalias: %s\n", modalias);
   if (modalias) {
      // printf("\nParsing modalias for values...\n");
      char * colonpos = strchr(modalias, ':');
      assert(colonpos);                // coverity complains that strchr() might return NULL
      assert(*(colonpos+1) == 'v');    // vendor_id
      char * vendor_id = substr(colonpos, 2, 8);
      // printf("vendor_id:        %s\n", vendor_id);
      assert(*(colonpos+10) == 'd');
      char * device_id = lsub(colonpos+11,8);
      // printf("device_id:        %s\n", device_id);
      assert( *(colonpos+19) == 's');
      assert( *(colonpos+20) == 'v');
      char * subsystem_vendor = lsub(colonpos+21,8);
      // printf("subsystem_vendor: %s\n", subsystem_vendor);
      assert( *(colonpos+29) == 's');
      assert( *(colonpos+30) == 'd');
      char * subsystem_device = lsub(colonpos+31,8);
      // printf("subsystem_device: %s\n", subsystem_device);
      assert( *(colonpos+39) == 'b');
      assert( *(colonpos+40) == 'c');
      // not used
      //char * base_class = lsub(colonpos+41,2);
      // printf("base_class:       %s\n", base_class);     // bytes 0-1 of value from class
      assert( *(colonpos+43) == 's');
      assert( *(colonpos+44) == 'c');
      // not used
      // char * sub_class = lsub(colonpos+45,2);          // bytes 1-2 of value from class
      // printf("sub_class:        %s\n", sub_class);
      assert( *(colonpos+47) == 'i');
      // not used
      // char * interface_id = lsub(colonpos+48,2);
      // printf("interface_id:     %s\n", interface_id);  // bytes 4-5 of value from class?

      result.vendor_id    = h2ushort(vendor_id);
      result.device_id    = h2ushort(device_id);
      result.subvendor_id = h2ushort(subsystem_vendor);
      result.subdevice_id = h2ushort(subsystem_device);

      free(vendor_id);
      free(device_id);
      free(subsystem_vendor);
      free(subsystem_device);
      free(modalias);
   }

   return result;
}



/* Scans /sys/bus/pci/devices for video devices.
 * Reports on the devices, and returns a singly linked list of driver names.
 *
 * Arguments:   none
 *
 * Returns:     singly linked list of video driver names
 */
static struct driver_name_node * query_card_and_driver_using_sysfs(Env_Accumulator * accum) {
   rpt_vstring(0,"Obtaining card and driver information from /sys...");

   // also of possible interest:
   // /sys/class/i2c-dev/i2c-*/name
   //    refers to video driver or piix4_smbus
   // also accessed at:
   // /sys/bus/i2c/devices/i2c-*/name
   // /sys/bus/pci/drivers/nouveau
   // /sys/bus/pci/drivers/piix4_smbus
   // /sys/bus/pci/drivers/nouveau/0000:01:00.0
   //                                           /name
   //                                           i2c-dev
   // /sys/module/nvidia
   // /sys/module/i2c_dev ?
   // /sys/module/... etc

   // Raspbian:
   // /sys/bus/platform/drivers/vc4_v3d
   // /sys/module/vc4

   char * driver_name = NULL;
   struct driver_name_node * driver_list = NULL;

   struct dirent *dent;
   DIR           *d;

   if (accum->is_arm) {
      rpt_vstring(0, "Machine architecture is %s.  Skipping /sys/bus/pci checks.", accum->architecture);
      char * platform_drivers_dir_name = "/sys/bus/platform/drivers";
      d = opendir(platform_drivers_dir_name);
      if (!d) {
         rpt_vstring(0,"Unable to open directory %s: %s", platform_drivers_dir_name, strerror(errno));
      }
      else {
         while ((dent = readdir(d)) != NULL) {
            // DBGMSG("%s", dent->d_name);
            char cur_fn[100];
            char cur_dir_name[100];
            if (!streq(dent->d_name, ".") && !streq(dent->d_name, "..") ) {
               // sprintf(cur_dir_name, "%s/%s", pci_devices_dir_name, dent->d_name);
               sprintf(cur_fn, "%s", cur_dir_name);
               if (streq(cur_dir_name, "vc4_v3d")) {
                  char * driver_name = cur_fn;
                  printf(    "   Driver name:    %s\n", driver_name);
                  struct driver_name_node * new_node = calloc(1, sizeof(struct driver_name_node));
                  new_node->driver_name = strdup(driver_name);
                  new_node->next = driver_list;
                  driver_list = new_node;
               }
            }
         }
      }
      closedir(d);
   }
   else {
      char * pci_devices_dir_name = "/sys/bus/pci/devices";
      d = opendir(pci_devices_dir_name);
      if (!d) {
         rpt_vstring(0,"Unable to open directory %s: %s", pci_devices_dir_name, strerror(errno));
      }
      else {
         while ((dent = readdir(d)) != NULL) {
            // DBGMSG("%s", dent->d_name);
            char cur_fn[100];
            char cur_dir_name[100];
            if (!streq(dent->d_name, ".") && !streq(dent->d_name, "..") ) {
               sprintf(cur_dir_name, "%s/%s", pci_devices_dir_name, dent->d_name);
               sprintf(cur_fn, "%s/class", cur_dir_name);
               // read /sys/bus/pci/devices/nnnn:nn:nn.n/class
               char * class_id = read_sysfs_attr(cur_dir_name, "class", /*verbose=*/true);
               // printf("%s: |%s|\n", cur_fn, class_id);
               if (str_starts_with(class_id, "0x03")) {
                  // printf("%s = 0x030000\n", cur_fn);
                  rpt_nl();
                  rpt_vstring(0,"Determining driver name and possibly version...");
                  // DBGMSG("cur_dir_name: %s", cur_dir_name);
                  char workfn[PATH_MAX];
                  sprintf(workfn, "%s/%s", cur_dir_name, "driver");
                  char resolved_path[PATH_MAX];
                  char * rpath = realpath(workfn, resolved_path);
                  if (!rpath) {
                     int errsv = errno;
                     rpt_vstring(0,"Cannot determine driver name");
                     rpt_vstring(0, "realpath(%s) returned NULL, errno=%d (%s)",
                                     workfn, errsv, linux_errno_name(errsv));
                     if (errsv == ENOENT) {
                        // fail in virtual environment?
                        // Raspberry Pi
                        rpt_vstring(0, "Directory not found: %s", cur_dir_name);
                     }
                     else {
                        // rpt_vstring(0, "realpath(%s) returned NULL, errno=%d (%s)",
                        //                 workfn, errsv, linux_errno_name(errsv));
                     }
                  }
                  else {
                     // printf("realpath returned %s\n", rpath);
                     // printf("%s --> %s\n",workfn, resolved_path);
                     char * final_slash_ptr = strrchr(rpath, '/');
                     // TODO: handle case where there are more than 1 video drivers loaded,
                     // say if the system contains both an AMD and Nvidia card
                     driver_name = final_slash_ptr+1;
                     printf(    "   Driver name:    %s\n", driver_name);
                     struct driver_name_node * new_node = calloc(1, sizeof(struct driver_name_node));
                     new_node->driver_name = strdup(driver_name);
                     new_node->next = driver_list;

                     driver_list = new_node;


                     char driver_module_dir[PATH_MAX];
                     sprintf(driver_module_dir, "%s/driver/module", cur_dir_name);
                     // printf("driver_module_dir: %s\n", driver_module_dir);
                     char * driver_version = read_sysfs_attr(driver_module_dir, "version", false);
                     if (driver_version)
                         rpt_vstring(0,"   Driver version: %s", driver_version);
                     else
                        rpt_vstring(0,"   Unable to determine driver version");
                  }

                  rpt_nl();
                  Device_Ids dev_ids = read_device_ids1(cur_dir_name);
                  Device_Ids dev_ids2 = read_device_ids2(cur_dir_name);
                  assert(dev_ids.vendor_id == dev_ids2.vendor_id);
                  assert(dev_ids.device_id == dev_ids2.device_id);
                  assert(dev_ids.subvendor_id == dev_ids2.subvendor_id);
                  assert(dev_ids.subdevice_id == dev_ids2.subdevice_id);

                  // printf("\nLooking up names in pci.ids...\n");
                  // rpt_nl();
                  rpt_vstring(0,"Video card identification:");
                  bool pci_ids_ok = devid_ensure_initialized();
                  if (pci_ids_ok) {
                     Pci_Usb_Id_Names names = devid_get_pci_names(
                                     dev_ids.vendor_id,
                                     dev_ids.device_id,
                                     dev_ids.subvendor_id,
                                     dev_ids.subdevice_id,
                                     4);
                     if (!names.vendor_name)
                        names.vendor_name = "unknown vendor";
                     if (!names.device_name)
                        names.device_name = "unknown device";

                     rpt_vstring(0,"   Vendor:              %04x       %s", dev_ids.vendor_id, names.vendor_name);
                     rpt_vstring(0,"   Device:              %04x       %s", dev_ids.device_id, names.device_name);
                     if (names.subsys_or_interface_name)
                     rpt_vstring(0,"   Subvendor/Subdevice: %04x/%04x  %s", dev_ids.subvendor_id, dev_ids.subdevice_id, names.subsys_or_interface_name);
                  }
                  else {
                     rpt_vstring(0,"Unable to find pci.ids file for name lookup.");
                     rpt_vstring(0,"   Vendor:              %04x       ", dev_ids.vendor_id);
                     rpt_vstring(0,"   Device:              %04x       ", dev_ids.device_id);
                     rpt_vstring(0,"   Subvendor/Subdevice: %04x/%04x  ", dev_ids.subvendor_id, dev_ids.subdevice_id);
                  }
               }
               else if (str_starts_with(class_id, "0x0a")) {
                  DBGMSG("Encountered docking station (class 0x0a) device. dir=%s", cur_dir_name);
               }
            }
         }
         closedir(d);
      }
   }
   accum->driver_list = driver_list;
   return driver_list;
}


/* Performs checks specific to the nvidia and fglrx proprietary video drivers.
 *
 * Arguments:
 *    driver list    list of loaded drivers
 *
 * Returns:          nothing
 */
static void driver_specific_tests(struct driver_name_node * driver_list) {
   rpt_vstring(0,"Performing driver specific checks...");
   bool found_driver_specific_checks = false;

   if (found_driver(driver_list, "nvidia")) {
      found_driver_specific_checks = true;
      rpt_nl();
      rpt_vstring(0,"Checking for special settings for proprietary Nvidia driver ");
      rpt_vstring(0,"(needed for some newer Nvidia cards).");
      execute_shell_cmd_rpt("grep -iH i2c /etc/X11/xorg.conf /etc/X11/xorg.conf.d/*", 1);
   }

   if (found_driver(driver_list, "fglrx")) {
      found_driver_specific_checks = true;
      rpt_nl();
      rpt_vstring(0,"Performing ADL specific checks...");
#ifdef HAVE_ADL
     if (!adlshim_is_available()) {
        set_output_level(DDCA_OL_VERBOSE);  // force error msg that names missing dll
        bool ok = adlshim_initialize();
        if (!ok)
           printf("WARNING: Using AMD proprietary video driver fglrx but unable to load ADL library\n");
     }
#else
     rpt_vstring(0,"WARNING: Using AMD proprietary video driver fglrx but ddcutil built without ADL support");
#endif
   }

   if (!found_driver_specific_checks)
      rpt_vstring(0,"No driver specific checks apply.");
}


//
// Using sysfs
//


static void query_loaded_modules_using_sysfs() {
   rpt_nl();
   rpt_vstring(0,"Testing if modules are loaded using /sys...");
   // known_video_driver_modules
   // other_driver_modules

   char ** pmodule_name = known_video_driver_modules;
   char * curmodule;
   int ndx;
   for (ndx=0; (curmodule=pmodule_name[ndx]) != NULL; ndx++) {
      bool is_loaded = is_module_loaded_using_sysfs(curmodule);
      // DBGMSF(debug, "is_loaded=%d", is_loaded);
      rpt_vstring(0,"   Module %-16s is %sloaded", curmodule, (is_loaded) ? "" : "NOT ");
   }
   pmodule_name = other_driver_modules;
   for (ndx=0; (curmodule=pmodule_name[ndx]) != NULL; ndx++) {
      bool is_loaded = is_module_loaded_using_sysfs(curmodule);
      rpt_vstring(0,"   Module %-16s is %sloaded", curmodule, (is_loaded) ? "" : "NOT ");
   }
}


static void query_i2c_bus_using_sysfs() {
   struct dirent *dent;
   DIR           *d;
   char          *dname;

   rpt_nl();
   rpt_vstring(0,"Examining /sys/bus/i2c/devices...");
   dname = "/sys/bus/i2c";
   d = opendir(dname);
   if (!d) {
      rpt_vstring(1, "i2c bus not defined in sysfs. Unable to open directory %s: %s\n",
                     dname, strerror(errno));
   }
   else {
      closedir(d);
      dname = "/sys/bus/i2c/devices";
      d = opendir(dname);
      if (!d) {
         rpt_vstring(1, "Unable to open sysfs directory %s: %s\n", dname, strerror(errno));
      }
      else {
         bool i2c_seen = false;
         while ((dent = readdir(d)) != NULL) {
            // DBGMSF("%s", dent->d_name);
            // char cur_fn[100];
            char cur_dir_name[100];
            if (!streq(dent->d_name, ".") && !streq(dent->d_name, "..") ) {
               // DBGMSF(debug, "dent->dname: %s", dent->d_name);
               sprintf(cur_dir_name, "%s/%s", dname, dent->d_name);
               char * dev_name = read_sysfs_attr(cur_dir_name, "name", true);
               rpt_vstring(1, "%s/name: %s", cur_dir_name, dev_name);
               i2c_seen = true;
            }
         }
         if (!i2c_seen)
            rpt_vstring(1, "No i2c devices found in %s", dname);
         closedir(d);
      }
   }
}


void query_drm_using_sysfs() {
   struct dirent *dent;
   struct dirent *dent2;
   DIR           *d;
   char          *dname;
   char          dnbuf[90];
   char          cardname[10];

   rpt_nl();
   rpt_vstring(0,"Examining /sys/class/drm...");
   dname = "/sys/class/drm";
   d = opendir(dname);
   if (!d) {
      rpt_vstring(1, "drm not defined in sysfs. Unable to open directory %s: %s\n",
                     dname, strerror(errno));
   }
   else {
      closedir(d);
      int cardno = 0;
      for (;;cardno++) {
         snprintf(cardname, 10, "card%d", cardno);
         snprintf(dnbuf, 80, "/sys/class/drm/%s", cardname);
         d = opendir(dnbuf);
         if (!d) {
            // rpt_vstring(1, "Unable to open sysfs directory %s: %s\n", dnbuf, strerror(errno));
            break;
         }
         else {
            while ((dent = readdir(d)) != NULL) {
               // DBGMSG("%s", dent->d_name);
               // char cur_fn[100];
               if (str_starts_with(dent->d_name, cardname)) {
                  rpt_vstring(1, "Found connector: %s", dent->d_name);
                 char cur_dir_name[100];
                 sprintf(cur_dir_name, "%s/%s", dnbuf, dent->d_name);

                 // char * s_dpms = read_sysfs_attr(cur_dir_name, "dpms", false);
                 // rpt_vstring(1, "%s/dpms: %s", cur_dir_name, s_dpms);

                 // char * s_enabled = read_sysfs_attr(cur_dir_name, "enabled", false);
                 //  rpt_vstring(1, "%s/enabled: %s", cur_dir_name, s_enabled);

                 char * s_status = read_sysfs_attr(cur_dir_name, "status", false);
                 rpt_vstring(2, "%s/status: %s", cur_dir_name, s_status);
                 // edid present iff status == "connected"
                 if (streq(s_status, "connected")) {
                    GByteArray * gba_edid = read_binary_sysfs_attr(
                          cur_dir_name, "edid", 128, /*verbose=*/ false);

                    // hex_dump(gba_edid->data, gba_edid->len);

#ifdef UNNEEDED
                    rpt_vstring(2, "Raw EDID:");
                    rpt_hex_dump(gba_edid->data, gba_edid->len, 2);
                    if (gba_edid->len >= 128) {
                       Parsed_Edid * parsed_edid = create_parsed_edid(gba_edid->data);
                       if (parsed_edid) {
                          report_parsed_edid_base(
                             parsed_edid,
                             true,   // verbose
                             false,  // show_hex
                             2);     // depth
                          free_parsed_edid(parsed_edid);

                       }
                       else {
                           rpt_vstring(2, "Unable to parse EDID");
                           // printf(" Unparsable EDID for output name: %s -> %p\n", prec->output_name, prec->edidbytes);
                           // hex_dump(prec->edidbytes, 128);
                       }
                    }
#endif

                    // look for i2c-n subdirectory, may or may not be present depending on driver
                    // DBGMSG("cur_dir_name: %s", cur_dir_name);
                    DIR* d2 = opendir(cur_dir_name);
                    char * i2c_node_name = NULL;

                    if (!d2) {
                       rpt_vstring(1, "Unexpected error. Unable to open sysfs directory %s: %s\n",
                                      cur_dir_name, strerror(errno));
                       break;
                    }
                    else {
                       while ((dent2 = readdir(d2)) != NULL) {
                          // DBGMSG("%s", dent2->d_name);
                          if (str_starts_with(dent2->d_name, "i2c")) {
                             rpt_vstring(2, "I2C device: %s", dent2->d_name);
                             i2c_node_name = strdup(dent2->d_name);
                             break;
                          }
                       }
                       closedir(d2);
                    }

                    // rpt_nl();

                    Device_Id_Xref * xref = device_xref_get(gba_edid->data);
                    // xref->sysfs_drm_name = strdup(dent->d_name);
                    xref->sysfs_drm_name = strdup(cur_dir_name);
                    xref->sysfs_drm_i2c  = i2c_node_name;

                    g_byte_array_free(gba_edid, true);

                 }
                 rpt_nl();
               }
            }
            closedir(d);
         }

      }
      if (cardno==0)
         rpt_vstring(1, "No drm class cards found in %s", dname);
      // closedir(d);

   }

   rpt_title("Query file system for i2c nodes under /sys/class/drm/card*...", 1);
   execute_shell_cmd_rpt("ls -ld /sys/class/drm/card*/card*/i2c*", 1);
}




#ifdef UNUSED
static bool query_card_and_driver_using_osinfo() {
   bool ok = false;

#ifdef FAILS
   printf("Trying Osinfo\n");

   OsinfoDb * info_db = osinfo_db_new();

   OsinfoDeviceList * device_list = osinfo_db_get_device_list(info_db);
   gint device_ct = osinfo_list_get_length(device_list);
   int ndx = 0;
   for (ndx=0; ndx < ct; ndx++) {
      OsinfoEntity * entity = osinfo_list_get_nth(device_list, ndx);
      char * entity_id = osinfo_entity_get_id(entity);
      DBGMSG("osinfo entity id = %s", entity_id );

   }
#endif

   return ok;
}
#endif


//
// Using internal i2c API
//

static void query_i2c_buses() {
   rpt_nl();
   rpt_vstring(0,"Examining I2C buses, as detected by I2C layer...");
   i2c_report_buses(true, 1 /* indentation depth */);    // in i2c_bus_core.c
}

#ifdef USE_X11
//
// Using X11 API
//

/* Reports EDIDs known to X11
 *
 * Arguments:    none
 *
 * Returns:      nothing
 */
void query_x11() {
   GPtrArray* edid_recs = get_x11_edids();
   rpt_nl();
   rpt_vstring(0,"EDIDs reported by X11 for connected xrandr outputs:");
   // DBGMSG("Got %d X11_Edid_Recs\n", edid_recs->len);

   for (int ndx=0; ndx < edid_recs->len; ndx++) {
      X11_Edid_Rec * prec = g_ptr_array_index(edid_recs, ndx);
      // printf(" Output name: %s -> %p\n", prec->output_name, prec->edid);
      // hex_dump(prec->edid, 128);
      rpt_vstring(1, "xrandr output: %s", prec->output_name);
      rpt_vstring(2, "Raw EDID:");
      rpt_hex_dump(prec->edidbytes, 128, 2);
      Parsed_Edid * parsed_edid = create_parsed_edid(prec->edidbytes);
      if (parsed_edid) {
         report_parsed_edid_base(
               parsed_edid,
               true,   // verbose
               false,  // show_hex
               2);     // depth
         free_parsed_edid(parsed_edid);
      }
      else {
         rpt_vstring(2, "Unable to parse EDID");
         // printf(" Unparsable EDID for output name: %s -> %p\n", prec->output_name, prec->edidbytes);
         // hex_dump(prec->edidbytes, 128);
      }
      rpt_nl();

      Device_Id_Xref * xref = device_xref_get(prec->edidbytes);
      xref->xrandr_name = strdup(prec->output_name);
   }
   free_x11_edids(edid_recs);

   // Display * x11_disp = open_default_x11_display();
   // GPtrArray *  outputs = get_x11_connected_outputs(x11_disp);
   // close_x11_display(x11_disp);
}
#endif


//
// i2cdetect
//

/* Uses i2cdetect to probe active addresses on I2C buses
 *
 * Arguments:    none
 *
 * Returns:      nothing
 */
#ifdef OLD
static void query_using_i2cdetect_old() {
   rpt_vstring(0,"Examining I2C buses using i2cdetect: ");
   // calling i2cdetect for an SMBUs device fills dmesg with error messages
   // avoid this if possible

   // GPtrArray * summaries = get_i2c_smbus_devices_using_udev();
   GPtrArray * summaries = get_i2c_devices_using_udev();

   // returns array of I2C bus numbers in string form, sorted in numeric order
   GPtrArray * busnums = execute_shell_cmd_collect("ls /dev/i2c* | cut -c 10- | sort -n");

   if (!busnums) {
      rpt_vstring(1, "No I2C buses found");
      goto bye;
   }
   if (busnums->len > 0) {
      int i;
      bool isint = str_to_int(g_ptr_array_index(busnums,0), &i);
      if (!isint) {
         rpt_vstring(1, "Apparently no I2C buses");
         goto bye;
      }
   }

   for (int ndx = 0; ndx < busnums->len; ndx++) {
      // printf("ndx=%d, value=|%s|\n", ndx, (char *) g_ptr_array_index(busnames, ndx));

      char cmd[80];
      char * busname = (char *) g_ptr_array_index(busnums, ndx);
      // busname+=9;   // strip off "/dev/i2c-"

      if (is_smbus_device_summary(summaries, busname) ) {
         rpt_nl();
         rpt_vstring(1, "Device /dev/i2c-%s is a SMBus device.  Skipping i2cdetect.", busname);
         continue;
      }

      snprintf(cmd, 80, "i2cdetect -y %s", busname);
      rpt_nl();
      rpt_vstring(1,"Probing bus /dev/i2c-%d using command \"%s\"", ndx, cmd);
      // DBGMSG("Executing command: |%s|\n", cmd);
      int rc = execute_shell_cmd_rpt(cmd, 2 /* depth */);
      // DBGMSG("execute_shell_cmd(\"%s\") returned %d", cmd, rc);
      if (rc != 1) {
         rpt_vstring(1,"i2cdetect command unavailable");
         break;
      }
   }

bye:
   if (busnums)
      g_ptr_array_free(busnums, true);


}
#endif


/** Examines /dev/i2c devices using command i2cdetect, if it exists.
 *
 *  \param  i2c_device_numbers  I2C bus numbers to check
 *
 */
static void query_using_i2cdetect(Byte_Value_Array i2c_device_numbers) {
   assert(i2c_device_numbers);

   int d0 = 0;
   int d1 = 1;

   rpt_vstring(d0,"Examining I2C buses using i2cdetect... ");

   if (bva_length(i2c_device_numbers) == 0) {
      rpt_vstring(d1, "No I2C buses found");
   }
   else {
      for (int ndx=0; ndx< bva_length(i2c_device_numbers); ndx++) {
         int busno = bva_get(i2c_device_numbers, ndx);
         if (is_ignorable_i2c_device(busno)) {
            // calling i2cdetect for an SMBUs device fills dmesg with error messages
            rpt_nl();
            rpt_vstring(d1, "Device /dev/i2c-%d is a SMBus or other ignorable device.  Skipping i2cdetect.", busno);
         }
         else {
            char cmd[80];
            snprintf(cmd, 80, "i2cdetect -y %d", busno);
            rpt_nl();
            rpt_vstring(d1,"Probing bus /dev/i2c-%d using command \"%s\"", busno, cmd);
            int rc = execute_shell_cmd_rpt(cmd, 2 /* depth */);
            // DBGMSG("execute_shell_cmd(\"%s\") returned %d", cmd, rc);
            if (rc != 1) {
               rpt_vstring(d1,"i2cdetect command unavailable");
               break;
            }
         }
      }
   }
}


/** Queries UDEV for devices in subsystem "i2c-dev".
 *  Also looks for devices with name attribute "DPMST"
 */
static void probe_i2c_devices_using_udev() {
   char * subsys_name = "i2c-dev";
   rpt_nl();
   rpt_vstring(0,"Probing I2C devices using udev, susbsystem %s...", subsys_name);
   // probe_udev_subsystem() is in udev_util.c, which is only linked in if USE_USB

   // Detailed scan of I2C device information
   probe_udev_subsystem(subsys_name, /*show_usb_parent=*/ false, 1);
   rpt_nl();

   GPtrArray * summaries = get_i2c_devices_using_udev();
   report_i2c_udev_device_summaries(summaries, "Summary of udev I2C devices",1);
   for (int ndx = 0; ndx < summaries->len; ndx++) {
      Udev_Device_Summary * summary = g_ptr_array_index(summaries, ndx);
      assert( memcmp(summary->marker, UDEV_DEVICE_SUMMARY_MARKER, 4) == 0);
      int busno = udev_i2c_device_summary_busno(summary);
      Device_Id_Xref * xref = device_xref_find_by_busno(busno);
      if (xref) {
         xref->udev_name = strdup(summary->sysattr_name);
         xref->udev_syspath = strdup(summary->devpath);
      }
      else {
         // DBGMSG("Device_Id_Xref not found for busno %d", busno);
      }
   }
   free_udev_device_summaries(summaries);   // ok if summaries == NULL

   rpt_nl();
   char * nameattr = "DPMST";
   rpt_vstring(0,"Looking for udev devices with name attribute %s...", nameattr);
   summaries = find_devices_by_sysattr_name(nameattr);
   report_i2c_udev_device_summaries(summaries, "Summary of udev DPMST devices...",1);
   free_udev_device_summaries(summaries);   // ok if summaries == NULL
}


//
// Log files
//

void filter_and_limit_g_ptr_array(
      GPtrArray * line_array,
      char **     filter_terms,
      bool        ignore_case,
      int         limit)
{
   bool debug = false;
   DBGMSF(debug, "line_array=%p, ct(filter_terms)=%d, ignore_case=%s, limit=%d",
            line_array, ntsa_length(filter_terms), bool_repr(ignore_case), limit);
   if (debug) {
      // (const char **) cast to conform to strjoin() signature
      char * s = strjoin( (const char **) filter_terms, -1, ", ");
      DBGMSG("Filter terms: %s", s);
      free(s);
   };
#ifdef TOO_MUCH
   if (debug) {
      if (filter_terms) {
         printf("(%s) filter_terms:\n", __func__);
         ntsa_show(filter_terms);
      }
   }
#endif
   // inefficient, just make it work for now
   for (int ndx = (line_array->len)-1 ; ndx >= 0; ndx--) {
      char * s = g_ptr_array_index(line_array, ndx);
      // DBGMSF(debug, "s=|%s|", s);
      bool keep = true;
      if (filter_terms)
         keep = apply_filter_terms(s, filter_terms, ignore_case);
      if (!keep) {
         g_ptr_array_remove_index(line_array, ndx);
      }
   }
   gaux_ptr_array_truncate(line_array, limit);

   DBGMSF(debug, "Done. line_array->len=%d", line_array->len);
}




/** Reads the contents of a file into a #GPtrArray of lines, optionally keeping only
 *  those lines containing at least one on a list of terms.  After filtering, the set
 *  of returned lines may be further reduced to either the first or last n number of
 *  lines.
 *
 *  \param  line_array #GPtrArray in which to return the lines read
 *  \param  fn         file name
 *  \param  filter_terms  #Null_Terminated_String_Away of filter terms
 *  \param  ignore_case   ignore case when testing filter terms
 *  \param  limit if 0, return all lines that pass filter terms
 *                if > 0, return at most the first #limit lines that satisfy the filter terms
 *                if < 0, return at most the last  #limit lines that satisfy the filter terms
 *  \return if >= 0, number of lines before filtering and limit applied
 *          if < 0,  -errno
 *
 *  \remark
 *  This function was created because using grep in conjunction with pipes was
 *  producing obscure shell errors.
 *  \remark The #GPtrArray is passed into this function instead of allocating it
 *          to allow for returning a status code.
 *  \remark Consider adding the ability to treat filter terms as regular expressions
 */
static int read_file_with_filter(
      GPtrArray * line_array,
      char *      fn,
      char **     filter_terms,
      bool        ignore_case,
      int         limit)
{
   bool debug = false;
   DBGMSF(debug, "line_array=%p, fn=%s, ct(filter_terms)=%d, ignore_case=%s, limit=%d",
            line_array, fn, ntsa_length(filter_terms), bool_repr(ignore_case), limit);

#ifdef TOO_MUCH
   if (debug) {
      if (filter_terms) {
         printf("(%s) filter_terms:\n", __func__);
         ntsa_show(filter_terms);
      }
   }
#endif

   g_ptr_array_set_free_func(line_array, g_free);    // in case not already set

   int rc = file_getlines(fn, line_array, /*verbose*/ true);
   DBGMSF((debug||(rc<0)), "file_getlines() returned %d", rc);

   if (rc > 0) {
      filter_and_limit_g_ptr_array(
         line_array,
         filter_terms,
         ignore_case,
         limit);
   }
   else { // rc == 0
      DBGMSF(debug, "Empty file");
   }

   DBGMSF(debug, "Returning: %d", rc);
   return rc;
}


int execute_cmd_collect_with_filter(
      char *       cmd,
      char **      filter_terms,
      bool         ignore_case,
      int          limit,
      GPtrArray ** result_loc)
{
   bool debug = false;
   DBGMSF(debug, "cmd|%s|, ct(filter_terms)=%d, ignore_case=%s, limit=%d",
            cmd, ntsa_length(filter_terms), bool_repr(ignore_case), limit);

   int rc = 0;
   GPtrArray *line_array = execute_shell_cmd_collect(cmd);
   if (!line_array) {
      rc = -1;
   }
   else {
      rc = line_array->len;
      if (rc > 0) {
         filter_and_limit_g_ptr_array(
            line_array,
            filter_terms,
            ignore_case,
            limit);
      }
   }
   *result_loc = line_array;

   DBGMSF(debug, "Returning: %d", rc);
   return rc;
}


#ifdef USE_SHELL
/* Helper function that scans a single log file
 *
 * \param  pre_grep     portion of command before the grep command
 * \param  grep_cmd     grep command
 * \param  post_grep    portion of command after the grep command
 * \param  title        describes what is being scanned
 * \param  depth        logical indentation depth
 */
bool probe_one_log_using_shell(
      char * pre_grep,
      char * grep_cmd,
      char * post_grep,
      char * title,
      int    depth)
{
   bool debug = false;
   bool result = true;
   assert(grep_cmd);
   assert(title);
   DBGMSF(debug, "Starting. pre_grep=\"%s\", grep_cmd=\"%s\", post_grep=\"%s\", title=\"%s\"",
          pre_grep, grep_cmd, post_grep, title);
   int l1 = (pre_grep) ? strlen(pre_grep) : 0;
   int l2 = strlen(grep_cmd);
   int l3 = (post_grep) ? strlen(post_grep) : 0;
   int bsz = l1 + l2 + l3 + 1;
   char * buf = malloc(bsz);
   DBGMSF(debug, "Allocated buffer of size %d", bsz);
   buf[0] = '\0';
   if (pre_grep)
      strcpy(buf, pre_grep);
   strcat(buf, grep_cmd);
   if (post_grep)
      strcat(buf, post_grep);

   // rpt_vstring(depth,"Checking %s for video and I2C related lines...", title);
   rpt_vstring(depth,"Checking %s for I2C related lines...", title);
   DBGMSF(debug, "Shell command, len=%d: \"%s\"", strlen(buf), buf);
   // GPtrArray * all_lines = execute_shell_cmd_collect(buf);
   // for (int ndx = 0; ndx < all_lines->len; ndx++)
   //    rpt_vstring(5, "%d: %s", ndx, g_ptr_array_index(all_lines, ndx));
   if ( !execute_shell_cmd_rpt(buf, depth+1) ) {
      rpt_vstring(depth+1,"Unable to process %s", title);
      result = false;
   }
   rpt_nl();
   free(buf);
   DBGMSF(debug, "Done.  Returning %s", bool_repr(result));
   return result;
}
#endif

/*  Helper function for building egrep command.  Appends search terms.
 *
 *  \param  terms  null terminated array of grep terms
 *  \param  buf    pointer to buffer to which terms are appended
 *  \param  bufsz  buffer size
 */
void add_egrep_terms(char ** terms, char * buf, int bufsz) {
   bool debug = false;
   DBGMSF(debug, "Starting. buf=|%s|", buf);
   char ** p = terms;
   char * src = NULL;
   while (*p) {
      // 11/4/2017: quoted search terms suddenly causing parsing error when
      // string is passed to popen() and the command ends with a "| head" or "|tail"
      // why?
      // Eliminated adding quotes and there's no problem
      // Luckily no search terms contain blanks
      // Alas, not true for future use - Raspbian
      // problem solved by eliminating spaces around "|" before "head"or "tail"

   src = " -e\""; strncat(buf, src, bufsz - (strlen(buf)+1));
   //                  strncat(buf, " -e\"", bufsz - (strlen(buf)+1));
                     strncat(buf, *p,  bufsz - (strlen(buf)+1));
   src = "\"";    strncat(buf, src, bufsz - (strlen(buf)+1));
   //                  strncat(buf, "\"", bufsz - (strlen(buf)+1));
#ifdef ALT
                   strncat(buf, " -e ", bufsz - (strlen(buf)+1));
                     strncat(buf, *p,  bufsz - (strlen(buf)+1));
   //                  strncat(buf, "\"", bufsz - (strlen(buf)+1));
#endif
      p++;
   }
   DBGMSF(debug, "Done. len=%d, buf=|%s|", strlen(buf), buf);
}


#ifdef USING_SHELL
/** Scan one log file using grep
 *
 *  \param  log_fn      file name
 *  \param  terms       #Null_Terminated_String_Array of grep terms
 *  \param  ignore_case if true, perform case insensitive grep
 *  \param  limit       if > 0, report only the first #limit lines found
 *                      if < 0, report only the last #limit lines found
 *                      if 0, report all lines found
 *  \param  depth       logical indentation depth
 *  \return true if log find found, false if not
 */
bool probe_log_using_shell(
      char *  log_fn,
      char ** terms,
      bool    ignore_case,
      int     limit,
      int     depth)
{
   bool debug = false;;
   DBGMSF(debug, "Starting.  log_fn=%s", log_fn);

   assert(log_fn);
   assert(terms);
   bool file_found = false;
   if (regular_file_exists(log_fn)) {
      const int limit_buf_sz = 200;
      char limit_buf[limit_buf_sz];
      limit_buf[0] = '\0';
      if (limit < 0) {
         rpt_vstring(depth, "Limiting output to last %d lines...", -limit);
         snprintf(limit_buf, 200, " %s|tail -n %d", log_fn, -limit);
      }
      else if (limit > 0) {
         rpt_vstring(depth, "Limiting output to first %d lines...", limit);
         snprintf(limit_buf, 200, " %s|head -n %d", log_fn, limit);
      }
      else {
         snprintf(limit_buf, 200, " %s", log_fn);
      }
      DBGMSF(debug, "limit_buf size=%d, len=%d, \"%s\"",
                    limit_buf_sz, strlen(limit_buf), limit_buf);
      char gbuf[1000];
      int  gbufsz = 1000;

      if (ignore_case)
         strncpy(gbuf, "grep -E -i ", gbufsz);
      else
         strncpy(gbuf, "grep -E ", gbufsz);
      add_egrep_terms(terms, gbuf, gbufsz);
      DBGMSF(debug, "gbuf size=%d, len=%d, \"%s\"",
                    gbufsz, strlen(gbuf), gbuf);
      probe_one_log_using_shell(NULL, gbuf, limit_buf, log_fn, depth);
      file_found = true;
   }
   else
      rpt_vstring(depth, "File not found: %s", log_fn);

   DBGMSF(debug, "Done. Returning %s", bool_repr(file_found));
   return file_found;
}
#endif


bool probe_log_using_api(
      char *  log_fn,
      char ** filter_terms,
      bool    ignore_case,
      int     limit,
      int     depth)
{
   bool debug = false;
   assert(log_fn);
   DBGMSF(debug, "Starting. log_fn=%s, filter_terms=%p, ignore_case=%s, limit=%d",
                 log_fn, filter_terms, bool_repr(ignore_case), limit);
   bool file_found = false;
   int rc = 0;
   if (regular_file_exists(log_fn)) {
      rpt_vstring(depth, "Scanning file: %s", log_fn);
      if (limit < 0) {
         rpt_vstring(depth, "Limiting output to last %d relevant lines...", -limit);
      }
      else if (limit > 0) {
          rpt_vstring(depth, "Limiting output to first %d relevant lines...", limit);
      }
      GPtrArray * found_lines = g_ptr_array_new_full(1000, g_free);
      rc = read_file_with_filter(found_lines, log_fn, filter_terms, ignore_case, limit);
      if (rc < 0) {
         f0printf(FERR, "Error reading file: %s\n", psc_desc(rc));
      }
      else if (rc == 0) {   // rc >0 is the original number of lines
         rpt_title("Empty file", depth);
         file_found = true;
      }
      else if (found_lines->len == 0) {
         rpt_title("No lines found after filtering", depth);
         file_found = true;
      }
      else {
         for (int ndx = 0; ndx < found_lines->len; ndx++) {
            rpt_title(g_ptr_array_index(found_lines, ndx), depth+1);
         }
         file_found = true;
      }
   }
   else {
      rpt_vstring(depth, "File not found: %s", log_fn);
      rc = -ENOENT;
   }
   DBGMSF(debug, "rc=%d, file_found=%s", rc, bool_repr(file_found));
   rpt_nl();
   return file_found;
}



bool probe_cmd_using_api(
      char *  cmd,
      char ** filter_terms,
      bool    ignore_case,
      int     limit,
      int     depth)
{
   bool debug = false;
   assert(cmd);
   DBGMSF(debug, "Starting. cmd=%s, filter_terms=%p, ignore_case=%s, limit=%d",
                 cmd, filter_terms, bool_repr(ignore_case), limit);

   rpt_vstring(depth, "Executing command: %s", cmd);
   if (limit < 0) {
      rpt_vstring(depth, "Limiting output to last %d relevant lines...", -limit);
   }
   else if (limit > 0) {
       rpt_vstring(depth, "Limiting output to first %d relevant lines...", limit);
   }
   GPtrArray * filtered_lines = NULL;
   int rc = execute_cmd_collect_with_filter(cmd, filter_terms, ignore_case, limit, &filtered_lines);
   if (rc < 0) {
      f0printf(FERR, "Error executing command: %s\n", psc_desc(rc));
   }
   else if (rc == 0) {   // rc >0 is the original number of lines
      rpt_title("No output", depth);
   }
   else if (filtered_lines->len == 0) {
         rpt_title("No lines found after filtering", depth);
      }
      else {
         for (int ndx = 0; ndx < filtered_lines->len; ndx++) {
            rpt_title(g_ptr_array_index(filtered_lines, ndx), depth+1);
         }
      }

   bool result = (rc >= 0);
   DBGMSF(debug, "rc=%d, returning %s", rc, bool_repr(result));
   rpt_nl();
   return result;
}




/** Scans log files for lines of interest.
 *
 *  Depending on operating environment, some subset of
 *  the following files and command output:
 *    - dmesg
 *    - journalctl
 *    - /var/log/daemon.log
 *    - /var/log/kern.log
 *    - /var/log/messages
 *    - /var/log/syslog
 *    - /var/log/Xorg.0.log
 *
 *  \param accum collected environment information
 */
void probe_logs(Env_Accumulator * accum) {
   // TODO: Function needs major cleanup

#ifdef USE_SHELL
   char gbuf[500];             // contains grep command
   int  gbufsz = sizeof(gbuf);
#endif

   int depth = 0;
   int d1 = depth+1;
   int d2 = depth+2;
   // DBGMSG("Starting");
   // debug_output_dest();

   rpt_nl();
   rpt_title("Examining system logs...", depth);

   // TODO: Pick simpler data structures.  Is Value_Name_Title_Table worth it?

   const Byte LOG_XORG       = 0x80;
   const Byte LOG_DAEMON     = 0x40;
   const Byte LOG_SYSLOG     = 0x20;
   const Byte LOG_KERN       = 0x10;
   const Byte LOG_JOURNALCTL = 0x08;
   const Byte LOG_MESSAGES   = 0x04;
   const Byte LOG_DMESG      = 0x02;

   Value_Name_Title_Table log_table = {
         VNT(LOG_DMESG,      "dmesg"              ),
         VNT(LOG_JOURNALCTL, "journalctl"         ),
         VNT(LOG_DAEMON,     "/var/log/daemon.log" ),
         VNT(LOG_KERN,       "/var/log/kern.log"  ),
         VNT(LOG_MESSAGES,   "/var/log/messages"  ),
         VNT(LOG_SYSLOG,     "/var/log/syslog"    ),
         VNT(LOG_XORG,       "/var/log/Xorg.0.log"),
         VNT_END
   };

   bool log_xorg_found       = false;
   bool log_daemon_found     = false;        // Raspbian
   bool log_syslog_found     = false;        // Ubuntu, Raspbian
   bool log_kern_found       = false;        // Raspbian
 //bool log_journalctl_found = false;        // Debian, Raspbian
   bool log_messages_found   = false;        // Raspbian
   bool log_dmesg_found      = false;

   Byte logs_checked = 0x00;
   Byte logs_found   = 0x00;

#ifdef USE_SHELL
   strncpy(gbuf, "egrep -i", gbufsz);
   add_egrep_terms(known_video_driver_modules, gbuf, gbufsz);
#endif

#ifdef NO
   // Problem: dmesg can be filled w i2c errors from i2cdetect trying to
   // read an SMBus device
   // Disable prefix_matches until filter out SMBUS devices
   p = prefix_matches;
#endif
   char * addl_matches[] = {
         "drm",
         "video",
         "eeprom",
         "i2c_",    // was i2c_
         NULL
   };

#ifdef USE_SHELL
   add_egrep_terms(addl_matches, gbuf, gbufsz);
#endif

   Null_Terminated_String_Array drivers_plus_addl_matches =
            ntsa_join(known_video_driver_modules, addl_matches, /*dup*/ false);

   // *** dmesg ***

   rpt_nl();
   // first few lines of dmesg are lost.  turning on any sort of debugging causes
   // them to reappear.  apparently a NL in the stream does the trick.  why?
   // it's a heisenbug.  Just use the more verbose journalctl output
   logs_checked |= LOG_DMESG;
#ifdef USE_SHELL
   log_dmesg_found = probe_one_log_using_shell("dmesg |",      gbuf, NULL,                   "dmesg",      depth+1);
#endif

   // DBGMSG("Alternative using API:");
   rpt_title("Scanning dmesg output for I2C related entries...", depth+1);
   log_dmesg_found = probe_cmd_using_api("dmesg", drivers_plus_addl_matches, /*ignore_case*/ true, 0, depth+1);
   if (log_dmesg_found)
      logs_found |= LOG_DMESG;

   // *** journalctl ***

   logs_checked |= LOG_JOURNALCTL;

#ifdef ALT
   // if don't use this version, don't need to link with libsystemd
   DBGMSG("Using get_current_boot_messages...");
   rpt_title("Checking journalctl for I2C related entries...", depth+1);
   GPtrArray * journalctl_msgs = get_current_boot_messages(drivers_plus_addl_matches, /* ignore case */true, 0);
   if (journalctl_msgs) {
      // log_journalctl_found = true;
      logs_found |= LOG_JOURNALCTL;

      for (int ndx = 0; ndx < journalctl_msgs->len; ndx++) {
         rpt_vstring(depth+2, "%s", g_ptr_array_index(journalctl_msgs, ndx));
      }
   }
   rpt_nl();
#endif

   // has a few more lines from nvidia-persistence, lines have timestamp, hostname, and subsystem
   // DBGMSG("Using probe_cmd_using_api()...:");
   rpt_title("Scanning journalctl output for I2C related entries...", depth+1);
   log_dmesg_found = probe_cmd_using_api("journalctl --no-pager --boot", drivers_plus_addl_matches, /*ignore_case*/ true, 0, depth+1);
   if (log_dmesg_found)
      logs_found |= LOG_DMESG;
    rpt_nl();

   // 11/4/17:  Now getting error msgs like:
   //   sh: 1: Syntax error: end of file unexpected
   // apparent problem with pipe and execute_shell_cmd_report()

   // no, it's journalctl that's the offender.  With just journalctl, earlier
   // messages re Summary of Udev devices is screwed up
   // --no-pager solves the problem

   // DBGMSG("Using probe_one_log_using_shell()...");
   // probe_one_log_using_shell("journalctl --no-pager --boot|", gbuf, NULL,                   "journalctl", depth+1);


   // *** Xorg.0.log ***

   char * xorg_terms[] = {
    //   "[Ll]oadModule:",     // matches LoadModule, UnloadModule
         "LoadModule:",     // matches LoadModule, UnloadModule
    //     "[Ll]oading",         // matches Loading Unloading
         "Loading",
         "driver for",
         "Matched .* as autoconfigured",
         "Loaded and initialized",
         "drm",
         "soc",
         "fbdev",       // matches fbdevhw
         "vc4",
         "i2c",
         NULL
   };

   // Null_Terminated_String_Array log_terms = all_terms;
   char * rasp_log_terms[] = {
         "i2c",
         NULL
   };

   Null_Terminated_String_Array log_terms = ntsa_join(drivers_plus_addl_matches, rasp_log_terms, false);
   Null_Terminated_String_Array all_terms = log_terms;

   if (accum->is_arm) {
      logs_checked |= LOG_XORG;
#ifdef ALT
      DBGMSG("Using probe_log_using_shell()...");
      log_xorg_found = probe_log_using_shell("/var/log/Xorg.0.log", xorg_terms, /*ignore_case*/ true, 0, depth+1);
      if (log_xorg_found)
         logs_found |= LOG_XORG;
#endif
      // DBGMSG("Using probe_log_using_api...");
      log_xorg_found =  probe_log_using_api("/var/log/Xorg.0.log", xorg_terms, /*ignore_case*/ true,  0, depth+1);
      if (log_xorg_found)
         logs_found |= LOG_XORG;
   }
   else {
      logs_checked |= LOG_XORG;
      // rpt_vstring(depth+1, "Limiting output to 200 lines...");

#ifdef SHELL_CMD
      DBGMSG("Using probe_one_log_using_shell()...");
      log_xorg_found = probe_one_log_using_shell(NULL,           gbuf, " /var/log/Xorg.0.log|head -n 200", "Xorg.0.log", depth+1);
      if (log_xorg_found)
         logs_found |= LOG_XORG;
#endif

      // DBGMSG("Using probe_log_using_api...");
      log_xorg_found =  probe_log_using_api("/var/log/Xorg.0.log", drivers_plus_addl_matches, /*ignore_case*/ true, 200, depth+1);
      if (log_xorg_found)
         logs_found |= LOG_XORG;
   }


   // ***/var/log/kern.log, /var/log/daemon.log, /var/log/syslog, /va/log/messages ***

#ifdef USING_SHELL
   // Problem: Commands sometimes produce obscure shell error messages
   log_messages_found = probe_log_using_shell("/var/log/messages",   log_terms, /*ignore_case*/ true, -40, d1);
   log_kern_found     = probe_log_using_shell("/var/log/kern.log",   log_terms, /*ignore_case*/ true, -20, d1);
   log_daemon_found   = probe_log_using_shell("/var/log/daemon.log", log_terms, /*ignore_case*/ true, -10, d1);
   log_syslog_found   = probe_log_using_shell("/var/log/syslog",     log_terms, /*ignore_case*/ true, -50, d1);
#endif

   // Using our own code instead of shell to scan files
   log_messages_found = probe_log_using_api("/var/log/messages",   log_terms, /*ignore_case*/ true, -40, d1);
   log_kern_found     = probe_log_using_api("/var/log/kern.log",   log_terms, /*ignore_case*/ true, -20, d1);
   log_daemon_found   = probe_log_using_api("/var/log/daemon.log", log_terms, /*ignore_case*/ true, -10, d1);
   log_syslog_found   = probe_log_using_api("/var/log/syslog",     log_terms, /*ignore_case*/ true, -50, d1);

   logs_checked |= (LOG_MESSAGES | LOG_KERN | LOG_DAEMON | LOG_SYSLOG);
   if (log_messages_found)
      logs_found |= LOG_MESSAGES;
   if (log_kern_found)
      logs_found |= LOG_KERN;
   if (log_daemon_found)
      logs_found |= LOG_DAEMON;
   if (log_syslog_found)
      logs_found |= LOG_SYSLOG;

   // for now, just report the logs seen to avoid warning about unused vars
#ifdef NO
   rpt_title("Log files found:  ", depth);
   rpt_bool("dmesg"               , NULL, log_dmesg_found,      d1);
   rpt_bool("/var/log/messages"   , NULL, log_messages_found,   d1);
   rpt_bool("journalctl"          , NULL, log_journalctl_found, d1);
   rpt_bool("/var/log/kern"       , NULL, log_kern_found,       d1);
   rpt_bool("/var/log/syslog"     , NULL, log_syslog_found,     d1);
   rpt_bool("/var/log/daemaon"    , NULL, log_daemon_found,     d1);
   rpt_bool("/var/log/Xorg.0.log" , NULL, log_xorg_found,       d1);
#endif
   rpt_nl();
   rpt_title("Log Summary", d1);
   rpt_vstring(d2,  "%-30s  %-7s   %-6s",  "Log", "Checked", "Found");
   rpt_vstring(d2,  "%-30s  %-7s   %-6s",  "===", "=======", "=====");
   for (Value_Name_Title * entry = log_table; entry->title; entry++) {
      rpt_vstring(d2, "%-30s  %-7s   %-6s",
                      entry->title,
                      bool_repr(logs_checked & entry->value),
                      bool_repr(logs_found & entry->value));
   }
   rpt_nl();
   if (log_terms != all_terms)
      ntsa_free(log_terms, false);
   ntsa_free(all_terms, false);
}


/** Examines kernel configuration files and DKMS.
 *
 *  \param  accum  accumulated environment
 */
void probe_config_files(Env_Accumulator * accum) {
   int depth = 0;
   // DBGMSG("Starting");
   // debug_output_dest();

   rpt_nl();
   rpt_title("Examining configuration files...", depth);

   if (accum->is_arm) {
#ifdef OLD
      probe_one_log_using_shell(
            NULL,
            "egrep -i -e\"dtparam\" -e\"dtoverlay\"",
            " /boot/config.txt | grep -v \"^ *#\"",
            "/boot/config.txt",
            depth+1
            );
#endif
      rpt_title("Examining /boot/config.txt:", depth+1);
      execute_shell_cmd_rpt("egrep -i -edtparam -edtoverlay -edevice_tree /boot/config.txt | grep -v \"^ *#\"", depth+2);
      rpt_nl();
      rpt_vstring(depth+1, "Looking for blacklisted drivers in /etc/modprobe.d:");
      execute_shell_cmd_rpt("grep -ir blacklist /etc/modprobe.d | grep -v \"^ *#\"", depth+2);
   }
   else {
      rpt_nl();
      rpt_vstring(0,"DKMS modules:");
      execute_shell_cmd_rpt("dkms status", 1 /* depth */);
      rpt_nl();
      rpt_vstring(0,"Kernel I2C configuration settings:");
      execute_shell_cmd_rpt("grep I2C /boot/config-$(uname -r)", 1 /* depth */);
      rpt_nl();
      rpt_vstring(0,"Kernel AMDGPU configuration settings:");
      execute_shell_cmd_rpt("grep AMDGPU /boot/config-$(uname -r)", 1 /* depth */);
      rpt_nl();
      // TMI:
      // rpt_vstring(0,"Full xrandr --props:");
      // execute_shell_cmd_rpt("xrandr --props", 1 /* depth */);
      // rpt_nl();
   }
}


//
// Mainline
//

/* Master function to query the system environment
 *
 * Arguments:    none
 *
 * Returns:      nothing
 */
void query_sysenv() {
   device_xref_init();

   Env_Accumulator * accumulator = calloc(1, sizeof(Env_Accumulator));

   rpt_nl();
   rpt_vstring(0,"*** Basic System Information ***");
   rpt_nl();
   query_base_env(accumulator);

   rpt_nl();
   rpt_vstring(0,"*** Primary Check 1: Identify video card and driver ***");
   rpt_nl();
   query_card_and_driver_using_sysfs(accumulator);


   rpt_nl();
   rpt_vstring(0,"*** Primary Check 2: Check that /dev/i2c-* exist and writable ***");
   rpt_nl();
   Byte_Value_Array i2c_device_numbers = identify_i2c_devices();
   accumulator->i2c_device_numbers = i2c_device_numbers;
   assert(i2c_device_numbers);
   rpt_vstring(0, "Identified %d I2C devices", bva_length(accumulator->i2c_device_numbers));
   rpt_nl();
   check_i2c_devices(accumulator->driver_list);

   rpt_nl();
   rpt_vstring(0,"*** Primary Check 3: Check that module i2c_dev is loaded ***");
   rpt_nl();
   check_i2c_dev_module(accumulator->driver_list);

   rpt_nl();
   rpt_vstring(0,"*** Primary Check 4: Driver specific checks ***");
   rpt_nl();
   driver_specific_tests(accumulator->driver_list);

   // TODO: move to end of function
   // Free the driver list created by query_card_and_driver_using_sysfs()
   // free_driver_name_list(accumulator->driver_list);
   // driver_list = NULL;

#ifdef OLD
   rpt_nl();
   rpt_vstring(0,"*** Primary Check 5: Installed packages ***");
   rpt_nl();
   query_packages();
   rpt_nl();
#endif

   rpt_nl();
   rpt_vstring(0,"*** Additional probes ***");
   // printf("Gathering card and driver information...\n");
   rpt_nl();
   query_proc_modules_for_video();
   if (!accumulator->is_arm) {
      rpt_nl();
      query_card_and_driver_using_lspci();
   }
   rpt_nl();
   query_loaded_modules_using_sysfs();
   query_i2c_bus_using_sysfs();

   DDCA_Output_Level output_level = get_output_level();
   if (output_level >= DDCA_OL_VERBOSE) {
      rpt_nl();
      query_proc_driver_nvidia();
   }

   if (output_level >= DDCA_OL_VERBOSE) {
      query_i2c_buses();

      rpt_nl();
      rpt_vstring(0,"xrandr connection report:");
      execute_shell_cmd_rpt("xrandr|grep connected", 1 /* depth */);
      rpt_nl();

      rpt_vstring(0,"Checking for possibly conflicting programs...");
      execute_shell_cmd_rpt("ps aux | grep ddccontrol | grep -v grep", 1);
      rpt_nl();

      query_using_i2cdetect(accumulator->i2c_device_numbers);

      raw_scan_i2c_devices();

#ifdef USE_X11
      query_x11();
#endif

      // probe_udev_subsystem() is in udev_util.c, which is only linked in if USE_USB
      probe_i2c_devices_using_udev();

      // temp
      // get_i2c_smbus_devices_using_udev();

      probe_config_files(accumulator);
      probe_logs(accumulator);

#ifdef USE_LIBDRM
      probe_using_libdrm();
#else
      rpt_vstring(0, "Not built with libdrm support.  Skipping DRM related checks");
#endif

      query_drm_using_sysfs();

      device_xref_report(0);
   }

   free_env_accumulator(accumulator);     // make Coverity happy
}

