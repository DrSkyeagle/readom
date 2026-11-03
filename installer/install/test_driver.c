/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Runs inst_ensure() once with fake surroundings, for test_install.py.
 *
 *   test_driver RELEASE USER WORK [option...]
 *     RELEASE        key=value lines: version, built, data_format, payload_name, payload_url,
 *                    payload_bytes, payload_sha256, system_bytes, system_sha256, template
 *                    (a file, read into memory), template_bytes, template_sha256,
 *                    data_bytes, data_sha256, boot_image, build_incremental
 *     battery=N|unknown  charging=0|1  online=0|1  now=SECONDS
 *     serve=FILE     what the "server" holds
 *     attempt=STATUS:HTTP:BYTES   one per download call, in order: copy at most BYTES
 *                    (-1: all) of the server's file after what the part holds, then
 *                    return STATUS with HTTP; once the list is used up: copy all, 0, 200
 *     die=STEP:BYTES end the process (exit 99) once STEP (system or data) reports BYTES
 *
 * Prints "result CODE short_bytes upgraded repaired" and the installer's log lines ("log ...").
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "install.h"

struct fake {
	int battery, battery_known, charging, online;
	long long now;
	const char *serve;
	int attempts, attempt_status[16], attempt_http[16];
	long long attempt_bytes[16];
	int used;
	int die_step;
	unsigned long long die_bytes;
	int downloads;
};

static int battery(void *ctx, int *percent, int *charging)
{
	struct fake *f = ctx;
	*percent = f->battery;
	*charging = f->charging;
	return f->battery_known ? 0 : -1;
}

static int online(void *ctx) { return ((struct fake *)ctx)->online; }
static int64_t now(void *ctx) { return ((struct fake *)ctx)->now; }
static void wait_seconds(void *ctx, unsigned s) { (void)ctx; printf("wait %u\n", s); }

static int download(void *ctx, const char *url, const char *part, const char *ca, uint64_t cap,
		    void (*progress)(void *, uint64_t), void *pctx, int *http)
{
	struct fake *f = ctx;
	struct stat st;
	f->downloads++;
	if (stat(ca, &st) || st.st_size == 0) {
		fprintf(stderr, "no CA bundle at %s\n", ca);
		exit(3);
	}
	printf("download %s\n", url);
	int status = 0;
	long long limit = -1;
	*http = 200;
	if (f->used < f->attempts) {
		status = f->attempt_status[f->used];
		*http = f->attempt_http[f->used];
		limit = f->attempt_bytes[f->used];
		f->used++;
	}
	int in = open(f->serve, O_RDONLY);
	int out = open(part, O_WRONLY | O_CREAT | O_APPEND, 0644);
	if (in < 0 || out < 0 || fstat(out, &st)) {
		perror("fake download");
		exit(3);
	}
	lseek(in, st.st_size, SEEK_SET);
	char buf[65536];
	long long copied = 0;
	for (;;) {
		size_t want = sizeof(buf);
		if (limit >= 0 && (long long)want > limit - copied)
			want = (size_t)(limit - copied);
		if (!want)
			break;
		ssize_t n = read(in, buf, want);
		if (n <= 0)
			break;
		if (write(out, buf, (size_t)n) != n) {
			perror("fake download write");
			exit(3);
		}
		copied += n;
		progress(pctx, (uint64_t)(st.st_size + copied));
		if ((uint64_t)(st.st_size + copied) > cap) {	/* as kindle.c does */
			close(in);
			close(out);
			return 63;
		}
	}
	close(in);
	close(out);
	return status;
}

static void show(void *ctx, enum inst_step step, uint64_t done, uint64_t total)
{
	struct fake *f = ctx;
	(void)total;
	if (f->die_step && (int)step == f->die_step && done >= f->die_bytes) {
		printf("died at step %d, %llu bytes\n", (int)step, (unsigned long long)done);
		fflush(stdout);
		_exit(99);
	}
}

static void log_line(void *ctx, const char *line)
{
	(void)ctx;
	printf("log %s\n", line);
}

static void sha_from_hex(const char *h, uint8_t out[32])
{
	if (strlen(h) != 64) {
		fprintf(stderr, "bad sha256 %s\n", h);
		exit(2);
	}
	for (int i = 0; i < 32; i++)
		sscanf(h + 2 * i, "%2hhx", &out[i]);
}

static unsigned char *slurp(const char *p, size_t *bytes)
{
	FILE *f = fopen(p, "rb");
	if (!f) {
		perror(p);
		exit(2);
	}
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	unsigned char *b = malloc((size_t)n + 1);
	if (!b || fread(b, 1, (size_t)n, f) != (size_t)n) {
		perror(p);
		exit(2);
	}
	b[n] = 0;
	fclose(f);
	*bytes = (size_t)n;
	return b;
}

