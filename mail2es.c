#include <stdio.h>
#include <stdlib.h>
#include <sysexits.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <string.h>

#include <glib.h>
#include <gmime/gmime.h>
#include <json.h>


struct mail_part {
    struct json_object *mail_tree;     /* global mail tree in JSON */
    int depth;                  /* depth of the current part */
    int rank;                   /* rank of the current part in this depth */
    char id[20];                /* id of the curent part */
    GMimeObject *last_part;     /* the part (GMime) before this one */
    char last_node[20];         /* the part (JSON) before this one */

};

struct ct_type {
    const char *type;
    const char *subtype;
};
struct file_type_mapping {
    const char **suffix;
    struct ct_type *ct;
};

#define S (const char *[])
#define T (struct ct_type[])
static struct file_type_mapping mappings[] = {
    { S{ "txt", NULL }, T{ {"text","plain"}, {"application","txt"}, {NULL,NULL} } },
    { S{ "html", "htm", NULL }, T{ {"text","html"}, {NULL,NULL} } },
    { S{ "c", NULL },   T{ {"text","x-csrc"}, {NULL,NULL} } },
    { S{ "pdf", NULL }, T{ {"application","pdf"}, {"application","x-pdf"}, {"text","pdf"}, {"text","x-pdf"}, {NULL,NULL} } },
    { S{ "rtf", NULL }, T{ {"application","rtf"}, {"application","x-rtf"}, {"text","rtf"}, {"text","richtext"}, {NULL,NULL} } },
    { S{ "doc", NULL }, T{ {"application","msword"}, {"application","x-msword"}, {"application","vnd.msword"}, {"application","vnd.ms-word"}, {NULL,NULL} } },
    { S{ "docx", NULL },T{ {"application","vnd.openxmlformats-officedocument.wordprocessingml.document"}, {NULL,NULL} } },
    { S{ "xls", NULL }, T{ {"application","vnd.ms-excel"}, {"application","msexcel"}, {"application","x-msexcel"}, {NULL,NULL} } },
    { S{ "xlsx", NULL },T{ {"application","vnd.openxmlformats-officedocument.spreadsheetml.sheet"}, {NULL,NULL} } },
    { S{ "ppt", NULL }, T{ {"application","vnd.ms-powerpoint"}, {"application","mspowerpoint"}, {"application","ms-powerpoint"}, {"application","x-mspowerpoint"}, {NULL,NULL} } },
    { S{ "pptx", NULL },T{ {"application","vnd.openxmlformats-officedocument.presentationml.presentation"}, {NULL,NULL} } },
    { S{ "odt", NULL }, T{ {"application","vnd.oasis.opendocument.text"}, {NULL,NULL} } },
    { S{ "ods", NULL }, T{ {"application","vnd.oasis.opendocument.spreadsheet"}, {NULL,NULL} } },
    { S{ "odp", NULL }, T{ {"application","vnd.oasis.opendocument.presentation"}, {NULL,NULL} } },
    { NULL, NULL }
};

extern int optind;
static int debug = 0;

static void usage(const char *name, int error);


static void get_content_type_from_filename(const char *filename,
                                           GMimeContentType **content_type)
{
    int i = -1, j;
    const char *suffix;

    /* can't deduce the content type */
    if (filename == NULL) {
        *content_type = NULL;
        return;
    }

    suffix = strrchr(filename, '.');

    if (suffix++) {
        while (mappings[++i].suffix) {
            for (j = 0; mappings[i].suffix[j]; j++)
                if (!strcasecmp(suffix, mappings[i].suffix[j])) {
                    g_mime_content_type_set_media_type(*content_type, mappings[i].ct[0].type);
                    g_mime_content_type_set_media_subtype(*content_type, mappings[i].ct[0].subtype);
                    return;
                }
        }
    }
}

static int is_indexable(GMimeContentType *content_type, const char *filename)
{
    int i = -1, j;

    /* try to find the real content-type from the attachment file name */
    if (g_mime_content_type_is_type(content_type, "application", "octet-stream"))
        get_content_type_from_filename(filename, &content_type);

    /* should we use lib magic for unknown content type */
    if (!content_type)
        return 0;

    while (mappings[++i].ct) {
        for (j = 0; mappings[i].ct[j].type; j++)
            if (g_mime_content_type_is_type(content_type,
                                            mappings[i].ct[j].type,
                                            mappings[i].ct[j].subtype))
                return 1;
    }

    return 0;
}

/*
 * Format the given part to JSON and add the node to the JSON tree.
 */
static void format_part(GMimeObject *part, struct mail_part *info)
{
    GMimeStream *outstream;
    GMimeStream *filtered_stream;
    GMimeContentType *content_type;
    GByteArray *bodypart;
    GMimeDataWrapper *wrapper;
    struct json_object *node;
    const char *filename;
    const char *field;

    content_type = g_mime_object_get_content_type(part);
    filename = g_mime_part_get_filename((GMimePart *)part);

    /*
     * add headers of the part
     */
    node = json_object_new_object();
    json_object_object_add(node, "content-type",
                           json_object_new_string(g_mime_content_type_to_string(content_type)));
    if (filename)
        json_object_object_add(node, "filename", json_object_new_string(filename));


    /*
     * add body of the part
     */
    outstream = g_mime_stream_mem_new();
    filtered_stream = g_mime_stream_filter_new(outstream);

    /* add filters to convert text in UTF-8 and binary in base64 */
    if (g_mime_content_type_is_type(content_type, "text", "*")) {
        GMimeFilter *decode_filter;
        GMimeFilter *charset_filter;
        const char *charset;

        /* default charset for text/plain is ASCII
         * choose arbitrarily ISO-8859-1 for others content type */
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
        field = "body";
    }
    else {
        GMimeFilter *binary_filter = g_mime_filter_basic_new(GMIME_CONTENT_ENCODING_BASE64, TRUE);

        g_mime_stream_filter_add(GMIME_STREAM_FILTER (filtered_stream), binary_filter);
        g_object_unref(binary_filter);
        field = "file";
    }

    /* apply filters */
    wrapper = g_mime_part_get_content_object(GMIME_PART(part));
    g_mime_data_wrapper_write_to_stream(wrapper, filtered_stream);
    g_mime_stream_flush(filtered_stream);
    g_object_unref(filtered_stream);
    g_mime_stream_reset(outstream);

    /* add result to JSON tree */
    bodypart = g_mime_stream_mem_get_byte_array((GMimeStreamMem*) outstream);
    json_object_object_add(node, field,
                           json_object_new_string_len((const char *)bodypart->data, (int)bodypart->len));
    json_object_object_add(info->mail_tree, info->id, node);
    g_object_unref(outstream);
}

