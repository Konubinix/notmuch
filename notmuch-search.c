/* notmuch - Not much of an email program, (just index and search)
 *
 * Copyright © 2009 Carl Worth
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see http://www.gnu.org/licenses/ .
 *
 * Author: Carl Worth <cworth@cworth.org>
 */

#include "notmuch-client.h"
#include "sprinter.h"
#include "string-util.h"

typedef enum {
    OUTPUT_SUMMARY	= 1 << 0,
    OUTPUT_THREADS	= 1 << 1,
    OUTPUT_MESSAGES	= 1 << 2,
    OUTPUT_FILES	= 1 << 3,
    OUTPUT_TAGS		= 1 << 4,
    OUTPUT_SENDER	= 1 << 5,
    OUTPUT_RECIPIENTS	= 1 << 6,
} output_t;

#define OUTPUT_ADDRESS_FLAGS (OUTPUT_SENDER | OUTPUT_RECIPIENTS)

typedef struct {
    sprinter_t *format;
    notmuch_query_t *query;
    notmuch_sort_t sort;
    output_t output;
    int offset;
    int limit;
    int dupe;
} search_options_t;

typedef struct {
    const char *name;
    const char *addr;
} mailbox_t;

/* Return two stable query strings that identify exactly the matched
 * and unmatched messages currently in thread.  If there are no
 * matched or unmatched messages, the returned buffers will be
 * NULL. */
static int
get_thread_query (notmuch_thread_t *thread,
		  char **matched_out, char **unmatched_out)
{
    notmuch_messages_t *messages;
    char *escaped = NULL;
    size_t escaped_len = 0;

    *matched_out = *unmatched_out = NULL;

    for (messages = notmuch_thread_get_messages (thread);
	 notmuch_messages_valid (messages);
	 notmuch_messages_move_to_next (messages))
    {
	notmuch_message_t *message = notmuch_messages_get (messages);
	const char *mid = notmuch_message_get_message_id (message);
	/* Determine which query buffer to extend */
	char **buf = notmuch_message_get_flag (
	    message, NOTMUCH_MESSAGE_FLAG_MATCH) ? matched_out : unmatched_out;
	/* Add this message's id: query.  Since "id" is an exclusive
	 * prefix, it is implicitly 'or'd together, so we only need to
	 * join queries with a space. */
	if (make_boolean_term (thread, "id", mid, &escaped, &escaped_len) < 0)
	    return -1;
	if (*buf)
	    *buf = talloc_asprintf_append_buffer (*buf, " %s", escaped);
	else
	    *buf = talloc_strdup (thread, escaped);
	if (!*buf)
	    return -1;
    }
    talloc_free (escaped);
    return 0;
}

