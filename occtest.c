#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <unistd.h>
#include <err.h>
#include <errno.h>
#include <stdlib.h>
#include <stdint.h>
#include <getopt.h>
#include <string.h>
#include <pthread.h>

static unsigned long global_flags = 0;

void display_buf(uint8_t *buf, unsigned long size)
{
	unsigned long i;

	for (i = 0; i < size; ++i) {
		printf("%02X ", buf[i]);
		if (!((i + 1) % 16))
			printf("\n");
	}
	printf("\n");
}

int getscom(char *path, uint32_t addr, uint8_t *data)
{
	int rc, fd, count = 0;
	uint32_t buf[5];

	buf[0] = __builtin_bswap32(0x4);
	buf[1] = __builtin_bswap32(0xa201);
	buf[2] = 0;
	buf[3] = __builtin_bswap32(addr);

	fd = open(path, O_RDWR | global_flags);
	if (fd < 0) {
		printf("failed to open %s\n", path);
		return -1;
	}

	rc = write(fd, buf, sizeof(uint32_t) * 4);
	if (rc < 0) {
		printf("failed to getscom %d\n", errno);
		close(fd);
		return rc;
	}

retry:
	rc = read(fd, buf, sizeof(uint32_t) * 5);
	if (rc < 0) {
		if (count < 100 && errno == EAGAIN) {
			count++;
			usleep(5000);
			goto retry;
		}

		printf("failed to read getscom response %d\n", errno);
		close(fd);
		return rc;
	}

	close(fd);

	if ((__builtin_bswap32(buf[2]) >> 16) != 0xC0DE) {
		printf("getscom response is bad\n");
		return -EAGAIN;
	}

	memcpy(data, buf, 8);

	return 0;
}

int putscom(char *path, uint32_t addr, uint32_t data0, uint32_t data1)
{
	int rc, fd, count = 0;
	uint32_t buf[6];

	buf[0] = __builtin_bswap32(0x6);
	buf[1] = __builtin_bswap32(0xa202);
	buf[2] = 0;
	buf[3] = __builtin_bswap32(addr);
	buf[4] = __builtin_bswap32(data0);
	buf[5] = __builtin_bswap32(data1);

	fd = open(path, O_RDWR | global_flags);
	if (fd < 0) {
		printf("failed to open %s\n", path);
		return -1;
	}

	rc = write(fd, buf, sizeof(uint32_t) * 6);
	if (rc < 0) {
		printf("failed to putscom %d\n", errno);
		close(fd);
		return rc;
	}

retry:
	rc = read(fd, buf, sizeof(uint32_t) * 6);
	if (rc < 0) {
		if (count < 100 && errno == EAGAIN) {
			count++;
			usleep(5000);
			goto retry;
		}
		printf("failed to read putscom response %d\n", errno);
		close(fd);
		return rc;
	}

	close(fd);

	return rc;
}

void translate_bus(char *bus, char *new_bus)
{
	int i;

	sscanf(bus, "/dev/occ%d", &i);
	sprintf(new_bus, "/dev/sbefifo%d", i);
}

void setup(char *bus)
{
	int rc, fd, count = 0, i = 1;
	uint64_t scom = 0ULL;
	uint32_t buf[6];
	char new_bus[32];

	translate_bus(bus, new_bus);

	printf("setting up %s\n", new_bus);

	do {
		rc = getscom(new_bus, 0x6d051, (uint8_t *)&scom);
		if (rc == EAGAIN && count++ < 10)
			sleep(2);
		else
			break;
	} while (1);

	if (__builtin_bswap64(scom) == 0x0800000000000000) {
		printf("channel 2 already setup\n");
		return;
	}

	putscom(new_bus, 0x6d053, 0x08000000, 0);
	putscom(new_bus, 0x6d052, 0x04000000, 0);
}

