/*
 * This file is part of libzbc.
 *
 * Copyright (C) 2009-2014, HGST, Inc. All rights reserved.
 * Copyright (C) 2016, Western Digital. All rights reserved.
 *
 * This software is distributed under the terms of the
 * GNU Lesser General Public License version 3, "as is," without technical
 * support, and WITHOUT ANY WARRANTY, without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. You should have
 * received a copy of the GNU Lesser General Public License along with libzbc.
 * If not, see <http://www.gnu.org/licenses/>.
 *
 * Author: Damien Le Moal (damien.lemoal@wdc.com)
 *         Christophe Louargant (christophe.louargant@wdc.com)
 */

#define _GNU_SOURCE     /* O_LARGEFILE & O_DIRECT */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <fcntl.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <linux/fs.h>

#include <libzbc/zbc.h>

/**
 * I/O abort.
 */
static int zbc_read_zone_abort = 0;

/**
 * System time in usecs.
 */
static inline unsigned long long zbc_read_zone_usec(void)
{
	struct timeval tv;

	gettimeofday(&tv, NULL);

	return (unsigned long long) tv.tv_sec * 1000000LL +
		(unsigned long long) tv.tv_usec;
}

/**
 * Signal handler.
 */
static void zbc_read_zone_sigcatcher(int sig)
{
	zbc_read_zone_abort = 1;
}