static int
do_search_threads (search_options_t *opt)
{
    notmuch_thread_t *thread;
    notmuch_threads_t *threads;
    notmuch_tags_t *tags;
    sprinter_t *format = opt->format;
    time_t date;
    int i;

    if (opt->offset < 0) {
	opt->offset += notmuch_query_count_threads (opt->query);
	if (opt->offset < 0)
	    opt->offset = 0;
    }

    threads = notmuch_query_search_threads (opt->query);
    if (threads == NULL)
	return 1;

    format->begin_list (format);

    for (i = 0;
	 notmuch_threads_valid (threads) && (opt->limit < 0 || i < opt->offset + opt->limit);
	 notmuch_threads_move_to_next (threads), i++)
    {
	thread = notmuch_threads_get (threads);

	if (i < opt->offset) {
	    notmuch_thread_destroy (thread);
	    continue;
	}

	if (opt->output == OUTPUT_THREADS) {
	    format->set_prefix (format, "thread");
	    format->string (format,
			    notmuch_thread_get_thread_id (thread));
	    format->separator (format);
	} else { /* output == OUTPUT_SUMMARY */
	    void *ctx_quote = talloc_new (thread);
	    const char *authors = notmuch_thread_get_authors (thread);
	    const char *subject = notmuch_thread_get_subject (thread);
	    const char *thread_id = notmuch_thread_get_thread_id (thread);
	    int matched = notmuch_thread_get_matched_messages (thread);
	    int total = notmuch_thread_get_total_messages (thread);
	    const char *relative_date = NULL;
	    notmuch_bool_t first_tag = TRUE;

	    format->begin_map (format);

	    if (opt->sort == NOTMUCH_SORT_OLDEST_FIRST)
		date = notmuch_thread_get_oldest_date (thread);
	    else
		date = notmuch_thread_get_newest_date (thread);

	    relative_date = notmuch_time_relative_date (ctx_quote, date);

	    if (format->is_text_printer) {
                /* Special case for the text formatter */
		printf ("thread:%s %12s [%d/%d] %s; %s (",
			thread_id,
			relative_date,
			matched,
			total,
			sanitize_string (ctx_quote, authors),
			sanitize_string (ctx_quote, subject));
	    } else { /* Structured Output */
		format->map_key (format, "thread");
		format->string (format, thread_id);
		format->map_key (format, "timestamp");
		format->integer (format, date);
		format->map_key (format, "date_relative");
		format->string (format, relative_date);
		format->map_key (format, "matched");
		format->integer (format, matched);
		format->map_key (format, "total");
		format->integer (format, total);
		format->map_key (format, "authors");
		format->string (format, authors);
		format->map_key (format, "subject");
		format->string (format, subject);
		if (notmuch_format_version >= 2) {
		    char *matched_query, *unmatched_query;
		    if (get_thread_query (thread, &matched_query,
					  &unmatched_query) < 0) {
			fprintf (stderr, "Out of memory\n");
			return 1;
		    }
		    format->map_key (format, "query");
		    format->begin_list (format);
		    if (matched_query)
			format->string (format, matched_query);
		    else
			format->null (format);
		    if (unmatched_query)
			format->string (format, unmatched_query);
		    else
			format->null (format);
		    format->end (format);
		}
	    }

	    talloc_free (ctx_quote);

	    format->map_key (format, "tags");
	    format->begin_list (format);

	    for (tags = notmuch_thread_get_tags (thread);
		 notmuch_tags_valid (tags);
		 notmuch_tags_move_to_next (tags))
	    {
		const char *tag = notmuch_tags_get (tags);

		if (format->is_text_printer) {
                  /* Special case for the text formatter */
		    if (first_tag)
			first_tag = FALSE;
		    else
			fputc (' ', stdout);
		    fputs (tag, stdout);
		} else { /* Structured Output */
		    format->string (format, tag);
		}
	    }

	    if (format->is_text_printer)
		printf (")");

	    format->end (format);
	    format->end (format);
	    format->separator (format);
	}

	notmuch_thread_destroy (thread);
    }

    format->end (format);

    return 0;
}

static void
print_mailbox (const search_options_t *opt, const mailbox_t *mailbox)
{
    const char *name = mailbox->name;
    const char *addr = mailbox->addr;
    sprinter_t *format = opt->format;
    InternetAddress *ia = internet_address_mailbox_new (name, addr);
    char *name_addr;

    /* name_addr has the name part quoted if necessary. Compare
     * 'John Doe <john@doe.com>' vs. '"Doe, John" <john@doe.com>' */
    name_addr = internet_address_to_string (ia, FALSE);

    if (format->is_text_printer) {
	format->string (format, name_addr);
	format->separator (format);
    } else {
	format->begin_map (format);
	format->map_key (format, "name");
	format->string (format, name);
	format->map_key (format, "address");
	format->string (format, addr);
	format->map_key (format, "name-addr");
	format->string (format, name_addr);
	format->end (format);
	format->separator (format);
    }

    g_object_unref (ia);
    g_free (name_addr);
}

