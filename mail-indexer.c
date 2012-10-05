#include <stdio.h>
#include <stdlib.h>
#include <sysexits.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <string.h>

#include <glib.h>
#include <gmime/gmime.h>
#include <json.h>


enum part_type { MESSAGE_PART, MESSAGE_PARTIAL, MULTIPART, PART };

struct mail_part {
    json_object *mail_tree;   /* global mail tree in JSON */
    json_object *current;     /* current part in JSON */
    int level;                /* depth of the part */
    int rank;                 /* rank of the part in the current depth */
    enum part_type last_type; /* type of the part before this one */
    char id[20];              /* id of the part */
};

extern int optind;
static int debug = 0;


static void parse_part(GMimeObject *parent, GMimeObject *part, gpointer rock)
{
    //struct mail_part *prev = rock;
    struct mail_part *cur = rock;
    json_object *node;
    const char *str;

    if (debug) {
        fprintf(stdout, "%s> %s\n",
                g_mime_object_get_content_type(parent) ? g_mime_content_type_to_string(g_mime_object_get_content_type(parent)) : "null",
                g_mime_content_type_to_string(g_mime_object_get_content_type(part)));
    }

    if (GMIME_IS_MESSAGE_PART(part)) {
        /* message/rfc822 or message/news */
        GMimeMessage *message;

        message = g_mime_message_part_get_message((GMimeMessagePart *) part);
        cur->last_type = MESSAGE_PART;
        g_mime_message_foreach(message, parse_part, cur);
    }
    else if (GMIME_IS_MESSAGE_PARTIAL(part)) {
        /* don't handle such very rare type of part */

        cur->last_type = MESSAGE_PARTIAL;
    }
    else if (GMIME_IS_MULTIPART(part)) {
        /* multipart/mixed, multipart/alternative,
         * multipart/related, multipart/signed,
         * multipart/encrypted, etc... */

        cur->last_type = MULTIPART;
    }
    else if (GMIME_IS_PART(part)) {
        /* a normal leaf part */
        GMimeStream *outstream;
        GMimeStream *filtered_stream;
        GMimeContentType *content_type;
        GByteArray *bodypart;
        GMimeDataWrapper *wrapper;

        if (cur->last_type == MULTIPART || cur->last_type == MESSAGE_PART) {
            cur->level++;
            cur->rank = 0;
        }
        sprintf(cur->id, "part-%d.%d", cur->level, ++(cur->rank));

        node = json_object_new_object();
        content_type = g_mime_object_get_content_type(part);

        if (content_type != NULL) {
            str = g_mime_content_type_to_string(content_type);
            json_object_object_add(node, "content-type", json_object_new_string(str));
        }
        if ((str = g_mime_part_get_filename((GMimePart *)part)) != NULL) {
            json_object_object_add(node, "filename", json_object_new_string(str));
        }

        /*
         * prepare the output stream with filters
         */
        outstream = g_mime_stream_mem_new();
        filtered_stream = g_mime_stream_filter_new(outstream);

        /*
         * filter text in UTF-8 and binary in base64
         */
        if (g_mime_content_type_is_type(content_type, "text", "*")) {
            GMimeFilter *decode_filter;
            GMimeFilter *charset_filter;
            const char *charset;

            /* default charset for text/plain is ASCII
             * choose arbitrarily ISO-8859-1 for others */
            charset = g_mime_content_type_get_parameter(content_type, "charset");
            if (charset == NULL) {
                if (g_mime_content_type_is_type(content_type, "text", "plain"))
                    charset = "ascii";
                else
                    charset = "iso-8859-1";
            }

            decode_filter = g_mime_filter_basic_new(GMIME_CONTENT_ENCODING_8BIT, TRUE);
            charset_filter = g_mime_filter_charset_new(charset, "UTF-8");
            if (charset_filter == NULL)
                fprintf(stderr, "[ERR] charset conversion is not possible from:%s to:UTF-8\n", charset);

            g_mime_stream_filter_add(GMIME_STREAM_FILTER (filtered_stream), decode_filter);
            g_mime_stream_filter_add(GMIME_STREAM_FILTER (filtered_stream), charset_filter);
            g_object_unref(decode_filter);
            g_object_unref(charset_filter);
        }
        else {
            GMimeFilter *binary_filter = g_mime_filter_basic_new(GMIME_CONTENT_ENCODING_BASE64, TRUE);

            g_mime_stream_filter_add(GMIME_STREAM_FILTER (filtered_stream), binary_filter);
            g_object_unref(binary_filter);
        }

        /*
         * apply filters
         */
        wrapper = g_mime_part_get_content_object(GMIME_PART(part));
        g_mime_data_wrapper_write_to_stream(wrapper, filtered_stream);
        g_mime_stream_flush(filtered_stream);
        g_object_unref(filtered_stream);
        g_mime_stream_reset(outstream);

        /*
         * add result to JSON tree
         */
        bodypart = g_mime_stream_mem_get_byte_array((GMimeStreamMem*) outstream);
        json_object_object_add(node, "body",
                               json_object_new_string_len((const char *)bodypart->data, (int)bodypart->len));
        json_object_object_add(cur->mail_tree, cur->id, node);
        g_object_unref(outstream);
    }
    else {
        /* unknown part type ... */
    }
}

