#!/bin/bash
#
# This file is part of libzbc.
#
# Copyright (C) 2009-2014, HGST, Inc. All rights reserved.
# Copyright (C) 2016-2018, Western Digital. All rights reserved.
#
# This software is distributed under the terms of the BSD 2-clause license,
# "as is," without technical support, and WITHOUT ANY WARRANTY, without
# even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
# PURPOSE. You should have received a copy of the BSD 2-clause license along
# with libzbc. If not, see  <http://opensource.org/licenses/BSD-2-Clause>.

. scripts/zbc_test_lib.sh

zbc_test_init $0 "OPEN_ZONE empty to explicit open (type=${test_zone_type:-${ZT_SEQ}})" $*

# Get drive information
zbc_test_get_device_info

# Search target LBA
zbc_test_search_wp_zone_cond_or_NA ${ZC_EMPTY}
target_lba=${target_slba}

if [[ ${target_type} != @(${ZT_SEQ}) ]]; then
    expected_sk="Illegal-request"
    expected_asc="Invalid-field-in-cdb"
    expected_cond="${ZC_EMPTY}"		# unchanged
else
    expected_cond="${ZC_EOPEN}"
fi

# Specify post process
zbc_test_case_on_exit zbc_test_run ${bin_path}/zbc_test_reset_zone ${device} ${target_lba}

# Start testing
zbc_test_run ${bin_path}/zbc_test_open_zone -v ${device} ${target_lba}

# Get SenseKey, ASC/ASCQ
zbc_test_get_sk_ascq

# Get target zone condition
zbc_test_get_target_zone_from_slba ${target_lba}

# Check result
if [[ ${target_type} == @(${ZT_SEQ}) ]]; then
    zbc_test_check_zone_cond_wp ${target_slba} \
				"type=${target_type} cond=${target_cond}"
else
    zbc_test_check_sk_ascq_zone_cond "type=${target_type} cond=${target_cond}"
fi