/* Print addresses from InternetAddressList.  */
static void
process_address_list (const search_options_t *opt, InternetAddressList *list)
{
    InternetAddress *address;
    int i;

    for (i = 0; i < internet_address_list_length (list); i++) {
	address = internet_address_list_get_address (list, i);
	if (INTERNET_ADDRESS_IS_GROUP (address)) {
	    InternetAddressGroup *group;
	    InternetAddressList *group_list;

	    group = INTERNET_ADDRESS_GROUP (address);
	    group_list = internet_address_group_get_members (group);
	    if (group_list == NULL)
		continue;

	    process_address_list (opt, group_list);
	} else {
	    InternetAddressMailbox *mailbox = INTERNET_ADDRESS_MAILBOX (address);
	    mailbox_t mbx = {
		.name = internet_address_get_name (address),
		.addr = internet_address_mailbox_get_addr (mailbox),
	    };

	    print_mailbox (opt, &mbx);
	}
    }
}

/* Print addresses from a message header.  */
static void
process_address_header (const search_options_t *opt, const char *value)
{
    InternetAddressList *list;

    if (value == NULL)
	return;

    list = internet_address_list_parse_string (value);
    if (list == NULL)
	return;

    process_address_list (opt, list);

    g_object_unref (list);
}

static int
_count_filenames (notmuch_message_t *message)
{
    notmuch_filenames_t *filenames;
    int i = 0;

    filenames = notmuch_message_get_filenames (message);

    while (notmuch_filenames_valid (filenames)) {
        notmuch_filenames_move_to_next (filenames);
        i++;
    }

    notmuch_filenames_destroy (filenames);

    return i;
}

static int
do_search_messages (search_options_t *opt)
{
    notmuch_message_t *message;
    notmuch_messages_t *messages;
    notmuch_filenames_t *filenames;
    sprinter_t *format = opt->format;
    int i;

    if (opt->offset < 0) {
	opt->offset += notmuch_query_count_messages (opt->query);
	if (opt->offset < 0)
	    opt->offset = 0;
    }

    messages = notmuch_query_search_messages (opt->query);
    if (messages == NULL)
	return 1;

    format->begin_list (format);

    for (i = 0;
	 notmuch_messages_valid (messages) && (opt->limit < 0 || i < opt->offset + opt->limit);
	 notmuch_messages_move_to_next (messages), i++)
    {
	if (i < opt->offset)
	    continue;

	message = notmuch_messages_get (messages);

	if (opt->output == OUTPUT_FILES) {
	    int j;
	    filenames = notmuch_message_get_filenames (message);

	    for (j = 1;
		 notmuch_filenames_valid (filenames);
		 notmuch_filenames_move_to_next (filenames), j++)
	    {
		if (opt->dupe < 0 || opt->dupe == j) {
		    format->string (format, notmuch_filenames_get (filenames));
		    format->separator (format);
		}
	    }
	    
	    notmuch_filenames_destroy( filenames );

	} else if (opt->output == OUTPUT_MESSAGES) {
            /* special case 1 for speed */
            if (opt->dupe <= 1 || opt->dupe <= _count_filenames (message)) {
                format->set_prefix (format, "id");
                format->string (format,
                                notmuch_message_get_message_id (message));
                format->separator (format);
            }
	} else {
	    if (opt->output & OUTPUT_SENDER) {
		const char *addrs;

		addrs = notmuch_message_get_header (message, "from");
		process_address_header (opt, addrs);
	    }

	    if (opt->output & OUTPUT_RECIPIENTS) {
		const char *hdrs[] = { "to", "cc", "bcc" };
		const char *addrs;
		size_t j;

		for (j = 0; j < ARRAY_SIZE (hdrs); j++) {
		    addrs = notmuch_message_get_header (message, hdrs[j]);
		    process_address_header (opt, addrs);
		}
	    }
	}

	notmuch_message_destroy (message);
    }

    notmuch_messages_destroy (messages);

    format->end (format);

    return 0;
}

