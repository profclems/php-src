/*
  +----------------------------------------------------------------------+
  | PHP Version 7                                                        |
  +----------------------------------------------------------------------+
  | Copyright (c) 1997-2016 The PHP Group                                |
  +----------------------------------------------------------------------+
  | This source file is subject to version 3.01 of the PHP license,      |
  | that is bundled with this package in the file LICENSE, and is        |
  | available through the world-wide-web at the following url:           |
  | http://www.php.net/license/3_01.txt.                                 |
  | If you did not receive a copy of the PHP license and are unable to   |
  | obtain it through the world-wide-web, please send a note to          |
  | license@php.net so we can mail you a copy immediately.               |
  +----------------------------------------------------------------------+
  | Author: Piere-Alain Joye <pierre@php.net>                            |
  +----------------------------------------------------------------------+
*/

#ifdef HAVE_CONFIG_H
#   include "config.h"
#endif
#include "php.h"
#if HAVE_ZIP

#include "php_streams.h"
#include "ext/standard/file.h"
#include "ext/standard/php_string.h"
#include "fopen_wrappers.h"
#include "php_zip.h"

#include "ext/standard/url.h"

struct php_zip_stream_data_t {
	struct zip *za;
	struct zip_file *zf;
	size_t cursor;
};

#define STREAM_DATA_FROM_STREAM() \
	struct php_zip_stream_data_t *self = (struct php_zip_stream_data_t *) stream->abstract;


/* {{{ php_zip_ops_read */
static size_t php_zip_ops_read(php_stream *stream, char *buf, size_t count)
{
	ssize_t n = 0;
	STREAM_DATA_FROM_STREAM();

	if (self->za && self->zf) {
		n = zip_fread(self->zf, buf, count);
		if (n < 0) {
#if LIBZIP_VERSION_MAJOR < 1
			int ze, se;
			zip_file_error_get(self->zf, &ze, &se);
			stream->eof = 1;
			php_error_docref(NULL, E_WARNING, "Zip stream error: %s", zip_file_strerror(self->zf));
#else
			zip_error_t *err;
			err = zip_file_get_error(self->zf);
			stream->eof = 1;
			php_error_docref(NULL, E_WARNING, "Zip stream error: %s", zip_error_strerror(err));
			zip_error_fini(err);
#endif
			return 0;
		}
		/* cast count to signed value to avoid possibly negative n
		 * being cast to unsigned value */
		if (n == 0 || n < (ssize_t)count) {
			stream->eof = 1;
		} else {
			self->cursor += n;
		}
	}
	return (n < 1 ? 0 : (size_t)n);
}
/* }}} */

/* {{{ php_zip_ops_write */
static size_t php_zip_ops_write(php_stream *stream, const char *buf, size_t count)
{
	php_error_docref(NULL, E_WARNING, "Writing to zip:// streams is not supported");
	return 0;
}
/* }}} */

/* {{{ php_zip_ops_close */
static int php_zip_ops_close(php_stream *stream, int close_handle)
{
	STREAM_DATA_FROM_STREAM();
	if (!close_handle) {
		/* We're the only ones who know our internal data struct,
		 * nobody should be trying to abscond with our handles
		 */
		php_error_docref(NULL, E_ERROR, "zip:// close called with PRESERVE_HANDLE");
		return EOF;
	}

	if (self->zf) {
		zip_fclose(self->zf);
		self->zf = NULL;
	}

	if (self->za) {
		zip_close(self->za);
		self->za = NULL;
	}

	efree(self);
	stream->abstract = NULL;
	return EOF;
}
/* }}} */

static int php_zip_ops_stat(php_stream *stream, php_stream_statbuf *ssb) /* {{{ */
{
	const char *path = stream->orig_path;
	int path_len = strlen(stream->orig_path);
	char file_dirname[MAXPATHLEN];
	struct zip *za;
	char *fragment;
	int fragment_len;
	int err;

	fragment = strchr(path, '#');
	if (!fragment) {
		return -1;
	}
	++fragment;
	fragment_len = strlen(fragment);
	if (fragment_len < 1) {
		return -1;
	}

	if (strncasecmp("zip://", path, 6) == 0) {
		path += 6;
	}
	path_len = fragment - path - 1;
	if (path_len >= MAXPATHLEN) {
		return -1;
	}

	memcpy(file_dirname, path, path_len);
	file_dirname[path_len] = '\0';
	if (ZIP_OPENBASEDIR_CHECKPATH(file_dirname)) {
		return -1;
	}

	za = zip_open(file_dirname, ZIP_CREATE, &err);
	if (za) {
		struct zip_stat sb;
		memset(ssb, 0, sizeof(php_stream_statbuf));
		if (zip_stat(za, fragment, ZIP_FL_NOCASE, &sb) != 0) {
			zip_close(za);
			return -1;
		}
		zip_close(za);

		/* This is unreliable.  We're depending on how the stat was asked for
		 * in /guessing/ whether or not the file is a directory.
		 * Leaving this along for now, but we need a better way.
		 */
		if (fragment[fragment_len-1] != '/') {
			ssb->sb.st_size = sb.size;
			ssb->sb.st_mode |= S_IFREG; /* regular file */
		} else {
			ssb->sb.st_size = 0;
			ssb->sb.st_mode |= S_IFDIR; /* regular directory */
		}

		ssb->sb.st_mtime = sb.mtime;
		ssb->sb.st_atime = sb.mtime;
		ssb->sb.st_ctime = sb.mtime;
		ssb->sb.st_nlink = 1;
		ssb->sb.st_rdev = -1;
#ifndef PHP_WIN32
		ssb->sb.st_blksize = -1;
		ssb->sb.st_blocks = -1;
#endif
		ssb->sb.st_ino = -1;
	}
	return 0;
}
/* }}} */