static void process_part(GMimeObject *parent, GMimeObject *part, gpointer rock)
{
    struct mail_part *cur = rock;
    GMimeContentType *content_type;
    GMimeContentType *parent_content_type;

    content_type = g_mime_object_get_content_type(part);
    parent_content_type = g_mime_object_get_content_type(parent);

    if (debug) {
        fprintf(stdout, "found... %*.s(%d)%s> %s ", 8*cur->depth, "", cur->depth,
                parent_content_type ? g_mime_content_type_to_string(parent_content_type) : "null",
                g_mime_content_type_to_string(content_type));
    }

    /* message/rfc822 or message/news */
    if (GMIME_IS_MESSAGE_PART(part)) {
        GMimeMessage *message;

        message = g_mime_message_part_get_message((GMimeMessagePart *) part);
        cur->last_part = part;
        g_mime_message_foreach(message, process_part, cur);
    }

    /* don't handle such very rare type of part */
    else if (GMIME_IS_MESSAGE_PARTIAL(part)) {
    }

    /* multipart/mixed, multipart/alternative,
     * multipart/related, multipart/signed,
     * multipart/encrypted, etc... */
    else if (GMIME_IS_MULTIPART(part)) {
    }

    /* a normal leaf part */
    else if (GMIME_IS_PART(part)) {
        /* XXX Fix depth that must decrease on exit of multipart */
        if (GMIME_IS_MULTIPART(parent) || GMIME_IS_MESSAGE_PART(parent)) {
            /* enter in a new level */
            if (parent == cur->last_part) {
                cur->depth++;
                cur->rank = 0;
            }
            /* prefer the last part in a multipart alternative set */
            else if (g_mime_content_type_is_type(parent_content_type, "multipart", "alternative")) {
                if (debug)
                    fprintf(stdout, "(which replace %s) ", cur->last_node);
                json_object_object_del(cur->mail_tree, cur->last_node);
            }
        }
        sprintf(cur->id, "part-%d.%d", cur->depth, ++(cur->rank));

        if (debug)
            fprintf(stdout, "[%s]", cur->id);

        /* don't index unknown part type or content-type not in the mappings list */
        if (is_indexable(content_type, g_mime_part_get_filename((GMimePart *)part)))
            format_part(part, cur);
    }
    else {
        /* unknown part type ... */
    }

    cur->last_part = part;
    snprintf(cur->last_node, 20, cur->id);

    if (debug)
        fprintf(stdout, "\n");
}

/*
 * Return a JSON document ready to use with Elasticsearch
 */
static void process_mail(int fd, struct json_object **es_json_doc)
{
    GMimeStream *mail_stream;
    GMimeParser *parser;
    GMimeMessage *message;
    InternetAddressList *recipients;
    InternetAddress *recipient;
    struct json_object *json_rcpts;
    const char *str;
    int i;
    struct mail_part rock;
    time_t date;

    //mail_stream = g_mime_stream_mmap_new(fd, PROT_READ, MAP_SHARED);
    mail_stream = g_mime_stream_fs_new(fd);
    parser = g_mime_parser_new_with_stream(mail_stream);
    message = g_mime_parser_construct_message(parser);

    g_object_unref(parser);
    g_object_unref(mail_stream);

    memset(&rock, 0, sizeof(struct mail_part));
    rock.mail_tree = json_object_new_object();
    rock.depth = 0;

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
    g_mime_message_get_date(message, &date, NULL);
    json_object_object_add(rock.mail_tree, "date", json_object_new_int64(date));

    /* add message parts */
    g_mime_message_foreach(message, process_part, &rock);
    g_object_unref(message);

    *es_json_doc = rock.mail_tree;
}


int main(int argc, char **argv)
{
    int option, fd;
    struct json_object *es_json_doc = NULL;

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

    /* XXX gmime needs to seek and doesn't work with stdin */
    if (optind == argc || *argv[optind] == '-') { /* read from stdin */
        fd = 0;
    }
    else {
        if ((fd = open(argv[optind], O_RDONLY)) == -1) {
            fprintf(stderr, "Cannot open mail: %s: %m", argv[1]);
            exit(1);
        }
    }

    g_mime_init(0);

    process_mail(fd, &es_json_doc);
    if (es_json_doc == NULL) {
        return EXIT_FAILURE;
    }

    g_mime_shutdown();

    fprintf(stdout, "%s\n", json_object_to_json_string_ext(es_json_doc, JSON_C_TO_STRING_PRETTY));
    return EXIT_SUCCESS;
}

static void usage(const char *name, int error)
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