int main(int argc, char **argv)
{
	if (argc < 4) {
		fprintf(stderr, "usage: test_driver RELEASE USER WORK [option...]\n");
		return 2;
	}
	static char strings[16][512];
	struct inst_release rel = { 0 };
	FILE *f = fopen(argv[1], "r");
	char line[1024];
	int k = 0;
	static const unsigned char ca[] = "-----BEGIN CERTIFICATE-----\ntest\n-----END CERTIFICATE-----\n";
	rel.ca_bundle = ca;
	rel.ca_bundle_bytes = sizeof(ca) - 1;
	while (f && fgets(line, sizeof(line), f)) {
		line[strcspn(line, "\n")] = 0;
		char *v = strchr(line, '=');
		if (!v)
			continue;
		*v++ = 0;
		if (!strcmp(line, "version"))
			rel.version = strcpy(strings[k++], v);
		else if (!strcmp(line, "built"))
			rel.built = atoll(v);
		else if (!strcmp(line, "data_format"))
			rel.data_format = (unsigned)atoi(v);
		else if (!strcmp(line, "payload_name"))
			rel.payload_name = strcpy(strings[k++], v);
		else if (!strcmp(line, "payload_url")) {
			/* Repeatable: one line per origin, in the order the installer tries
			 * them. The fixtures that give it once describe a one-origin
			 * release, which is what every test before origins existed meant. */
			if (rel.payload_origins >= INST_MAX_ORIGINS) {
				fprintf(stderr, "too many payload_url lines\n");
				return 2;
			}
			rel.payload_urls[rel.payload_origins++] = strcpy(strings[k++], v);
		}
		else if (!strcmp(line, "payload_bytes"))
			rel.payload.bytes = strtoull(v, NULL, 10);
		else if (!strcmp(line, "payload_sha256"))
			sha_from_hex(v, rel.payload.sha256);
		else if (!strcmp(line, "system_bytes"))
			rel.system.bytes = strtoull(v, NULL, 10);
		else if (!strcmp(line, "system_sha256"))
			sha_from_hex(v, rel.system.sha256);
		else if (!strcmp(line, "template")) {
			size_t n;
			rel.template_data = slurp(v, &n);
		} else if (!strcmp(line, "template_bytes"))
			rel.template_file.bytes = strtoull(v, NULL, 10);
		else if (!strcmp(line, "template_sha256"))
			sha_from_hex(v, rel.template_file.sha256);
		else if (!strcmp(line, "data_bytes"))
			rel.data.bytes = strtoull(v, NULL, 10);
		else if (!strcmp(line, "data_sha256"))
			sha_from_hex(v, rel.data.sha256);
		else if (!strcmp(line, "boot_image"))
			sha_from_hex(v, rel.boot_image);
		else if (!strcmp(line, "build_incremental"))
			rel.build_incremental = strcpy(strings[k++], v);
	}
	if (!f || !rel.version || !rel.template_data || !rel.build_incremental) {
		fprintf(stderr, "incomplete release file\n");
		return 2;
	}
	fclose(f);

	struct fake fake = { .battery = 100, .battery_known = 1, .online = 1, .now = rel.built };
	for (int i = 4; i < argc; i++) {
		char *a = argv[i];
		if (!strncmp(a, "battery=", 8)) {
			fake.battery_known = strcmp(a + 8, "unknown") != 0;
			fake.battery = atoi(a + 8);
		} else if (!strncmp(a, "charging=", 9))
			fake.charging = atoi(a + 9);
		else if (!strncmp(a, "online=", 7))
			fake.online = atoi(a + 7);
		else if (!strncmp(a, "now=", 4))
			fake.now = atoll(a + 4);
		else if (!strncmp(a, "serve=", 6))
			fake.serve = a + 6;
		else if (!strncmp(a, "attempt=", 8) && fake.attempts < 16) {
			int j = fake.attempts++;
			if (sscanf(a + 8, "%d:%d:%lld", &fake.attempt_status[j], &fake.attempt_http[j], &fake.attempt_bytes[j]) != 3)
				return 2;
		} else if (!strncmp(a, "die=", 4)) {
			char step[16];
			if (sscanf(a + 4, "%15[a-z]:%llu", step, &fake.die_bytes) != 2)
				return 2;
			fake.die_step = !strcmp(step, "system") ? INST_STEP_SYSTEM : INST_STEP_DATA;
		} else {
			fprintf(stderr, "unknown option %s\n", a);
			return 2;
		}
	}
	struct inst_ops ops = { .ctx = &fake, .battery = battery, .online = online, .now = now, .download = download,
				.wait = wait_seconds, .show = show, .log = log_line };
	struct inst_paths paths = { .user = argv[2], .work = argv[3] };
	struct inst_result res;
	enum inst_code c = inst_ensure(&rel, &paths, &ops, &res);
	printf("downloads %d\n", fake.downloads);
	printf("result %d %llu %d %d\n", (int)c, (unsigned long long)res.short_bytes, res.upgraded, res.repaired);
	free((void *)rel.template_data);
	return 0;
}