php_stream_ops php_stream_zipio_ops = {
	php_zip_ops_write,
	php_zip_ops_read,
	php_zip_ops_close,
	NULL, /* flush */
	"zip",
	NULL, /* seek */
	NULL, /* cast */
	php_zip_ops_stat, /* stat */
	NULL  /* set_option */
};

/* {{{ php_stream_zip_open */
php_stream *php_stream_zip_open(const char *filename, const char *path, const char *mode STREAMS_DC)
{
	struct zip_file *zf = NULL;
	int err = 0;

	php_stream *stream = NULL;
	struct php_zip_stream_data_t *self;
	struct zip *stream_za;

	if (strncmp(mode,"r", strlen("r")) != 0) {
		return NULL;
	}

	if (filename) {
		if (ZIP_OPENBASEDIR_CHECKPATH(filename)) {
			return NULL;
		}

		/* duplicate to make the stream za independent (esp. for MSHUTDOWN) */
		stream_za = zip_open(filename, ZIP_CREATE, &err);
		if (!stream_za) {
			return NULL;
		}

		zf = zip_fopen(stream_za, path, 0);
		if (zf) {
			self = emalloc(sizeof(*self));

			self->za = stream_za;
			self->zf = zf;
			self->cursor = 0;
			stream = php_stream_alloc(&php_stream_zipio_ops, self, NULL, mode);
			stream->orig_path = estrdup(path);
		} else {
			zip_close(stream_za);
		}
	}

	return stream;
}
/* }}} */

/* {{{ php_stream_zip_opener */
php_stream *php_stream_zip_opener(php_stream_wrapper *wrapper,
											const char *path,
											const char *mode,
											int options,
											zend_string **opened_path,
											php_stream_context *context STREAMS_DC)
{
	char file_dirname[MAXPATHLEN];
	char *fragment;
	int path_len, fragment_len, err;
	struct zip *za;
	php_stream *stream = NULL;

	fragment = strchr(path, '#');
	if (!fragment) {
		return NULL;
	}
	fragment++;
	fragment_len = strlen(fragment);
	if (!fragment_len) {
		return NULL;
	}

	if (strncasecmp("zip://", path, 6) == 0) {
		path += 6;
	}
	path_len = fragment - path - 1;;

	if (path_len >= MAXPATHLEN || mode[0] != 'r') {
		return NULL;
	}

	memcpy(file_dirname, path, path_len);
	file_dirname[path_len] = '\0';
	if (ZIP_OPENBASEDIR_CHECKPATH(file_dirname)) {
		return NULL;
	}

	za = zip_open(file_dirname, ZIP_CREATE, &err);
	if (za) {
		struct zip_file *zf = zip_fopen(za, fragment, 0);
		if (zf) {
			struct php_zip_stream_data_t *self = emalloc(sizeof(*self));

			self->za = za;
			self->zf = zf;
			self->cursor = 0;
			stream = php_stream_alloc(&php_stream_zipio_ops, self, NULL, mode);

			if (opened_path) {
				*opened_path = zend_string_init(path, path_len + 1 + fragment_len, 0);
			}
		} else {
			zip_close(za);
		}
	}

	return stream;
}
/* }}} */

static php_stream_wrapper_ops zip_stream_wops = {
	php_stream_zip_opener,
	NULL,	/* close */
	NULL,	/* fstat */
	NULL,	/* stat */
	NULL,	/* opendir */
	"zip wrapper",
	NULL,	/* unlink */
	NULL,	/* rename */
	NULL,	/* mkdir */
	NULL	/* rmdir */
};

php_stream_wrapper php_stream_zip_wrapper = {
	&zip_stream_wops,
	NULL,
	0 /* is_url */
};
#endif /* HAVE_ZIP */
