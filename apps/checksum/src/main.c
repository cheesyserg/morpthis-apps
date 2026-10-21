#include <dirent.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

// A headless tool: the current player runs it with fork/execve, not as a
// plugin. It reads the files in its own folder (the player sets the working
// directory) or the paths given on the command line.

#define BUFFER_SIZE 4096

static uint32_t checksum_stream(FILE *fp, unsigned long long *size_out) {
	unsigned char buffer[BUFFER_SIZE];
	uint32_t crc = 0xffffffffu;
	unsigned long long size = 0;
	size_t got;

	while ((got = fread(buffer, 1, sizeof(buffer), fp)) > 0) {
		size += got;
		for (size_t i = 0; i < got; i++) {
			crc ^= buffer[i];
			for (int bit = 0; bit < 8; bit++) {
				crc = (crc >> 1) ^ (0xedb88320u & (uint32_t)-(int32_t)(crc & 1u));
			}
		}
	}

	*size_out = size;
	return ~crc;
}

static bool checksum_file(const char *path, const char *label) {
	struct stat st;
	if (stat(path, &st) != 0) {
		fprintf(stderr, "checksum: %s: %s\n", path, strerror(errno));
		return false;
	}
	if (!S_ISREG(st.st_mode)) {
		fprintf(stderr, "checksum: %s: not a regular file\n", path);
		return false;
	}

	FILE *fp = fopen(path, "rb");
	if (!fp) {
		fprintf(stderr, "checksum: %s: %s\n", path, strerror(errno));
		return false;
	}

	unsigned long long size = 0;
	uint32_t crc = checksum_stream(fp, &size);
	bool failed = ferror(fp) != 0;
	fclose(fp);

	if (failed) {
		fprintf(stderr, "checksum: %s: read failed\n", path);
		return false;
	}

	printf("%08lx  %10llu  %s\n", (unsigned long)crc, size, label);
	return true;
}

static bool checksum_here(void) {
	DIR *dir = opendir(".");
	if (!dir) {
		fprintf(stderr, "checksum: .: %s\n", strerror(errno));
		return false;
	}

	bool ok = true;
	struct dirent *entry;
	while ((entry = readdir(dir)) != NULL) {
		if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
			continue;
		}

		// The folder also holds build/ and src/. A scan is about files; an
		// explicit argument that names a directory is still an error.
		struct stat st;
		if (stat(entry->d_name, &st) != 0 || !S_ISREG(st.st_mode)) {
			continue;
		}

		if (!checksum_file(entry->d_name, entry->d_name)) {
			ok = false;
		}
	}

	closedir(dir);
	return ok;
}

int main(int argc, char **argv) {
	bool ok = true;
	if (argc > 1) {
		for (int i = 1; i < argc; i++) {
			if (!checksum_file(argv[i], argv[i])) {
				ok = false;
			}
		}
	} else {
		ok = checksum_here();
	}
	return ok ? 0 : 1;
}