static int
do_search_tags (notmuch_database_t *notmuch,
		const search_options_t *opt)
{
    notmuch_messages_t *messages = NULL;
    notmuch_tags_t *tags;
    const char *tag;
    sprinter_t *format = opt->format;
    notmuch_query_t *query = opt->query;

    /* should the following only special case if no excluded terms
     * specified? */

    /* Special-case query of "*" for better performance. */
    if (strcmp (notmuch_query_get_query_string (query), "*") == 0) {
	tags = notmuch_database_get_all_tags (notmuch);
    } else {
	messages = notmuch_query_search_messages (query);
	if (messages == NULL)
	    return 1;

	tags = notmuch_messages_collect_tags (messages);
    }
    if (tags == NULL)
	return 1;

    format->begin_list (format);

    for (;
	 notmuch_tags_valid (tags);
	 notmuch_tags_move_to_next (tags))
    {
	tag = notmuch_tags_get (tags);

	format->string (format, tag);
	format->separator (format);

    }

    notmuch_tags_destroy (tags);

    if (messages)
	notmuch_messages_destroy (messages);

    format->end (format);

    return 0;
}

int
notmuch_search_command (notmuch_config_t *config, int argc, char *argv[])
{
    notmuch_database_t *notmuch;
    search_options_t opt = {
	.sort = NOTMUCH_SORT_NEWEST_FIRST,
	.output = 0,
	.offset = 0,
	.limit = -1, /* unlimited */
	.dupe = -1,
    };
    char *query_str;
    int opt_index, ret;
    notmuch_exclude_t exclude = NOTMUCH_EXCLUDE_TRUE;
    unsigned int i;

    enum {
	NOTMUCH_FORMAT_JSON,
	NOTMUCH_FORMAT_TEXT,
	NOTMUCH_FORMAT_TEXT0,
	NOTMUCH_FORMAT_SEXP
    } format_sel = NOTMUCH_FORMAT_TEXT;

    notmuch_opt_desc_t options[] = {
	{ NOTMUCH_OPT_KEYWORD, &opt.sort, "sort", 's',
	  (notmuch_keyword_t []){ { "oldest-first", NOTMUCH_SORT_OLDEST_FIRST },
				  { "newest-first", NOTMUCH_SORT_NEWEST_FIRST },
				  { 0, 0 } } },
	{ NOTMUCH_OPT_KEYWORD, &format_sel, "format", 'f',
	  (notmuch_keyword_t []){ { "json", NOTMUCH_FORMAT_JSON },
				  { "sexp", NOTMUCH_FORMAT_SEXP },
				  { "text", NOTMUCH_FORMAT_TEXT },
				  { "text0", NOTMUCH_FORMAT_TEXT0 },
				  { 0, 0 } } },
	{ NOTMUCH_OPT_INT, &notmuch_format_version, "format-version", 0, 0 },
	{ NOTMUCH_OPT_KEYWORD_FLAGS, &opt.output, "output", 'o',
	  (notmuch_keyword_t []){ { "summary", OUTPUT_SUMMARY },
				  { "threads", OUTPUT_THREADS },
				  { "messages", OUTPUT_MESSAGES },
				  { "sender", OUTPUT_SENDER },
				  { "recipients", OUTPUT_RECIPIENTS },
				  { "files", OUTPUT_FILES },
				  { "tags", OUTPUT_TAGS },
				  { 0, 0 } } },
        { NOTMUCH_OPT_KEYWORD, &exclude, "exclude", 'x',
          (notmuch_keyword_t []){ { "true", NOTMUCH_EXCLUDE_TRUE },
                                  { "false", NOTMUCH_EXCLUDE_FALSE },
                                  { "flag", NOTMUCH_EXCLUDE_FLAG },
                                  { "all", NOTMUCH_EXCLUDE_ALL },
                                  { 0, 0 } } },
	{ NOTMUCH_OPT_INT, &opt.offset, "offset", 'O', 0 },
	{ NOTMUCH_OPT_INT, &opt.limit, "limit", 'L', 0  },
	{ NOTMUCH_OPT_INT, &opt.dupe, "duplicate", 'D', 0  },
	{ 0, 0, 0, 0, 0 }
    };

    opt_index = parse_arguments (argc, argv, options, 1);
    if (opt_index < 0)
	return EXIT_FAILURE;

    if (! opt.output)
	opt.output = OUTPUT_SUMMARY;

    if (opt.output != OUTPUT_FILES && opt.output != OUTPUT_MESSAGES &&
	opt.dupe != -1) {
        fprintf (stderr, "Error: --duplicate=N is only supported with --output=files and --output=messages.\n");
        return EXIT_FAILURE;
    }

    switch (format_sel) {
    case NOTMUCH_FORMAT_TEXT:
	opt.format = sprinter_text_create (config, stdout);
	break;
    case NOTMUCH_FORMAT_TEXT0:
	if (opt.output == OUTPUT_SUMMARY) {
	    fprintf (stderr, "Error: --format=text0 is not compatible with --output=summary.\n");
	    return EXIT_FAILURE;
	}
	opt.format = sprinter_text0_create (config, stdout);
	break;
    case NOTMUCH_FORMAT_JSON:
	opt.format = sprinter_json_create (config, stdout);
	break;
    case NOTMUCH_FORMAT_SEXP:
	opt.format = sprinter_sexp_create (config, stdout);
	break;
    default:
	/* this should never happen */
	INTERNAL_ERROR("no output format selected");
    }

    notmuch_exit_if_unsupported_format ();

    if (notmuch_database_open (notmuch_config_get_database_path (config),
			       NOTMUCH_DATABASE_MODE_READ_ONLY, &notmuch))
	return EXIT_FAILURE;

    query_str = query_string_from_args (notmuch, argc-opt_index, argv+opt_index);
    if (query_str == NULL) {
	fprintf (stderr, "Out of memory.\n");
	return EXIT_FAILURE;
    }
    if (*query_str == '\0') {
	fprintf (stderr, "Error: notmuch search requires at least one search term.\n");
	return EXIT_FAILURE;
    }

    opt.query = notmuch_query_create (notmuch, query_str);
    if (opt.query == NULL) {
	fprintf (stderr, "Out of memory\n");
	return EXIT_FAILURE;
    }

    notmuch_query_set_sort (opt.query, opt.sort);

    if (exclude == NOTMUCH_EXCLUDE_FLAG && opt.output != OUTPUT_SUMMARY) {
	/* If we are not doing summary output there is nowhere to
	 * print the excluded flag so fall back on including the
	 * excluded messages. */
	fprintf (stderr, "Warning: this output format cannot flag excluded messages.\n");
	exclude = NOTMUCH_EXCLUDE_FALSE;
    }

    if (exclude != NOTMUCH_EXCLUDE_FALSE) {
	const char **search_exclude_tags;
	size_t search_exclude_tags_length;

	search_exclude_tags = notmuch_config_get_search_exclude_tags
	    (config, &search_exclude_tags_length);
	for (i = 0; i < search_exclude_tags_length; i++)
	    notmuch_query_add_tag_exclude (opt.query, search_exclude_tags[i]);
	notmuch_query_set_omit_excluded (opt.query, exclude);
    }

    if (opt.output == OUTPUT_SUMMARY ||
	opt.output == OUTPUT_THREADS)
	ret = do_search_threads (&opt);
    else if (opt.output == OUTPUT_MESSAGES ||
	     opt.output == OUTPUT_FILES ||
	     (opt.output & OUTPUT_ADDRESS_FLAGS && !(opt.output & ~OUTPUT_ADDRESS_FLAGS)))
	ret = do_search_messages (&opt);
    else if (opt.output == OUTPUT_TAGS)
	ret = do_search_tags (notmuch, &opt);
    else {
	fprintf (stderr, "Error: the combination of outputs is not supported.\n");
	ret = 1;
    }

    notmuch_query_destroy (opt.query);
    notmuch_database_destroy (notmuch);

    talloc_free (opt.format);

    return ret ? EXIT_FAILURE : EXIT_SUCCESS;
}