int main(int argc, char **argv)
{
	struct zbc_device_info info;
	struct zbc_device *dev = NULL;
	unsigned long long elapsed;
	unsigned long long bcount = 0;
	unsigned long long brate;
	int zidx, fd = -1, i;
	ssize_t ret = 1;
	size_t bufsize, iosize;
	void *iobuf = NULL;
	ssize_t sector_count, byte_count;
	unsigned long long ionum = 0, iocount = 0;
	struct zbc_zone *zones = NULL;
	struct zbc_zone *iozone = NULL;
	unsigned int nr_zones;
	char *path, *file = NULL, *end;
	unsigned char *b;
	long long sector_ofst = 0;
	long long sector_max = 0;
	long long zone_ofst = 0;
	int flags = O_RDONLY;
	bool vio = false, ptrn_set = false;
	unsigned long pattern = 0;
	struct iovec *iov = NULL;
	int iovcnt = 1, n;

	/* Check command line */
	if (argc < 4) {
usage:
		printf("Usage: %s [options] <dev> <zone no> <I/O size (B)>\n"
		       "  Read a zone up to the current write pointer\n"
		       "  or the number of I/O specified is executed\n"
		       "Options:\n"
		       "    -v         : Verbose mode\n"
		       "    -dio       : Use direct I/Os\n"
		       "    -vio <num> : Use vectored I/Os with <num> buffers\n"
		       "                 of <I/O size> bytes, resulting in effective\n"
		       "                 I/O size of <num> x <I/O size> B\n"
		       "    -nio <num> : Limit the number of I/Os to <num>\n"
		       "    -p <num>   : Expect all bytes that are read to have\n"
		       "                 the value <num>. If there is a mismatch,\n"
		       "                 the program will output it's data offset\n"
		       "    -f <file>  : Write the content of the zone to <file>\n"
		       "                 If <file> is \"-\", the zone content is\n"
		       "                 written to the standard output\n"
		       "    -ofst      : sector offset from the start sector of\n"
		       "                 the zone (default 0 or write pointer)\n",
		       argv[0]);
		return 1;
	}

	/* Parse options */
	for (i = 1; i < (argc - 1); i++) {

		if (strcmp(argv[i], "-v") == 0) {

			zbc_set_log_level("debug");

		} else if (strcmp(argv[i], "-p") == 0) {

			if (i >= (argc - 1))
				goto usage;
			i++;

			pattern = strtol(argv[i], &end, 0);
			if (*end != '\0' || errno != 0) {
				fprintf(stderr,
					"Invalid data pattern value \"%s\"\n",
					argv[i]);
				goto usage;
			}
			if (pattern > 0xff) {
				fprintf(stderr,
					"Not a single-byte pattern:\"%s\"\n",
					argv[i]);
				goto usage;
			}
			ptrn_set = true;

		} else if (strcmp(argv[i], "-dio") == 0) {

			flags |= O_DIRECT;

		} else if (strcmp(argv[i], "-vio") == 0) {

			if (i >= (argc - 1))
				goto usage;
			i++;

			iovcnt = atoi(argv[i]);
			if (iovcnt <= 0) {
				fprintf(stderr,
					"Invalid number of VIO buffers\n");
				return 1;
			}
			vio = true;

		} else if (strcmp(argv[i], "-nio") == 0) {

			if (i >= (argc - 1))
				goto usage;
			i++;

			ionum = atoi(argv[i]);
			if (ionum <= 0) {
				fprintf(stderr, "Invalid number of I/Os\n");
				return 1;
			}

		} else if (strcmp(argv[i], "-f") == 0) {

			if (i >= (argc - 1))
				goto usage;
			i++;

			file = argv[i];

		} else if (strcmp(argv[i], "-ofst") == 0) {

			if (i >= (argc - 1))
				goto usage;
			i++;

			zone_ofst = atoll(argv[i]);
			if (zone_ofst < 0) {
				fprintf(stderr, "Invalid zone sector offset\n");
				return 1;
			}

		} else if (argv[i][0] == '-') {

			fprintf(stderr, "Unknown option \"%s\"\n", argv[i]);
			goto usage;

		} else {

			break;

		}

	}

	if (i != (argc - 3))
		goto usage;

	/* Get parameters */
	path = argv[i];
	zidx = atoi(argv[i + 1]);
	if (zidx < 0) {
		fprintf(stderr, "Invalid zone number %s\n", argv[i + 1]);
		return 1;
	}

	bufsize = atol(argv[i + 2]);
	if (!bufsize) {
		fprintf(stderr, "Invalid buffer (I/O) size %s\n", argv[i + 2]);
		return 1;
	}

	/* Setup signal handler */
	signal(SIGQUIT, zbc_read_zone_sigcatcher);
	signal(SIGINT, zbc_read_zone_sigcatcher);
	signal(SIGTERM, zbc_read_zone_sigcatcher);

	/* Open device */
	ret = zbc_open(path, flags, &dev);
	if (ret != 0) {
		if (ret == -ENODEV)
			fprintf(stderr,
				"Open %s failed (not a zoned block device)\n",
				path);
		else
			fprintf(stderr, "Open %s failed (%s)\n",
				path, strerror(-ret));
		return 1;
	}

	zbc_get_device_info(dev, &info);

	printf("Device %s:\n", path);
	zbc_print_device_info(&info, stdout);

	/* Get zone list */
	ret = zbc_list_zones(dev, 0, ZBC_RO_ALL, &zones, &nr_zones);
	if (ret != 0) {
		fprintf(stderr, "zbc_list_zones failed\n");
		ret = 1;
		goto out;
	}

	/* Get target zone */
	if ((unsigned int)zidx >= nr_zones) {
		fprintf(stderr, "Target zone not found\n");
		ret = 1;
		goto out;
	}
	iozone = &zones[zidx];

	if (zbc_zone_conventional(iozone))
		printf("Target zone: Conventional zone %d / %d, "
		       "sector %llu, %llu sectors\n",
		       zidx,
		       nr_zones,
		       zbc_zone_start(iozone),
		       zbc_zone_length(iozone));
	else
		printf("Target zone: Zone %d / %d, type 0x%x (%s), "
		       "cond 0x%x (%s), rwp %d, non_seq %d, "
		       "sector %llu, %llu sectors, wp %llu\n",
		       zidx,
		       nr_zones,
		       zbc_zone_type(iozone),
		       zbc_zone_type_str(zbc_zone_type(iozone)),
		       zbc_zone_condition(iozone),
		       zbc_zone_condition_str(zbc_zone_condition(iozone)),
		       zbc_zone_rwp_recommended(iozone),
		       zbc_zone_non_seq(iozone),
		       zbc_zone_start(iozone),
		       zbc_zone_length(iozone),
		       zbc_zone_wp(iozone));

	/* Check I/O alignment and get an I/O buffer */
	if (bufsize % info.zbd_lblock_size) {
		fprintf(stderr,
			"Invalid I/O size %zu (must be a multiple of %u B)\n",
			bufsize, (unsigned int) info.zbd_lblock_size);
		ret = 1;
		goto out;
	}

	if (vio) {
		iov = calloc(iovcnt, sizeof(struct iovec));
		if (!iov) {
			fprintf(stderr, "No memory for I/O vector\n");
			ret = -ENOMEM;
			goto out;
		}
	}

	iosize = bufsize * iovcnt;
	ret = posix_memalign((void **) &iobuf, sysconf(_SC_PAGESIZE), iosize);
	if (ret != 0) {
		fprintf(stderr, "No memory for I/O buffer (%zu B)\n", iosize);
		ret = 1;
		goto out;
	}

	/* Open the file to write, if any */
	if (file) {

		if (strcmp(file, "-") == 0) {

			fd = fileno(stdout);
			printf("Writing target zone %d to standard output, "
			       "%zu B I/Os\n",
			       zidx, iosize);

		} else {

			fd = open(file,
				  O_CREAT | O_TRUNC | O_LARGEFILE | O_WRONLY,
				  S_IRUSR | S_IWUSR | S_IRGRP);
			if (fd < 0) {
				fprintf(stderr,
					"Open file \"%s\" failed %d (%s)\n",
					file,
					errno,
					strerror(errno));
				ret = 1;
				goto out;
			}

			printf("Writing target zone %d data to file \"%s\", "
			       "%zu B I/Os\n",
			       zidx, file, iosize);

		}

	} else if (!ionum) {

		printf("Reading target zone %d, %zu B I/Os\n",
		       zidx, iosize);

	} else {

		printf("Reading target zone %d, %llu I/Os of %zu B\n",
		       zidx, ionum, iosize);

	}

	if (zbc_zone_sequential_req(iozone) && !zbc_zone_full(iozone))
		sector_max = zbc_zone_wp(iozone) - zbc_zone_start(iozone);
	else
		sector_max = zbc_zone_length(iozone);

	elapsed = zbc_read_zone_usec();

	while (!zbc_read_zone_abort) {

		/* Do not exceed the end of the zone */
		sector_count = iosize >> 9;
		if (zone_ofst + sector_count > sector_max)
			sector_count = sector_max - zone_ofst;
		if (!sector_count)
			break;

		sector_ofst = zbc_zone_start(iozone) + zone_ofst;

		/* Read zone */
		if (vio) {
			n = zbc_map_iov(iobuf, sector_count,
					iov, iovcnt, bufsize >> 9);
			if (n < 0) {
				fprintf(stderr, "iov map failed %d (%s)\n",
					-n, strerror(-n));
				ret = 1;
				goto out;
			}
			ret = zbc_preadv(dev, iov, n, sector_ofst);
		} else {
			ret = zbc_pread(dev, iobuf, sector_count, sector_ofst);
		}
		if (ret <= 0) {
			fprintf(stderr, "%s failed %zd (%s)\n",
				vio ? "zbc_preadv" : "zbc_pread",
				-ret, strerror(-ret));
			ret = 1;
			goto out;
		}

		sector_count = ret;
		byte_count = sector_count << 9;

		if (ptrn_set) {
			for (i = 0, b = iobuf; i < byte_count; i++, b++) {
				if (*b != (unsigned char)pattern) {
					fprintf(stderr,
						"Data mismatch @%llu: read %#x, exp %#lx\n",
						sector_ofst + i, *b, pattern);
					ret = ERANGE;
					break;
				}
			}
		}

		if (file) {
			/* Write zone data to output file */
			ret = write(fd, iobuf, byte_count);
			if (ret < 0) {
				fprintf(stderr, "Write file \"%s\" failed %d (%s)\n",
					file,
					errno, strerror(errno));
				ret = 1;
				goto out;
			}
		}

		zone_ofst += sector_count;
		bcount += byte_count;
		iocount++;
		ret = 0;

		if (ionum > 0 && iocount >= ionum)
			break;

	}

	elapsed = zbc_read_zone_usec() - elapsed;
	if (elapsed) {
		printf("Read %llu B (%llu I/Os) in %llu.%03llu sec\n",
		       bcount,
		       iocount,
		       elapsed / 1000000,
		       (elapsed % 1000000) / 1000);
		printf("  IOPS %llu\n",
		       iocount * 1000000 / elapsed);
		brate = bcount * 1000000 / elapsed;
		printf("  BW %llu.%03llu MB/s\n",
		       brate / 1000000,
		       (brate % 1000000) / 1000);
	} else {
		printf("Read %llu B (%llu I/Os)\n",
		       bcount,
		       iocount);
	}

out:
	if (file && fd > 0) {
		if (fd != fileno(stdout))
			close(fd);
		if (ret != 0)
			unlink(file);
	}

	free(iobuf);
	free(zones);
	free(iov);

	zbc_close(dev);

	return ret;
}

