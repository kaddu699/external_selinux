#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <getopt.h>
#include <errno.h>
#include <stdbool.h>
#include <sys/xattr.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <selinux/selinux.h>
#include <selinux/label.h>
#include <selinux/restorecon.h>

#include "restore.h"

static __attribute__((__noreturn__)) void usage(const char *progname)
{
	fprintf(stderr,
		"\nusage: %s [-vnrmdD] [-e directory] [-f specfile] pathname\n"
		"\nWhere:\n\t"
		"-v  Display digest generated by specfile set.\n\t"
		"-n  Do not append \"Match\" or \"No Match\" to displayed digests.\n\t"
		"-r  Recursively descend directories.\n\t"
		"-m  Do not read /proc/mounts for entries to be excluded.\n\t"
		"-d  Delete non-matching digest entries.\n\t"
		"-D  Delete all digest entries.\n\t"
		"-e  Directory to exclude (repeat option for more than one directory).\n\t"
		"-f  Optional specfile for calculating the digest.\n\t"
		"pathname  Path to search for xattr \"security.sehash\" entries.\n\n",
		progname);
	exit(-1);
}

int main(int argc, char **argv)
{
	int opt, rc;
	unsigned int xattr_flags = 0, delete_digest = 0, recurse = 0;
	unsigned int delete_all_digests = 0, ignore_mounts = 0;
	bool display_digest = false;
	char *sha1_buf, **specfiles, *fc_file = NULL;
	unsigned char *fc_digest = NULL;
	size_t i, fc_digest_len = 0, num_specfiles;

	struct stat sb;
	struct selabel_handle *hnd = NULL;
	struct dir_xattr *current, *next, **xattr_list = NULL;

	bool no_comment = true;

	if (argc < 2)
		usage(argv[0]);

	if (is_selinux_enabled() <= 0) {
		fprintf(stderr,
		    "SELinux must be enabled to perform this operation.\n");
		exit(-1);
	}

	exclude_list = NULL;

	while ((opt = getopt(argc, argv, "vnrmdDe:f:")) > 0) {
		switch (opt) {
		case 'v':
			display_digest = true;
			break;
		case 'n':
			no_comment = false;
			break;
		case 'r':
			recurse = SELINUX_RESTORECON_XATTR_RECURSE;
			break;
		case 'm':
			ignore_mounts = SELINUX_RESTORECON_XATTR_IGNORE_MOUNTS;
			break;
		case 'd':
			delete_digest =
			    SELINUX_RESTORECON_XATTR_DELETE_NONMATCH_DIGESTS;
			break;
		case 'D':
			delete_all_digests =
			    SELINUX_RESTORECON_XATTR_DELETE_ALL_DIGESTS;
			break;
		case 'e':
			if (lstat(optarg, &sb) < 0 && errno != EACCES) {
				fprintf(stderr, "Can't stat exclude path \"%s\", %s - ignoring.\n",
					optarg, strerror(errno));
				break;
			}
			add_exclude(optarg);
			break;
		case 'f':
			fc_file = optarg;
			break;
		default:
			usage(argv[0]);
		}
	}

	if (optind >= argc) {
		fprintf(stderr, "No pathname specified\n");
		exit(-1);
	}

	struct selinux_opt selinux_opts[] = {
		{ SELABEL_OPT_PATH, fc_file },
		{ SELABEL_OPT_DIGEST, (char *)1 }
	};

	hnd = selabel_open(SELABEL_CTX_FILE, selinux_opts, 2);
	if (!hnd) {
		switch (errno) {
		case EOVERFLOW:
			fprintf(stderr, "Error: Number of specfiles or"
				 " specfile buffer caused an overflow.\n");
			break;
		default:
			fprintf(stderr, "Error: selabel_open: %s\n",
							    strerror(errno));
		}
		exit(-1);
	}

	/* Use own handle as need to allow different file_contexts. */
	selinux_restorecon_set_sehandle(hnd);

	if (display_digest) {
		if (selabel_digest(hnd, &fc_digest, &fc_digest_len,
				   &specfiles, &num_specfiles) < 0) {
			fprintf(stderr,
				"Error: selabel_digest: Digest not available.\n");
			selabel_close(hnd);
			exit(-1);
		}

		sha1_buf = malloc(fc_digest_len * 2 + 1);
		if (!sha1_buf) {
			fprintf(stderr,
				"Error allocating digest buffer: %s\n",
							    strerror(errno));
			selabel_close(hnd);
			exit(-1);
		}

		for (i = 0; i < fc_digest_len; i++)
			sprintf((&sha1_buf[i * 2]), "%02x", fc_digest[i]);

		printf("specfiles SHA1 digest: %s\n", sha1_buf);

		printf("calculated using the following specfile(s):\n");
		if (specfiles) {
			for (i = 0; i < num_specfiles; i++)
				printf("%s\n", specfiles[i]);
		}
		free(sha1_buf);
		printf("\n");
	}

	if (exclude_list)
		selinux_restorecon_set_exclude_list
						 ((const char **)exclude_list);

	xattr_flags = delete_digest | delete_all_digests |
		      ignore_mounts | recurse;

	if (selinux_restorecon_xattr(argv[optind], xattr_flags, &xattr_list)) {
		fprintf(stderr,
			"Error selinux_restorecon_xattr: %s\n",
			strerror(errno));
		rc = -1;
		goto out;
	}

	if (xattr_list) {
		current = *xattr_list;
		while (current) {
			next = current->next;
			printf("%s ", current->directory);

			switch (current->result) {
			case MATCH:
				printf("Digest: %s%s", current->digest,
				       no_comment ? " Match\n" : "\n");
				break;
			case NOMATCH:
				printf("Digest: %s%s", current->digest,
				       no_comment ? " No Match\n" : "\n");
				break;
			case DELETED_MATCH:
				printf("Deleted Digest: %s%s", current->digest,
				       no_comment ? " Match\n" : "\n");
				break;
			case DELETED_NOMATCH:
				printf("Deleted Digest: %s%s",
				       current->digest,
				       no_comment ? " No Match\n" : "\n");
				break;
			case ERROR:
				printf("Digest: %s Error removing xattr\n",
				       current->digest);
				break;
			}
			current = next;
		}
		/* Free memory */
		current = *xattr_list;
		while (current) {
			next = current->next;
			free(current->directory);
			free(current->digest);
			free(current);
			current = next;
		}
	}

	rc = 0;
out:
	selabel_close(hnd);
	restore_finish();
	return rc;
}