void test_occ(int fd)
{
	int i, rc, total = 0, real_total;
	uint8_t buf[8];
	uint8_t *resp = NULL;

	printf("start test\n");

	buf[0] = 0;	// seq
	buf[1] = 0;	// cmd
	buf[2] = 0;	// data len msb
	buf[3] = 1;	// data len lsb
	buf[4] = 0x20;	// data
	buf[5] = 0;	// chksum msb
	buf[6] = 0x21;	// chksum lsb
	buf[7] = 0;

	rc = write(fd, buf, sizeof(buf));
	if (rc < 0) {
		printf("failed to write %d\n", errno);
		goto done;
	}

	memset(buf, 0, sizeof(buf));

	do {
		rc = read(fd, buf, sizeof(buf));
		if (rc < 0) {
			if (errno == EAGAIN)
				continue;

			printf("failed to read %d\n", errno);
			goto done;
		}
		else if (!rc)
			break;

		total += rc;
	} while (total < sizeof(buf));

	total = (buf[3] << 8) | buf[4];
	if (!total || total > 4096) {
		printf("no/bad data %d\n", total);
		goto done;
	}
	else
		printf("found %d bytes in response\n", total);

	real_total = total + 7;
	resp = malloc(real_total);
	memset(resp, 0, real_total);
	memcpy(resp, buf, sizeof(buf));
	i = sizeof(buf);

	do {
		rc = read(fd, &resp[i], total - 1);
		if (rc < 0) {
			if (errno == EAGAIN)
				continue;

			printf("failed to read %d\n", errno);
			goto done;
		}
		else if (!rc)
			break;

		i += rc;
		total -= rc;
	} while (total > 0);

	display_buf(resp, real_total);
done:
	if (resp)
		free(resp);
}

void test_occ_wrap(char *bus)
{
	int fd;

	fd = open(bus, O_RDWR | global_flags);
	if (fd < 0) {
		printf("failed to open %s\n", bus);
		return;
	}

	test_occ(fd);

	close(fd);
}

void test_raw(char *bus)
{
	int rc;
	int i = 8;
	int len;
	int total = 8;
	char new_bus[32];
	uint8_t *buf = malloc(4096);

	memset(buf, 0, 4096);

	translate_bus(bus, new_bus);

	putscom(new_bus, 0x6d050, 0xfffbe000, 0);
	putscom(new_bus, 0x6d055, 0x1, 0x20002100);
	putscom(new_bus, 0x6d035, 0x20010000, 0);
	putscom(new_bus, 0x6d050, 0xfffbf000, 0);
	rc = getscom(new_bus, 0x6d055, buf);
	if (rc < 0) 
		goto done;

	len = (buf[3] << 8) | buf[4];
	if (!len || len > 4096) {
		printf("no/bad data %d\n", len);
		goto print;
	}
	else
		printf("found %d bytes in response\n", len);

	total = len + 7;

	do {
		rc = getscom(new_bus, 0x6d055, &buf[i]);
		if (rc < 0)
			goto print;

		i += 8;
		len -= 8;
	} while (len > 0);

print:
	display_buf(buf, total);

done:
	free(buf);
}

int main(int argc, char **argv)
{
	char dotest = 0;
	char doread = 1;
	int option;
	unsigned long addr;
	unsigned long long data;
	char bus[32] = "/dev/occ1";
	char new_bus[32];

	while ((option = getopt(argc, argv, "fd:r:w:b:ts")) != -1) {
		switch (option) {
		case 'f':
			global_flags = O_NONBLOCK;
			break;
		case 'd':
			data = strtoull(optarg, NULL, 0);
			break;
		case 'w':
			doread = 0;
		case 'r':
			addr = strtoul(optarg, NULL, 0);
			break;
		case 'b':
			strncpy(bus, optarg, 32);
			break;
		case 't':
			dotest = 1;
			break;
		case 's':
			dotest = 2;
			break;
		default:
			printf("unknown option\n");
		}
	}

	if (dotest) {
		setup(bus);
		if (dotest == 2)
			test_raw(bus);
		else
			test_occ_wrap(bus);
		return 0;
	}

	translate_bus(bus, new_bus);

	if (doread) {
		data = 0ULL;
		getscom(new_bus, addr, (uint8_t *)&data);
		printf("getscom %08X: %016llX\n", addr, __builtin_bswap64(data));
	}
	else {
		printf("putscom %08X: %016llX\n", addr, data);
		putscom(new_bus, addr, (uint32_t)(data >> 32), (uint32_t)(data & 0xFFFFFFFFULL));
	}

	return 0;
}
