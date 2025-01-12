#include <assert.h>
#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <pthread.h>
#include <dirent.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/stat.h>

#define LOCALE_COUNT 3
#define MAX_FPATH_SIZE 4096
#define TABLE_SIZE 1024

struct path_entry {
	const char *desk_name;
	const char *icon_path;
};

struct sq_node {
	const char *dirpath;
	struct sq_node *next, *prev;
};

struct search_queue {
	struct sq_node *head, *tail;
};

struct watch {
	const char *fpath;
	const char *ifield;
};

int
hashfunc(const char *key, size_t len)
{
	unsigned int hash = 0;

	for (int i = 0; i < len; i++) {
		hash = 31 * hash + key[i];
	}

	return hash % TABLE_SIZE;
}

void
paths_add_entry(struct path_entry **paths, struct path_entry *entry)
{
	unsigned int hash = hashfunc(entry->desk_name, strlen(entry->desk_name));
	for (int i = 0; i < TABLE_SIZE; i++) {
		if (paths[(hash + i) % TABLE_SIZE] == NULL) {
			paths[(hash + i) % TABLE_SIZE] = entry;
			break;
		}
	}
}

void
paths_debug(struct path_entry **paths)
{
	puts("{");
	for (int i = 0; i < TABLE_SIZE; i++) {
		if (paths[i] != NULL) {
			printf("\t{\"%s\": \"%s\"}\n",
				paths[i]->desk_name, paths[i]->icon_path);
		}
	}
	puts("}");
}

void
sq_add_dir(struct search_queue *sq, const char *dirpath)
{
	struct sq_node *new;
	if ((new = malloc(sizeof(*new))) == NULL) {
		perror("Err reserving memory");
		exit(EXIT_FAILURE);
	}
	new->dirpath = dirpath;
	new->prev = NULL;

	if ((new->next = sq->head) != NULL)
		sq->head->prev = new;
	else
		sq->tail = new;
	sq->head = new;
}

void
sq_del_dir(struct search_queue *sq)
{
	struct sq_node *oldtail;
	if (sq->head == sq->tail)
		sq->head = NULL;
	if ((oldtail = sq->tail) != NULL) {
		sq->tail = oldtail->prev;
		if (sq->tail != NULL)
			sq->tail->next = NULL;
	}

	free(oldtail);
}

int
sq_empty(struct search_queue *sq)
{
	return sq->head == NULL;
}

void
sq_destroy(struct search_queue *sq)
{
	while(!sq_empty(sq)) {
		sq_del_dir(sq);
	}
}

int
file_fast_forward_to(FILE *fp, const char *target, const size_t len)
{
	assert(strlen(target) == len);

	char searchbuf[len];

	for (int filepos = 0; (searchbuf[filepos % len] = fgetc(fp)) != EOF;
	     filepos++) {
		int targetpos;
		for (targetpos = 0; targetpos < len; targetpos++) {
			if (searchbuf[(filepos % len + targetpos + 1) % len] !=
			    target[targetpos])
				break;
		}
		if (targetpos == len)
			break;
	}

	return !feof(fp);
}

const char *
file_copy_line(FILE *fp)
{
	char ch;
	char tmpbuf[MAX_FPATH_SIZE] = {0};

	for (int i = 0; (ch = fgetc(fp)) != EOF && ch != '\n'; i++) {
		tmpbuf[i] = ch;
	}

	return strndup(tmpbuf, MAX_FPATH_SIZE);
}

const char *
find_icon_path(const char *ifield)
{
	if (ifield[0] == '/')
		return strndup(ifield, strlen(ifield));

	const char *ipath = NULL;
	struct search_queue sq = { NULL };
	DIR *dirp;
	struct dirent *dp;
	const char *dirpath;
	char tmpfpath[MAX_FPATH_SIZE];
	const char *fpath;
	struct stat statbuf;
	const char *fname;

	char target[strlen(ifield) + 4];
	snprintf(target, strlen(ifield) + 5, "%s%s",
		ifield, ".png");

	sq_add_dir(&sq, strdup("/usr/share/icons/hicolor/48x48/apps/"));

	while (!sq_empty(&sq)) {
		dirpath = sq.tail->dirpath;
		dirp = opendir(dirpath);

		while ((dp = readdir(dirp)) != NULL) {
			if (dp->d_name[0] == '.')
				continue;

			memset(tmpfpath, 0, sizeof(tmpfpath));
			snprintf(tmpfpath, strlen(dirpath) + strlen(dp->d_name) + 1,
				"%s%s", dirpath, dp->d_name);
			fpath = strndup(tmpfpath, strlen(tmpfpath));

			stat(fpath, &statbuf);
			if (S_ISDIR(statbuf.st_mode)) {
				if (fpath[sizeof(fpath - 1)] != '/') {
					free((void *)fpath);
					tmpfpath[strlen(tmpfpath)] = '/';
					fpath = strndup(tmpfpath, strlen(tmpfpath));
				}
				sq_add_dir(&sq, fpath);
			} else if (S_ISREG(statbuf.st_mode)) {
				int fnamelen;
				for (fnamelen = 0; fnamelen < strlen(fpath); fnamelen++) {
					if (fpath[strlen(fpath) - fnamelen] == '/')
						break;
				}
				if (fnamelen == strlen(fpath))
					continue;

				fname = &fpath[strlen(fpath) - fnamelen + 1];

				if (strcmp(fname, target) == 0) {
					ipath = strdup(fpath);
					sq_destroy(&sq);
					break;
				}
			}
		}
		sq_del_dir(&sq);
	}
	free((void *)fpath);

	return ipath;
}

