#!/bin/bash
#
# This file is part of libzbc.
#
# Copyright (C) 2018, Western Digital. All rights reserved.
#
# This software is distributed under the terms of the BSD 2-clause license,
# "as is," without technical support, and WITHOUT ANY WARRANTY, without
# even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
# PURPOSE. You should have received a copy of the BSD 2-clause license along
# with libzbc. If not, see  <http://opensource.org/licenses/BSD-2-Clause>.

. scripts/zbc_test_lib.sh

zbc_test_init $0 "WRITE ${test_io_size:-"one physical"} block(s) empty to implicit open (type=${test_zone_type:-${ZT_SEQ}})" $*

expected_cond="${ZC_IOPEN}"

# Get drive information
zbc_test_get_device_info

# Search target LBA
zbc_test_search_wp_zone_cond_or_NA ${ZC_EMPTY}
write_size=${test_io_size:-${lblk_per_pblk}}

# Specify post process
zbc_test_case_on_exit zbc_test_run ${bin_path}/zbc_test_reset_zone ${device} ${target_slba}

# Start testing
# Write at the start of the zone
zbc_test_run ${bin_path}/zbc_test_write_zone -v ${device} ${target_slba} ${write_size}
zbc_test_fail_exit_if_sk_ascq "WRITE failed, zone_type=${target_type}"

zbc_test_get_target_zone_from_slba ${target_slba}
zbc_test_check_zone_cond_wp $(( ${target_slba} + ${write_size} ))