/*
 * Return a JSON document to send to Elasticsearch for indexing
 */
static void parse_mail(int fd, json_object **es_json_doc)
{
    GMimeStream *mail_stream;
    GMimeParser *parser;
    GMimeMessage *message;
    InternetAddressList *recipients;
    InternetAddress *recipient;
    json_object *json_rcpts;
    const char *str;
    int i;
    struct mail_part rock;

    //mail_stream = g_mime_stream_mmap_new(fd, PROT_READ, MAP_SHARED);
    mail_stream = g_mime_stream_fs_new(fd);
    parser = g_mime_parser_new_with_stream(mail_stream);
    message = g_mime_parser_construct_message(parser);

    g_object_unref(parser);
    g_object_unref(mail_stream);

    memset(&rock, 0, sizeof(struct mail_part));
    rock.mail_tree = json_object_new_object();
    rock.level = 0;

    /* add message headers */
    if ((str = g_mime_message_get_sender(message)) != NULL) {
        json_object_object_add(rock.mail_tree, "from", json_object_new_string(str));
    }
    if ((recipients = g_mime_message_get_all_recipients(message)) != NULL) {
        json_rcpts = json_object_new_array();
        for (i = 0; i < internet_address_list_length(recipients); i++) {
            recipient = internet_address_list_get_address(recipients, i);
            json_object_array_add(json_rcpts,
                                  json_object_new_string(internet_address_to_string(recipient, FALSE)));
        }
        json_object_object_add(rock.mail_tree, "to", json_rcpts);
    }
    if ((str = g_mime_message_get_subject(message)) != NULL) {
        json_object_object_add(rock.mail_tree, "subject", json_object_new_string(str));
    }
    if ((str = g_mime_message_get_date_as_string (message)) != NULL) {
        json_object_object_add(rock.mail_tree, "date", json_object_new_string(str));
    }

    /* add message parts */
    g_mime_message_foreach(message, parse_part, &rock);

    *es_json_doc = rock.mail_tree;
}

static int usage(const char *name, int error)
{
    FILE *out = error ? stderr : stdout;

    fprintf(out, "usage: %s [OPTIONS]... [FILE]\n", name);
    fprintf(out, "Produce JSON for Elasticsearch from mail from FILE,"
                 " or standard input, to standard output.\n");
    fprintf(out, "\n");
    fprintf(out, "  -d,     enable verbose mode\n");
    fprintf(out, "  -h,     display this help and exit\n");
    fprintf(out, "\n");
    fprintf(out, "With no FILE, or when FILE is -, read standard input.\n");

    error ? exit(EX_USAGE) : exit(0);
}

int main(int argc, char **argv)
{
    int option, fd;
    json_object *es_json_doc = NULL;

    while ((option = getopt(argc, argv, "dh")) != EOF) {
        switch(option) {
        case 'd':
            debug = 1;
            break;
        case 'h':
            usage(argv[0], 0);
            break;
        default:
            usage(argv[0], 1);
            break;
        }
    }

    if (optind == argc || *argv[optind] == '-') { /* read from stdin */
        fd = 0;
    }
    else {
        if ((fd = open(argv[optind], O_LARGEFILE, O_RDONLY)) == -1) {
            fprintf(stderr, "Cannot open mail: %s: %m", argv[1]);
            exit(1);
        }
    }

    g_mime_init(0);

    parse_mail(fd, &es_json_doc);
    if (es_json_doc == NULL) {
        return EXIT_FAILURE;
    }

    g_mime_shutdown();

    if (debug)
        fprintf(stdout, "--\n");
    fprintf(stdout, "%s", json_object_to_json_string_ext(es_json_doc, JSON_C_TO_STRING_PRETTY));

    return EXIT_SUCCESS;
}