struct path_entry *icon_paths[TABLE_SIZE];

void
search_dir(DIR *dirp, const char *dirpath, struct search_queue *sq, int in_fd,
	   struct watch *watches)
{
	int wd;
	struct dirent *dp;
	char tmpfpath[MAX_FPATH_SIZE];
	const char *fpath;
	struct stat statbuf;
	const char *fname;
	FILE *fp;

	while ((dp = readdir(dirp)) != NULL) {
		if (dp->d_name[0] == '.')
			continue;

		memset(tmpfpath, 0, sizeof(tmpfpath));
		snprintf(tmpfpath, strlen(dirpath) + strlen(dp->d_name) + 1,
			 "%s%s", dirpath, dp->d_name);
		fpath = strndup(tmpfpath, strlen(tmpfpath));

		stat(fpath, &statbuf);
		if (S_ISDIR(statbuf.st_mode)) {
			if (fpath[sizeof(fpath - 1)] != '/') {
				free((void *)fpath);
				tmpfpath[strlen(tmpfpath)] = '/';
				fpath = strndup(tmpfpath, strlen(tmpfpath));
			}
			sq_add_dir(sq, fpath);
		} else if (S_ISREG(statbuf.st_mode)) {
			if (strncmp(&fpath[strlen(fpath) - 8], ".desktop", 8) !=
			    0)
				continue;

			wd = inotify_add_watch(in_fd, fpath,
					       IN_DELETE_SELF |
					       IN_MOVE_SELF | IN_MODIFY);
			watches[wd].fpath = fpath;

			if ((fp = fopen(fpath, "r")) == NULL) {
				fprintf(stderr, "Failed to open '%s': %s\n", fpath, strerror(errno));
				continue;
			}
			if (!file_fast_forward_to(fp, "\nIcon=", 6)) {
				watches[wd].ifield = NULL;
				continue;
			}

			const char *ifield = file_copy_line(fp);
			watches[wd].ifield = ifield;

			const char *ipath = find_icon_path(ifield);
			if (ipath == NULL)
				continue;

			int fnamelen;
			for (fnamelen = 0; fnamelen < strlen(fpath); fnamelen++) {
				if (fpath[strlen(fpath) - fnamelen] == '/')
					break;
			}
			if (fnamelen == strlen(fpath))
				continue;

			fname = &fpath[strlen(fpath) - fnamelen + 1];

			struct path_entry *icon_info = malloc(sizeof(struct path_entry));
			icon_info->desk_name = strndup(fname, fnamelen - 9);
			icon_info->icon_path = ipath;

			paths_add_entry(icon_paths, icon_info);
		}

		errno = 0;
	}
}

void
populate_paths(struct search_queue sq, const char **top_dirs)
{
	for (int i = 0; i < LOCALE_COUNT; i++) {
		sq_add_dir(&sq,
			   strndup(top_dirs[i], strlen(top_dirs[i])));
	}

	int fd = inotify_init();

	DIR *dirp;
	const char *dirpath;
	int wd;
	struct watch watches[4096];

	while (!sq_empty(&sq)) {
		dirpath = sq.tail->dirpath;
		if ((dirp = opendir(dirpath)) == NULL) {
			fprintf(stderr, "Failed to open dir '%s': %s\n",
				dirpath, strerror(errno));
			sq_del_dir(&sq);
			continue;
		}

		wd = inotify_add_watch(fd, dirpath,
				       IN_MOVE | IN_CREATE | IN_DELETE);
		watches[wd].fpath = dirpath;
		watches[wd].ifield = NULL;

		errno = 0;
		search_dir(dirp, dirpath, &sq, fd, watches);

		if (errno) {
			fprintf(stderr, "Failed reading dir '%s': %s\n",
				dirpath, strerror(errno));
			sq_del_dir(&sq);
			continue;
		}

		sq_del_dir(&sq);
	}
}

void *
manage_paths(void *args)
{
	const char **app_locales = (const char **)args;
	struct search_queue sq = { NULL };
	
	populate_paths(sq, app_locales);

	paths_debug(icon_paths);

	return NULL;
}

int
main(void)
{
	char user_apps[64];
	snprintf(user_apps, sizeof(user_apps), "%s%s",
		 getenv("HOME"), "/.local/share/applications/");
	const char *app_locales[LOCALE_COUNT] = {
		user_apps,
		"/usr/share/applications/",
		"/usr/local/share/applications/"
	};

	pthread_t paths_thread;

	pthread_create(&paths_thread, NULL, manage_paths, app_locales);

	pthread_join(paths_thread, NULL);

	return EXIT_SUCCESS;
}
